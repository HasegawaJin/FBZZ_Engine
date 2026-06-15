// FBZZ Engine
// AnimatorSystem.cpp | fbzz::scene
// スケルタルアニメーションのサンプリングとスキニングパレットのアップロード
#include <Engine/Scene/Systems/AnimatorSystem.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <string>
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

const asset::AnimationClip* ResolveClip(AnimatorComponent& animator)
{
    if (animator.clips.empty()) return nullptr;

    if (!animator.clipName.empty()) {
        for (const auto& clip : animator.clips)
            if (clip.name == animator.clipName)
                return &clip;
        FBZZ_LOG_WARN("AnimatorSystem: clip '%s' not found; falling back to clip index",
                      animator.clipName.c_str());
    }

    int index = animator.clipIndex;
    if (index < 0 || index >= static_cast<int>(animator.clips.size())) {
        FBZZ_LOG_WARN("AnimatorSystem: clip index %d out of range; using 0", index);
        index = 0;
        animator.clipIndex = 0;
    }
    return &animator.clips[static_cast<size_t>(index)];
}

math::Vector3 SampleVectorKeys(const std::vector<asset::VectorKey>& keys,
                               double ticks,
                               const math::Vector3& fallback)
{
    if (keys.empty()) return fallback;
    if (keys.size() == 1 || ticks <= keys.front().time) return keys.front().value;
    if (ticks >= keys.back().time) return keys.back().value;

    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        const auto& a = keys[i];
        const auto& b = keys[i + 1];
        if (ticks < a.time || ticks > b.time) continue;
        const double span = b.time - a.time;
        const float t = span > 0.0 ? static_cast<float>((ticks - a.time) / span) : 0.0f;
        return math::Vector3::Lerp(a.value, b.value, t);
    }
    return keys.back().value;
}

math::Quaternion SampleQuaternionKeys(const std::vector<asset::QuaternionKey>& keys,
                                      double ticks,
                                      const math::Quaternion& fallback)
{
    if (keys.empty()) return fallback;
    if (keys.size() == 1 || ticks <= keys.front().time) return keys.front().value;
    if (ticks >= keys.back().time) return keys.back().value;

    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        const auto& a = keys[i];
        const auto& b = keys[i + 1];
        if (ticks < a.time || ticks > b.time) continue;
        const double span = b.time - a.time;
        const float t = span > 0.0 ? static_cast<float>((ticks - a.time) / span) : 0.0f;
        return math::Quaternion::Slerp(a.value, b.value, t);
    }
    return keys.back().value;
}

const asset::NodeAnimationTrack* FindTrack(const asset::AnimationClip& clip,
                                           const std::string& nodeName)
{
    for (const auto& track : clip.tracks)
        if (track.nodeName == nodeName)
            return &track;

    // FBX は DCC / exporter の設定により、同じボーンでも
    //   mixamorig:RightFoot
    //   RightFoot
    //   mixamorig:RightFoot_$AssimpFbx$_PreRotation
    // のようにチャンネル名が揺れることがある。
    // exact match を優先した上で、補助ノード suffix と namespace 差だけを吸収する。
    auto canonical = [](std::string name) {
        std::replace(name.begin(), name.end(), '\\', '/');

        const std::string helper = "_$AssimpFbx$_";
        const size_t helperPos = name.find(helper);
        if (helperPos != std::string::npos)
            name = name.substr(0, helperPos);

        const size_t pathPos = name.find_last_of("/|");
        if (pathPos != std::string::npos)
            name = name.substr(pathPos + 1);

        const size_t namespacePos = name.find_last_of(':');
        if (namespacePos != std::string::npos)
            name = name.substr(namespacePos + 1);

        return name;
    };

    const std::string canonicalNodeName = canonical(nodeName);
    for (const auto& track : clip.tracks)
        if (canonical(track.nodeName) == canonicalNodeName)
            return &track;

    return nullptr;
}

