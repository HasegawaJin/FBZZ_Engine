/// @file    AnimatorSystem.cpp
/// @brief   スケルタルアニメーションのサンプリングとスキニングパレットのアップロード。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#include <Engine/Scene/Systems/AnimatorSystem.hpp>
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

// 補間規則は .sequence と共有する (AnimationSampling.hpp)。
using asset::SampleVectorKeys;
using asset::SampleQuaternionKeys;
using asset::SampleFloatKeys;

const asset::NodeAnimationTrack* FindTrack(const asset::AnimationClip& clip,
                                           const std::string& nodeName)
{
    for (const auto& track : clip.tracks)
        if (track.nodeName == nodeName)
            return &track;

    // FBX は exporter 次第で同じボーンのチャンネル名が揺れる
    // (mixamorig:RightFoot / RightFoot / mixamorig:RightFoot_$AssimpFbx$_PreRotation)。
    // exact match を優先し、補助ノード suffix と namespace 差だけを吸収する。
    // 正規化規則は asset::CanonicalNodeName に集約し、インポーター側と共有する。
    const std::string canonicalNodeName = asset::CanonicalNodeName(nodeName);
    for (const auto& track : clip.tracks)
        if (asset::CanonicalNodeName(track.nodeName) == canonicalNodeName)
            return &track;

    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Root Motion
// ─────────────────────────────────────────────────────────────────────────────

// あるクリップに対して確定したルートモーション設定。
// トラック解決とクリップ側フラグの上書きを 1 か所で済ませ、
// 「ポーズから抜く軸」と「delta に載せる軸」が食い違わないようにする。
struct ResolvedRootMotion {
    uint32_t trackIndex = UINT32_MAX;
    // delta に載せる軸。mode == None のときは全て false。
    bool applyXZ       = false;
    bool applyY        = false;
    bool applyRotation = false;
    // ポーズ (骨のローカル姿勢) からルート成分を除去する軸。
    // 抽出する軸は必ず除去する。除去しないと Transform 移動とポーズ移動で二重に進む。
    bool stripXZ       = false;
    bool stripY        = false;
    bool stripRotation = false;

    bool HasTrack() const { return trackIndex != UINT32_MAX; }
    bool ExtractsAnyAxis() const { return applyXZ || applyY || applyRotation; }
    bool StripsAnyAxis() const { return stripXZ || stripY || stripRotation; }
};

// RootMotionAxisOverride を、クリップ側フラグへ適用して最終値を決める。
bool ResolveAxis(RootMotionAxisOverride axisOverride, bool clipValue)
{
    switch (axisOverride) {
    case RootMotionAxisOverride::Disabled: return false;
    case RootMotionAxisOverride::Enabled:  return true;
    case RootMotionAxisOverride::UseClip:
    default:                               return clipValue;
    }
}

// Animator の設定とクリップの内容から、そのクリップのルートモーション構成を確定する。
// 抽出可否と除去可否をここで一元的に決める。片方だけクリップ側フラグを直接読むと、
// 「ポーズからは抜かれるが Transform も delta も動かない」= 前進が消える状態になる。
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
        // クリップ自身の指定を最優先し、無ければ候補名 → スケルトンルートで探す。
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

    // 軸マスク: クリップに焼かれた値を Animator 側の三値オーバーライドで上書きする。
    const bool axisXZ = ResolveAxis(settings.applyXZ, clip.rootMotionApplyXZ);
    const bool axisY = ResolveAxis(settings.applyY, clip.rootMotionApplyY);
    const bool axisRotation =
        ResolveAxis(settings.applyRotation, clip.rootMotionApplyRotation);

    if (settings.Extracts()) {
        // 抽出する軸はそのまま除去する軸でもある。
        resolved.applyXZ = resolved.stripXZ = axisXZ;
        resolved.applyY = resolved.stripY = axisY;
        resolved.applyRotation = resolved.stripRotation = axisRotation;
        return resolved;
    }

    // mode == None。delta には何も載せず、ポーズをどうするかだけ poseMode が決める。
    //   Strip: ルート成分を除去して「その場再生」にする (旧 applyRootMotion=false 相当)
    //   Keep : クリップのまま残す。ルートごと前進する DCC そのままの見た目になる
    if (settings.poseMode == RootMotionPoseMode::Strip) {
        resolved.stripXZ = axisXZ;
        resolved.stripY = axisY;
        resolved.stripRotation = axisRotation;
    } else {
        resolved.trackIndex = UINT32_MAX; // サンプリング側の分岐を丸ごと省く
    }
    return resolved;
}

// 階層パス ("Armature/Hips/Spine/Head") から対象 GameObject を引く。
// 毎フレーム × アニメーター数 × トラック数で呼ばれる。substr で切ると 1 フレームに
// 数千回のヒープ確保が出るので、区間は string_view で切り出す。
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

// targetPath が無い旧 .anim を、BoneComponent の名前から解決する。
// BoneComponent に限定して検索し、同名の装飾用 GameObject を誤って動かさない。
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

// マテリアルアニメーションの適用先 GameObject を探す。
// 1 GameObject = モデル全体で submesh は MaterialComponent のスロットなので、
// ここは Renderer 側の GO を返し、どのスロットへ書くかは ApplyMaterialProperty が決める。
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

// 1 クリップ分のルートモーション移動量。
struct RootMotionDelta {
    math::Vector3    position = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();
};

// クリップの前フレームサンプルを取り出す (無ければ新規作成)。
// clip ポインタを同一性キーにする。LoadClips で clips 配列を作り直したときは
// AnimatorSystem 側でキャッシュごと破棄するため、無効ポインタは残らない。
RootMotionClipSample& AcquireRootMotionSample(AnimatorComponent& animator,
                                              const asset::AnimationClip& clip)
{
    for (auto& sample : animator.rootMotionSamples)
        if (sample.clip == &clip) return sample;
    animator.rootMotionSamples.push_back(RootMotionClipSample{ &clip });
    return animator.rootMotionSamples.back();
}

