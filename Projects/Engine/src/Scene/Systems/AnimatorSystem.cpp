/// @file    AnimatorSystem.cpp
/// @brief   スケルタルアニメーションのサンプリングとスキニングパレットのアップロード。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#include <Engine/Scene/Systems/AnimatorSystem.hpp>
#include <Engine/Scene/SkinnedPoseBounds.hpp>
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Systems/TransformSystem.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/MotionWarpComponent.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/AnimationPropertyBinding.hpp>
#include <Engine/Asset/AnimationSampling.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Asset/AvatarMaskAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <Engine/Profiler/ProfileScope.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::scene {

namespace {

struct SkinningCB {
    math::Matrix4 boneMatrices[asset::MAX_SKINNING_BONES];
};

struct NodeLocalPose {
    math::Vector3 translation = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();
    math::Vector3 scale = math::Vector3::ONE;
};

float WrapTime(float time, float duration)
{
    if (duration <= 0.0f) return 0.0f;
    float wrapped = std::fmod(time, duration);
    return wrapped < 0.0f ? wrapped + duration : wrapped;
}

/// @note 補間規則は .sequence と共有する。
/// @see AnimationSampling.hpp
using asset::SampleVectorKeys;
using asset::SampleQuaternionKeys;
using asset::SampleFloatKeys;

/// @brief ノード名に対応するトラックを探す。完全一致を優先し、無ければ正規化名で照合する。
/// @note FBX は exporter 次第でチャンネル名が揺れる (mixamorig:RightFoot / RightFoot / *_$AssimpFbx$_PreRotation)。
/// @note 正規化規則は asset::CanonicalNodeName に集約し、インポーターと共有する。
/// @return 見つからなければ nullptr。
const asset::NodeAnimationTrack* FindTrack(const asset::AnimationClip& clip,
                                           const std::string& nodeName)
{
    for (const auto& track : clip.tracks)
        if (track.nodeName == nodeName)
            return &track;

    const std::string canonicalNodeName = asset::CanonicalNodeName(nodeName);
    for (const auto& track : clip.tracks)
        if (asset::CanonicalNodeName(track.nodeName) == canonicalNodeName)
            return &track;

    return nullptr;
}

/// @brief あるクリップに対して確定したルートモーション設定。
/// @note 解決を 1 か所に集め、「ポーズから抜く軸」と「delta に載せる軸」を食い違わせない。
struct ResolvedRootMotion {
    uint32_t trackIndex = UINT32_MAX;
    /// @note apply* は delta に載せる軸。mode == None のときは全て false。
    bool applyXZ       = false;
    bool applyY        = false;
    bool applyRotation = false;
    /// @note strip* はポーズから除去する軸。抽出軸は必ず除去する (残すと Transform とポーズで二重に進む)。
    bool stripXZ       = false;
    bool stripY        = false;
    bool stripRotation = false;