math::Matrix4 SampleNodeLocal(const asset::SkeletonNode& node,
                              const asset::AnimationClip& clip,
                              double ticks)
{
    const auto* track = FindTrack(clip, node.name);
    if (!track) return node.localBindTransform;

    const math::Vector3 translation = SampleVectorKeys(track->positions, ticks, node.bindTranslation);
    const math::Quaternion rotation = SampleQuaternionKeys(track->rotations, ticks, node.bindRotation);
    const math::Vector3 scale = SampleVectorKeys(track->scales, ticks, node.bindScale);
    return math::Matrix4::TRS(translation, rotation, scale);
}

NodeLocalPose SampleNodeLocalPose(const asset::SkeletonNode& node,
                                  const asset::AnimationClip& clip,
                                  double ticks)
{
    const auto* track = FindTrack(clip, node.name);
    if (!track) {
        return {
            node.bindTranslation,
            node.bindRotation,
            node.bindScale
        };
    }

    return {
        SampleVectorKeys(track->positions, ticks, node.bindTranslation),
        SampleQuaternionKeys(track->rotations, ticks, node.bindRotation),
        SampleVectorKeys(track->scales, ticks, node.bindScale)
    };
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

GameObject& EnsureBoneObject(Scene& scene,
                             GameObject& owner,
                             SkinnedMeshRenderer& smr,
                             const asset::Skeleton& skeleton,
                             int nodeIndex)
{
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

    if (GameObject* found = FindBoneDescendant(owner, nodeIndex)) {
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
    if (node.parentIndex >= 0)
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
                              SkinnedMeshRenderer& smr)
{
    for (size_t i = 0; i < skeleton.nodes.size(); ++i) {
        GameObject* boneObject = scene.GetGameObject(smr.nodeEntities[i]);
        if (!boneObject) continue;

        const NodeLocalPose pose = SampleNodeLocalPose(skeleton.nodes[i], clip, ticks);
        boneObject->transform.position = pose.translation;
        boneObject->transform.rotation = pose.rotation;
        boneObject->transform.scale = pose.scale;
    }
}

void PropagateBoneTransforms(Scene& scene,
                             const asset::Skeleton& skeleton,
                             SkinnedMeshRenderer& smr,
                             int nodeIndex,
                             const Transform& parentTransform)
{
    GameObject* boneObject = scene.GetGameObject(smr.nodeEntities[static_cast<size_t>(nodeIndex)]);
    if (!boneObject) return;

    UpdateWorldTransform(*boneObject, parentTransform);

    for (int child : skeleton.nodes[static_cast<size_t>(nodeIndex)].children)
        PropagateBoneTransforms(scene, skeleton, smr, child, boneObject->transform);
}

void RebuildSkinningFromBoneTransforms(Scene& scene,
                                       GameObject& owner,
                                       const asset::Skeleton& skeleton,
                                       SkinnedMeshRenderer& smr,
                                       AnimatorComponent& animator)
{
    const math::Matrix4 ownerInverse = math::Matrix4::Inverse(owner.transform.GetWorldMatrix());

    for (size_t nodeIndex = 0; nodeIndex < skeleton.nodes.size(); ++nodeIndex) {
        GameObject* boneObject = scene.GetGameObject(smr.nodeEntities[nodeIndex]);
        if (!boneObject) continue;

        animator.nodeGlobalTransforms[nodeIndex] =
            ownerInverse * boneObject->transform.GetWorldMatrix();
    }

    for (size_t boneIndex = 0; boneIndex < animator.boneMatrices.size(); ++boneIndex) {
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
                  std::vector<math::Matrix4>& nodeGlobals)
{
    const auto& node = skeleton.nodes[static_cast<size_t>(nodeIndex)];
    const math::Matrix4 local = SampleNodeLocal(node, clip, ticks);
    const math::Matrix4 global = parentGlobal * local;

    if (nodeIndex < static_cast<int>(nodeGlobals.size()))
        nodeGlobals[static_cast<size_t>(nodeIndex)] = global;

    if (node.boneIndex >= 0 && node.boneIndex < static_cast<int>(palette.size())) {
        const auto& bone = skeleton.bones[static_cast<size_t>(node.boneIndex)];
        palette[static_cast<size_t>(node.boneIndex)] =
            skeleton.rootInverseTransform * global * bone.offsetMatrix;
    }

    for (int child : node.children)
        EvaluateNode(skeleton, clip, child, global, ticks, palette, nodeGlobals);
}

void UploadBindPose(AnimatorComponent& animator, renderer::ResourceManager& resources)
{
    SkinningCB cb{};
    for (int i = 0; i < asset::MAX_SKINNING_BONES; ++i)
        cb.boneMatrices[i] = math::Matrix4::Identity();
    resources.Update(animator.skinningBuffer, &cb, sizeof(SkinningCB));
}

void LoadClips(AnimatorComponent& animator)
{
    animator.clips.clear();
    animator.clipSourcePaths.clear();

    // Node が直接参照する Source を収集する。旧データの clipSources は互換用に併合する。
    std::vector<std::string> sources = animator.clipSources;
    auto addSource = [&sources](const std::string& sourcePath) {
        if (sourcePath.empty()) return;
        if (std::find(sources.begin(), sources.end(), sourcePath) == sources.end())
            sources.push_back(sourcePath);
    };
    for (const auto& state : animator.states) {
        addSource(state.sourcePath);
        for (const auto& motion : state.blendTree1D.motions) addSource(motion.sourcePath);
        for (const auto& motion : state.blendTree2D.motions) addSource(motion.sourcePath);
    }

    for (const auto& src : sources) {
        if (src.empty()) continue;
        auto model = asset::AssetManager::Load<asset::Model>(src);
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

// animator.clips からクリップ名で探す。
// 完全一致 → 大文字小文字無視の含有一致 の順でフォールバックする。
// WHY: FBX エクスポーターによっては "Walk" → "Armature|Walk" のように
//      オブジェクト名がプレフィックスとして付くため、完全一致だけでは取得できない。
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

// ステートに対応するクリップを返す。
// clipName 名前検索 → clipIndex 直接指定 の順でフォールバックする。
// WHY: Mixamo 等は FBX 内クリップ名を "mixamo.com" にするため名前検索が失敗する。
//      clipIndex を明示することで任意の FBX でも確実に動作させる。
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

    float px = animator.GetFloat(tree.paramX);
    float py = animator.GetFloat(tree.paramY);
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
    if (state.mode == AnimationStateMode::Clip) {
        if (const auto* clip = FindClipForState(animator, state)) {
            const double tps = clip->ticksPerSecond > 0.0 ? clip->ticksPerSecond : 30.0;
            result.push_back({
                clip, nullptr, static_cast<double>(stateTime) * tps, 1.0f, state.ikWeight
            });
        }
        return result;
    }

    for (const auto& weighted : ComputeStateWeights(animator, state)) {
        const auto* clip = FindClipForMotion(animator, *weighted.motion);
        if (!clip) continue;
        const double tps = clip->ticksPerSecond > 0.0 ? clip->ticksPerSecond : 30.0;
        const float duration = static_cast<float>(clip->durationTicks / tps);
        float motionTime = stateTime * weighted.motion->speed;
        motionTime = state.loop
            ? WrapTime(motionTime, duration)
            : std::clamp(motionTime, 0.0f, duration);
        result.push_back({
            clip,
            weighted.motion,
            static_cast<double>(motionTime) * tps,
            weighted.weight,
            // State を全体係数、Motion をクリップ固有係数として扱う。
            // WHY: BlendTree 全体を一括調整しつつ、Run だけ足IKを弱められるようにする。
            state.ikWeight * weighted.motion->ikWeight
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
        const double tps = clip->ticksPerSecond > 0.0 ? clip->ticksPerSecond : 30.0;
        return static_cast<float>(clip->durationTicks / tps);
    }

    // BlendTree の再生周期は現在 Weight に依存させず、全 Motion の最大実効 Length で固定する。
    // WHY: Damping 中は Weight が毎フレーム変わる。加重平均 Length を WrapTime に使うと、
    //      周期が途中で短くなった瞬間に stateTime / blendToTime が巻き戻り、Walk が再生し直されるため。
    float duration = 0.0f;
    const auto accumulateMotionDuration = [&](const BlendTreeMotion& motion) {
        const auto* clip = FindClipForMotion(animator, motion);
        if (!clip) return;
        const double tps = clip->ticksPerSecond > 0.0 ? clip->ticksPerSecond : 30.0;
        const float speed = (std::max)(std::abs(motion.speed), 1e-4f);
        duration = (std::max)(
            duration,
            static_cast<float>(clip->durationTicks / tps) / speed);
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
        const NodeLocalPose pose = SampleNodeLocalPose(node, *weighted.clip, weighted.ticks);
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
                                   std::vector<math::Matrix4>& nodeGlobals)
{
    const auto& node = skeleton.nodes[static_cast<size_t>(nodeIndex)];
    const NodeLocalPose blended = BlendNodePose(node, clips);
    const math::Matrix4 global =
        parentGlobal * math::Matrix4::TRS(
            blended.translation, blended.rotation, blended.scale);

    if (nodeIndex < static_cast<int>(nodeGlobals.size()))
        nodeGlobals[static_cast<size_t>(nodeIndex)] = global;
    if (node.boneIndex >= 0 && node.boneIndex < static_cast<int>(palette.size())) {
        const auto& bone = skeleton.bones[static_cast<size_t>(node.boneIndex)];
        palette[static_cast<size_t>(node.boneIndex)] =
            skeleton.rootInverseTransform * global * bone.offsetMatrix;
    }
    for (int child : node.children)
        EvaluateNBlendedNodeRecursive(
            skeleton, clips, child, global, palette, nodeGlobals);
}

void ApplyNBlendedPoseToBones(Scene& scene,
                              const asset::Skeleton& skeleton,
                              const std::vector<WeightedClip>& clips,
                              SkinnedMeshRenderer& smr)
{
    for (size_t i = 0; i < skeleton.nodes.size(); ++i) {
        GameObject* boneObject = scene.GetGameObject(smr.nodeEntities[i]);
        if (!boneObject) continue;
        const NodeLocalPose pose = BlendNodePose(skeleton.nodes[i], clips);
        boneObject->transform.position = pose.translation;
        boneObject->transform.rotation = pose.rotation;
        boneObject->transform.scale = pose.scale;
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
void InitStateMachine(AnimatorComponent& animator)
{
    if (!animator.currentStateName.empty()) return;
    if (animator.states.empty()) return;

    if (!animator.defaultStateName.empty() &&
        FindState(animator, animator.defaultStateName))
        animator.currentStateName = animator.defaultStateName;
    else
        animator.currentStateName = animator.states[0].name;

    animator.stateTime   = 0.0f;
    animator.blendToState = "";
    animator.blendWeight = 0.0f;
}

bool TryStartTransition(AnimatorComponent& animator,
                        const std::vector<AnimationTransition>& transitions,
                        float normalizedTime)
{
    for (const auto& tr : transitions) {
        if (tr.toStateName.empty()) continue;
        // 現在ステート自身へ戻る遷移は開始しない。
        // WHY: Fall 条件のような継続条件で毎フレーム自己遷移すると、再生時刻が0へ戻り続けるため。
        if (tr.toStateName == animator.currentStateName) continue;
        if (!FindState(animator, tr.toStateName)) continue;
        if (!EvaluateTransition(tr, animator, normalizedTime)) continue;

        animator.blendToState  = tr.toStateName;
        animator.blendToTime   = 0.0f;
        animator.blendWeight   = 0.0f;
        // 正規化指定は遷移元ステートの Length を基準に実秒へ変換する。
        // WHY: クリップを差し替えても同じ割合の Motion Blend を維持できる。
        const AnimationState* currentState =
            FindState(animator, animator.currentStateName);
        const float sourceDuration = currentState
            ? GetStateDuration(animator, *currentState)
            : 0.0f;
        animator.blendDuration = tr.fixedDuration
            ? tr.transitionDuration
            : tr.transitionDuration * sourceDuration;
        animator.blendDuration = (std::max)(animator.blendDuration, 0.0f);
        ConsumeTriggers(animator, tr);
        return true;
    }
    return false;
}

// 1D BlendTree の入力値を時定数ベースで平滑化する。
// WHY: Script が Speed を 0 / 4 / 7.2 と離散的に設定しても、姿勢 Weight は連続変化させる。
void UpdateBlendTree1DDamping(AnimatorComponent& animator, float dt)
{
    if (!animator.playing) return;
    for (auto& state : animator.states) {
        if (state.mode != AnimationStateMode::BlendTree1D) continue;
        auto& tree = state.blendTree1D;
        const float target = animator.GetFloat(tree.paramName);
        if (!tree.dampedValueInitialized || tree.dampTime <= math::EPSILON) {
            tree.dampedValue = target;
            tree.dampedValueInitialized = true;
            continue;
        }
        const float alpha =
            1.0f - std::exp(
                -(std::max)(dt, 0.0f) / (std::max)(tree.dampTime, 1e-4f));
        tree.dampedValue += (target - tree.dampedValue) * std::clamp(alpha, 0.0f, 1.0f);
        // 指数補間は理論上目標へ到達しないため、近傍で確定して不要な2Clip評価を終了する。
        // WHY: 極小WeightのClipも全ボーンをサンプリングすると、定常時のCPU負荷が倍増する。
        const float snapEpsilon =
            (std::max)(0.001f, std::abs(target) * 0.001f);
        if (std::abs(target - tree.dampedValue) <= snapEpsilon)
            tree.dampedValue = target;
    }
}

// ステートマシンを1フレーム分更新する。
// 遷移中のブレンド進行 → stateTime 進行 → 遷移条件チェックの順で処理する。
void UpdateStateMachine(AnimatorComponent& animator, float dt)
{
    // ── ブレンド進行 ───────────────────────────────────────────────────────
    if (!animator.blendToState.empty()) {
        if (!animator.playing) return;

        animator.blendWeight += dt / (std::max)(animator.blendDuration, 1e-4f);

        // クロスフェード中も遷移元のポーズを進め、静止ポーズへのフェードを防ぐ。
        const AnimationState* currentSt =
            FindState(animator, animator.currentStateName);
        if (currentSt) {
            const float dur = GetStateDuration(animator, *currentSt);
            if (dur > 0.0f) {
                animator.stateTime += dt * currentSt->speed * animator.speed;
                animator.stateTime = currentSt->loop
                    ? WrapTime(animator.stateTime, dur)
                    : std::clamp(animator.stateTime, 0.0f, dur);
            }
        }

        // 遷移先ステートの時刻も進める
        const AnimationState* nextSt = FindState(animator, animator.blendToState);
        if (nextSt) {
            const float dur = GetStateDuration(animator, *nextSt);
            if (dur > 0.0f) {
                animator.blendToTime += dt * nextSt->speed * animator.speed;
                if (nextSt->loop)
                    animator.blendToTime = WrapTime(animator.blendToTime, dur);
                else
                    animator.blendToTime = std::clamp(animator.blendToTime, 0.0f, dur);
            }
        }

        if (animator.blendWeight >= 1.0f) {
            // 遷移完了: 現ステートを次ステートに切り替える
            animator.currentStateName = animator.blendToState;
            animator.stateTime        = animator.blendToTime;
            animator.blendToState     = "";
            animator.blendWeight      = 0.0f;
        }
        // 遷移中は新たな遷移チェックをしない
        return;
    }

    // ── 現ステートの時刻進行 ──────────────────────────────────────────────
    const AnimationState* curSt = FindState(animator, animator.currentStateName);
    if (!curSt) return;

    const float duration = GetStateDuration(animator, *curSt);
    if (duration > 0.0f) {
        if (animator.playing) {
            animator.stateTime += dt * curSt->speed * animator.speed;
            if (curSt->loop)
                animator.stateTime = WrapTime(animator.stateTime, duration);
            else
                animator.stateTime = std::clamp(animator.stateTime, 0.0f, duration);
        }
    }

    // ── 遷移条件チェック ──────────────────────────────────────────────────
    const float normalizedTime = (duration > 0.0f)
        ? std::clamp(animator.stateTime / duration, 0.0f, 1.0f)
        : 0.0f;

    // 通常遷移を優先し、成立しなかった場合だけ AnyState を評価する。
    if (!TryStartTransition(animator, curSt->transitions, normalizedTime))
        TryStartTransition(animator, animator.anyStateTransitions, normalizedTime);
}

} // namespace

// ── 後方互換パス（states が空のとき）────────────────────────────────────────
static void RunLegacyAnimatorPath(AnimatorComponent& animator,
                                  const asset::Skeleton& skeleton,
                                  Scene& scene,
                                  GameObject& go,
                                  SkinnedMeshRenderer& smr,
                                  renderer::ResourceManager& resources,
                                  float dt)
{
    animator.currentBlendWeights.clear();
    animator.currentBlendDuration = 0.0f;

    const auto* clip = ResolveClip(animator);
    if (!clip) {
        UploadBindPose(animator, resources);
        return;
    }

    const double ticksPerSecond = clip->ticksPerSecond > 0.0 ? clip->ticksPerSecond : 30.0;
    const float durationSeconds = static_cast<float>(clip->durationTicks / ticksPerSecond);
    if (animator.playing) {
        animator.time += dt * animator.speed;
        animator.time = animator.loop
            ? WrapTime(animator.time, durationSeconds)
            : std::clamp(animator.time, 0.0f, durationSeconds);
    }

    const size_t boneCount = (std::min)(skeleton.bones.size(),
                                        static_cast<size_t>(asset::MAX_SKINNING_BONES));
    animator.boneMatrices.assign(boneCount, math::Matrix4::Identity());
    animator.nodeGlobalTransforms.assign(skeleton.nodes.size(), math::Matrix4::Identity());

    if (skeleton.rootNodeIndex >= 0 && !skeleton.nodes.empty()) {
        EnsureBoneHierarchy(scene, go, smr, skeleton);

        EvaluateNode(skeleton, *clip, skeleton.rootNodeIndex,
                     math::Matrix4::Identity(),
                     static_cast<double>(animator.time) * ticksPerSecond,
                     animator.boneMatrices, animator.nodeGlobalTransforms);

        const double ticks = static_cast<double>(animator.time) * ticksPerSecond;
        ApplyAnimatedPoseToBones(scene, skeleton, *clip, ticks, smr);
        PropagateBoneTransforms(scene, skeleton, smr, skeleton.rootNodeIndex, go.transform);
        RebuildSkinningFromBoneTransforms(scene, go, skeleton, smr, animator);
    }
}

// ── ステートマシンパス（states が存在するとき）──────────────────────────────
static void RunStateMachineAnimatorPath(AnimatorComponent& animator,
                                        const asset::Skeleton& skeleton,
                                        Scene& scene,
                                        GameObject& go,
                                        SkinnedMeshRenderer& smr,
                                        renderer::ResourceManager& resources,
                                        float dt)
{
    InitStateMachine(animator);
    UpdateBlendTree1DDamping(animator, dt);
    UpdateStateMachine(animator, dt);

    const AnimationState* curSt = FindState(animator, animator.currentStateName);
    animator.currentBlendWeights.clear();
    animator.currentBlendDuration = 0.0f;
    if (!curSt) {
        UploadBindPose(animator, resources);
        return;
    }

    auto currentClips = BuildStateClips(animator, *curSt, animator.stateTime);
    if (currentClips.empty()) {
        UploadBindPose(animator, resources);
        return;
    }

    animator.currentBlendDuration = GetStateDuration(animator, *curSt);
    bool exposeBlendWeights = curSt->mode != AnimationStateMode::Clip;

    const size_t boneCount = (std::min)(skeleton.bones.size(),
                                        static_cast<size_t>(asset::MAX_SKINNING_BONES));
    animator.boneMatrices.assign(boneCount, math::Matrix4::Identity());
    animator.nodeGlobalTransforms.assign(skeleton.nodes.size(), math::Matrix4::Identity());

    if (skeleton.rootNodeIndex < 0 || skeleton.nodes.empty()) return;

    EnsureBoneHierarchy(scene, go, smr, skeleton);

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

    EvaluateNBlendedNodeRecursive(
        skeleton, currentClips, skeleton.rootNodeIndex,
        math::Matrix4::Identity(),
        animator.boneMatrices, animator.nodeGlobalTransforms);
    ApplyNBlendedPoseToBones(scene, skeleton, currentClips, smr);

    PropagateBoneTransforms(scene, skeleton, smr, skeleton.rootNodeIndex, go.transform);
    RebuildSkinningFromBoneTransforms(scene, go, skeleton, smr, animator);
}

void AnimatorSystem(Scene& scene, renderer::ResourceManager& resources, float dt)
{
    const auto animatorSpan = scene.GetEntities<AnimatorComponent>();
    const auto animatorEntities = std::vector<EntityID>(
        animatorSpan.begin(),
        animatorSpan.end());

    for (EntityID id : animatorEntities) {
        GameObject* gameObject = scene.GetGameObject(id);
        if (!gameObject) continue;
        GameObject& go = *gameObject;

        auto* animator = go.GetComponent<AnimatorComponent>();
        if (!animator || !animator->enabled) continue;

        if (!animator->controllerPath.empty() &&
            animator->loadedControllerPath != animator->controllerPath) {
            asset::AnimatorControllerAsset controller;
            if (asset::LoadAnimatorControllerAsset(animator->controllerPath, controller)) {
                asset::ApplyAnimatorControllerAsset(controller, *animator);
            }
            animator->loadedControllerPath = animator->controllerPath;
        }

        // FlushFailed() が呼ばれて世代が進んだときだけ再試行する。
        // WHY: clips.empty() だけを条件にすると毎フレーム WARN スパムが発生する。
        //      世代番号で「FlushFailed() 以降に未試行」の場合のみ再試行を許可する。
        const bool needsRetry = !animator->clipsLoaded ||
            (animator->clips.empty() &&
             (!animator->clipSources.empty() || !animator->states.empty()) &&
             asset::AssetManager::GetFlushGeneration() > animator->clipsAttemptGeneration);
        if (needsRetry)
            LoadClips(*animator);

        if (!animator->skinningBuffer.IsValid())
            animator->skinningBuffer = resources.CreateConstantBuffer(sizeof(SkinningCB));

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
            smr->model = asset::AssetManager::Load<asset::Model>(smr->modelPath);

        const asset::Skeleton* skeleton = nullptr;
        if (smr && smr->model && smr->model->skeleton)
            skeleton = smr->model->skeleton.get();

        if (!skeleton) {
            UploadBindPose(*animator, resources);
            continue;
        }

        // states が空なら後方互換パス、存在すればステートマシンパス
        if (animator->states.empty())
            RunLegacyAnimatorPath(*animator, *skeleton, scene, go, *smr, resources, dt);
        else
            RunStateMachineAnimatorPath(*animator, *skeleton, scene, go, *smr, resources, dt);

        SkinningCB cb{};
        for (int i = 0; i < asset::MAX_SKINNING_BONES; ++i)
            cb.boneMatrices[i] = math::Matrix4::Identity();
        for (size_t i = 0; i < animator->boneMatrices.size(); ++i)
            cb.boneMatrices[i] = animator->boneMatrices[i];
        resources.Update(animator->skinningBuffer, &cb, sizeof(SkinningCB));
    }
}

} // namespace fbzz::scene