// 前フレームからの差分としてクリップ 1 本のルートモーションを取り出す。
// クリップが自分の前回サンプル tick を覚えていれば、ブレンド構成が毎フレーム変わっても
// 各クリップの delta は連続する (ステート時刻の差分で出すと支配クリップ 1 本しか扱えない)。
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

    // 前フレームに評価されていないクリップ (フェードインした直後など) は差分を取れない。
    // 最初の 1 フレームだけ寄与ゼロになるが、その時点の weight はほぼ 0 なので影響しない。
    const bool continuous = sample.frame + 1 == frame;
    if (continuous) {
        const double endTicks = clip.durationTicks > 0.0
            ? clip.durationTicks
            : clip.GetDurationSeconds() *
              (clip.ticksPerSecond > 0.0 ? clip.ticksPerSecond : 30.0);
        // 時刻が巻き戻っていればループ 1 周ぶんを跨いだとみなす。
        // 逆再生 (speed < 0) では大小関係が反転するため reverse で判定を切り替える。
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

// 前フレームに評価されなかったクリップのサンプルを捨てる。
// WHY: 放置すると Animator が触った全クリップぶん配列が伸び続ける。
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
            // 接線を持たないモデルや、デルタが元の向きを打ち消した頂点はゼロになる。
            // 描けない値ではあるが読み込めてしまうデータなので、既定の軸へ逃がす。
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

// ポーズからルート成分を除去する。除去する軸は ResolvedRootMotion が決める。
// 「抜くかどうか」は Animator の設定で決まる ─ クリップ側フラグを直接読むと、
// 抽出を止めてもポーズからは抜かれ続けて前進成分が消える。
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

// アフィン行列を Transform の TRS へ分解する。
// TransformSystem / ConstraintSystem は local から world を再計算するので、world だけへ
// 補正を入れても後段の Flush で消える。root bone の local 値へ組み込む必要がある。
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

void PropagateNonBoneChildTransforms(GameObject& parent)
{
    // AnimatorSystem は TransformSystem の後で Bone の姿勢を上書きするので、Bone 配下へ
    // 置いた Particle / Attachment は放っておくと手や武器に追従しない。
    // BoneComponent を持つ子は Skeleton 再帰側が処理する。ここは通常子だけ。
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

// スケルトンの親参照が循環していないかを確認し、生成時の再帰を止める。
// WHY: 通常のインポーターは木構造を作るが、古いキャッシュや破損した .fzasset は
//      親インデックスだけが循環することがあり、Base Layer の初回評価をハングさせる。
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

GameObject& EnsureBoneObject(Scene& scene,
                             GameObject& owner,
                             SkinnedMeshRenderer& smr,
                             const asset::Skeleton& skeleton,
                             int nodeIndex)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size()))
        return owner;
    auto& node = skeleton.nodes[static_cast<size_t>(nodeIndex)];

    if (nodeIndex < static_cast<int>(smr.nodeEntities.size())) {
        if (auto* existing = scene.GetGameObject(smr.nodeEntities[static_cast<size_t>(nodeIndex)])) {
            if (auto* bone = existing->GetComponent<BoneComponent>()) {
                bone->boneName = node.name;
                bone->boneIndex = node.boneIndex;
                bone->skinnedMeshEntity = owner.GetID();
            }
            return *existing;
        }
    }

    // 既存ボーンの捜索範囲。
    // Renderer は Body / Visor といった子に付き、ボーンは兄弟の Armature 側に居るので、
    // owner の子孫しか見ないと Renderer ごとにボーン階層が生えてしまう。
    // skeletonRootEntity が解決できていればその親を範囲にする。別キャラの兄弟へ
    // 誤って束縛しないよう、範囲は「起点ボーンの親」までに限る。
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
        return *found;
    }

    GameObject* parent = &owner;
    if (node.parentIndex >= 0 &&
        node.parentIndex < static_cast<int>(skeleton.nodes.size()) &&
        HasAcyclicParentChain(skeleton, node.parentIndex))
        parent = &EnsureBoneObject(scene, owner, smr, skeleton, node.parentIndex);

    GameObject& boneObject = scene.CreateGameObject(node.name);
    boneObject.layer = owner.layer;
    boneObject.transform.position = node.bindTranslation;
    boneObject.transform.rotation = node.bindRotation;
    boneObject.transform.scale = node.bindScale;
    boneObject.SetParent(parent);

    BoneComponent bone{};
    bone.boneName = node.name;
    bone.nodeIndex = nodeIndex;
    bone.boneIndex = node.boneIndex;
    bone.skinnedMeshEntity = owner.GetID();
    bone.generated = true;
    boneObject.AddComponent<BoneComponent>(std::move(bone));

    smr.nodeEntities[static_cast<size_t>(nodeIndex)] = boneObject.GetID();
    if (nodeIndex == skeleton.rootNodeIndex)
        smr.skeletonRootEntity = boneObject.GetID();
    return boneObject;
}

void EnsureBoneHierarchy(Scene& scene,
                         GameObject& owner,
                         SkinnedMeshRenderer& smr,
                         const asset::Skeleton& skeleton)
{
    if (smr.nodeEntities.size() != skeleton.nodes.size())
        smr.nodeEntities.assign(skeleton.nodes.size(), EntityID::INVALID);

    for (size_t i = 0; i < skeleton.nodes.size(); ++i)
        EnsureBoneObject(scene, owner, smr, skeleton, static_cast<int>(i));
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
    // アセット破損で children が循環していても、Base Layer の全身評価を
    // 無限再帰にしない。通常の Assimp 階層では各ノードは一度だけ通る。
    if (visited[static_cast<size_t>(nodeIndex)] != 0)
        return;
    visited[static_cast<size_t>(nodeIndex)] = 1;
    GameObject* boneObject = scene.GetGameObject(smr.nodeEntities[static_cast<size_t>(nodeIndex)]);
    if (!boneObject) return;

    if (nodeIndex == skeleton.rootNodeIndex) {
        // 描画側の boneMatrix = rootInverse * nodeGlobal * offset と同じ補正を
        // 階層の root local へ移す。これで ConstraintSystem の Flush 後も維持される。
        SetRootBoneLocalInSkinningSpace(*boneObject, rootInverseTransform);
    }

    UpdateWorldTransform(*boneObject, parentTransform);
    PropagateNonBoneChildTransforms(*boneObject);

    for (int child : skeleton.nodes[static_cast<size_t>(nodeIndex)].children)
        PropagateBoneTransforms(scene, skeleton, smr, rootInverseTransform,
                                child, boneObject->transform, visited);
}