    bool HasTrack() const { return trackIndex != UINT32_MAX; }
    bool ExtractsAnyAxis() const { return applyXZ || applyY || applyRotation; }
    bool StripsAnyAxis() const { return stripXZ || stripY || stripRotation; }
};

bool ResolveAxis(RootMotionAxisOverride axisOverride, bool clipValue)
{
    switch (axisOverride) {
    case RootMotionAxisOverride::Disabled: return false;
    case RootMotionAxisOverride::Enabled:  return true;
    case RootMotionAxisOverride::UseClip:
    default:                               return clipValue;
    }
}

/// @brief Animator の設定とクリップから、そのクリップのルートモーション構成を確定する。
/// @note 抽出と除去は必ずここで一緒に決める。片方だけクリップ側フラグを読むと前進成分が消える。
ResolvedRootMotion ResolveRootMotion(const AnimatorComponent& animator,
                                     const asset::AnimationClip& clip)
{
    ResolvedRootMotion resolved;
    const RootMotionSettings& settings = animator.rootMotion;

    switch (settings.source) {
    case RootMotionSource::NodeName:
        resolved.trackIndex =
            asset::FindRootMotionTrackIndexByName(clip, settings.nodeName);
        break;
    case RootMotionSource::AutoDetect:
        /// @note クリップ自身の指定を最優先し、無ければ候補名 → スケルトンルートで探す。
        resolved.trackIndex =
            clip.hasRootMotion && clip.rootMotionTrackIndex < clip.tracks.size()
            ? clip.rootMotionTrackIndex
            : asset::AutoDetectRootMotionTrackIndex(clip, animator.skeletonRootNodeName);
        break;
    case RootMotionSource::ClipDefined:
    default:
        if (clip.hasRootMotion) resolved.trackIndex = clip.rootMotionTrackIndex;
        break;
    }
    if (resolved.trackIndex >= clip.tracks.size()) {
        resolved.trackIndex = UINT32_MAX;
        return resolved;
    }

    const bool axisXZ = ResolveAxis(settings.applyXZ, clip.rootMotionApplyXZ);
    const bool axisY = ResolveAxis(settings.applyY, clip.rootMotionApplyY);
    const bool axisRotation =
        ResolveAxis(settings.applyRotation, clip.rootMotionApplyRotation);

    if (settings.Extracts()) {
        resolved.applyXZ = resolved.stripXZ = axisXZ;
        resolved.applyY = resolved.stripY = axisY;
        resolved.applyRotation = resolved.stripRotation = axisRotation;
        return resolved;
    }

    /// @note mode == None。Strip はその場再生 (旧 applyRootMotion=false 相当)、Keep は DCC どおりルートごと前進する。
    if (settings.poseMode == RootMotionPoseMode::Strip) {
        resolved.stripXZ = axisXZ;
        resolved.stripY = axisY;
        resolved.stripRotation = axisRotation;
    } else {
        resolved.trackIndex = UINT32_MAX;
    }
    return resolved;
}

/// @brief 階層パス ("Armature/Hips/Spine/Head") から対象 GameObject を引く。
/// @note 毎フレーム × Animator 数 × トラック数で呼ばれるため、区間は string_view で切りヒープ確保を避ける。
/// @return 見つからなければ nullptr。
GameObject* FindAnimationTarget(GameObject& root, const std::string& path)
{
    const std::string_view pathView{ path };
    if (pathView.empty() || pathView == "." || pathView == std::string_view{ root.name })
        return &root;

    GameObject* current = &root;
    size_t begin = 0;
    while (begin < pathView.size()) {
        const size_t end = pathView.find('/', begin);
        const std::string_view part = pathView.substr(
            begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
        begin = end == std::string_view::npos ? pathView.size() : end + 1;
        if (part.empty() || (current == &root && part == std::string_view{ root.name })) continue;
        GameObject* next = nullptr;
        for (int i = 0; i < current->GetChildCount(); ++i) {
            GameObject* child = current->GetChild(i);
            if (child && std::string_view{ child->name } == part) {
                next = child;
                break;
            }
        }
        if (!next) return nullptr;
        current = next;
    }
    return current;
}

/// @brief targetPath が無い旧 .anim の対象を、BoneComponent の名前から解決する。
/// @note BoneComponent に限定し、同名の装飾用 GameObject を誤って動かさない。
GameObject* FindAnimationBoneByNameRecursive(GameObject& current,
                                             std::string_view canonicalName)
{
    if (auto* bone = current.GetComponent<BoneComponent>()) {
        if (asset::CanonicalNodeName(bone->boneName) == canonicalName ||
            asset::CanonicalNodeName(current.name) == canonicalName)
            return &current;
    }

    for (int i = 0; i < current.GetChildCount(); ++i) {
        GameObject* child = current.GetChild(i);
        if (!child) continue;
        if (GameObject* found = FindAnimationBoneByNameRecursive(*child, canonicalName))
            return found;
    }
    return nullptr;
}

GameObject* FindAnimationTargetByNodeName(GameObject& root, std::string_view nodeName)
{
    const std::string canonicalName = asset::CanonicalNodeName(nodeName);
    if (canonicalName.empty()) return nullptr;
    return FindAnimationBoneByNameRecursive(root, canonicalName);
}

/// @brief マテリアルアニメーションの適用先 (SkinnedMeshRenderer と MaterialComponent を持つ GO) を探す。
/// @note submesh は MaterialComponent のスロットなので、どのスロットへ書くかは ApplyMaterialProperty が決める。
GameObject* FindMaterialSlotTarget(GameObject& root)
{
    if (root.GetComponent<SkinnedMeshRenderer>() && root.GetComponent<MaterialComponent>())
        return &root;
    for (int i = 0; i < root.GetChildCount(); ++i) {
        GameObject* child = root.GetChild(i);
        if (!child) continue;
        if (GameObject* found = FindMaterialSlotTarget(*child)) return found;
    }
    return nullptr;
}

void ApplyTransformTracks(GameObject& root,
                          const asset::AnimationClip& clip,
                          double ticks)
{
    for (const auto& track : clip.tracks) {
        GameObject* target = track.targetPath.empty()
            ? nullptr : FindAnimationTarget(root, track.targetPath);
        if (!target) continue;
        target->transform.position = SampleVectorKeys(
            track.positions, ticks, target->transform.position, track.interp);
        target->transform.rotation = SampleQuaternionKeys(
            track.rotations, ticks, target->transform.rotation, track.interp);
        target->transform.scale = SampleVectorKeys(
            track.scales, ticks, target->transform.scale, track.interp);
    }
}

void ApplyPropertyTracks(GameObject& root,
                         const asset::AnimationClip& clip,
                         double ticks,
                         SkinnedMeshRenderer* smr)
{
    for (const auto& track : clip.propertyTracks) {
        if (track.targetType == asset::AnimTargetType::MorphWeight) {
            if (smr && !track.floatKeys.empty())
                smr->morphWeights[track.propertyName] =
                    SampleFloatKeys(track.floatKeys, ticks, track.interp);
            continue;
        }
        if (track.targetPath.empty()) continue;
        GameObject* target = FindAnimationTarget(root, track.targetPath);
        if (track.targetType == asset::AnimTargetType::MaterialProperty &&
            (!target || !target->GetComponent<MaterialComponent>()))
            target = FindMaterialSlotTarget(root);
        if (!target) continue;
        switch (track.targetType) {
        case asset::AnimTargetType::ComponentProperty:
            ApplyComponentProperty(*target, track, ticks);
            break;
        case asset::AnimTargetType::MaterialProperty:
            ApplyMaterialProperty(*target, track, ticks);
            break;
        case asset::AnimTargetType::MorphWeight:
            break;
        }
    }
}

void DispatchAnimationEvents(GameObject& owner,
                              AnimatorComponent& animator,
                              const asset::AnimationClip& clip,
                             float previousTime,
                             float currentTime,
                             bool looped,
                             bool reverse)
{
    ScriptComponent* scripts = owner.GetComponent<ScriptComponent>();
    if (clip.events.empty()) return;
    const float duration = static_cast<float>(clip.GetDurationSeconds());
    for (const auto& event : clip.events) {
        const float eventTime = static_cast<float>(event.time);
        bool crossed = false;
        if (!reverse)
            crossed = looped ? (eventTime > previousTime || eventTime <= currentTime)
                             : (eventTime > previousTime && eventTime <= currentTime);
        else
            crossed = looped ? (eventTime < previousTime || eventTime >= currentTime)
                             : (eventTime < previousTime && eventTime >= currentTime);
        if (!crossed || eventTime < 0.0f || eventTime > duration) continue;
        const AnimationEventInfo info{
            event.name.c_str(), event.intParam, event.floatParam, eventTime
        };
        animator.firedEvents.push_back({
            event.name, event.intParam, event.floatParam, eventTime, Time::frameCount });
        if (scripts != nullptr)
            for (auto& entry : scripts->scripts)
                if (entry.script)
                    entry.script->ExecuteCallback(&Script::OnAnimationEvent, info);
    }
}

/// @brief 1 クリップ分のルートモーション移動量。
struct RootMotionDelta {
    math::Vector3    position = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();
};

/// @brief クリップの前フレームサンプルを取り出す。無ければ作る。
/// @note clip ポインタが同一性キー。LoadClips で clips を作り直すときはキャッシュごと破棄するので無効ポインタは残らない。
RootMotionClipSample& AcquireRootMotionSample(AnimatorComponent& animator,
                                              const asset::AnimationClip& clip)
{
    for (auto& sample : animator.rootMotionSamples)
        if (sample.clip == &clip) return sample;
    animator.rootMotionSamples.push_back(RootMotionClipSample{ &clip });
    return animator.rootMotionSamples.back();
}

/// @brief クリップ 1 本のルートモーションを前フレームからの差分として取り出す。
/// @note クリップごとに前回 tick を持つので、ブレンド構成が毎フレーム変わっても delta は連続する。
RootMotionDelta SampleClipRootMotion(AnimatorComponent& animator,
                                     const asset::AnimationClip& clip,
                                     const ResolvedRootMotion& resolved,
                                     double ticks,
                                     bool reverse,
                                     std::uint64_t frame)
{
    RootMotionDelta delta;
    if (!resolved.HasTrack() || !resolved.ExtractsAnyAxis()) return delta;

    const auto& track = clip.tracks[resolved.trackIndex];
    const auto samplePosition = [&](double t) {
        return SampleVectorKeys(track.positions, t, math::Vector3::ZERO, track.interp);
    };
    const auto sampleRotation = [&](double t) {
        return SampleQuaternionKeys(
            track.rotations, t, math::Quaternion::Identity(), track.interp);
    };

    RootMotionClipSample& sample = AcquireRootMotionSample(animator, clip);
    const math::Vector3 currentPosition = samplePosition(ticks);
    const math::Quaternion currentRotation = sampleRotation(ticks);

    /// @note フェードイン直後のクリップは初回だけ寄与ゼロになるが、その時点の weight はほぼ 0 なので影響しない。
    const bool continuous = sample.frame + 1 == frame;
    if (continuous) {
        const double endTicks = clip.durationTicks > 0.0
            ? clip.durationTicks
            : clip.GetDurationSeconds() *
              (clip.ticksPerSecond > 0.0 ? clip.ticksPerSecond : 30.0);
        /// @note 時刻が巻き戻っていればループ境界を跨いだとみなす。逆再生では大小関係が反転する。
        const bool wrapped = reverse ? (ticks > sample.ticks) : (ticks < sample.ticks);
        if (!wrapped) {
            delta.position = currentPosition - sample.position;
            delta.rotation = sample.rotation.Inverse() * currentRotation;
        } else if (!reverse) {
            delta.position = (samplePosition(endTicks) - sample.position)
                           + (currentPosition - samplePosition(0.0));
            delta.rotation = (sample.rotation.Inverse() * sampleRotation(endTicks))
                           * (sampleRotation(0.0).Inverse() * currentRotation);
        } else {
            delta.position = (samplePosition(0.0) - sample.position)
                           + (currentPosition - samplePosition(endTicks));
            delta.rotation = (sample.rotation.Inverse() * sampleRotation(0.0))
                           * (sampleRotation(endTicks).Inverse() * currentRotation);
        }
    }

    sample.ticks = ticks;
    sample.position = currentPosition;
    sample.rotation = currentRotation;
    sample.frame = frame;

    if (!resolved.applyXZ) delta.position.x = delta.position.z = 0.0f;
    if (!resolved.applyY) delta.position.y = 0.0f;
    if (!resolved.applyRotation) delta.rotation = math::Quaternion::Identity();
    delta.rotation = delta.rotation.Normalized();
    return delta;
}

/// @brief 今フレーム評価されなかったクリップのサンプルを捨てる。
/// @note 放置すると Animator が触った全クリップぶん配列が伸び続ける。
void PruneRootMotionSamples(AnimatorComponent& animator, std::uint64_t frame)
{
    animator.rootMotionSamples.erase(
        std::remove_if(animator.rootMotionSamples.begin(),
                       animator.rootMotionSamples.end(),
                       [frame](const RootMotionClipSample& sample) {
                           return sample.frame != frame;
                       }),
        animator.rootMotionSamples.end());
}

void UpdateMorphVertexBuffers(SkinnedMeshRenderer& smr,
                              renderer::ResourceManager& resources)
{
    if (!smr.model) return;
    if (smr.morphWeights == smr.appliedMorphWeights &&
        smr.morphVertexBuffers.size() == smr.model->meshes.size()) return;
    smr.morphVertexBuffers.resize(smr.model->meshes.size());
    for (size_t meshIndex = 0; meshIndex < smr.model->meshes.size(); ++meshIndex) {
        const auto& mesh = smr.model->meshes[meshIndex];
        if (!mesh || mesh->morphTargets.empty()) continue;

        if (mesh->isSkinned) {
            auto vertices = mesh->cpuSkinnedVertices;
            for (const auto& target : mesh->morphTargets) {
                const auto weightIt = smr.morphWeights.find(target.name);
                if (weightIt == smr.morphWeights.end()) continue;
                const float weight = weightIt->second;
                const size_t count = (std::min)(vertices.size(), target.positionDeltas.size());
                for (size_t i = 0; i < count; ++i) {
                    vertices[i].position += target.positionDeltas[i] * weight;
                    vertices[i].normal += target.normalDeltas[i] * weight;
                    vertices[i].tangent += target.tangentDeltas[i] * weight;
                }
            }
            /// @note 接線なしのモデルやデルタが向きを打ち消した頂点はゼロになるので、既定の軸へ逃がす。
            for (auto& vertex : vertices) {
                vertex.normal = vertex.normal.NormalizedOr(math::Vector3::UP);
                vertex.tangent = vertex.tangent.NormalizedOr(math::Vector3::RIGHT);
            }
            auto& buffer = smr.morphVertexBuffers[meshIndex];
            if (!buffer.IsValid())
                buffer = resources.CreateVertexBuffer(
                    vertices.data(), vertices.size() * sizeof(renderer::SkinnedVertex),
                    sizeof(renderer::SkinnedVertex));
            else
                resources.Update(buffer, vertices.data(),
                                 vertices.size() * sizeof(renderer::SkinnedVertex));
        } else {
            auto vertices = mesh->cpuVertices;
            for (const auto& target : mesh->morphTargets) {
                const auto weightIt = smr.morphWeights.find(target.name);
                if (weightIt == smr.morphWeights.end()) continue;
                const float weight = weightIt->second;
                const size_t count = (std::min)(vertices.size(), target.positionDeltas.size());
                for (size_t i = 0; i < count; ++i) {
                    vertices[i].position += target.positionDeltas[i] * weight;
                    vertices[i].normal += target.normalDeltas[i] * weight;
                    vertices[i].tangent += target.tangentDeltas[i] * weight;
                }
            }
            for (auto& vertex : vertices) {
                vertex.normal = vertex.normal.NormalizedOr(math::Vector3::UP);
                vertex.tangent = vertex.tangent.NormalizedOr(math::Vector3::RIGHT);
            }
            auto& buffer = smr.morphVertexBuffers[meshIndex];
            if (!buffer.IsValid())
                buffer = resources.CreateVertexBuffer(
                    vertices.data(), vertices.size() * sizeof(renderer::Vertex),
                    sizeof(renderer::Vertex));
            else
                resources.Update(buffer, vertices.data(),
                                 vertices.size() * sizeof(renderer::Vertex));
        }
    }
    smr.appliedMorphWeights = smr.morphWeights;
}

/// @brief ポーズからルート成分をバインド姿勢へ戻す。
/// @note 軸はクリップ側フラグではなく ResolvedRootMotion で決める (直接読むと抽出停止中も前進が消える)。
void StripRootMotionFromPose(const asset::SkeletonNode& node,
                             const ResolvedRootMotion& rootMotion,
                             math::Vector3& translation,
                             math::Quaternion& rotation)
{
    if (rootMotion.stripXZ) {
        translation.x = node.bindTranslation.x;
        translation.z = node.bindTranslation.z;
    }
    if (rootMotion.stripY) translation.y = node.bindTranslation.y;
    if (rootMotion.stripRotation) rotation = node.bindRotation;
}

math::Matrix4 SampleNodeLocal(const asset::SkeletonNode& node,
                              const asset::AnimationClip& clip,
                              double ticks,
                              const ResolvedRootMotion& rootMotion)
{
    const auto* track = FindTrack(clip, node.name);
    if (!track) return node.localBindTransform;

    math::Vector3 translation = SampleVectorKeys(
        track->positions, ticks, node.bindTranslation, track->interp);
    math::Quaternion rotation = SampleQuaternionKeys(
        track->rotations, ticks, node.bindRotation, track->interp);
    if (rootMotion.HasTrack() && track == &clip.tracks[rootMotion.trackIndex])
        StripRootMotionFromPose(node, rootMotion, translation, rotation);
    const math::Vector3 scale = SampleVectorKeys(track->scales, ticks, node.bindScale, track->interp);
    return math::Matrix4::TRS(translation, rotation, scale);
}

NodeLocalPose SampleNodeLocalPose(const asset::SkeletonNode& node,
                                  const asset::AnimationClip& clip,
                                  double ticks,
                                  const ResolvedRootMotion& rootMotion)
{
    const auto* track = FindTrack(clip, node.name);
    if (!track) {
        return {
            node.bindTranslation,
            node.bindRotation,
            node.bindScale
        };
    }

    math::Vector3 translation = SampleVectorKeys(
        track->positions, ticks, node.bindTranslation, track->interp);
    math::Quaternion rotation = SampleQuaternionKeys(
        track->rotations, ticks, node.bindRotation, track->interp);
    if (rootMotion.HasTrack() && track == &clip.tracks[rootMotion.trackIndex])
        StripRootMotionFromPose(node, rootMotion, translation, rotation);
    return { translation, rotation,
             SampleVectorKeys(track->scales, ticks, node.bindScale, track->interp) };
}

void UpdateWorldTransform(GameObject& go, const Transform& parentTransform)
{
    Transform& tf = go.transform;
    math::Vector3 scaledLocal = {
        tf.position.x * parentTransform.worldScale.x,
        tf.position.y * parentTransform.worldScale.y,
        tf.position.z * parentTransform.worldScale.z
    };
    tf.worldRotation   = (parentTransform.worldRotation * tf.rotation).Normalized();
    tf.worldPosition   = parentTransform.worldPosition + parentTransform.worldRotation * scaledLocal;
    tf.worldScale = {
        parentTransform.worldScale.x * tf.scale.x,
        parentTransform.worldScale.y * tf.scale.y,
        parentTransform.worldScale.z * tf.scale.z
    };
}

/// @brief アフィン行列を TRS へ分解し、Transform の world* へ入れて返す。
/// @note せん断は捨てる。スケールが EPSILON 以下の軸は回転抽出で 0 扱いになる。
Transform DecomposeAffineMatrix(const math::Matrix4& worldMatrix)
{
    Transform result;
    result.worldPosition = {
        worldMatrix.m[0][3], worldMatrix.m[1][3], worldMatrix.m[2][3]
    };

    const math::Vector3 basisX{
        worldMatrix.m[0][0], worldMatrix.m[1][0], worldMatrix.m[2][0]
    };
    const math::Vector3 basisY{
        worldMatrix.m[0][1], worldMatrix.m[1][1], worldMatrix.m[2][1]
    };
    const math::Vector3 basisZ{
        worldMatrix.m[0][2], worldMatrix.m[1][2], worldMatrix.m[2][2]
    };
    result.worldScale = { basisX.Length(), basisY.Length(), basisZ.Length() };

    math::Matrix4 rotationMatrix = math::Matrix4::Identity();
    const float inverseScaleX = result.worldScale.x > math::EPSILON
        ? 1.0f / result.worldScale.x : 0.0f;
    const float inverseScaleY = result.worldScale.y > math::EPSILON
        ? 1.0f / result.worldScale.y : 0.0f;
    const float inverseScaleZ = result.worldScale.z > math::EPSILON
        ? 1.0f / result.worldScale.z : 0.0f;
    for (int row = 0; row < 3; ++row) {
        rotationMatrix.m[row][0] = worldMatrix.m[row][0] * inverseScaleX;
        rotationMatrix.m[row][1] = worldMatrix.m[row][1] * inverseScaleY;
        rotationMatrix.m[row][2] = worldMatrix.m[row][2] * inverseScaleZ;
    }
    result.worldRotation = math::Quaternion::FromMatrix4(rotationMatrix).Normalized();
    return result;
}

/// @brief root bone のローカル TRS へ rootInverseTransform を焼き込む。
/// @note TransformSystem / ConstraintSystem は local から world を組み直すので、world だけ補正しても後段で消える。
void SetRootBoneLocalInSkinningSpace(GameObject& rootBone,
                                     const math::Matrix4& rootInverseTransform)
{
    const math::Matrix4 renderLocal =
        rootInverseTransform * math::Matrix4::TRS(
            rootBone.transform.position,
            rootBone.transform.rotation,
            rootBone.transform.scale);
    const Transform decomposed = DecomposeAffineMatrix(renderLocal);
    rootBone.transform.position = decomposed.worldPosition;
    rootBone.transform.rotation = decomposed.worldRotation;
    rootBone.transform.scale = decomposed.worldScale;
}

/// @brief SetRootBoneLocalInSkinningSpace の逆変換。
void SetRootBoneLocalInAnimationSpace(GameObject& rootBone,
                                      const math::Matrix4& rootInverseTransform)
{
    const math::Matrix4 animationLocal =
        math::Matrix4::Inverse(rootInverseTransform) * math::Matrix4::TRS(
            rootBone.transform.position,
            rootBone.transform.rotation,
            rootBone.transform.scale);
    const Transform decomposed = DecomposeAffineMatrix(animationLocal);
    rootBone.transform.position = decomposed.worldPosition;
    rootBone.transform.rotation = decomposed.worldRotation;
    rootBone.transform.scale = decomposed.worldScale;
}

/// @brief Bone 配下の非 Bone 子 (Particle / Attachment 等) の world 値を組み直す。
/// @note AnimatorSystem は TransformSystem の後に Bone を上書きするので、これが無いと子が追従しない。
/// @note BoneComponent を持つ子は Skeleton 再帰側が処理する。
void PropagateNonBoneChildTransforms(GameObject& parent)
{
    for (int i = 0; i < parent.GetChildCount(); ++i) {
        GameObject* child = parent.GetChild(i);
        if (!child) continue;
        if (child->GetComponent<BoneComponent>())
            continue;

        UpdateWorldTransform(*child, parent.transform);
        PropagateNonBoneChildTransforms(*child);
    }
}

GameObject* FindBoneDescendant(GameObject& root, int nodeIndex)
{
    for (int i = 0; i < root.GetChildCount(); ++i) {
        GameObject* child = root.GetChild(i);
        if (!child) continue;

        if (auto* bone = child->GetComponent<BoneComponent>())
            if (bone->nodeIndex == nodeIndex)
                return child;

        if (GameObject* found = FindBoneDescendant(*child, nodeIndex))
            return found;
    }
    return nullptr;
}

/// @brief nodeIndex から根までの親参照が循環せず範囲内に収まるかを確かめる。
/// @note 古いキャッシュや破損 .fzasset は親インデックスが循環し、Bone 生成の再帰をハングさせる。
bool HasAcyclicParentChain(const asset::Skeleton& skeleton, int nodeIndex)
{
    std::vector<uint8_t> visited(skeleton.nodes.size(), 0);
    int current = nodeIndex;
    while (current >= 0) {
        if (current >= static_cast<int>(skeleton.nodes.size()))
            return false;
        if (visited[static_cast<size_t>(current)] != 0)
            return false;
        visited[static_cast<size_t>(current)] = 1;
        current = skeleton.nodes[static_cast<size_t>(current)].parentIndex;
    }
    return true;
}

/// @brief ノードに対応する Bone GameObject を既存から束縛し、無ければ親から順に生成する。
/// @return nodeIndex が範囲外なら owner、生成できなければ nullptr。
GameObject* EnsureBoneObject(Scene& scene,
                             GameObject& owner,
                             SkinnedMeshRenderer& smr,
                             const asset::Skeleton& skeleton,
                             int nodeIndex)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size()))
        return &owner;
    auto& node = skeleton.nodes[static_cast<size_t>(nodeIndex)];

    if (nodeIndex < static_cast<int>(smr.nodeEntities.size())) {
        if (auto* existing = scene.GetGameObject(smr.nodeEntities[static_cast<size_t>(nodeIndex)])) {
            if (auto* bone = existing->GetComponent<BoneComponent>()) {
                bone->boneName = node.name;
                bone->boneIndex = node.boneIndex;
                bone->skinnedMeshEntity = owner.GetID();
            }
            if (nodeIndex == skeleton.rootNodeIndex)
                smr.skeletonRootEntity = existing->GetID();
            return existing;
        }
    }

    /// @note ボーンは Renderer の兄弟 (Armature) に居るので、owner の子孫だけ探すと Renderer ごとに階層が生える。
    /// @note 別キャラへ誤って束縛しないよう、範囲は起点ボーンの親までに限る。
    GameObject* searchScope = &owner;
    if (GameObject* skeletonRoot = scene.GetGameObject(smr.skeletonRootEntity)) {
        if (auto* skeletonParent = skeletonRoot->GetParent())
            searchScope = skeletonParent;
        else
            searchScope = skeletonRoot;
    }

    if (GameObject* found = FindBoneDescendant(*searchScope, nodeIndex)) {
        if (auto* bone = found->GetComponent<BoneComponent>()) {
            bone->boneName = node.name;
            bone->boneIndex = node.boneIndex;
            bone->skinnedMeshEntity = owner.GetID();
        }
        smr.nodeEntities[static_cast<size_t>(nodeIndex)] = found->GetID();
        if (nodeIndex == skeleton.rootNodeIndex)
            smr.skeletonRootEntity = found->GetID();
        return found;
    }

    GameObject* parent = &owner;
    if (node.parentIndex >= 0 &&
        node.parentIndex < static_cast<int>(skeleton.nodes.size()) &&
        HasAcyclicParentChain(skeleton, node.parentIndex))
        parent = EnsureBoneObject(scene, owner, smr, skeleton, node.parentIndex);
    if (!parent) return nullptr;

    GameObject* boneObject = scene.TryCreateGameObject(node.name);
    if (!boneObject) return nullptr;
    boneObject->layer = owner.layer;
    boneObject->transform.position = node.bindTranslation;
    boneObject->transform.rotation = node.bindRotation;
    boneObject->transform.scale = node.bindScale;
    boneObject->SetParent(parent);

    BoneComponent bone{};
    bone.boneName = node.name;
    bone.nodeIndex = nodeIndex;
    bone.boneIndex = node.boneIndex;
    bone.skinnedMeshEntity = owner.GetID();
    bone.generated = true;
    boneObject->AddComponent<BoneComponent>(std::move(bone));

    smr.nodeEntities[static_cast<size_t>(nodeIndex)] = boneObject->GetID();
    if (nodeIndex == skeleton.rootNodeIndex)
        smr.skeletonRootEntity = boneObject->GetID();
    return boneObject;
}

/// @return 不足ノードを全て作れる容量がなければ、既存の階層と Renderer の束縛を変えず false。
bool EnsureBoneHierarchy(Scene& scene,
                         GameObject& owner,
                         SkinnedMeshRenderer& smr,
                         const asset::Skeleton& skeleton)
{
    if (smr.nodeEntities.size() == skeleton.nodes.size() &&
        std::all_of(smr.nodeEntities.begin(), smr.nodeEntities.end(),
                    [&scene](EntityID id) { return scene.GetGameObject(id) != nullptr; })) {
        for (size_t i = 0; i < skeleton.nodes.size(); ++i)
            if (!EnsureBoneObject(scene, owner, smr, skeleton, static_cast<int>(i)))
                return false;
        return true;
    }

    std::vector<EntityID> resolvedNodes(skeleton.nodes.size(), EntityID::INVALID);
    GameObject* searchScope = &owner;
    if (GameObject* skeletonRoot = scene.GetGameObject(smr.skeletonRootEntity)) {
        searchScope = skeletonRoot->GetParent() ? skeletonRoot->GetParent() : skeletonRoot;
    }
    size_t missingCount = 0;
    for (size_t i = 0; i < skeleton.nodes.size(); ++i) {
        GameObject* existing = smr.nodeEntities.size() == skeleton.nodes.size()
            ? scene.GetGameObject(smr.nodeEntities[i]) : nullptr;
        if (!existing) {
            if (auto* bone = searchScope->GetComponent<BoneComponent>();
                bone && bone->nodeIndex == static_cast<int>(i))
                existing = searchScope;
            else
                existing = FindBoneDescendant(*searchScope, static_cast<int>(i));
        }
        if (existing) resolvedNodes[i] = existing->GetID();
        else ++missingCount;
    }
    if (!scene.CanCreateGameObjects(missingCount)) {
        FBZZ_LOG_WARN("AnimatorSystem: '%s' needs %zu Bone entities, but only %zu slots remain",
                      owner.name.c_str(), missingCount, scene.RemainingEntityCapacity());
        return false;
    }
    smr.nodeEntities = std::move(resolvedNodes);

    for (size_t i = 0; i < skeleton.nodes.size(); ++i)
        if (!EnsureBoneObject(scene, owner, smr, skeleton, static_cast<int>(i)))
            return false;
    return true;
}

void ApplyAnimatedPoseToBones(Scene& scene,
                              const asset::Skeleton& skeleton,
                              const asset::AnimationClip& clip,
                              double ticks,
                              SkinnedMeshRenderer& smr,
                              const ResolvedRootMotion& rootMotion)
{
    for (size_t i = 0; i < skeleton.nodes.size(); ++i) {
        if (i >= smr.nodeEntities.size()) continue;
        GameObject* boneObject = scene.GetGameObject(smr.nodeEntities[i]);
        if (!boneObject) continue;

        const NodeLocalPose pose =
            SampleNodeLocalPose(skeleton.nodes[i], clip, ticks, rootMotion);
        boneObject->transform.position = pose.translation;
        boneObject->transform.rotation = pose.rotation;
        boneObject->transform.scale = pose.scale;
    }
}

/// @brief ボーン階層の world を積み始める親 Transform。root ボーンの実際の親で、無ければ owner。
/// @note owner 固定にすると owner と root ボーンの間に挟んだノード (体ごと傾ける等) の回転・移動が毎フレーム捨てられる。
/// @note スキニング行列は owner ローカル系で組むので、挟んだノードのぶんはメッシュにも当たり判定にも乗る。
const Transform& SkeletonParentTransform(Scene& scene,
                                         const SkinnedMeshRenderer& smr,
                                         const asset::Skeleton& skeleton,
                                         GameObject& owner)
{
    if (skeleton.rootNodeIndex < 0 ||
        skeleton.rootNodeIndex >= static_cast<int>(smr.nodeEntities.size()))
        return owner.transform;
    GameObject* rootBone =
        scene.GetGameObject(smr.nodeEntities[static_cast<size_t>(skeleton.rootNodeIndex)]);
    if (!rootBone) return owner.transform;
    GameObject* parent = rootBone->GetParent();
    if (!parent || parent == &owner) return owner.transform;
    return parent->transform;
}

/// @brief nodeIndex 以下のボーンと非ボーン子の world を親から順に組み直す。
/// @param visited skeleton.nodes と同じ長さ。呼び出し側で 0 初期化する。
void PropagateBoneTransforms(Scene& scene,
                             const asset::Skeleton& skeleton,
                             SkinnedMeshRenderer& smr,
                             const math::Matrix4& rootInverseTransform,
                             int nodeIndex,
                             const Transform& parentTransform,
                             std::vector<uint8_t>& visited)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size()) ||
        nodeIndex >= static_cast<int>(smr.nodeEntities.size()))
        return;
    /// @note 破損アセットで children が循環していても無限再帰にしない。
    if (visited[static_cast<size_t>(nodeIndex)] != 0)
        return;
    visited[static_cast<size_t>(nodeIndex)] = 1;
    GameObject* boneObject = scene.GetGameObject(smr.nodeEntities[static_cast<size_t>(nodeIndex)]);
    if (!boneObject) return;

    if (nodeIndex == skeleton.rootNodeIndex) {
        /// @note boneMatrix = rootInverse · nodeGlobal · offset の rootInverse を root local へ移し、Flush 後も残す。
        SetRootBoneLocalInSkinningSpace(*boneObject, rootInverseTransform);
    }

    UpdateWorldTransform(*boneObject, parentTransform);
    PropagateNonBoneChildTransforms(*boneObject);

    for (int child : skeleton.nodes[static_cast<size_t>(nodeIndex)].children)
        PropagateBoneTransforms(scene, skeleton, smr, rootInverseTransform,
                                child, boneObject->transform, visited);
}

/// @brief ボーン GameObject をバインド姿勢へ戻し、world を組み直す。
/// @note 停止中も GPU は referencePose を描くので、ボーンに前フレーム姿勢を残すとソケットとメッシュがずれる。
void ApplyBindPoseToBones(Scene& scene,
                          const asset::Skeleton& skeleton,
                          SkinnedMeshRenderer& smr,
                          GameObject& owner)
{
    for (size_t i = 0; i < skeleton.nodes.size(); ++i) {
        if (i >= smr.nodeEntities.size()) continue;
        GameObject* boneObject = scene.GetGameObject(smr.nodeEntities[i]);
        if (!boneObject) continue;
        const auto& node = skeleton.nodes[i];
        boneObject->transform.position = node.bindTranslation;
        boneObject->transform.rotation = node.bindRotation;
        boneObject->transform.scale = node.bindScale;
    }

    std::vector<uint8_t> visited(skeleton.nodes.size(), 0);
    PropagateBoneTransforms(scene, skeleton, smr, skeleton.rootInverseTransform,
                            skeleton.rootNodeIndex,
                            SkeletonParentTransform(scene, smr, skeleton, owner), visited);
}

/// @brief 現在の骨位置から、カリング用の球を owner ローカル空間で組み直す。
/// @note バインドポーズ由来の球だと、骨を大きく動かす構成で球だけ取り残されて明滅する。
/// @note 肉の厚みは Renderer ごとに違うので足さない。読む側 (ComputeSkinnedWorldBounds) が足す。
void UpdateSkinnedBounds(AnimatorComponent& animator, const asset::Skeleton& skeleton)
{
    UpdateSkinnedPoseBounds(animator, skeleton);
}

/// @brief ボーン GameObject の world から nodeGlobalTransforms と boneMatrices を組み直す。
/// @note 配列長が足りない (評価失敗・リグ差し替え直後) ときは何もしない。
void RebuildSkinningFromBoneTransforms(Scene& scene,
                                       GameObject& owner,
                                       const asset::Skeleton& skeleton,
                                       SkinnedMeshRenderer& smr,
                                       AnimatorComponent& animator)
{
    if (smr.nodeEntities.size() < skeleton.nodes.size() ||
        animator.nodeGlobalTransforms.size() < skeleton.nodes.size())
        return;

    const math::Matrix4 ownerInverse = math::Matrix4::Inverse(owner.transform.GetWorldMatrix());
    /// @note PropagateBoneTransforms が root local へ rootInverse を焼き込んでいるので、ここで打ち消す。
    const math::Matrix4 rootTransform =
        math::Matrix4::Inverse(skeleton.rootInverseTransform);

    for (size_t nodeIndex = 0; nodeIndex < skeleton.nodes.size(); ++nodeIndex) {
        GameObject* boneObject = scene.GetGameObject(smr.nodeEntities[nodeIndex]);
        if (!boneObject) continue;

        animator.nodeGlobalTransforms[nodeIndex] =
            rootTransform * ownerInverse * boneObject->transform.GetWorldMatrix();
    }

    for (size_t boneIndex = 0; boneIndex < animator.boneMatrices.size(); ++boneIndex) {
        if (boneIndex >= skeleton.bones.size()) continue;
        const auto& bone = skeleton.bones[boneIndex];
        if (bone.nodeIndex < 0 ||
            bone.nodeIndex >= static_cast<int>(animator.nodeGlobalTransforms.size()))
            continue;

        animator.boneMatrices[boneIndex] =
            skeleton.rootInverseTransform
          * animator.nodeGlobalTransforms[static_cast<size_t>(bone.nodeIndex)]
          * bone.offsetMatrix;
    }
}

/// @brief GameObject を介さずにノード階層を再帰評価し、パレットとノード global を埋める。
void EvaluateNode(const asset::Skeleton& skeleton,
                  const asset::AnimationClip& clip,
                  int nodeIndex,
                  const math::Matrix4& parentGlobal,
                  double ticks,
                  std::vector<math::Matrix4>& palette,
                  std::vector<math::Matrix4>& nodeGlobals,
                  const ResolvedRootMotion& rootMotion)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size())) return;
    const auto& node = skeleton.nodes[static_cast<size_t>(nodeIndex)];
    const math::Matrix4 local = SampleNodeLocal(node, clip, ticks, rootMotion);
    const math::Matrix4 global = parentGlobal * local;

    if (nodeIndex < static_cast<int>(nodeGlobals.size()))
        nodeGlobals[static_cast<size_t>(nodeIndex)] = global;

    if (node.boneIndex >= 0 &&
        node.boneIndex < static_cast<int>(palette.size()) &&
        node.boneIndex < static_cast<int>(skeleton.bones.size())) {
        const auto& bone = skeleton.bones[static_cast<size_t>(node.boneIndex)];
        palette[static_cast<size_t>(node.boneIndex)] =
            skeleton.rootInverseTransform * global * bone.offsetMatrix;
    }

    for (int child : node.children)
        EvaluateNode(skeleton, clip, child, global, ticks, palette, nodeGlobals, rootMotion);
}

/// @brief スケルトンのリファレンスポーズを boneMatrices と定数バッファへ送る。
/// @param skeleton nullptr (スケルトン未解決) のときだけ単位行列を送る。
/// @note 単位行列は正しい無アニメ状態ではない。Blender 製 FBX はアーマチュアが -90°X を担うので 90° 倒れる。
/// @note boneMatrices は IKSystem / MeshTrailRenderPass / ParticlePass / AnimatorDebugPass も読むので CPU 側も揃える。
void UploadBindPose(AnimatorComponent& animator,
                    renderer::ResourceManager& resources,
                    const asset::Skeleton* skeleton = nullptr)
{
    SkinningCB cb{};
    for (int i = 0; i < asset::MAX_SKINNING_BONES; ++i)
        cb.boneMatrices[i] = math::Matrix4::Identity();

    if (skeleton && !skeleton->referencePose.empty()) {
        const size_t boneCount = (std::min)(skeleton->referencePose.size(),
                                            static_cast<size_t>(asset::MAX_SKINNING_BONES));
        animator.boneMatrices.assign(skeleton->referencePose.begin(),
                                     skeleton->referencePose.begin()
                                         + static_cast<std::ptrdiff_t>(boneCount));
        for (size_t i = 0; i < boneCount; ++i)
            cb.boneMatrices[i] = skeleton->referencePose[i];
    }

    resources.Update(animator.skinningBuffer, &cb, sizeof(SkinningCB));
}

/// @brief 現在の boneMatrices を前フレームのパレットとして確定させる。
/// @pre ポーズを上書きする直前に、1 フレーム 1 回だけ呼ぶ。
void SnapshotPreviousBonePalette(AnimatorComponent& animator,
                                 renderer::ResourceManager& resources)
{
    if (animator.boneMatrices.empty() || !animator.prevSkinningBuffer.IsValid()) {
        animator.prevBoneMatricesValid = false;
        return;
    }

    animator.prevBoneMatrices = animator.boneMatrices;

    SkinningCB cb{};
    for (int i = 0; i < asset::MAX_SKINNING_BONES; ++i)
        cb.boneMatrices[i] = math::Matrix4::Identity();
    const size_t boneCount = (std::min)(animator.prevBoneMatrices.size(),
                                        static_cast<size_t>(asset::MAX_SKINNING_BONES));
    for (size_t i = 0; i < boneCount; ++i)
        cb.boneMatrices[i] = animator.prevBoneMatrices[i];

    resources.Update(animator.prevSkinningBuffer, &cb, sizeof(SkinningCB));
    animator.prevBoneMatricesValid = true;
}

/// @brief 全レイヤーのステート・BlendTree・Additive 基準・外部参照が指すクリップを animator.clips へ読み込む。
/// @note clips を作り直すと clip ポインタが無効になるので、それをキーにする rootMotionSamples も捨てる。
void LoadClips(AnimatorComponent& animator)
{
    animator.clips.clear();
    animator.clipSourcePaths.clear();
    animator.rootMotionSamples.clear();

    std::vector<std::string> sources;
    auto addSource = [&sources](const std::string& sourcePath) {
        if (sourcePath.empty()) return;
        if (std::find(sources.begin(), sources.end(), sourcePath) == sources.end())
            sources.push_back(sourcePath);
    };
    const auto addStateSources = [&addSource](const AnimationState& state) {
        addSource(state.sourcePath);
        for (const auto& motion : state.blendTree1D.motions) addSource(motion.sourcePath);
        for (const auto& motion : state.blendTree2D.motions) addSource(motion.sourcePath);
    };

    /// @note 追加 Layer にだけ接続された Motion も集める。漏れると Layer のステートが動かない。
    for (const auto& state : animator.states)
        addStateSources(state);
    for (const auto& layer : animator.layers)
    {
        for (const auto& state : layer.states)
            addStateSources(state);
        /// @note Additive 基準はステートから参照されないことがある。未ロードだと先頭キーへ黙って縮退する。
        addSource(layer.additiveReference.sourcePath);
    }
    /// @note ステートから辿れない演出専用クリップ (.sequence の AnimationTrack 等)。
    for (const auto& src : animator.externalClipSources)
        addSource(src);

    for (const auto& src : sources) {
        if (src.empty()) continue;

        /// @note Controller は .anim を GUID で持つので拡張子判定は解決後に行う。前だと .anim をモデルとして読んでクラッシュする。
        const std::string resolvedSource = asset::AssetManager::ResolveAssetPath(src);
        const auto hasAnimExtension = [](const std::string& path) {
            if (path.size() < 5) return false;
            const size_t offset = path.size() - 5;
            for (size_t i = 0; i < 5; ++i) {
                const char c = static_cast<char>(std::tolower(
                    static_cast<unsigned char>(path[offset + i])));
                constexpr char suffix[] = ".anim";
                if (c != suffix[i]) return false;
            }
            return true;
        };

        /// @note インポート済みクリップは .anim に分離され、.fzasset には含まれない。
        if (hasAnimExtension(resolvedSource)) {
            auto h = asset::AssetManager::Load<asset::AnimationClip>(src);
            if (!h.IsValid()) {
                FBZZ_LOG_WARN("AnimatorSystem: .anim source '%s' failed to load", src.c_str());
                continue;
            }
            const auto* clip = asset::AssetManager::Get<asset::AnimationClip>(h);
            if (clip) {
                animator.clips.push_back(*clip);
                animator.clipSourcePaths.push_back(src);
            }
            continue;
        }

        auto model = asset::AssetManager::LoadAndGet<asset::Model>(src);
        if (!model) {
            FBZZ_LOG_WARN("AnimatorSystem: clip source '%s' failed to load", src.c_str());
            continue;
        }
        for (const auto& clip : model->clips) {
            animator.clips.push_back(clip);
            animator.clipSourcePaths.push_back(src);
        }
    }
    animator.clipsLoaded = true;
    animator.clipsAttemptGeneration = asset::AssetManager::GetFlushGeneration();
}