// ボーン GameObject をリファレンスポーズへ戻し、ソケットを停止中にも描画空間へ揃える。
// 停止中も GPU は referencePose を描くので、ボーンだけ前フレームの姿勢を残すと
// SOCKET_HAND Gizmo と bind pose のメッシュがずれる。
void ApplyBindPoseToBones(Scene& scene,
                          const asset::Skeleton& skeleton,
                          SkinnedMeshRenderer& smr,
                          const Transform& owner)
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
                            skeleton.rootNodeIndex, owner, visited);
}

void RebuildSkinningFromBoneTransforms(Scene& scene,
                                       GameObject& owner,
                                       const asset::Skeleton& skeleton,
                                       SkinnedMeshRenderer& smr,
                                       AnimatorComponent& animator)
{
    // Animator の評価失敗やリグ差し替え時も、配列外書き込みを起こさず VS 経路へ戻す。
    if (smr.nodeEntities.size() < skeleton.nodes.size() ||
        animator.nodeGlobalTransforms.size() < skeleton.nodes.size())
        return;

    const math::Matrix4 ownerInverse = math::Matrix4::Inverse(owner.transform.GetWorldMatrix());
    // PropagateBoneTransforms は root local へ rootInverse を組み込むため、
    // world -> nodeGlobal の復元ではその逆行列を先に戻す。
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

// バインドポーズのスキニング行列をノード階層から構築する。単位行列ではない。
// boneMatrix = rootInverse · nodeGlobal · offsetMatrix の nodeGlobal をバインド TRS で
// 埋めた値が正しい「無アニメ状態」。Mixamo (Y-up) ではたまたま identity になるが、
// Blender 製 FBX は Y-up への変換 (-90°X) をアーマチュアノードが担うので、
// 単位行列を入れるとモデルが X 軸まわりに 90° 倒れる。
// 無アニメ時はスケルトンのリファレンスポーズを送る。単位行列は skeleton = nullptr
// (スケルトン未解決の汎用アニメータ経路) のときだけ。
// animator 側の配列も埋める ─ boneMatrices は IKSystem / MeshTrailRenderPass /
// ParticlePass / AnimatorDebugPass が読むので、CPU 側だけ identity だと描画とズレる。
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

// SnapshotPreviousBonePalette — 現在の boneMatrices を「前フレーム」として確定させる。
// AnimatorSystem がポーズを上書きする直前に 1 回だけ呼ぶこと。
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

void LoadClips(AnimatorComponent& animator)
{
    animator.clips.clear();
    animator.clipSourcePaths.clear();
    // clips を作り直すとポインタが無効になる。ルートモーションのサンプルキャッシュは
    // clip ポインタをキーにしているため、ここで必ず捨てる。
    animator.rootMotionSamples.clear();

    // Node が直接参照する Source を収集する。
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

    // Base Layer だけでなく、追加 Layer の Draw/Holster や攻撃モーションも同じ
    // Animator クリップ配列へ登録する。Layer 側だけに接続された Motion を見落とすと、
    // Base Layer を有効にした後に「Layer のステート名は合っているのに動かない」状態になる。
    for (const auto& state : animator.states)
        addStateSources(state);
    for (const auto& layer : animator.layers)
    {
        for (const auto& state : layer.states)
            addStateSources(state);
        // Additive の基準クリップはステートから参照されないことがあるため、ここでも
        // 収集する。未ロードの基準ポーズを先頭キーへ黙って縮退させない。
        addSource(layer.additiveReference.sourcePath);
    }
    // ステートから辿れない演出専用クリップ (.sequence の AnimationTrack など)。
    for (const auto& src : animator.externalClipSources)
        addSource(src);

    for (const auto& src : sources) {
        if (src.empty()) continue;

        // Controller は .anim を GUID で保存するため、GUID 文字列そのものには
        // 拡張子が無い。解決前に ModelImporter へ渡すと、追加レイヤーの .anim を
        // FBX / .fzasset として解釈してクラッシュする経路になる。
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

        // .anim ファイルは AnimationClip として直接ロードする。
        // WHY: FBX インポート時のアニメーションクリップは .anim に分離されており、
        //      .fzasset (モデルファイル) には clips が含まれないため。
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

        auto model = asset::AssetManager::LoadModel(src);
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

// ── ステートマシン用ヘルパー ──────────────────────────────────────────────────

// animator.clips からクリップ名で探す。完全一致 → 大文字小文字無視の含有一致。
// エクスポーターによっては "Walk" → "Armature|Walk" とプレフィックスが付く。
const asset::AnimationClip* FindClipByName(const AnimatorComponent& animator,
                                           const std::string& clipName)
{
    if (clipName.empty() || animator.clips.empty()) return nullptr;

    // 1st pass: 完全一致
    for (const auto& c : animator.clips)
        if (c.name == clipName) return &c;

    // 2nd pass: 大文字小文字無視の部分一致
    //   FBX の clip.name が clipName を含んでいれば採用
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

// ステートに対応するクリップを返す。clipName 名前検索 → clipIndex 直接指定。
// Mixamo 等は FBX 内クリップ名を "mixamo.com" にするため名前検索が失敗する。
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

// animator.states からステート名で探す。見つからなければ nullptr。
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

// animator.parameters からパラメーター名で探す。見つからなければ nullptr。
AnimatorParameter* FindParam(AnimatorComponent& animator, const std::string& paramName)
{
    for (auto& p : animator.parameters)
        if (p.name == paramName) return &p;
    return nullptr;
}

// Source Path と Clip Name の組でクリップを解決する。
// WHY: 異なる FBX が同名クリップを持っていても Node の参照先を一意に保つため。
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
    // このクリップのルートモーション構成。ポーズ除去と delta 抽出の両方がここを見る。
    // クリップごとに持たせて weight で加重合成する (1 本だけ見ると Walk↔Run が 0.5 を
    // 跨いだ瞬間に移動量が段差状に飛ぶ)。
    ResolvedRootMotion          rootMotion{};
    // 逆再生中か。ループ跨ぎの判定方向がひっくり返る。
    bool                        reverse = false;
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
    // 実効再生方向。Animator / State / Motion の speed すべての符号で決まる。
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
            // WHY: Motionごとの秒数で個別Wrapすると、Weight 0から復帰したWalkが別位相で現れる。
            //      同じ0..1位相を各Clip長へ写像し、Idle/Walk/Runの足運びを連続させる。
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
            // State を全体係数、Motion をクリップ固有係数として扱う。
            // WHY: BlendTree 全体を一括調整しつつ、Run だけ足IKを弱められるようにする。
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

    // BlendTree の再生周期は現在 Weight に依存させず、全 Motion の最大実効 Length で固定する。
    // WHY: Damping 中は Weight が毎フレーム変わる。加重平均 Length を WrapTime に使うと、
    //      周期が途中で短くなった瞬間に stateTime / blendToTime が巻き戻り、Walk が再生し直されるため。
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
    // Base Layer は全スケルトンを評価するため、壊れたキャッシュの循環参照が
    // あるとここがフレームを返さなくなる。評価済みノードを一度だけ処理する。
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

        // Base Layer マスク: 重みが 1 未満のボーンはバインドポーズ側へ寄せる。
        // 前フレームの姿勢を残すと、上のレイヤーが weight 0 になった瞬間に固まる。
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

// 遷移条件を1つ評価する。パラメーターが見つからない場合は false。
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

// 遷移の全条件（AND）と hasExitTime を評価する。
bool EvaluateTransition(const AnimationTransition& tr,
                        AnimatorComponent& animator,
                        float normalizedTime)
{
    // hasExitTime: 再生位置が exitTime に達していないと遷移しない
    if (tr.hasExitTime && normalizedTime < tr.exitTime) return false;

    // 条件リストが空で hasExitTime=false → 無効定義として遷移しない
    if (tr.conditions.empty()) return tr.hasExitTime;

    // 全条件 AND
    for (const auto& cond : tr.conditions) {
        const AnimatorParameter* param = FindParam(animator, cond.paramName);
        if (!CheckCondition(cond, param)) return false;
    }
    return true;
}

// 遷移に使われた Trigger パラメーターを false にリセットする。
// WHY: Trigger は「1フレームの発火信号」なので、遷移に消費されたら自動で戻す必要がある。
void ConsumeTriggers(AnimatorComponent& animator, const AnimationTransition& tr)
{
    for (const auto& cond : tr.conditions) {
        auto* param = FindParam(animator, cond.paramName);
        if (param && param->type == ParamType::Trigger)
            param->boolValue = false;
    }
}

// ステートマシンを defaultStateName または states[0] で初期化する。
// currentStateName が既に設定されている場合は何もしない。
// ── ステートマシンのスコープ化 ───────────────────────────────────────────────
// Base Layer の状態は AnimatorComponent 直下のフィールド (currentStateName / stateTime /
// blendTo*) にあり、Script・Editor・MCP・VFX が直接参照している。構造体へ畳むと
// 参照側を全部書き換えることになる。
// 「どの states を、どのランタイム変数で回すか」だけを参照で束ねたビューにすれば、
// Base Layer も追加レイヤーも同じ評価コードで回せる。
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

// 遷移条件の評価は animator.parameters を見る。パラメーターは Unity 同様
// レイヤーをまたいで共有されるため、scope ではなく animator をそのまま渡す。
bool TryStartTransitionScoped(AnimatorComponent& animator,
                              StateMachineScope scope,
                              const std::vector<AnimationTransition>& transitions,
                              float normalizedTime)
{
    for (const auto& tr : transitions) {
        if (tr.toStateName.empty()) continue;
        // 現在ステート自身へ戻る遷移は開始しない。
        // WHY: Fall 条件のような継続条件で毎フレーム自己遷移すると、再生時刻が0へ戻り続けるため。
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
        // 正規化指定は遷移元ステートの Length を基準に実秒へ変換する。
        // WHY: クリップを差し替えても同じ割合の Motion Blend を維持できる。
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

// BlendTree の入力値を時定数ベースで平滑化する (1D は 1 本、2D は 2 軸とも)。
// WHY: Script が Speed を 0 / 4 / 7.2 と離散的に設定しても、姿勢 Weight は連続変化させる。
//      2D では移動方向が入力を離した瞬間に不連続へ飛ぶため、同じ平滑化が要る。
void UpdateBlendTreeDampingIn(AnimatorComponent& animator,
                              std::vector<AnimationState>& states,
                              float dt)
{
    if (!animator.playing) return;

    // 1 本ぶんの指数補間。1D の値と 2D の各軸で式を分けない。
    const auto damp = [dt](float current, float target, float dampTime) {
        const float alpha =
            1.0f - std::exp(-(std::max)(dt, 0.0f) / (std::max)(dampTime, 1e-4f));
        float value = current + (target - current) * std::clamp(alpha, 0.0f, 1.0f);
        // 指数補間は理論上目標へ到達しないため、近傍で確定して不要な2Clip評価を終了する。
        // WHY: 極小WeightのClipも全ボーンをサンプリングすると、定常時のCPU負荷が倍増する。
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

// 現在のBlend Weightからサイクル周波数を補間し、共通の正規化位相を積分する。
// WHY: 最長Clipへ全Motionを引き伸ばす位相同期ではWalk本来の再生速度が失われるため、
//      純粋なMotionでは元速度、ブレンド中は両者の中間テンポになるよう周波数を合成する。
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

    // 位相の置き場だけが 1D / 2D で違う。進め方は同じ。
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

// ステートマシンを1フレーム分更新する。
// 遷移中のブレンド進行 → stateTime 進行 → 遷移条件チェックの順で処理する。
void UpdateStateMachineScoped(AnimatorComponent& animator, StateMachineScope scope, float dt)
{
    // ── ブレンド進行 ───────────────────────────────────────────────────────
    if (!scope.blendToState.empty()) {
        if (!animator.playing) return;

        scope.blendWeight += dt / (std::max)(scope.blendDuration, 1e-4f);

        // クロスフェード中も遷移元のポーズを進め、静止ポーズへのフェードを防ぐ。
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

        // 遷移先ステートの時刻も進める
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
            // 遷移完了: 現ステートを次ステートに切り替える
            scope.currentStateName = scope.blendToState;
            scope.stateTime        = scope.blendToTime;
            scope.blendToState     = "";
            scope.blendWeight      = 0.0f;
        }
        // 遷移中は新たな遷移チェックをしない
        return;
    }

    // ── 現ステートの時刻進行 ──────────────────────────────────────────────
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

    // ── 遷移条件チェック ──────────────────────────────────────────────────
    const float normalizedTime = (duration > 0.0f)
        ? std::clamp(scope.stateTime / duration, 0.0f, 1.0f)
        : 0.0f;

    // 通常遷移を優先し、成立しなかった場合だけ AnyState を評価する。
    if (!TryStartTransitionScoped(animator, scope, curSt->transitions, normalizedTime))
        TryStartTransitionScoped(animator, scope, scope.anyStateTransitions, normalizedTime);
}

void UpdateStateMachine(AnimatorComponent& animator, float dt)
{
    UpdateStateMachineScoped(animator, BaseScope(animator), dt);
}

// ─────────────────────────────────────────────────────────────────────────────
// Root Motion の合成と適用
// ─────────────────────────────────────────────────────────────────────────────

// 加重クリップ集合からルートモーションを合成する。
// 位置は weight で線形加重、回転は姿勢ブレンドと同じ逐次 Slerp。
// ポーズが 6:4 で混ざっているのに移動量だけ Walk 100% だと足が滑る。
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

// ルートモーションの適用先 GameObject を解決する。
// 空文字列なら Animator 自身。".." で親を遡り、それ以外は owner からの子孫パス。
// Animator がメッシュ側の子に付いていると、その子だけ動かして親からずれていく。
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
        if (!next) return owner; // 解決できないパスは自身へフォールバックする
        current = next;
    }
    return *current;
}

// target から owner までの親子チェーンのワールド Transform を更新する。
// AnimatorSystem は TransformLateUpdate の後に走るので、Transform を書き換えても
// worldPosition は前の値のまま。放置するとボーンのワールド行列が 1 フレーム遅れる。
void RefreshWorldChain(GameObject& target, GameObject& owner)
{
    std::vector<GameObject*> chain;
    for (GameObject* go = &owner;; go = go->GetParent()) {
        chain.push_back(go);
        if (go == &target) break;
        if (!go->GetParent()) {
            // target が owner の祖先ではない (別枝)。target 単体だけ更新する。
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

// Motion Warping — クリップが生む移動量へ「まだ足りないぶん」を上乗せし、
// 指定時間の終わりにちょうど目標へ着かせる。
// 本来の Motion Warping は残り区間の総量を先読みして倍率を求めるが、BlendTree と遷移で
// 毎フレーム合成が変わる本実装では総量が確定しない。毎フレーム誤差の一定割合を配れば
// 先読みが要らず、残り時間が 0 に近づくほど割合が 1 へ寄って必ず収束する。
// 置き換えでなく上乗せなのは、置き換えると区間中の緩急が消えて等速で滑るため。
// 戻り値はワールド空間の補正量。呼び出し側が localDelta へも反映する。
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

    // 残り時間より dt が大きいフレームでは alpha が 1 になり、誤差を一括で詰める。
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

// 合成済みの delta を Animator の出力へ書き、mode に応じて適用する。
// ポーズ評価より前に呼ぶこと (適用後のワールド行列でボーンを伝播させるため)。
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

    // ワールド空間の移動量。適用前の worldRotation を基準にする。
    math::Vector3 worldDelta = target.transform.worldRotation * localDelta;

    // Motion Warping はクリップの移動量が出そろった後、適用の直前に上乗せする。
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
        // 水平成分だけ速度へ渡し、鉛直成分は重力と衝突解決に任せる。
        // WHY: Transform 直書きは PhysicsSystem の SetPosition() でテレポートになり、
        //      スイープも押し戻しも無いまま壁を抜ける。速度として渡せばソルバーが解く。
        auto* rb = target.GetComponent<RigidBodyComponent>();
        if (rb && rb->enabled && rb->rigidBody && dt > math::EPSILON) {
            math::Vector3 velocity = rb->rigidBody->GetVelocity();
            velocity.x = worldDelta.x / dt;
            velocity.z = worldDelta.z / dt;
            // Y はクリップが明示的に指定しているときだけ上書きする。
            if (std::fabs(worldDelta.y) > math::EPSILON)
                velocity.y = worldDelta.y / dt;
            rb->rigidBody->SetVelocity(velocity);
            animator.rootMotionAppliedByEngine = true;
        } else {
            // RigidBody が無い / dt が 0 のフレームは Transform へフォールバックする。
            // WHY: 何もしないと「設定ミスで移動だけ消える」不可解な挙動になる。
            target.transform.position += target.transform.rotation * localDelta;
            animator.rootMotionAppliedByEngine = true;
        }
        // 回転は物理ボディに任せず Transform 側で回す。次フレームの
        // PhysicsSystem::SyncRigidBodies が worldRotation をボディへ書き戻す。
        target.transform.rotation =
            (target.transform.rotation * localRotation).Normalized();
        RefreshWorldChain(target, owner);
        break;
    }

    case RootMotionMode::ExtractOnly:
        // エンジンは何も動かさない。移動の適用は OnAnimatorMove を受けた Script の責任。
        break;

    case RootMotionMode::None:
    default:
        break;
    }
}

// 抽出した結果を同一フレーム内で Script へ通知する。
// WHY: ScriptAnimatorProxy のポーリングだけだと、AnimatorSystem が Phase::LateUpdate に
//      居るため OnUpdate は常に 1 フレーム前の delta を読むことになる。
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

// 加重クリップ集合からルートモーションを抽出し、適用して Script へ通知するまでを行う。
// ポーズ評価より前に呼ぶ。
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

// ── 後方互換パス（states が空のとき）────────────────────────────────────────
// ── ステートマシンパス（states が存在するとき）──────────────────────────────
static const asset::AnimationClip* ResolveStateMachineEffectClip(AnimatorComponent& animator)
{
    const AnimationState* state = FindState(animator, animator.currentStateName);
    if (!state) return nullptr;
    auto clips = BuildStateClips(animator, *state, animator.stateTime);
    const auto best = std::max_element(clips.begin(), clips.end(),
        [](const WeightedClip& a, const WeightedClip& b) { return a.weight < b.weight; });
    return best != clips.end() ? best->clip : nullptr;
}

static const asset::AnimationClip* AdvanceStateMachineAnimator(AnimatorComponent& animator, float dt)
{
    InitStateMachine(animator);
    UpdateBlendTreeDamping(animator, dt);
    UpdateStateMachine(animator, dt);
    if (auto* state = FindMutableState(animator, animator.currentStateName))
        AdvanceBlendTreePhase(animator, *state, animator.stateTime, dt);
    return ResolveStateMachineEffectClip(animator);
}

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
    // スケルトン経路では RunStateMachineAnimatorPath がブレンド結果を Bone Transform へ
    // 確定済みなので、単独クリップを再適用しない。すると GPU スキニングはブレンド姿勢、
    // socket は最大ウェイト 1 本の姿勢になり、手と武器が別軌道になる。
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
    // Event の区間判定だけは「同じクリップを続けて再生している」ことが前提になる。
    // ルートモーションはクリップ単位のサンプル差分へ移したため、この分岐から外れた。
    if (animator.previousEventClipName == clip.name)
        DispatchAnimationEvents(owner, animator, clip, previousTime, currentTime, looped, reverse);
    animator.previousEventTime = currentTime;
    animator.previousEventClipName = clip.name;
}

// ── アニメーションレイヤー ────────────────────────────────────────────────────
// 上半身 / 下半身の出し分けはここが実装本体。Base Layer が全身ポーズを作った後に、
// レイヤーごとの姿勢をボーン単位のウェイトで上書き (Override) / 加算 (Additive) する。

// .mask アセットを必要なときだけ読み込む。Base Layer と各 AnimationLayer で共用する。
// WHY: 毎フレーム TOML をパースすると数十体のキャラで即破綻する。パスが変わったときだけ読み直す。
static void EnsureMaskLoaded(AnimationMaskRef& ref)
{
    if (ref.path.empty()) {
        if (ref.loaded) {
            ref.loaded = false;
            ref.asset = asset::AvatarMaskAsset{};
            ref.loadedPath.clear();
        }
        return;
    }
    if (ref.loaded && ref.loadedPath == ref.path) return;

    ref.loadedPath = ref.path;
    ref.asset      = asset::AvatarMaskAsset{};
    const std::string resolved = asset::AssetManager::ResolveAssetPath(ref.path);
    ref.loaded = asset::LoadAvatarMaskAsset(resolved, ref.asset);
    if (!ref.loaded)
        FBZZ_LOG_WARN("AnimatorSystem: avatar mask load failed [%s]", ref.path.c_str());
}

// このレイヤーが対象ボーンへ効く割合 0..1 を返す。.mask が無ければ全身に適用する。
// 0/1 の二値だと境界ボーン (Spine 等) でポーズが折れるので、blendDepth で数階層かけて立ち上げる。
static float LayerBoneWeight(const AnimationLayer& layer,
                             const std::string& path,
                             const std::string& nodeName)
{
    if (layer.mask.loaded)
        return asset::EvaluateAvatarMaskWeight(layer.mask.asset, path, nodeName);

    (void)path;
    (void)nodeName;
    return 1.0f;
}

// 加算レイヤーの基準ポーズを解決する。
// additiveReference が未設定なら nullptr を返し、呼び出し側は従来どおり
// 「加算クリップ自身の先頭キー」を基準に使う。
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

// 基準クリップから、対象ボーンの基準 TRS を取り出す。
// referenceClip が無い場合は track 自身の先頭キーを基準にする (従来動作)。
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
    // フォールバック: 加算クリップ自身の先頭キー。
    outPosition = track.positions.empty() ? math::Vector3::ZERO : track.positions.front().value;
    outRotation = track.rotations.empty() ? math::Quaternion::Identity() : track.rotations.front().value;
    outScale    = track.scales.empty()    ? math::Vector3::ONE : track.scales.front().value;
}

// Slot (ワンショット差し込み) のフェードと再生位置を進める。
// WHY: 「移動を流したまま上半身に攻撃を割り込ませ、終わったら自動で戻る」を
//      ステートマシンへ専用ステートと復帰遷移を足さずに成立させる。
static const asset::AnimationClip* UpdateLayerSlot(
    AnimatorComponent& animator, AnimationLayer& layer, float dt)
{
    AnimationSlotPlayback& slot = layer.slot;
    if (!slot.active) { slot.weight = 0.0f; return nullptr; }

    const asset::AnimationClip* clip =
        FindClipBySource(animator, slot.sourcePath, slot.clipName);
    if (!clip && !slot.clipName.empty()) clip = FindClipByName(animator, slot.clipName);
    if (!clip) {
        // 参照が解決できない Slot は鳴らしっぱなしにせず畳む。
        slot.active = false;
        slot.weight = 0.0f;
        return nullptr;
    }

    // 外部 (SequenceSystem) が時刻とフェードを書いている間は、こちらから触らない。
    if (slot.driven) return clip;

    const float duration = static_cast<float>(clip->GetDurationSeconds());
    if (animator.playing) {
        slot.time += dt * slot.speed * animator.speed;
        if (slot.loop && duration > 0.0f) {
            slot.time = WrapTime(slot.time, duration);
        } else if (duration > 0.0f) {
            slot.time = std::clamp(slot.time, 0.0f, duration);
            // 末尾に到達したら自動でフェードアウトへ移す。
            // WHY: ワンショットは「終わったら戻る」まで含めて 1 操作であってほしい。
            const float fadeOutStart = (std::max)(duration - slot.fadeOutDuration, 0.0f);
            if (slot.time >= fadeOutStart) slot.stopping = true;
        }
    }

    // フェード進行。stopping なら 0 へ、そうでなければ 1 へ向かう。
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

// レイヤー内で 1 ボーンぶんのポーズを重み付き平均で積む作業バッファ。
// 順次 Lerp だと 3 つ以上の Motion で後ろの Clip ほど強く出る。
// 「累積ウェイトに対する比率」で積めば順序に依存しない加重平均になる。
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
    float            boneWeight   = 1.0f;
};

// 1 レイヤーぶんのクリップ集合を評価して、ボーンごとのポーズを積む。
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

            // 新しいクリップは正規パスを使うが、旧クリップは targetPath が空のため
            // nodeName から BoneComponent を引く。非空パスが古い階層を指している場合も
            // 名前へフォールバックし、インポートし直すまでレイヤー全体を無効にしない。
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

            // 同じボーンの既存エントリを探す (レイヤー内のクリップ数は多くて数本)。
            LayerBonePose* pose = nullptr;
            for (auto& p : poses)
                if (p.target == target) { pose = &p; break; }
            if (!pose) {
                poses.push_back(LayerBonePose{});
                pose = &poses.back();
                pose->target = target;
                pose->boneWeight = boneWeight;
            }

            if (layer.mode == AnimationLayerMode::Override) {
                // 累積ウェイトに対する比率で積み、順序に依存しない加重平均にする。
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

// レイヤーのステートマシンを進め、評価対象クリップを返す。
// クロスフェード中は遷移元と遷移先を blendWeight で混ぜたリストになる。
static std::vector<WeightedClip> BuildLayerStateClips(
    AnimatorComponent& animator, AnimationLayer& layer, float dt)
{
    std::vector<WeightedClip> result;

    if (layer.states.empty()) return result;

    // 新形式: レイヤー専用のステートマシンを 1 フレーム進める。
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

    // クロスフェード: 遷移元を (1-w)、遷移先を w で混ぜる。
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

    // Base Layer の伝播で root local へ一度組み込んだ rootInverse を外してから
    // 追加 Layer を合成する。これを戻さずに再度掛けると、Layer が存在するモデルだけ
    // rootInverse が毎フレーム二重に積み上がる。
    if (GameObject* rootBone =
            scene.GetGameObject(smr.nodeEntities[static_cast<size_t>(skeleton.rootNodeIndex)]))
        SetRootBoneLocalInAnimationSpace(*rootBone, skeleton.rootInverseTransform);

    bool poseChanged = false;
    std::vector<LayerBonePose> poses;

    for (auto& layer : animator.layers) {
        if (!layer.enabled) continue;
        // Layer を追加した直後はステートも Slot も無い。空 Layer で Mask のロードや
        // Slot の更新まで行うと、まだ何も接続していない編集操作が再生経路へ副作用を
        // 持ち込むため、定義と一時再生の両方が空なら評価を完全に省略する。
        if (layer.states.empty() && !layer.slot.active) continue;
        EnsureMaskLoaded(layer.mask);

        // Slot はレイヤー weight が 0 でも時間を進める。
        // WHY: weight を 0 にして一時的に黙らせている間も、割り込みモーションの
        //      再生位置とフェードは進んでいてほしい (復帰時に途中から鳴る)。
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
        // ステートマシン出力は Slot に押しのけられる分だけ弱める。
        AccumulateLayerClips(animator, layer, skeleton, owner, smr, stateClips,
                             1.0f - slotWeight, additiveReference, poses);

        if (slotClip && slotWeight > math::EPSILON) {
            const double tps = slotClip->ticksPerSecond > 0.0 ? slotClip->ticksPerSecond : 30.0;
            std::vector<WeightedClip> slotClips;
            // 割り込みクリップも Base と同じルートモーション設定でポーズを扱う。
            // WHY: Base だけルート成分を抜き、Slot は抜かないと、Slot が乗った瞬間に
            //      キャラクターがルートごと飛ぶ。
            slotClips.push_back(WeightedClip{
                slotClip, nullptr, static_cast<double>(layer.slot.time) * tps, 1.0f, 1.0f,
                ResolveRootMotion(animator, *slotClip), animator.speed < 0.0f });
            AccumulateLayerClips(animator, layer, skeleton, owner, smr, slotClips,
                                 slotWeight, additiveReference, poses);
        }

        // 積んだポーズを、レイヤー weight × ボーン weight で実際の Transform へ適用する。
        for (const auto& pose : poses) {
            if (!pose.target || pose.accumWeight <= math::EPSILON) continue;
            // Override は補間係数なので 1.0 で頭打ち。Additive は差分の倍率なので上限を
            // 残さない (Quaternion::Slerp は t > 1 を大円上へ正しく外挿する)。
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
                                skeleton.rootNodeIndex, owner.transform, visited);
        RebuildSkinningFromBoneTransforms(scene, owner, skeleton, smr, animator);
    }
}

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

    // クリップ未設定でも後段の Layer / IK が参照できるよう、スケルトンの配列と
    // ボーン階層は早期 return より前に必ず準備する。
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
    EnsureBoneHierarchy(scene, go, smr, skeleton);

    // 評価できるクリップが無いフレームは移動量ゼロを公開する。
    // WHY: 前フレームの delta が残ると、ExtractOnly の Script が止まった値で動き続ける。
    if (!curSt) {
        ApplyBindPoseToBones(scene, skeleton, smr, go.transform);
        UploadBindPose(animator, resources, &skeleton);
        ProcessRootMotion(animator, go, {}, dt);
        return;
    }

    auto currentClips = BuildStateClips(animator, *curSt, animator.stateTime);
    if (currentClips.empty()) {
        ApplyBindPoseToBones(scene, skeleton, smr, go.transform);
        UploadBindPose(animator, resources, &skeleton);
        ProcessRootMotion(animator, go, {}, dt);
        return;
    }

    animator.currentBlendDuration = GetStateDuration(animator, *curSt);
    bool exposeBlendWeights = curSt->mode != AnimationStateMode::Clip;

    // Clip / BlendTree を共通の加重クリップ集合として評価する。

    if (!animator.blendToState.empty()) {
        // ── クロスフェードモード ─────────────────────────────────────────
        const AnimationState* nextSt = FindState(animator, animator.blendToState);
        // Clip 同士の遷移も含め、最終姿勢への実寄与率を Editor へ公開する。
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
            // 遷移先クリップが見つからなければ現クリップ単独で続ける
            animator.blendToState.clear();
            animator.blendWeight = 0.0f;
        }
    } else {
        // ── 単一クリップモード ───────────────────────────────────────────
        // 遷移していない場合は currentClips をそのまま評価する。
    }

    // デバッグ API には、クロスフェードを含む最終姿勢への実寄与率を公開する。
    // WHY: 遷移元・遷移先で同じクリップを使う場合は、別項目ではなく合算値が必要になる。
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

    // ルートモーションはポーズ評価より前に確定・適用する。
    // 遷移中は currentClips に遷移元・遷移先の両方が weight 付きで入っているため、
    // 移動量も同じ比率で混ざる (旧実装は支配クリップ 1 本しか見ていなかった)。
    ProcessRootMotion(animator, go, currentClips, dt);

    std::vector<uint8_t> evaluationVisited(skeleton.nodes.size(), 0);
    EvaluateNBlendedNodeRecursive(
        skeleton, currentClips, skeleton.rootNodeIndex,
        math::Matrix4::Identity(),
        animator.boneMatrices, animator.nodeGlobalTransforms, evaluationVisited);
    // Base Layer マスク: 未設定なら nullptr を渡し、従来どおり全ボーンへ適用する。
    EnsureMaskLoaded(animator.baseLayerMask);
    ApplyNBlendedPoseToBones(scene, skeleton, currentClips, smr,
                             animator.baseLayerMask.loaded ? &animator.baseLayerMask.asset : nullptr);

    std::vector<uint8_t> propagationVisited(skeleton.nodes.size(), 0);
    PropagateBoneTransforms(scene, skeleton, smr, skeleton.rootInverseTransform,
                            skeleton.rootNodeIndex, go.transform, propagationVisited);
    RebuildSkinningFromBoneTransforms(scene, go, skeleton, smr, animator);
}

ComponentAccess AnimatorSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<AnimatorComponent>()
        .Writes<AnimatorComponent, BoneComponent>();
}

OrderingHints AnimatorSystem::GetOrder() const
{
    return OrderingHints{}.After<TransformLateUpdate>();
}

void AnimatorSystem::Update(SystemContext& ctx)
{
    // WHY: このファイルには計測スコープが 1 つも無かったため、アニメーション評価の
    //      コストが Profiler のどこにも現れず、フレーム時間の未帰属分に紛れていた。
    //      スキンドメッシュを出した瞬間に重くなる症状の切り分けに必要なので入れる。
    FBZZ_PROFILE_SCOPE("AnimatorSystem");
    if (!ctx.resources) return;
    Scene& scene = ctx.scene;
    renderer::ResourceManager& resources = *ctx.resources;
    const float dt = ctx.dt;
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
        animator->firedEvents.clear();

        if (!animator->controllerPath.empty() &&
            animator->loadedControllerPath != animator->controllerPath) {
            asset::AnimatorControllerAsset controller;
            if (asset::LoadAnimatorControllerAsset(animator->controllerPath, controller)) {
                asset::ApplyAnimatorControllerAsset(controller, *animator);
            }
            animator->loadedControllerPath = animator->controllerPath;
            animator->appliedAssetGeneration = asset::AssetManager::GetAssetGeneration();
        }

        // アセットがディスク上で書き換わったら、そこから作った派生キャッシュを捨てる。
        // loadedControllerPath は消さない。初回ロード扱いにすると preservePlayback が
        // 働かず、保存のたびに再生が頭へ巻き戻る。
        if (const int assetGeneration = asset::AssetManager::GetAssetGeneration();
            animator->appliedAssetGeneration != assetGeneration) {
            animator->appliedAssetGeneration = assetGeneration;

            if (!animator->controllerPath.empty()) {
                asset::AnimatorControllerAsset controller;
                if (asset::LoadAnimatorControllerAsset(animator->controllerPath, controller))
                    asset::ApplyAnimatorControllerAsset(controller, *animator);
            }
            // クリップは animator へ複製されるため、ストア側を差し替えても届かない。
            animator->clipsLoaded = false;
            // .mask は AssetManager を通さず直読みしているので、ここで明示的に落とす。
            animator->baseLayerMask.Invalidate();
            for (auto& layer : animator->layers)
                layer.mask.Invalidate();
        }

        // FlushFailed() が呼ばれて世代が進んだときだけ再試行する。
        // WHY: clips.empty() だけを条件にすると毎フレーム WARN スパムが発生する。
        //      世代番号で「FlushFailed() 以降に未試行」の場合のみ再試行を許可する。
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

        // これから boneMatrices を上書きするので、その直前の値が「前フレームの最終ポーズ」。
        // IKSystem と SpringBoneSystem の補正も含んだ確定値がここで手に入る。
        SnapshotPreviousBonePalette(*animator, resources);

        // SMR が自 GO になければ子 GO を探す (sub-mesh 分割ヒエラルキー対応)。
        auto* smr = go.GetComponent<SkinnedMeshRenderer>();
        if (!smr) {
            for (int ci = 0, cn = go.GetChildCount(); ci < cn; ++ci) {
                if (auto* child = go.GetChild(ci)) {
                    if (auto* s = child->GetComponent<SkinnedMeshRenderer>()) { smr = s; break; }
                }
            }
        }
        if (smr && !smr->model && !smr->modelPath.empty())
            smr->model = asset::AssetManager::LoadModel(smr->modelPath);

        const asset::Skeleton* skeleton = nullptr;
        if (smr && smr->model && smr->model->skeleton)
            skeleton = smr->model->skeleton.get();

        // ルートモーションの AutoDetect が使うスケルトンルート名を焼いておく。
        // WHY: トラック解決は Skeleton を持たない BuildStateClips からも呼ばれる。
        animator->skeletonRootNodeName.clear();
        if (skeleton && skeleton->rootNodeIndex >= 0 &&
            static_cast<size_t>(skeleton->rootNodeIndex) < skeleton->nodes.size()) {
            animator->skeletonRootNodeName =
                skeleton->nodes[static_cast<size_t>(skeleton->rootNodeIndex)].name;
        }

        // ── 再生は Play Mode のみ ──────────────────────────────────────────
        // 停止中にクリップが進むと、シーンビューのポーズが「最後に流れたフレーム」で
        // 固定されて編集の基準にならない。停止中はバインドポーズで静止させる
        // (モーション確認は Inspector の Animation Preview の役目)。
        // セットアップ (クリップ読み込み・スキニングバッファ・EnsureBoneHierarchy) は通す。
        if (!ctx.simulating) {
            // ボーン GameObject 階層だけは停止中にも用意する。
            // WHY: ソケットの親付けや Inspector からのボーン選択は
            //      Play Mode に入る前から使えている必要がある。
            if (smr && skeleton && skeleton->rootNodeIndex >= 0 &&
                skeleton->rootNodeIndex < static_cast<int>(skeleton->nodes.size()))
            {
                EnsureBoneHierarchy(scene, go, *smr, *skeleton);
                ApplyBindPoseToBones(scene, *skeleton, *smr, go.transform);
            }
            UploadBindPose(*animator, resources, skeleton);
            // 停止中は移動量ゼロを公開する。前フレームの delta が残ると
            // Inspector の表示や Script のポーリングが止まった値を掴み続ける。
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
                // スケルトンを持たない Animator でもルートモーションは取り出せる。
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

        SkinningCB cb{};
        for (int i = 0; i < asset::MAX_SKINNING_BONES; ++i)
            cb.boneMatrices[i] = math::Matrix4::Identity();
        for (size_t i = 0; i < animator->boneMatrices.size(); ++i)
            cb.boneMatrices[i] = animator->boneMatrices[i];
        resources.Update(animator->skinningBuffer, &cb, sizeof(SkinningCB));
    }
}

} // namespace fbzz::scene