/// @brief クリップ名で探す。完全一致を優先し、無ければ大文字小文字を無視した部分一致。
/// @note エクスポーターによっては "Walk" が "Armature|Walk" のようにプレフィックス付きになる。
const asset::AnimationClip* FindClipByName(const AnimatorComponent& animator,
                                           const std::string& clipName)
{
    if (clipName.empty() || animator.clips.empty()) return nullptr;

    for (const auto& c : animator.clips)
        if (c.name == clipName) return &c;

    auto toLower = [](std::string s) {
        for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    const std::string lowerTarget = toLower(clipName);
    for (const auto& c : animator.clips) {
        if (toLower(c.name).find(lowerTarget) != std::string::npos) return &c;
    }

    return nullptr;
}

const asset::AnimationClip* FindClipBySource(const AnimatorComponent& animator,
                                             const std::string& sourcePath,
                                             const std::string& clipName);

/// @brief ステートのクリップを Source → 名前 → clipIndex の順で解決する。
/// @note Mixamo はクリップ名が "mixamo.com" になり名前検索が効かないので clipIndex へ落とす。
const asset::AnimationClip* FindClipForState(const AnimatorComponent& animator,
                                             const AnimationState& state)
{
    if (const auto* clip =
            FindClipBySource(animator, state.sourcePath, state.clipName))
        return clip;
    if (!state.clipName.empty()) {
        if (const auto* c = FindClipByName(animator, state.clipName)) return c;
    }
    const int idx = state.clipIndex;
    if (idx >= 0 && idx < static_cast<int>(animator.clips.size()))
        return &animator.clips[idx];
    return nullptr;
}

const AnimationState* FindState(const AnimatorComponent& animator,
                                const std::string& stateName)
{
    for (const auto& s : animator.states)
        if (s.name == stateName) return &s;
    return nullptr;
}

AnimationState* FindMutableState(AnimatorComponent& animator,
                                 const std::string& stateName)
{
    for (auto& state : animator.states)
        if (state.name == stateName) return &state;
    return nullptr;
}

AnimatorParameter* FindParam(AnimatorComponent& animator, const std::string& paramName)
{
    for (auto& p : animator.parameters)
        if (p.name == paramName) return &p;
    return nullptr;
}

/// @brief Source Path と Clip Name の組でクリップを解決する。
/// @note 別 FBX が同名クリップを持っていても参照先を一意に保つため。
/// @return 名前が合わなければ同 Source の先頭クリップ。Source 不一致なら nullptr。
const asset::AnimationClip* FindClipBySource(const AnimatorComponent& animator,
                                             const std::string& sourcePath,
                                             const std::string& clipName)
{
    if (sourcePath.empty()) return nullptr;
    const asset::AnimationClip* firstFromSource = nullptr;
    for (size_t i = 0; i < animator.clips.size(); ++i) {
        if (i >= animator.clipSourcePaths.size() ||
            animator.clipSourcePaths[i] != sourcePath) continue;
        if (!firstFromSource) firstFromSource = &animator.clips[i];
        if (!clipName.empty() && animator.clips[i].name == clipName)
            return &animator.clips[i];
    }
    return firstFromSource;
}

const asset::AnimationClip* FindClipForMotion(const AnimatorComponent& animator,
                                              const BlendTreeMotion& motion)
{
    if (const auto* clip =
            FindClipBySource(animator, motion.sourcePath, motion.clipName))
        return clip;
    if (!motion.clipName.empty()) {
        if (const auto* clip = FindClipByName(animator, motion.clipName)) return clip;
    }
    if (motion.clipIndex >= 0 &&
        motion.clipIndex < static_cast<int>(animator.clips.size()))
        return &animator.clips[static_cast<size_t>(motion.clipIndex)];
    return nullptr;
}

struct WeightedMotion {
    const BlendTreeMotion* motion = nullptr;
    float                  weight = 0.0f;
};

struct WeightedClip {
    const asset::AnimationClip* clip   = nullptr;
    const BlendTreeMotion*      motion = nullptr;
    double                      ticks  = 0.0;
    float                       weight = 0.0f;
    float                       ikWeight = 1.0f;
    /// @note クリップごとに持ち weight で合成する。1 本だけ見ると Walk↔Run が 0.5 を跨ぐ瞬間に移動量が飛ぶ。
    ResolvedRootMotion          rootMotion{};
    bool                        reverse = false;   ///< ループ跨ぎの判定方向が反転する。
};

std::vector<WeightedMotion> Compute1DWeights(const AnimatorComponent& animator,
                                             const BlendTree1D& tree)
{
    std::vector<WeightedMotion> result;
    if (tree.motions.empty()) return result;

    std::vector<const BlendTreeMotion*> sorted;
    sorted.reserve(tree.motions.size());
    for (const auto& motion : tree.motions) sorted.push_back(&motion);
    std::sort(sorted.begin(), sorted.end(),
              [](const auto* a, const auto* b) { return a->threshold < b->threshold; });

    const float value = tree.dampTime > math::EPSILON &&
                        tree.dampedValueInitialized
        ? tree.dampedValue
        : animator.GetFloat(tree.paramName);
    if (sorted.size() == 1 || value <= sorted.front()->threshold)
        return { { sorted.front(), 1.0f } };
    if (value >= sorted.back()->threshold)
        return { { sorted.back(), 1.0f } };

    for (size_t i = 0; i + 1 < sorted.size(); ++i) {
        const float a = sorted[i]->threshold;
        const float b = sorted[i + 1]->threshold;
        if (value < a || value > b) continue;
        const float span = b - a;
        const float t = span > math::EPSILON ? (value - a) / span : 0.0f;
        return { { sorted[i], 1.0f - t }, { sorted[i + 1], t } };
    }
    return { { sorted.back(), 1.0f } };
}

std::vector<float> ComputeGradientBandWeights(
    const std::vector<const BlendTreeMotion*>& motions,
    float px,
    float py)
{
    std::vector<float> weights(motions.size(), 0.0f);
    if (motions.empty()) return weights;
    if (motions.size() == 1) {
        weights[0] = 1.0f;
        return weights;
    }

    for (size_t i = 0; i < motions.size(); ++i) {
        float score = 1.0f;
        bool compared = false;
        for (size_t j = 0; j < motions.size(); ++j) {
            if (i == j) continue;
            const float dx = motions[i]->posX - motions[j]->posX;
            const float dy = motions[i]->posY - motions[j]->posY;
            const float denominator = dx * dx + dy * dy;
            if (denominator <= math::EPSILON) continue;
            const float band =
                ((px - motions[j]->posX) * dx + (py - motions[j]->posY) * dy)
                / denominator;
            score = (std::min)(score, band);
            compared = true;
        }
        weights[i] = compared ? (std::max)(0.0f, score) : 1.0f;
    }

    const float sum = std::accumulate(weights.begin(), weights.end(), 0.0f);
    if (sum > math::EPSILON) {
        for (float& weight : weights) weight /= sum;
    } else {
        size_t nearest = 0;
        float nearestDistance = std::numeric_limits<float>::max();
        for (size_t i = 0; i < motions.size(); ++i) {
            const float dx = px - motions[i]->posX;
            const float dy = py - motions[i]->posY;
            const float distance = dx * dx + dy * dy;
            if (distance < nearestDistance) {
                nearestDistance = distance;
                nearest = i;
            }
        }
        weights[nearest] = 1.0f;
    }
    return weights;
}

std::vector<WeightedMotion> Compute2DWeights(const AnimatorComponent& animator,
                                             const BlendTree2D& tree)
{
    std::vector<WeightedMotion> result;
    if (tree.motions.empty()) return result;

    const bool damped = tree.dampTime > math::EPSILON && tree.dampedValueInitialized;
    float px = damped ? tree.dampedX : animator.GetFloat(tree.paramX);
    float py = damped ? tree.dampedY : animator.GetFloat(tree.paramY);
    std::vector<const BlendTreeMotion*> motions;
    motions.reserve(tree.motions.size());
    for (const auto& motion : tree.motions) motions.push_back(&motion);

    std::vector<float> weights;
    if (tree.type == BlendTree2DType::SimpleDirectional) {
        size_t origin = motions.size();
        float originDistance = std::numeric_limits<float>::max();
        for (size_t i = 0; i < motions.size(); ++i) {
            const float distance =
                motions[i]->posX * motions[i]->posX + motions[i]->posY * motions[i]->posY;
            if (distance < originDistance) {
                originDistance = distance;
                origin = i;
            }
        }

        const float magnitude = std::sqrt(px * px + py * py);
        if (magnitude <= math::EPSILON) {
            weights.assign(motions.size(), 0.0f);
            weights[origin] = 1.0f;
        } else {
            std::vector<const BlendTreeMotion*> directional;
            std::vector<BlendTreeMotion> normalizedDirectional;
            std::vector<size_t> directionalIndices;
            normalizedDirectional.reserve(motions.size());
            for (size_t i = 0; i < motions.size(); ++i) {
                if (i == origin && originDistance <= math::EPSILON) continue;
                const float motionMagnitude = std::sqrt(
                    motions[i]->posX * motions[i]->posX +
                    motions[i]->posY * motions[i]->posY);
                if (motionMagnitude <= math::EPSILON) continue;
                normalizedDirectional.push_back(*motions[i]);
                normalizedDirectional.back().posX /= motionMagnitude;
                normalizedDirectional.back().posY /= motionMagnitude;
                directionalIndices.push_back(i);
            }
            directional.reserve(normalizedDirectional.size());
            for (const auto& motion : normalizedDirectional)
                directional.push_back(&motion);

            weights.assign(motions.size(), 0.0f);
            const float directionAmount = std::clamp(magnitude, 0.0f, 1.0f);
            if (directional.empty()) {
                weights[origin] = 1.0f;
            } else {
                const auto directionalWeights =
                    ComputeGradientBandWeights(
                        directional, px / magnitude, py / magnitude);
                if (originDistance <= math::EPSILON)
                    weights[origin] = 1.0f - directionAmount;
                for (size_t i = 0; i < directionalWeights.size(); ++i)
                    weights[directionalIndices[i]] =
                        directionalWeights[i] * directionAmount;
            }
        }
    } else {
        weights = ComputeGradientBandWeights(motions, px, py);
    }

    for (size_t i = 0; i < motions.size(); ++i)
        if (weights[i] > math::EPSILON)
            result.push_back({ motions[i], weights[i] });
    return result;
}

std::vector<WeightedMotion> ComputeStateWeights(const AnimatorComponent& animator,
                                                const AnimationState& state)
{
    if (state.mode == AnimationStateMode::BlendTree1D)
        return Compute1DWeights(animator, state.blendTree1D);
    if (state.mode == AnimationStateMode::BlendTree2D)
        return Compute2DWeights(animator, state.blendTree2D);
    return {};
}

std::vector<WeightedClip> BuildStateClips(const AnimatorComponent& animator,
                                          const AnimationState& state,
                                          float stateTime)
{
    std::vector<WeightedClip> result;
    const auto isReverse = [&animator, &state](float motionSpeed) {
        return animator.speed * state.speed * motionSpeed < 0.0f;
    };

    if (state.mode == AnimationStateMode::Clip) {
        if (const auto* clip = FindClipForState(animator, state)) {
            const double tps = clip->ticksPerSecond > 0.0 ? clip->ticksPerSecond : 30.0;
            result.push_back({
                clip, nullptr, static_cast<double>(stateTime) * tps, 1.0f, state.ikWeight,
                ResolveRootMotion(animator, *clip), isReverse(1.0f)
            });
        }
        return result;
    }

    for (const auto& weighted : ComputeStateWeights(animator, state)) {
        const auto* clip = FindClipForMotion(animator, *weighted.motion);
        if (!clip) continue;
        const double tps = clip->ticksPerSecond > 0.0 ? clip->ticksPerSecond : 30.0;
        const float duration = static_cast<float>(clip->GetDurationSeconds());
        float motionTime = stateTime * weighted.motion->speed;
        const bool sync1D = state.mode == AnimationStateMode::BlendTree1D &&
                            state.blendTree1D.syncNormalizedTime &&
                            state.blendTree1D.normalizedPhaseInitialized;
        const bool sync2D = state.mode == AnimationStateMode::BlendTree2D &&
                            state.blendTree2D.syncNormalizedTime &&
                            state.blendTree2D.normalizedPhaseInitialized;
        if (sync1D || sync2D) {
            /// @note 共通の 0..1 位相を各クリップ長へ写す。個別に Wrap すると weight 0 から戻った Walk が別位相で現れる。
            motionTime = (sync1D ? state.blendTree1D.normalizedPhase
                                 : state.blendTree2D.normalizedPhase) * duration;
        } else {
            motionTime = state.loop
                ? WrapTime(motionTime, duration)
                : std::clamp(motionTime, 0.0f, duration);
        }
        result.push_back({
            clip,
            weighted.motion,
            static_cast<double>(motionTime) * tps,
            weighted.weight,
            /// @note State は全体係数、Motion は固有係数。BlendTree を一括調整しつつ Run だけ足 IK を弱められる。
            state.ikWeight * weighted.motion->ikWeight,
            ResolveRootMotion(animator, *clip),
            isReverse(weighted.motion->speed)
        });
    }

    const float sum = std::accumulate(
        result.begin(), result.end(), 0.0f,
        [](float total, const WeightedClip& clip) { return total + clip.weight; });
    if (sum > math::EPSILON)
        for (auto& clip : result) clip.weight /= sum;
    return result;
}

float GetStateDuration(const AnimatorComponent& animator, const AnimationState& state)
{
    if (state.mode == AnimationStateMode::Clip) {
        const auto* clip = FindClipForState(animator, state);
        if (!clip) return 0.0f;
        return static_cast<float>(clip->GetDurationSeconds());
    }

    /// @note BlendTree の周期は Weight に依存させず全 Motion の最大実効長で固定する。加重平均だと Damping 中に周期が縮んで巻き戻る。
    float duration = 0.0f;
    const auto accumulateMotionDuration = [&](const BlendTreeMotion& motion) {
        const auto* clip = FindClipForMotion(animator, motion);
        if (!clip) return;
        const float speed = (std::max)(std::abs(motion.speed), 1e-4f);
        duration = (std::max)(
            duration,
            static_cast<float>(clip->GetDurationSeconds()) / speed);
    };

    if (state.mode == AnimationStateMode::BlendTree1D) {
        for (const auto& motion : state.blendTree1D.motions)
            accumulateMotionDuration(motion);
    } else if (state.mode == AnimationStateMode::BlendTree2D) {
        for (const auto& motion : state.blendTree2D.motions)
            accumulateMotionDuration(motion);
    }
    return duration;
}

/// @brief 加重クリップ集合からノード 1 つのローカル姿勢を合成する。位置・スケールは線形、回転は逐次 Slerp。
/// @return 有効なクリップが無ければバインド姿勢。
NodeLocalPose BlendNodePose(const asset::SkeletonNode& node,
                            const std::vector<WeightedClip>& clips)
{
    NodeLocalPose blended{};
    blended.translation = math::Vector3::ZERO;
    blended.scale = math::Vector3::ZERO;
    bool hasRotation = false;
    float accumulatedRotationWeight = 0.0f;

    for (const auto& weighted : clips) {
        if (!weighted.clip || weighted.weight <= math::EPSILON) continue;
        const NodeLocalPose pose = SampleNodeLocalPose(
            node, *weighted.clip, weighted.ticks, weighted.rootMotion);
        blended.translation += pose.translation * weighted.weight;
        blended.scale += pose.scale * weighted.weight;
        if (!hasRotation) {
            blended.rotation = pose.rotation;
            accumulatedRotationWeight = weighted.weight;
            hasRotation = true;
        } else {
            const float total = accumulatedRotationWeight + weighted.weight;
            const float t = total > math::EPSILON ? weighted.weight / total : 0.0f;
            blended.rotation =
                math::Quaternion::Slerp(blended.rotation, pose.rotation, t).Normalized();
            accumulatedRotationWeight = total;
        }
    }
    if (!hasRotation) {
        blended.translation = node.bindTranslation;
        blended.rotation = node.bindRotation;
        blended.scale = node.bindScale;
    }
    return blended;
}

void EvaluateNBlendedNodeRecursive(const asset::Skeleton& skeleton,
                                   const std::vector<WeightedClip>& clips,
                                   int nodeIndex,
                                   const math::Matrix4& parentGlobal,
                                   std::vector<math::Matrix4>& palette,
                                   std::vector<math::Matrix4>& nodeGlobals,
                                   std::vector<uint8_t>& visited)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size())) return;
    /// @note 壊れたキャッシュの循環参照でフレームが返らなくならないよう、各ノードは一度だけ処理する。
    if (visited[static_cast<size_t>(nodeIndex)] != 0)
        return;
    visited[static_cast<size_t>(nodeIndex)] = 1;
    const auto& node = skeleton.nodes[static_cast<size_t>(nodeIndex)];
    const NodeLocalPose blended = BlendNodePose(node, clips);
    const math::Matrix4 global =
        parentGlobal * math::Matrix4::TRS(
            blended.translation, blended.rotation, blended.scale);

    if (nodeIndex < static_cast<int>(nodeGlobals.size()))
        nodeGlobals[static_cast<size_t>(nodeIndex)] = global;
    if (node.boneIndex >= 0 &&
        node.boneIndex < static_cast<int>(palette.size()) &&
        node.boneIndex < static_cast<int>(skeleton.bones.size())) {
        const auto& bone = skeleton.bones[static_cast<size_t>(node.boneIndex)];
        palette[static_cast<size_t>(node.boneIndex)] =
            skeleton.rootInverseTransform * global * bone.offsetMatrix;
    }
    for (int child : node.children)
        EvaluateNBlendedNodeRecursive(
            skeleton, clips, child, global, palette, nodeGlobals, visited);
}

void ApplyNBlendedPoseToBones(Scene& scene,
                              const asset::Skeleton& skeleton,
                              const std::vector<WeightedClip>& clips,
                              SkinnedMeshRenderer& smr,
                              const asset::AvatarMaskAsset* baseMask)
{
    for (size_t i = 0; i < skeleton.nodes.size(); ++i) {
        if (i >= smr.nodeEntities.size()) continue;
        GameObject* boneObject = scene.GetGameObject(smr.nodeEntities[i]);
        if (!boneObject) continue;
        const asset::SkeletonNode& node = skeleton.nodes[i];
        const NodeLocalPose pose = BlendNodePose(node, clips);

        /// @note マスク重み 1 未満はバインド姿勢へ寄せる。前フレーム姿勢を残すと上位レイヤーが weight 0 になった瞬間に固まる。
        float weight = 1.0f;
        if (baseMask != nullptr) {
            const std::string path = asset::BuildSkeletonNodePath(
                skeleton, static_cast<int>(i));
            weight = asset::EvaluateAvatarMaskWeight(*baseMask, path, node.name);
        }

        if (weight >= 1.0f - math::EPSILON) {
            boneObject->transform.position = pose.translation;
            boneObject->transform.rotation = pose.rotation;
            boneObject->transform.scale = pose.scale;
        } else {
            boneObject->transform.position =
                math::Vector3::Lerp(node.bindTranslation, pose.translation, weight);
            boneObject->transform.rotation =
                math::Quaternion::Slerp(node.bindRotation, pose.rotation, weight).Normalized();
            boneObject->transform.scale =
                math::Vector3::Lerp(node.bindScale, pose.scale, weight);
        }
    }
}

/// @return param が nullptr なら false。
bool CheckCondition(const AnimatorCondition& cond, const AnimatorParameter* param)
{
    if (!param) return false;

    switch (cond.op) {
    case ConditionOp::Greater:
        if (param->type == ParamType::Int)
            return static_cast<float>(param->intValue) > cond.threshold;
        return param->floatValue > cond.threshold;
    case ConditionOp::Less:
        if (param->type == ParamType::Int)
            return static_cast<float>(param->intValue) < cond.threshold;
        return param->floatValue < cond.threshold;
    case ConditionOp::Equal:
        if (param->type == ParamType::Int)
            return param->intValue == static_cast<int>(cond.threshold);
        return std::abs(param->floatValue - cond.threshold) < 1e-4f;
    case ConditionOp::NotEqual:
        if (param->type == ParamType::Int)
            return param->intValue != static_cast<int>(cond.threshold);
        return std::abs(param->floatValue - cond.threshold) >= 1e-4f;
    case ConditionOp::True:
        return param->boolValue;
    case ConditionOp::False:
        return !param->boolValue;
    }
    return false;
}

/// @brief exitTime と全条件の AND で遷移可否を決める。
/// @return 条件が空なら hasExitTime の値 (exitTime なし・条件なしは無効定義として遷移しない)。
bool EvaluateTransition(const AnimationTransition& tr,
                        AnimatorComponent& animator,
                        float normalizedTime)
{
    if (tr.hasExitTime && normalizedTime < tr.exitTime) return false;

    if (tr.conditions.empty()) return tr.hasExitTime;

    for (const auto& cond : tr.conditions) {
        const AnimatorParameter* param = FindParam(animator, cond.paramName);
        if (!CheckCondition(cond, param)) return false;
    }
    return true;
}

/// @brief 遷移に使われた Trigger を false に戻す (Trigger は遷移に消費される発火信号)。
void ConsumeTriggers(AnimatorComponent& animator, const AnimationTransition& tr)
{
    for (const auto& cond : tr.conditions) {
        auto* param = FindParam(animator, cond.paramName);
        if (param && param->type == ParamType::Trigger)
            param->boolValue = false;
    }
}

/// @brief ステートマシン 1 本ぶんの states とランタイム変数を参照で束ねたビュー。
/// @note Base Layer の状態は AnimatorComponent 直下にあり外部が直接読むので、構造体へ畳まずビューで共通化する。
struct StateMachineScope {
    std::vector<AnimationState>&            states;
    const std::vector<AnimationTransition>& anyStateTransitions;
    const std::string&                      defaultStateName;
    std::string&                            currentStateName;
    float&                                  stateTime;
    std::string&                            blendToState;
    float&                                  blendToTime;
    float&                                  blendWeight;
    float&                                  blendDuration;
};

StateMachineScope BaseScope(AnimatorComponent& animator)
{
    return StateMachineScope{
        animator.states, animator.anyStateTransitions, animator.defaultStateName,
        animator.currentStateName, animator.stateTime, animator.blendToState,
        animator.blendToTime, animator.blendWeight, animator.blendDuration
    };
}

StateMachineScope LayerScope(AnimationLayer& layer)
{
    return StateMachineScope{
        layer.states, layer.anyStateTransitions, layer.defaultStateName,
        layer.runtime.currentStateName, layer.runtime.stateTime, layer.runtime.blendToState,
        layer.runtime.blendToTime, layer.runtime.blendWeight, layer.runtime.blendDuration
    };
}

const AnimationState* FindStateIn(const std::vector<AnimationState>& states,
                                  const std::string& stateName)
{
    for (const auto& s : states)
        if (s.name == stateName) return &s;
    return nullptr;
}

AnimationState* FindMutableStateIn(std::vector<AnimationState>& states,
                                   const std::string& stateName)
{
    for (auto& s : states)
        if (s.name == stateName) return &s;
    return nullptr;
}

/// @brief defaultStateName (無ければ states[0]) で初期化する。currentStateName が設定済みなら何もしない。
void InitStateMachineScoped(StateMachineScope scope)
{
    if (!scope.currentStateName.empty()) return;
    if (scope.states.empty()) return;

    if (!scope.defaultStateName.empty() &&
        FindStateIn(scope.states, scope.defaultStateName))
        scope.currentStateName = scope.defaultStateName;
    else
        scope.currentStateName = scope.states[0].name;

    scope.stateTime    = 0.0f;
    scope.blendToState = "";
    scope.blendWeight  = 0.0f;
}

void InitStateMachine(AnimatorComponent& animator)
{
    InitStateMachineScoped(BaseScope(animator));
}

/// @brief 最初に成立した遷移のクロスフェードを開始する。
/// @param animator パラメーターはレイヤー間で共有なので scope ではなく animator から読む。
/// @return 遷移を開始したら true。
bool TryStartTransitionScoped(AnimatorComponent& animator,
                              StateMachineScope scope,
                              const std::vector<AnimationTransition>& transitions,
                              float normalizedTime)
{
    for (const auto& tr : transitions) {
        if (tr.toStateName.empty()) continue;
        /// @note 自己遷移は開始しない。継続条件で毎フレーム成立すると再生時刻が 0 へ戻り続ける。
        if (tr.toStateName == scope.currentStateName) continue;
        if (!FindStateIn(scope.states, tr.toStateName)) continue;
        if (!EvaluateTransition(tr, animator, normalizedTime)) continue;

        scope.blendToState = tr.toStateName;
        scope.blendToTime  = 0.0f;
        scope.blendWeight  = 0.0f;
        if (auto* targetState = FindMutableStateIn(scope.states, tr.toStateName)) {
            targetState->blendTree1D.normalizedPhase = 0.0f;
            targetState->blendTree1D.normalizedPhaseInitialized = false;
            targetState->blendTree2D.normalizedPhase = 0.0f;
            targetState->blendTree2D.normalizedPhaseInitialized = false;
        }
        /// @note 正規化指定は遷移元ステート長で実秒へ換算する。クリップを差し替えても同じ割合で混ざる。
        const AnimationState* currentState =
            FindStateIn(scope.states, scope.currentStateName);
        const float sourceDuration = currentState
            ? GetStateDuration(animator, *currentState)
            : 0.0f;
        scope.blendDuration = tr.fixedDuration
            ? tr.transitionDuration
            : tr.transitionDuration * sourceDuration;
        scope.blendDuration = (std::max)(scope.blendDuration, 0.0f);
        ConsumeTriggers(animator, tr);
        return true;
    }
    return false;
}

/// @brief BlendTree の入力値を時定数 dampTime [s] の指数補間で平滑化する。
/// @note Script が離散的に値を入れても Weight を連続させる。2D は入力を離した瞬間の方向の飛びも抑える。
void UpdateBlendTreeDampingIn(AnimatorComponent& animator,
                              std::vector<AnimationState>& states,
                              float dt)
{
    if (!animator.playing) return;

    const auto damp = [dt](float current, float target, float dampTime) {
        const float alpha =
            1.0f - std::exp(-(std::max)(dt, 0.0f) / (std::max)(dampTime, 1e-4f));
        float value = current + (target - current) * std::clamp(alpha, 0.0f, 1.0f);
        /// @note 指数補間は目標に届かないので近傍で確定する。極小 weight のクリップを評価し続けると定常負荷が倍になる。
        const float snapEpsilon = (std::max)(0.001f, std::abs(target) * 0.001f);
        if (std::abs(target - value) <= snapEpsilon) value = target;
        return value;
    };

    for (auto& state : states) {
        if (state.mode == AnimationStateMode::BlendTree1D) {
            auto& tree = state.blendTree1D;
            const float target = animator.GetFloat(tree.paramName);
            if (!tree.dampedValueInitialized || tree.dampTime <= math::EPSILON) {
                tree.dampedValue = target;
                tree.dampedValueInitialized = true;
                continue;
            }
            tree.dampedValue = damp(tree.dampedValue, target, tree.dampTime);
        } else if (state.mode == AnimationStateMode::BlendTree2D) {
            auto& tree = state.blendTree2D;
            const float targetX = animator.GetFloat(tree.paramX);
            const float targetY = animator.GetFloat(tree.paramY);
            if (!tree.dampedValueInitialized || tree.dampTime <= math::EPSILON) {
                tree.dampedX = targetX;
                tree.dampedY = targetY;
                tree.dampedValueInitialized = true;
                continue;
            }
            tree.dampedX = damp(tree.dampedX, targetX, tree.dampTime);
            tree.dampedY = damp(tree.dampedY, targetY, tree.dampTime);
        }
    }
}

void UpdateBlendTreeDamping(AnimatorComponent& animator, float dt)
{
    UpdateBlendTreeDampingIn(animator, animator.states, dt);
}

/// @brief 現在の Weight でサイクル周波数を加重平均し、共通の正規化位相を積分する。
/// @note 最長クリップへ引き伸ばす同期だと本来の速度が失われる。単独 Motion は元速度、ブレンド中は中間テンポになる。
void AdvanceBlendTreePhase(AnimatorComponent& animator,
                           AnimationState& state,
                           float stateTime,
                           float dt)
{
    if (!animator.playing) return;
    const bool is1D = state.mode == AnimationStateMode::BlendTree1D &&
                      state.blendTree1D.syncNormalizedTime;
    const bool is2D = state.mode == AnimationStateMode::BlendTree2D &&
                      state.blendTree2D.syncNormalizedTime;
    if (!is1D && !is2D) return;

    float cycleFrequency = 0.0f;
    float validWeight = 0.0f;
    for (const auto& weighted : ComputeStateWeights(animator, state)) {
        const asset::AnimationClip* clip =
            FindClipForMotion(animator, *weighted.motion);
        if (!clip) continue;
        const float duration = static_cast<float>(clip->GetDurationSeconds());
        if (duration <= math::EPSILON) continue;
        cycleFrequency += weighted.weight * weighted.motion->speed / duration;
        validWeight += weighted.weight;
    }
    if (validWeight <= math::EPSILON) return;
    cycleFrequency /= validWeight;

    float& phase = is1D ? state.blendTree1D.normalizedPhase
                        : state.blendTree2D.normalizedPhase;
    bool&  initialized = is1D ? state.blendTree1D.normalizedPhaseInitialized
                              : state.blendTree2D.normalizedPhaseInitialized;
    if (!initialized) {
        phase = state.loop
            ? stateTime * cycleFrequency -
                std::floor(stateTime * cycleFrequency)
            : math::Clamp01(stateTime * cycleFrequency);
        initialized = true;
        return;
    }

    phase += dt * state.speed * animator.speed * cycleFrequency;
    if (state.loop) {
        phase -= std::floor(phase);
    } else {
        phase = math::Clamp01(phase);
    }
}

/// @brief ステートマシンを 1 フレーム進める。クロスフェード中は新たな遷移を評価しない。
void UpdateStateMachineScoped(AnimatorComponent& animator, StateMachineScope scope, float dt)
{
    if (!scope.blendToState.empty()) {
        if (!animator.playing) return;

        scope.blendWeight += dt / (std::max)(scope.blendDuration, 1e-4f);

        /// @note クロスフェード中も遷移元を進める。止めると静止ポーズへフェードする。
        const AnimationState* currentSt =
            FindStateIn(scope.states, scope.currentStateName);
        if (currentSt) {
            const float dur = GetStateDuration(animator, *currentSt);
            if (dur > 0.0f) {
                scope.stateTime += dt * currentSt->speed * animator.speed;
                scope.stateTime = currentSt->loop
                    ? WrapTime(scope.stateTime, dur)
                    : std::clamp(scope.stateTime, 0.0f, dur);
            }
        }

        const AnimationState* nextSt = FindStateIn(scope.states, scope.blendToState);
        if (nextSt) {
            const float dur = GetStateDuration(animator, *nextSt);
            if (dur > 0.0f) {
                scope.blendToTime += dt * nextSt->speed * animator.speed;
                if (nextSt->loop)
                    scope.blendToTime = WrapTime(scope.blendToTime, dur);
                else
                    scope.blendToTime = std::clamp(scope.blendToTime, 0.0f, dur);
            }
        }

        if (scope.blendWeight >= 1.0f) {
            scope.currentStateName = scope.blendToState;
            scope.stateTime        = scope.blendToTime;
            scope.blendToState     = "";
            scope.blendWeight      = 0.0f;
        }
        return;
    }

    const AnimationState* curSt = FindStateIn(scope.states, scope.currentStateName);
    if (!curSt) return;

    const float duration = GetStateDuration(animator, *curSt);
    if (duration > 0.0f) {
        if (animator.playing) {
            scope.stateTime += dt * curSt->speed * animator.speed;
            if (curSt->loop)
                scope.stateTime = WrapTime(scope.stateTime, duration);
            else
                scope.stateTime = std::clamp(scope.stateTime, 0.0f, duration);
        }
    }

    const float normalizedTime = (duration > 0.0f)
        ? std::clamp(scope.stateTime / duration, 0.0f, 1.0f)
        : 0.0f;

    /// @note 通常遷移を優先し、成立しなかったときだけ AnyState を評価する。
    if (!TryStartTransitionScoped(animator, scope, curSt->transitions, normalizedTime))
        TryStartTransitionScoped(animator, scope, scope.anyStateTransitions, normalizedTime);
}

void UpdateStateMachine(AnimatorComponent& animator, float dt)
{
    UpdateStateMachineScoped(animator, BaseScope(animator), dt);
}

/// @brief 加重クリップ集合からルートモーションを合成する。位置は線形加重、回転は逐次 Slerp。
/// @note 姿勢ブレンドと同じ比率で混ぜる。移動量だけ支配クリップ 100% にすると足が滑る。
RootMotionDelta AccumulateRootMotion(AnimatorComponent& animator,
                                     const std::vector<WeightedClip>& clips,
                                     std::uint64_t frame)
{
    RootMotionDelta total;
    float accumulatedRotationWeight = 0.0f;
    bool hasRotation = false;

    for (const auto& weighted : clips) {
        if (!weighted.clip || weighted.weight <= math::EPSILON) continue;
        const RootMotionDelta delta = SampleClipRootMotion(
            animator, *weighted.clip, weighted.rootMotion,
            weighted.ticks, weighted.reverse, frame);

        total.position += delta.position * weighted.weight;

        if (!hasRotation) {
            total.rotation = delta.rotation;
            accumulatedRotationWeight = weighted.weight;
            hasRotation = true;
        } else {
            const float sum = accumulatedRotationWeight + weighted.weight;
            const float t = sum > math::EPSILON ? weighted.weight / sum : 0.0f;
            total.rotation =
                math::Quaternion::Slerp(total.rotation, delta.rotation, t).Normalized();
            accumulatedRotationWeight = sum;
        }
    }
    if (!hasRotation) total.rotation = math::Quaternion::Identity();
    return total;
}

/// @brief ルートモーションの適用先を owner からの相対パスで解決する。".." で親へ遡る。
/// @note Animator がメッシュ側の子に付く構成では、親を指さないと子だけ動いてずれていく。
/// @return 空パスまたは解決できないパスなら owner。
GameObject& ResolveRootMotionTarget(GameObject& owner, const std::string& path)
{
    if (path.empty()) return owner;

    GameObject* current = &owner;
    size_t begin = 0;
    while (begin < path.size()) {
        const size_t end = path.find('/', begin);
        const std::string part = path.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        begin = end == std::string::npos ? path.size() : end + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (GameObject* parent = current->GetParent()) current = parent;
            continue;
        }
        GameObject* next = nullptr;
        for (int i = 0; i < current->GetChildCount(); ++i) {
            GameObject* child = current->GetChild(i);
            if (child && child->name == part) { next = child; break; }
        }
        if (!next) return owner;
        current = next;
    }
    return *current;
}

/// @brief target から owner までの親子チェーンの world を組み直す。
/// @note AnimatorSystem は Transform 伝播の後に走るので、放置するとボーンの world が 1 フレーム遅れる。
/// @note target が owner の祖先でなければ target 単体だけ更新する。
void RefreshWorldChain(GameObject& target, GameObject& owner)
{
    std::vector<GameObject*> chain;
    for (GameObject* go = &owner;; go = go->GetParent()) {
        chain.push_back(go);
        if (go == &target) break;
        if (!go->GetParent()) {
            chain.assign(1, &target);
            break;
        }
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        GameObject* go = *it;
        if (GameObject* parent = go->GetParent())
            UpdateWorldTransform(*go, parent->transform);
        else {
            go->transform.worldPosition = go->transform.position;
            go->transform.worldRotation = go->transform.rotation;
            go->transform.worldScale = go->transform.scale;
        }
        PropagateNonBoneChildTransforms(*go);
    }
}

/// @brief クリップの移動量へ目標までの不足分を上乗せし、残り時間の終わりに目標へ着かせる。
/// @note BlendTree と遷移で総量を先読みできないので、毎フレーム誤差の dt/remaining を配る。remaining→0 で必ず収束する。
/// @note 置き換えでなく上乗せ。置き換えると区間中の緩急が消えて等速で滑る。
/// @param localRotation 回転ワープを適用して書き戻す。
/// @return ワールド空間の位置補正量。呼び出し側が localDelta へも反映する。
math::Vector3 ApplyMotionWarp(MotionWarpComponent& warp,
                              const GameObject& target,
                              const math::Vector3& predictedWorldPosition,
                              math::Quaternion& localRotation,
                              float dt)
{
    MotionWarpTarget& goal = warp.target;
    warp.runtimeLastCorrection = math::Vector3::ZERO;
    if (!warp.enabled || !goal.active || dt <= math::EPSILON) {
        warp.runtimeRemainingDistance = 0.0f;
        return math::Vector3::ZERO;
    }

    const float alpha = math::Clamp01(dt / std::max(goal.remaining, dt));

    math::Vector3 correction = math::Vector3::ZERO;
    if (goal.warpPosition) {
        const math::Vector3 error = goal.position - predictedWorldPosition;
        correction = {
            error.x * math::Clamp01(goal.positionAxisWeight.x) * alpha,
            error.y * math::Clamp01(goal.positionAxisWeight.y) * alpha,
            error.z * math::Clamp01(goal.positionAxisWeight.z) * alpha,
        };
        if (goal.maxSpeed > 0.0f) {
            const float limit  = goal.maxSpeed * dt;
            const float length = correction.Length();
            if (length > limit && length > math::EPSILON)
                correction = correction * (limit / length);
        }
        warp.runtimeRemainingDistance = error.Length();
    }

    if (goal.warpRotation) {
        const math::Quaternion predicted =
            (target.transform.worldRotation * localRotation).Normalized();
        const math::Quaternion error = (predicted.Inverse() * goal.rotation).Normalized();
        localRotation = (localRotation *
            math::Quaternion::Slerp(math::Quaternion::Identity(), error, alpha)).Normalized();
    }

    goal.remaining -= dt;
    if (goal.remaining <= 0.0f) {
        goal.active    = false;
        goal.remaining = 0.0f;
        warp.runtimeRemainingDistance = 0.0f;
    }

    warp.runtimeLastCorrection = correction;
    return correction;
}

/// @brief 合成済み delta を Animator の出力へ書き、mode に応じて Transform / RigidBody へ適用する。
/// @pre ポーズ評価より前に呼ぶ (適用後の world でボーンを伝播させるため)。
void ApplyRootMotionResult(AnimatorComponent& animator,
                           GameObject& owner,
                           const RootMotionDelta& rawDelta,
                           float dt)
{
    const RootMotionSettings& settings = animator.rootMotion;

    animator.rootMotionDeltaPosition = math::Vector3::ZERO;
    animator.rootMotionDeltaRotation = math::Quaternion::Identity();
    animator.rootMotionWorldDelta = math::Vector3::ZERO;
    animator.rootMotionWorldVelocity = math::Vector3::ZERO;
    animator.rootMotionDeltaTime = dt;
    animator.rootMotionAppliedByEngine = false;
    if (!settings.Extracts()) return;

    GameObject& target = ResolveRootMotionTarget(owner, settings.targetPath);

    math::Vector3 localDelta = rawDelta.position * settings.positionScale;
    math::Quaternion localRotation = math::Quaternion::Slerp(
        math::Quaternion::Identity(), rawDelta.rotation,
        std::clamp(settings.rotationScale, 0.0f, 1.0f)).Normalized();

    /// @note 適用前の worldRotation を基準にする。
    math::Vector3 worldDelta = target.transform.worldRotation * localDelta;

    if (auto* warp = owner.GetComponent<MotionWarpComponent>()) {
        const math::Vector3 correction = ApplyMotionWarp(
            *warp, target, target.transform.worldPosition + worldDelta,
            localRotation, dt);
        if (correction.LengthSq() > 0.0f) {
            worldDelta = worldDelta + correction;
            localDelta = target.transform.worldRotation.Inverse() * worldDelta;
        }
    }

    animator.rootMotionDeltaPosition = localDelta;
    animator.rootMotionDeltaRotation = localRotation;
    animator.rootMotionWorldDelta = worldDelta;
    animator.rootMotionWorldVelocity =
        dt > math::EPSILON ? worldDelta / dt : math::Vector3::ZERO;

    switch (settings.mode) {
    case RootMotionMode::ApplyToTransform:
        target.transform.position += target.transform.rotation * localDelta;
        target.transform.rotation =
            (target.transform.rotation * localRotation).Normalized();
        RefreshWorldChain(target, owner);
        animator.rootMotionAppliedByEngine = true;
        break;

    case RootMotionMode::ApplyToRigidBody: {
        /// @note 速度として渡す。Transform 直書きはテレポート扱いでスイープも押し戻しも無く壁を抜ける。
        auto* rb = target.GetComponent<RigidBodyComponent>();
        if (rb && rb->enabled && rb->rigidBody && dt > math::EPSILON) {
            math::Vector3 velocity = rb->rigidBody->GetVelocity();
            velocity.x = worldDelta.x / dt;
            velocity.z = worldDelta.z / dt;
            /// @note Y はクリップが動かしているときだけ上書きし、それ以外は重力と衝突解決に任せる。
            if (std::fabs(worldDelta.y) > math::EPSILON)
                velocity.y = worldDelta.y / dt;
            rb->rigidBody->SetVelocity(velocity);
            animator.rootMotionAppliedByEngine = true;
        } else {
            /// @note RigidBody 無し / dt 0 は Transform へフォールバックする。何もしないと移動だけ黙って消える。
            target.transform.position += target.transform.rotation * localDelta;
            animator.rootMotionAppliedByEngine = true;
        }
        /// @note 回転は Transform で回す。次フレームの PhysicsSystem::SyncRigidBodies がボディへ書き戻す。
        target.transform.rotation =
            (target.transform.rotation * localRotation).Normalized();
        RefreshWorldChain(target, owner);
        break;
    }

    case RootMotionMode::ExtractOnly:
        /// @note エンジンは動かさない。適用は OnAnimatorMove を受けた Script の責任。
        break;

    case RootMotionMode::None:
    default:
        break;
    }
}

/// @brief 抽出したルートモーションを同一フレーム内で OnAnimatorMove として Script へ通知する。
/// @note AnimatorSystem は LateUpdate なので、プロキシのポーリングだけだと OnUpdate は 1 フレーム前の delta を読む。
void DispatchAnimatorMove(GameObject& owner, const AnimatorComponent& animator)
{
    if (!animator.rootMotion.Extracts()) return;
    ScriptComponent* scripts = owner.GetComponent<ScriptComponent>();
    if (!scripts) return;

    RootMotionInfo info{};
    info.deltaPosition = animator.rootMotionDeltaPosition;
    info.deltaRotation = animator.rootMotionDeltaRotation;
    info.worldDeltaPosition = animator.rootMotionWorldDelta;
    info.worldVelocity = animator.rootMotionWorldVelocity;
    info.deltaTime = animator.rootMotionDeltaTime;
    info.appliedByEngine = animator.rootMotionAppliedByEngine;

    for (auto& entry : scripts->scripts)
        if (entry.script && entry.script->enabled)
            entry.script->ExecuteCallback(&Script::OnAnimatorMove, info);
}

/// @brief ルートモーションの抽出・適用・Script 通知をまとめて行う。
/// @pre ポーズ評価より前に呼ぶ。
void ProcessRootMotion(AnimatorComponent& animator,
                       GameObject& owner,
                       const std::vector<WeightedClip>& clips,
                       float dt)
{
    const std::uint64_t frame = Time::frameCount;
    const RootMotionDelta delta = AccumulateRootMotion(animator, clips, frame);
    PruneRootMotionSamples(animator, frame);
    ApplyRootMotionResult(animator, owner, delta, dt);
    DispatchAnimatorMove(owner, animator);
}

} // namespace

/// @brief 現在ステートで最も weight が大きいクリップ。イベント・プロパティトラックの対象になる。
static const asset::AnimationClip* ResolveStateMachineEffectClip(AnimatorComponent& animator)
{
    const AnimationState* state = FindState(animator, animator.currentStateName);
    if (!state) return nullptr;
    auto clips = BuildStateClips(animator, *state, animator.stateTime);
    const auto best = std::max_element(clips.begin(), clips.end(),
        [](const WeightedClip& a, const WeightedClip& b) { return a.weight < b.weight; });
    return best != clips.end() ? best->clip : nullptr;
}

/// @brief スケルトンを持たない Animator のステートマシンを 1 フレーム進める。
/// @return イベント・トラック適用の対象クリップ。無ければ nullptr。
static const asset::AnimationClip* AdvanceStateMachineAnimator(AnimatorComponent& animator, float dt)
{
    InitStateMachine(animator);
    UpdateBlendTreeDamping(animator, dt);
    UpdateStateMachine(animator, dt);
    if (auto* state = FindMutableState(animator, animator.currentStateName))
        AdvanceBlendTreePhase(animator, *state, animator.stateTime, dt);
    return ResolveStateMachineEffectClip(animator);
}

/// @brief クリップのプロパティトラックとイベントを適用する。
/// @param smr nullptr 可 (モーフウェイトを書かない)。
/// @param applyTransformTracks スケルトン経路では false。ブレンド済み Bone を単独クリップで上書きすると手と武器が別軌道になる。
static void ApplyClipSideEffects(GameObject& owner,
                                  AnimatorComponent& animator,
                                  const asset::AnimationClip& clip,
                                  SkinnedMeshRenderer* smr,
                                  float previousTime,
                                  float currentTime,
                                  bool applyTransformTracks)
{
    const double ticksPerSecond = clip.ticksPerSecond > 0.0 ? clip.ticksPerSecond : 30.0;
    const double ticks = static_cast<double>(currentTime) * ticksPerSecond;
    if (applyTransformTracks) {
        FBZZ_PROFILE_SCOPE("AnimatorSystem::ApplyTransformTracks");
        ApplyTransformTracks(owner, clip, ticks);
    }
    {
        FBZZ_PROFILE_SCOPE("AnimatorSystem::ApplyPropertyTracks");
        ApplyPropertyTracks(owner, clip, ticks, smr);
    }
    const bool reverse = animator.speed < 0.0f;
    const bool looped = reverse ? currentTime > previousTime : currentTime < previousTime;
    /// @note イベントの区間判定は同じクリップを続けて再生していることが前提。
    if (animator.previousEventClipName == clip.name)
        DispatchAnimationEvents(owner, animator, clip, previousTime, currentTime, looped, reverse);
    animator.previousEventTime = currentTime;
    animator.previousEventClipName = clip.name;
}

/// @brief .mask をパスが変わったときだけ読み込む。Base Layer と各 AnimationLayer で共用する。
/// @note 毎フレーム TOML をパースすると数十体で破綻する。
static void EnsureMaskLoaded(AnimationMaskRef& ref)
{
    if (ref.path.empty()) {
        if (ref.loaded || ref.failed) {
            ref.loaded = false;
            ref.failed = false;
            ref.asset = asset::AvatarMaskAsset{};
            ref.loadedPath.clear();
        }
        return;
    }
    if ((ref.loaded || ref.failed) && ref.loadedPath == ref.path) return;

    ref.loadedPath = ref.path;
    ref.asset      = asset::AvatarMaskAsset{};
    const std::string resolved = asset::AssetManager::ResolveAssetPath(ref.path);
    ref.loaded = asset::LoadAvatarMaskAsset(resolved, ref.asset);
    ref.failed = !ref.loaded;
    if (ref.failed) {
        /// @note 読めないマスクはレイヤーを黙らせるので ERROR で出す。
        FBZZ_LOG_ERROR("AnimatorSystem: avatar mask load failed [%s] "
                       "- layer is disabled until it loads", ref.path.c_str());
    }
}

/// @brief このレイヤーが対象ボーンへ効く割合 0..1。
/// @return マスク未指定なら 1、指定したのに未ロードなら 0。
/// @note 未ロードで 1 に落とすと、上半身用レイヤーが全身を上書きして原因が見えなくなる。
static float LayerBoneWeight(const AnimationLayer& layer,
                             const std::string& path,
                             const std::string& nodeName)
{
    if (layer.mask.loaded)
        return asset::EvaluateAvatarMaskWeight(layer.mask.asset, path, nodeName);
    if (!layer.mask.path.empty())
        return 0.0f;

    (void)path;
    (void)nodeName;
    return 1.0f;
}

/// @brief 加算レイヤーの基準クリップを解決する。
/// @return 未設定・未解決なら nullptr (呼び出し側は加算クリップ自身の先頭キーを基準にする)。
static const asset::AnimationClip* ResolveAdditiveReferenceClip(
    const AnimatorComponent& animator, const AnimationLayer& layer)
{
    if (layer.additiveReference.sourcePath.empty() &&
        layer.additiveReference.clipName.empty()) return nullptr;
    if (const auto* clip = FindClipBySource(animator,
            layer.additiveReference.sourcePath, layer.additiveReference.clipName))
        return clip;
    if (!layer.additiveReference.clipName.empty())
        return FindClipByName(animator, layer.additiveReference.clipName);
    return nullptr;
}

/// @brief 対象ボーンの加算基準 TRS を取り出す。
/// @param referenceClip nullptr または該当トラック無しなら track 自身の先頭キーを使う。
/// @param referenceTime 基準クリップ上の時刻 [s]。
static void ResolveAdditiveReferencePose(const asset::AnimationClip* referenceClip,
                                         float referenceTime,
                                         const asset::NodeAnimationTrack& track,
                                         math::Vector3& outPosition,
                                         math::Quaternion& outRotation,
                                         math::Vector3& outScale)
{
    if (referenceClip) {
        const double tps = referenceClip->ticksPerSecond > 0.0
            ? referenceClip->ticksPerSecond : 30.0;
        const double ticks = static_cast<double>(referenceTime) * tps;
        for (const auto& refTrack : referenceClip->tracks) {
            const bool sameTarget =
                (!track.targetPath.empty() && refTrack.targetPath == track.targetPath) ||
                (!track.nodeName.empty()   && refTrack.nodeName   == track.nodeName);
            if (!sameTarget) continue;
            outPosition = SampleVectorKeys(
                refTrack.positions, ticks, math::Vector3::ZERO, refTrack.interp);
            outRotation = SampleQuaternionKeys(
                refTrack.rotations, ticks, math::Quaternion::Identity(), refTrack.interp);
            outScale = SampleVectorKeys(
                refTrack.scales, ticks, math::Vector3::ONE, refTrack.interp);
            return;
        }
    }
    outPosition = track.positions.empty() ? math::Vector3::ZERO : track.positions.front().value;
    outRotation = track.rotations.empty() ? math::Quaternion::Identity() : track.rotations.front().value;
    outScale    = track.scales.empty()    ? math::Vector3::ONE : track.scales.front().value;
}

/// @brief Slot (ワンショット差し込み) の再生位置とフェードを進める。末尾に達したら自動でフェードアウトする。
/// @note 専用ステートと復帰遷移を足さずに「上半身へ割り込ませて終わったら戻る」を成立させる。
/// @note slot.driven の間は外部 (SequenceSystem) が時刻とフェードを書くので触らない。
/// @return 再生中のクリップ。非アクティブ・未解決・フェードアウト完了なら nullptr (未解決はその場で畳む)。
static const asset::AnimationClip* UpdateLayerSlot(
    AnimatorComponent& animator, AnimationLayer& layer, float dt)
{
    AnimationSlotPlayback& slot = layer.slot;
    if (!slot.active) { slot.weight = 0.0f; return nullptr; }

    const asset::AnimationClip* clip =
        FindClipBySource(animator, slot.sourcePath, slot.clipName);
    if (!clip && !slot.clipName.empty()) clip = FindClipByName(animator, slot.clipName);
    if (!clip) {
        slot.active = false;
        slot.weight = 0.0f;
        return nullptr;
    }

    if (slot.driven) return clip;

    const float duration = static_cast<float>(clip->GetDurationSeconds());
    if (animator.playing) {
        slot.time += dt * slot.speed * animator.speed;
        if (slot.loop && duration > 0.0f) {
            slot.time = WrapTime(slot.time, duration);
        } else if (duration > 0.0f) {
            slot.time = std::clamp(slot.time, 0.0f, duration);
            const float fadeOutStart = (std::max)(duration - slot.fadeOutDuration, 0.0f);
            if (slot.time >= fadeOutStart) slot.stopping = true;
        }
    }

    const float fadeDuration = slot.stopping ? slot.fadeOutDuration : slot.fadeInDuration;
    const float step = fadeDuration > math::EPSILON ? dt / fadeDuration : 1.0f;
    slot.weight += slot.stopping ? -step : step;
    slot.weight = std::clamp(slot.weight, 0.0f, 1.0f);

    if (slot.stopping && slot.weight <= math::EPSILON) {
        slot.active = false;
        slot.weight = 0.0f;
        return nullptr;
    }
    return clip;
}

/// @brief レイヤー内で 1 ボーンぶんのポーズを積む作業バッファ。
/// @note 累積 weight に対する比率で積み、順序に依存しない加重平均にする (単純な順次 Lerp は後ろほど強く出る)。
struct LayerBonePose {
    GameObject*      target       = nullptr;
    math::Vector3    position     = math::Vector3::ZERO;
    math::Quaternion rotation     = math::Quaternion::Identity();
    math::Vector3    scale        = math::Vector3::ZERO;
    math::Vector3    deltaPosition= math::Vector3::ZERO;
    math::Quaternion deltaRotation= math::Quaternion::Identity();
    math::Vector3    deltaScale   = math::Vector3::ZERO;
    float            accumWeight  = 0.0f;
    bool             hasRotation  = false;
    float            boneWeight   = 0.0f;   ///< 積むたびに max で更新する。
};

/// @brief 1 レイヤーぶんのクリップ集合を評価し、ボーンごとのポーズを poses へ積む。
/// @param clipSetWeight クリップ集合全体に掛ける係数 (Slot に押しのけられる割合)。
static void AccumulateLayerClips(AnimatorComponent& animator,
                                 AnimationLayer& layer,
                                 const asset::Skeleton& skeleton,
                                 GameObject& owner,
                                 SkinnedMeshRenderer& smr,
                                 const std::vector<WeightedClip>& clips,
                                 float clipSetWeight,
                                 const asset::AnimationClip* additiveReferenceClip,
                                 std::vector<LayerBonePose>& poses)
{
    if (clipSetWeight <= math::EPSILON) return;

    for (const auto& weighted : clips) {
        if (!weighted.clip || weighted.weight <= math::EPSILON) continue;
        const float clipWeight = weighted.weight * clipSetWeight;

        for (const auto& track : weighted.clip->tracks) {
            std::string targetPath = track.targetPath;
            std::string targetName = track.nodeName;
            const RetargetBoneMapping* retarget = nullptr;
            for (const auto& mapping : layer.retargetMappings) {
                if (mapping.sourcePath == targetPath || mapping.sourcePath == targetName) {
                    retarget = &mapping;
                    targetPath = mapping.targetPath;
                    const size_t slash = targetPath.find_last_of('/');
                    targetName = slash == std::string::npos
                        ? targetPath : targetPath.substr(slash + 1);
                    break;
                }
            }

            /// @note 旧クリップは targetPath が空、または古い階層を指すので名前へフォールバックする。
            GameObject* target = targetPath.empty()
                ? nullptr : FindAnimationTarget(owner, targetPath);
            if (!target && !targetName.empty())
                target = FindAnimationTargetByNodeName(owner, targetName);
            if (!target) continue;

            const BoneComponent* bone = target->GetComponent<BoneComponent>();
            if (!bone) continue;

            std::string maskPath = targetPath;
            if (bone->nodeIndex >= 0)
                maskPath = asset::BuildSkeletonNodePath(skeleton, bone->nodeIndex);
            if (maskPath.empty()) maskPath = bone->boneName;
            const std::string maskName = bone->boneName.empty() ? targetName : bone->boneName;
            const float boneWeight = LayerBoneWeight(layer, maskPath, maskName);
            if (boneWeight <= math::EPSILON) continue;

            math::Vector3 sampledPosition = SampleVectorKeys(
                track.positions, weighted.ticks, target->transform.position, track.interp);
            math::Quaternion sampledRotation = SampleQuaternionKeys(
                track.rotations, weighted.ticks, target->transform.rotation, track.interp);
            math::Vector3 sampledScale = SampleVectorKeys(
                track.scales, weighted.ticks, target->transform.scale, track.interp);
            if (retarget) {
                sampledPosition = sampledPosition * retarget->translationScale;
                sampledRotation = (retarget->rotationOffset * sampledRotation).Normalized();
            }

            /// @note 線形探索で足りる (レイヤー内のクリップは多くて数本)。
            LayerBonePose* pose = nullptr;
            for (auto& p : poses)
                if (p.target == target) { pose = &p; break; }
            if (!pose) {
                poses.push_back(LayerBonePose{});
                pose = &poses.back();
                pose->target = target;
            }
            /// @note retarget でボーンパスが変わると値が揺れるので、積む順序に依存しない最大値を採る。
            pose->boneWeight = (std::max)(pose->boneWeight, boneWeight);

            if (layer.mode == AnimationLayerMode::Override) {
                const float total = pose->accumWeight + clipWeight;
                const float t = total > math::EPSILON ? clipWeight / total : 0.0f;
                pose->position = math::Vector3::Lerp(pose->position, sampledPosition, t);
                pose->scale    = math::Vector3::Lerp(pose->scale, sampledScale, t);
                if (!pose->hasRotation) {
                    pose->rotation = sampledRotation;
                    pose->hasRotation = true;
                } else {
                    pose->rotation =
                        math::Quaternion::Slerp(pose->rotation, sampledRotation, t).Normalized();
                }
                pose->accumWeight = total;
            } else {
                math::Vector3    refPosition;
                math::Quaternion refRotation;
                math::Vector3    refScale;
                ResolveAdditiveReferencePose(
                    additiveReferenceClip, layer.additiveReference.time, track,
                    refPosition, refRotation, refScale);

                pose->deltaPosition += (sampledPosition - refPosition) * clipWeight;
                pose->deltaScale    += (sampledScale - refScale) * clipWeight;
                const math::Quaternion rotationDelta =
                    refRotation.Inverse() * sampledRotation;
                pose->deltaRotation = (pose->deltaRotation * math::Quaternion::Slerp(
                    math::Quaternion::Identity(), rotationDelta, clipWeight)).Normalized();
                pose->accumWeight += clipWeight;
                pose->hasRotation = true;
            }
        }
        ApplyPropertyTracks(owner, *weighted.clip, weighted.ticks, &smr);
    }
}

/// @brief レイヤーのステートマシンを進め、評価対象クリップを返す。
/// @return クロスフェード中は遷移元 (1-w) と遷移先 (w) を混ぜたリスト。
static std::vector<WeightedClip> BuildLayerStateClips(
    AnimatorComponent& animator, AnimationLayer& layer, float dt)
{
    std::vector<WeightedClip> result;

    if (layer.states.empty()) return result;

    StateMachineScope scope = LayerScope(layer);
    InitStateMachineScoped(scope);
    UpdateBlendTreeDampingIn(animator, layer.states, dt);
    UpdateStateMachineScoped(animator, scope, dt);

    if (auto* currentState = FindMutableStateIn(layer.states, scope.currentStateName))
        AdvanceBlendTreePhase(animator, *currentState, scope.stateTime, dt);
    if (!scope.blendToState.empty()) {
        if (auto* nextState = FindMutableStateIn(layer.states, scope.blendToState))
            AdvanceBlendTreePhase(animator, *nextState, scope.blendToTime, dt);
    }

    const AnimationState* curSt = FindStateIn(layer.states, scope.currentStateName);
    if (!curSt) return result;

    result = BuildStateClips(animator, *curSt, scope.stateTime);
    if (scope.blendToState.empty()) return result;

    const float w = std::clamp(scope.blendWeight, 0.0f, 1.0f);
    for (auto& clip : result) clip.weight *= (1.0f - w);
    if (const AnimationState* nextSt = FindStateIn(layer.states, scope.blendToState)) {
        for (auto clip : BuildStateClips(animator, *nextSt, scope.blendToTime)) {
            clip.weight *= w;
            result.push_back(clip);
        }
    }
    return result;
}

/// @brief Base Layer の全身ポーズの上へ、追加レイヤーをボーン単位の weight で Override / Additive 合成する。
/// @pre RunStateMachineAnimatorPath の後に呼ぶ。
static void ApplyAnimationLayers(AnimatorComponent& animator,
                                 const asset::Skeleton& skeleton,
                                 Scene& scene,
                                 GameObject& owner,
                                 SkinnedMeshRenderer& smr,
                                 float dt)
{
    if (skeleton.rootNodeIndex < 0 ||
        skeleton.rootNodeIndex >= static_cast<int>(skeleton.nodes.size()) ||
        smr.nodeEntities.size() < skeleton.nodes.size() ||
        animator.nodeGlobalTransforms.size() < skeleton.nodes.size())
        return;
    if (animator.layers.empty()) return;

    /// @note Base Layer の伝播で root local へ焼いた rootInverse を先に外す。外さないと再伝播で二重に積まれる。
    if (GameObject* rootBone =
            scene.GetGameObject(smr.nodeEntities[static_cast<size_t>(skeleton.rootNodeIndex)]))
        SetRootBoneLocalInAnimationSpace(*rootBone, skeleton.rootInverseTransform);

    bool poseChanged = false;
    std::vector<LayerBonePose> poses;

    for (auto& layer : animator.layers) {
        if (!layer.enabled) continue;
        /// @note 追加直後の空レイヤーでマスク読み込み等の副作用を起こさない。
        if (layer.states.empty() && !layer.slot.active) continue;
        EnsureMaskLoaded(layer.mask);

        /// @note Slot とステートマシンはレイヤー weight 0 でも進める。黙らせている間も時間は流れ、復帰時に途中から鳴る。
        const asset::AnimationClip* slotClip = UpdateLayerSlot(animator, layer, dt);
        const float slotWeight = slotClip ? layer.slot.weight : 0.0f;

        auto stateClips = BuildLayerStateClips(animator, layer, dt);
        if (layer.weight <= math::EPSILON) continue;
        if (stateClips.empty() && !slotClip) continue;

        const asset::AnimationClip* additiveReference =
            layer.mode == AnimationLayerMode::Additive
                ? ResolveAdditiveReferenceClip(animator, layer)
                : nullptr;

        poses.clear();
        AccumulateLayerClips(animator, layer, skeleton, owner, smr, stateClips,
                             1.0f - slotWeight, additiveReference, poses);

        if (slotClip && slotWeight > math::EPSILON) {
            const double tps = slotClip->ticksPerSecond > 0.0 ? slotClip->ticksPerSecond : 30.0;
            std::vector<WeightedClip> slotClips;
            /// @note Slot も Base と同じルートモーション設定で抜く。Base だけ抜くと Slot が乗った瞬間にルートごと飛ぶ。
            slotClips.push_back(WeightedClip{
                slotClip, nullptr, static_cast<double>(layer.slot.time) * tps, 1.0f, 1.0f,
                ResolveRootMotion(animator, *slotClip), animator.speed < 0.0f });
            AccumulateLayerClips(animator, layer, skeleton, owner, smr, slotClips,
                                 slotWeight, additiveReference, poses);
        }

        for (const auto& pose : poses) {
            if (!pose.target || pose.accumWeight <= math::EPSILON) continue;
            /// @note Override は補間係数なので 1 で頭打ち。Additive は差分の倍率なので MAX_LAYER_WEIGHT まで許す (Slerp は t > 1 を外挿する)。
            const float rawAlpha = layer.weight * pose.boneWeight;
            const float alpha = layer.mode == AnimationLayerMode::Override
                ? std::clamp(rawAlpha, 0.0f, 1.0f)
                : std::clamp(rawAlpha, 0.0f, MAX_LAYER_WEIGHT);
            if (alpha <= math::EPSILON) continue;

            if (layer.mode == AnimationLayerMode::Override) {
                pose.target->transform.position = math::Vector3::Lerp(
                    pose.target->transform.position, pose.position, alpha);
                if (pose.hasRotation) {
                    pose.target->transform.rotation = math::Quaternion::Slerp(
                        pose.target->transform.rotation, pose.rotation, alpha).Normalized();
                }
                pose.target->transform.scale = math::Vector3::Lerp(
                    pose.target->transform.scale, pose.scale, alpha);
            } else {
                pose.target->transform.position += pose.deltaPosition * alpha;
                pose.target->transform.rotation =
                    (pose.target->transform.rotation * math::Quaternion::Slerp(
                        math::Quaternion::Identity(), pose.deltaRotation, alpha)).Normalized();
                pose.target->transform.scale += pose.deltaScale * alpha;
            }
            poseChanged = true;
        }
    }

    if (poseChanged || !animator.layers.empty()) {
        std::vector<uint8_t> visited(skeleton.nodes.size(), 0);
        PropagateBoneTransforms(scene, skeleton, smr, skeleton.rootInverseTransform,
                                skeleton.rootNodeIndex,
                                SkeletonParentTransform(scene, smr, skeleton, owner), visited);
        RebuildSkinningFromBoneTransforms(scene, owner, skeleton, smr, animator);
    }
}

/// @brief Base Layer を評価し、ルートモーション適用 → ボーン姿勢 → パレット構築まで行う。
static void RunStateMachineAnimatorPath(AnimatorComponent& animator,
                                        const asset::Skeleton& skeleton,
                                        Scene& scene,
                                        GameObject& go,
                                        SkinnedMeshRenderer& smr,
                                        renderer::ResourceManager& resources,
                                        float dt)
{
    InitStateMachine(animator);
    UpdateBlendTreeDamping(animator, dt);
    UpdateStateMachine(animator, dt);
    if (auto* currentState = FindMutableState(animator, animator.currentStateName))
        AdvanceBlendTreePhase(animator, *currentState, animator.stateTime, dt);
    if (!animator.blendToState.empty()) {
        if (auto* nextState = FindMutableState(animator, animator.blendToState))
            AdvanceBlendTreePhase(animator, *nextState, animator.blendToTime, dt);
    }

    const AnimationState* curSt = FindState(animator, animator.currentStateName);
    animator.currentBlendWeights.clear();
    animator.currentBlendDuration = 0.0f;

    /// @note 後段の Layer / IK が読むので、配列とボーン階層は早期 return より前に必ず用意する。
    const size_t boneCount = (std::min)(skeleton.bones.size(),
                                        static_cast<size_t>(asset::MAX_SKINNING_BONES));
    animator.boneMatrices.assign(boneCount, math::Matrix4::Identity());
    animator.nodeGlobalTransforms.assign(skeleton.nodes.size(), math::Matrix4::Identity());
    if (skeleton.rootNodeIndex < 0 ||
        skeleton.rootNodeIndex >= static_cast<int>(skeleton.nodes.size())) {
        UploadBindPose(animator, resources, &skeleton);
        ProcessRootMotion(animator, go, {}, dt);
        return;
    }
    if (!EnsureBoneHierarchy(scene, go, smr, skeleton)) {
        UploadBindPose(animator, resources, &skeleton);
        ProcessRootMotion(animator, go, {}, dt);
        return;
    }

    /// @note externalPose は Script が骨のローカルを直接書く構成。バインド姿勢へ戻さず、今の骨からパレットを組む。
    /// @note パレットを埋めるのは Animator だけなので、Animator を外す運用ではメッシュが動かない。
    const auto applyRestPose = [&]() {
        if (animator.externalPose) {
            std::vector<uint8_t> visited(skeleton.nodes.size(), 0);
            PropagateBoneTransforms(scene, skeleton, smr, skeleton.rootInverseTransform,
                                    skeleton.rootNodeIndex,
                                    SkeletonParentTransform(scene, smr, skeleton, go), visited);
            RebuildSkinningFromBoneTransforms(scene, go, skeleton, smr, animator);
            return;
        }
        ApplyBindPoseToBones(scene, skeleton, smr, go);
        UploadBindPose(animator, resources, &skeleton);
    };

    /// @note 評価できるクリップが無くても移動量ゼロを公開する。前フレームの delta が残ると ExtractOnly の Script が動き続ける。
    if (!curSt) {
        applyRestPose();
        ProcessRootMotion(animator, go, {}, dt);
        return;
    }

    auto currentClips = BuildStateClips(animator, *curSt, animator.stateTime);
    if (currentClips.empty()) {
        applyRestPose();
        ProcessRootMotion(animator, go, {}, dt);
        return;
    }

    animator.currentBlendDuration = GetStateDuration(animator, *curSt);
    bool exposeBlendWeights = curSt->mode != AnimationStateMode::Clip;

    if (!animator.blendToState.empty()) {
        const AnimationState* nextSt = FindState(animator, animator.blendToState);
        exposeBlendWeights = true;
        auto nextClips = nextSt
            ? BuildStateClips(animator, *nextSt, animator.blendToTime)
            : std::vector<WeightedClip>{};

        if (!nextClips.empty()) {
            const float w = std::clamp(animator.blendWeight, 0.0f, 1.0f);
            for (auto& clip : currentClips) clip.weight *= 1.0f - w;
            for (auto& clip : nextClips) clip.weight *= w;
            currentClips.insert(currentClips.end(), nextClips.begin(), nextClips.end());
        } else {
            /// @note 遷移先クリップが無ければ遷移を打ち切り、現クリップ単独で続ける。
            animator.blendToState.clear();
            animator.blendWeight = 0.0f;
        }
    }

    /// @note 最終姿勢への実寄与率を公開する。遷移元と遷移先で同じクリップを使う場合は合算する。
    if (exposeBlendWeights) {
        for (const auto& weighted : currentClips) {
            if (!weighted.clip || weighted.weight <= math::EPSILON) continue;
            const std::string& name =
                weighted.motion && !weighted.motion->clipName.empty()
                ? weighted.motion->clipName
                : weighted.clip->name;
            const auto existing = std::find_if(
                animator.currentBlendWeights.begin(),
                animator.currentBlendWeights.end(),
                [&name](const auto& entry) { return entry.first == name; });
            if (existing != animator.currentBlendWeights.end())
                existing->second += weighted.weight;
            else
                animator.currentBlendWeights.emplace_back(name, weighted.weight);
        }
    }

    animator.currentIKWeight = 0.0f;
    for (const auto& weighted : currentClips)
        animator.currentIKWeight += weighted.ikWeight * weighted.weight;
    animator.currentIKWeight = std::clamp(animator.currentIKWeight, 0.0f, 1.0f);

    /// @note 遷移中は currentClips に遷移元・遷移先が weight 付きで入るので、移動量も姿勢と同じ比率で混ざる。
    ProcessRootMotion(animator, go, currentClips, dt);

    std::vector<uint8_t> evaluationVisited(skeleton.nodes.size(), 0);
    EvaluateNBlendedNodeRecursive(
        skeleton, currentClips, skeleton.rootNodeIndex,
        math::Matrix4::Identity(),
        animator.boneMatrices, animator.nodeGlobalTransforms, evaluationVisited);
    EnsureMaskLoaded(animator.baseLayerMask);
    ApplyNBlendedPoseToBones(scene, skeleton, currentClips, smr,
                             animator.baseLayerMask.loaded ? &animator.baseLayerMask.asset : nullptr);

    std::vector<uint8_t> propagationVisited(skeleton.nodes.size(), 0);
    PropagateBoneTransforms(scene, skeleton, smr, skeleton.rootInverseTransform,
                            skeleton.rootNodeIndex,
                            SkeletonParentTransform(scene, smr, skeleton, go), propagationVisited);
    RebuildSkinningFromBoneTransforms(scene, go, skeleton, smr, animator);
}

/// @note Transform と SkinnedMeshRenderer も書くと宣言する。漏らすと LODSystem 等と同じ並列バッチに入り競合する。
ComponentAccess AnimatorSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<AnimatorComponent>()
        .Writes<AnimatorComponent, BoneComponent, SkinnedMeshRenderer, Transform>();
}

OrderingHints AnimatorSystem::GetOrder() const
{
    return OrderingHints{}.After<TransformLateUpdate>();
}

void AnimatorSystem::Update(SystemContext& ctx)
{
    FBZZ_PROFILE_SCOPE("AnimatorSystem");
    if (!ctx.resources) return;
    Scene& scene = ctx.scene;
    renderer::ResourceManager& resources = *ctx.resources;
    const auto animatorSpan = scene.GetEntities<AnimatorComponent>();
    const auto animatorEntities = std::vector<EntityID>(
        animatorSpan.begin(),
        animatorSpan.end());

    for (EntityID id : animatorEntities) {
        GameObject* gameObject = scene.GetGameObject(id);
        if (!gameObject || !gameObject->activeInHierarchy()) continue;
        GameObject& go = *gameObject;

        auto* animator = go.GetComponent<AnimatorComponent>();
        if (!animator || !animator->enabled) continue;
        const float dt = ctx.dt * std::clamp(animator->localTimeScale, 0.0f, 8.0f);
        animator->firedEvents.clear();
        /// @note 骨由来のカリング球はポーズを組み終えた経路だけが入れ直す。0 のままならバインドポーズ球へ落ちる。
        animator->skinnedBoundsRadius = 0.0f;

        if (!animator->controllerPath.empty() &&
            animator->loadedControllerPath != animator->controllerPath) {
            asset::AnimatorControllerAsset controller;
            if (asset::LoadAnimatorControllerAsset(animator->controllerPath, controller)) {
                asset::ApplyAnimatorControllerAsset(controller, *animator);
            }
            animator->loadedControllerPath = animator->controllerPath;
            animator->appliedAssetGeneration = asset::AssetManager::GetAssetGeneration();
        }

        /// @note アセットがディスクで書き換わったら派生キャッシュを捨てる。loadedControllerPath は残す (消すと保存のたびに再生が頭へ戻る)。
        if (const int assetGeneration = asset::AssetManager::GetAssetGeneration();
            animator->appliedAssetGeneration != assetGeneration) {
            animator->appliedAssetGeneration = assetGeneration;

            if (!animator->controllerPath.empty()) {
                asset::AnimatorControllerAsset controller;
                if (asset::LoadAnimatorControllerAsset(animator->controllerPath, controller))
                    asset::ApplyAnimatorControllerAsset(controller, *animator);
            }
            /// @note クリップは animator へ複製され、.mask は AssetManager を通さず直読みなので、どちらも明示的に落とす。
            animator->clipsLoaded = false;
            animator->baseLayerMask.Invalidate();
            for (auto& layer : animator->layers)
                layer.mask.Invalidate();
        }

        /// @note 空クリップの再試行は FlushFailed() で世代が進んだときだけ。毎フレーム試すと WARN が溢れる。
        const bool needsRetry = !animator->clipsLoaded ||
            (animator->clips.empty() &&
             !animator->states.empty() &&
             asset::AssetManager::GetFlushGeneration() > animator->clipsAttemptGeneration);
        if (needsRetry)
            LoadClips(*animator);

        if (!animator->skinningBuffer.IsValid())
            animator->skinningBuffer = resources.CreateConstantBuffer(sizeof(SkinningCB));
        if (!animator->prevSkinningBuffer.IsValid())
            animator->prevSkinningBuffer = resources.CreateConstantBuffer(sizeof(SkinningCB));

        /// @note 上書き直前の boneMatrices が、IKSystem / SpringBoneSystem の補正込みの前フレーム確定ポーズ。
        SnapshotPreviousBonePalette(*animator, resources);

        /// @note SkinnedMeshRenderer が自身に無ければ直下の子から探す (submesh 分割階層)。
        auto* smr = go.GetComponent<SkinnedMeshRenderer>();
        if (!smr) {
            for (int ci = 0, cn = go.GetChildCount(); ci < cn; ++ci) {
                if (auto* child = go.GetChild(ci)) {
                    if (auto* s = child->GetComponent<SkinnedMeshRenderer>()) { smr = s; break; }
                }
            }
        }
        if (smr && !smr->model && !smr->modelPath.empty())
            smr->model = asset::AssetManager::LoadAndGet<asset::Model>(smr->modelPath);

        const asset::Skeleton* skeleton = nullptr;
        if (smr && smr->model && smr->model->skeleton)
            skeleton = smr->model->skeleton.get();

        /// @note AutoDetect のトラック解決は Skeleton を持たない BuildStateClips からも呼ばれるので、ルート名を焼いておく。
        animator->skeletonRootNodeName.clear();
        if (skeleton && skeleton->rootNodeIndex >= 0 &&
            static_cast<size_t>(skeleton->rootNodeIndex) < skeleton->nodes.size()) {
            animator->skeletonRootNodeName =
                skeleton->nodes[static_cast<size_t>(skeleton->rootNodeIndex)].name;
        }

        /// @note 再生は Play Mode のみ。停止中はバインドポーズで静止させる (モーション確認は Animation Preview の役目)。
        if (!ctx.simulating) {
            /// @note ソケットの親付けやボーン選択のため、ボーン階層は停止中も用意する。
            if (smr && skeleton && skeleton->rootNodeIndex >= 0 &&
                skeleton->rootNodeIndex < static_cast<int>(skeleton->nodes.size()))
            {
                if (EnsureBoneHierarchy(scene, go, *smr, *skeleton))
                    ApplyBindPoseToBones(scene, *skeleton, *smr, go);
            }
            UploadBindPose(*animator, resources, skeleton);
            /// @note 停止中は移動量ゼロを公開する。前フレームの delta が残ると Inspector やポーリングが古い値を掴む。
            animator->rootMotionDeltaPosition = math::Vector3::ZERO;
            animator->rootMotionDeltaRotation = math::Quaternion::Identity();
            animator->rootMotionWorldDelta = math::Vector3::ZERO;
            animator->rootMotionWorldVelocity = math::Vector3::ZERO;
            animator->rootMotionDeltaTime = 0.0f;
            animator->rootMotionAppliedByEngine = false;
            animator->rootMotionSamples.clear();
            continue;
        }

        const float previousTime = animator->stateTime;
        if (!skeleton) {
            const asset::AnimationClip* clip = AdvanceStateMachineAnimator(*animator, dt);
            if (clip) {
                const float currentTime = animator->stateTime;
                const double tps = clip->ticksPerSecond > 0.0 ? clip->ticksPerSecond : 30.0;
                ProcessRootMotion(*animator, go,
                    { WeightedClip{ clip, nullptr,
                                    static_cast<double>(currentTime) * tps, 1.0f, 1.0f,
                                    ResolveRootMotion(*animator, *clip),
                                    animator->speed < 0.0f } },
                    dt);
                ApplyClipSideEffects(go, *animator, *clip, smr,
                                     previousTime, currentTime, true);
                if (smr) UpdateMorphVertexBuffers(*smr, resources);
            }
            UploadBindPose(*animator, resources);
            continue;
        }

        RunStateMachineAnimatorPath(*animator, *skeleton, scene, go, *smr, resources, dt);

        ApplyAnimationLayers(*animator, *skeleton, scene, go, *smr, dt);

        const asset::AnimationClip* effectClip = ResolveStateMachineEffectClip(*animator);
        if (effectClip) {
            const float currentTime = animator->stateTime;
            ApplyClipSideEffects(go, *animator, *effectClip, smr,
                                 previousTime, currentTime, false);
        }
        UpdateMorphVertexBuffers(*smr, resources);
        UpdateSkinnedBounds(*animator, *skeleton);

        SkinningCB cb{};
        for (int i = 0; i < asset::MAX_SKINNING_BONES; ++i)
            cb.boneMatrices[i] = math::Matrix4::Identity();
        for (size_t i = 0; i < animator->boneMatrices.size(); ++i)
            cb.boneMatrices[i] = animator->boneMatrices[i];
        resources.Update(animator->skinningBuffer, &cb, sizeof(SkinningCB));
    }
}

} // namespace fbzz::scene
