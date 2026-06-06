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
#include <Engine/Asset/Model.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cmath>
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
        tf.localPosition.x * parentTransform.worldScale.x,
        tf.localPosition.y * parentTransform.worldScale.y,
        tf.localPosition.z * parentTransform.worldScale.z
    };
    tf.rotation   = (parentTransform.rotation * tf.localRotation).Normalized();
    tf.position   = parentTransform.position + parentTransform.rotation * scaledLocal;
    tf.worldScale = {
        parentTransform.worldScale.x * tf.localScale.x,
        parentTransform.worldScale.y * tf.localScale.y,
        parentTransform.worldScale.z * tf.localScale.z
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
    boneObject.transform.localPosition = node.bindTranslation;
    boneObject.transform.localRotation = node.bindRotation;
    boneObject.transform.localScale = node.bindScale;
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
        boneObject->transform.localPosition = pose.translation;
        boneObject->transform.localRotation = pose.rotation;
        boneObject->transform.localScale = pose.scale;
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
    for (const auto& src : animator.clipSources) {
        if (src.empty()) continue;
        auto model = asset::AssetManager::Load<asset::Model>(src);
        if (!model) {
            FBZZ_LOG_WARN("AnimatorSystem: clip source '%s' failed to load", src.c_str());
            continue;
        }
        for (const auto& clip : model->clips)
            animator.clips.push_back(clip);
    }
    animator.clipsLoaded = true;
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

// ステートに対応するクリップを返す。
// clipName 名前検索 → clipIndex 直接指定 の順でフォールバックする。
// WHY: Mixamo 等は FBX 内クリップ名を "mixamo.com" にするため名前検索が失敗する。
//      clipIndex を明示することで任意の FBX でも確実に動作させる。
const asset::AnimationClip* FindClipForState(const AnimatorComponent& animator,
                                             const AnimationState& state)
{
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

// 2クリップをブレンドした NodeLocalPose をノード階層に再帰適用し、
// boneMatrices と nodeGlobalTransforms を構築する。
// WHY: Matrix4 を直接 lerp すると回転の精度が落ちるため、
//      TRS 分解済みの NodeLocalPose レベルで Lerp/Slerp してから Matrix4 に変換する。
void EvaluateBlendedNodeRecursive(const asset::Skeleton& skeleton,
                                  const asset::AnimationClip& clipA, double ticksA,
                                  const asset::AnimationClip& clipB, double ticksB,
                                  float weight,
                                  int nodeIndex,
                                  const math::Matrix4& parentGlobal,
                                  std::vector<math::Matrix4>& palette,
                                  std::vector<math::Matrix4>& nodeGlobals)
{
    const auto& node = skeleton.nodes[static_cast<size_t>(nodeIndex)];

    const NodeLocalPose poseA = SampleNodeLocalPose(node, clipA, ticksA);
    const NodeLocalPose poseB = SampleNodeLocalPose(node, clipB, ticksB);

    NodeLocalPose blended;
    blended.translation = math::Vector3::Lerp(poseA.translation, poseB.translation, weight);
    blended.rotation    = math::Quaternion::Slerp(poseA.rotation, poseB.rotation, weight);
    blended.scale       = math::Vector3::Lerp(poseA.scale, poseB.scale, weight);

    const math::Matrix4 local  = math::Matrix4::TRS(blended.translation,
                                                     blended.rotation,
                                                     blended.scale);
    const math::Matrix4 global = parentGlobal * local;

    if (nodeIndex < static_cast<int>(nodeGlobals.size()))
        nodeGlobals[static_cast<size_t>(nodeIndex)] = global;

    if (node.boneIndex >= 0 && node.boneIndex < static_cast<int>(palette.size())) {
        const auto& bone = skeleton.bones[static_cast<size_t>(node.boneIndex)];
        palette[static_cast<size_t>(node.boneIndex)] =
            skeleton.rootInverseTransform * global * bone.offsetMatrix;
    }

    for (int child : node.children)
        EvaluateBlendedNodeRecursive(skeleton, clipA, ticksA, clipB, ticksB, weight,
                                     child, global, palette, nodeGlobals);
}

// ApplyAnimatedPoseToBones のブレンド版。
// 各 BoneComponent GameObject のローカル TRS に2クリップのブレンドポーズを書き込む。
void ApplyBlendedPoseToBones(Scene& scene,
                             const asset::Skeleton& skeleton,
                             const asset::AnimationClip& clipA, double ticksA,
                             const asset::AnimationClip& clipB, double ticksB,
                             float weight,
                             SkinnedMeshRenderer& smr)
{
    for (size_t i = 0; i < skeleton.nodes.size(); ++i) {
        GameObject* boneObject = scene.GetGameObject(smr.nodeEntities[i]);
        if (!boneObject) continue;

        const NodeLocalPose poseA = SampleNodeLocalPose(skeleton.nodes[i], clipA, ticksA);
        const NodeLocalPose poseB = SampleNodeLocalPose(skeleton.nodes[i], clipB, ticksB);

        boneObject->transform.localPosition =
            math::Vector3::Lerp(poseA.translation, poseB.translation, weight);
        boneObject->transform.localRotation =
            math::Quaternion::Slerp(poseA.rotation, poseB.rotation, weight);
        boneObject->transform.localScale =
            math::Vector3::Lerp(poseA.scale, poseB.scale, weight);
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

// ステートマシンを1フレーム分更新する。
// 遷移中のブレンド進行 → stateTime 進行 → 遷移条件チェックの順で処理する。
void UpdateStateMachine(AnimatorComponent& animator, float dt)
{
    // ── ブレンド進行 ───────────────────────────────────────────────────────
    if (!animator.blendToState.empty()) {
        animator.blendWeight += dt / std::max(animator.blendDuration, 1e-4f);

        // 遷移先ステートの時刻も進める
        const AnimationState* nextSt = FindState(animator, animator.blendToState);
        if (nextSt) {
            const auto* nextClip = FindClipForState(animator, *nextSt);
            if (nextClip) {
                const double tps = nextClip->ticksPerSecond > 0.0 ? nextClip->ticksPerSecond : 30.0;
                const float dur  = static_cast<float>(nextClip->durationTicks / tps);
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

    const auto* curClip = FindClipForState(animator, *curSt);
    float duration = 0.0f;
    if (curClip) {
        const double tps = curClip->ticksPerSecond > 0.0 ? curClip->ticksPerSecond : 30.0;
        duration = static_cast<float>(curClip->durationTicks / tps);
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

    for (const auto& tr : curSt->transitions) {
        if (tr.toStateName.empty()) continue;
        if (!FindState(animator, tr.toStateName)) continue;
        if (!EvaluateTransition(tr, animator, normalizedTime)) continue;

        // 遷移開始
        animator.blendToState    = tr.toStateName;
        animator.blendToTime     = 0.0f;
        animator.blendWeight     = 0.0f;
        animator.blendDuration   = tr.transitionDuration;
        ConsumeTriggers(animator, tr);
        break;
    }
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

    const size_t boneCount = std::min(skeleton.bones.size(),
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
    UpdateStateMachine(animator, dt);

    const AnimationState* curSt = FindState(animator, animator.currentStateName);
    if (!curSt) {
        UploadBindPose(animator, resources);
        return;
    }

    const auto* curClip = FindClipForState(animator, *curSt);
    if (!curClip) {
        UploadBindPose(animator, resources);
        return;
    }

    const size_t boneCount = std::min(skeleton.bones.size(),
                                      static_cast<size_t>(asset::MAX_SKINNING_BONES));
    animator.boneMatrices.assign(boneCount, math::Matrix4::Identity());
    animator.nodeGlobalTransforms.assign(skeleton.nodes.size(), math::Matrix4::Identity());

    if (skeleton.rootNodeIndex < 0 || skeleton.nodes.empty()) return;

    EnsureBoneHierarchy(scene, go, smr, skeleton);

    const double tpsA = curClip->ticksPerSecond > 0.0 ? curClip->ticksPerSecond : 30.0;
    const double ticksA = static_cast<double>(animator.stateTime) * tpsA;

    if (!animator.blendToState.empty()) {
        // ── クロスフェードモード ─────────────────────────────────────────
        const AnimationState* nextSt = FindState(animator, animator.blendToState);
        const auto* nextClip = nextSt ? FindClipForState(animator, *nextSt) : nullptr;

        if (nextClip) {
            const double tpsB  = nextClip->ticksPerSecond > 0.0 ? nextClip->ticksPerSecond : 30.0;
            const double ticksB = static_cast<double>(animator.blendToTime) * tpsB;
            const float w = std::clamp(animator.blendWeight, 0.0f, 1.0f);

            EvaluateBlendedNodeRecursive(skeleton,
                                         *curClip,  ticksA,
                                         *nextClip, ticksB,
                                         w,
                                         skeleton.rootNodeIndex,
                                         math::Matrix4::Identity(),
                                         animator.boneMatrices,
                                         animator.nodeGlobalTransforms);

            ApplyBlendedPoseToBones(scene, skeleton,
                                    *curClip, ticksA,
                                    *nextClip, ticksB,
                                    w, smr);
        } else {
            // 遷移先クリップが見つからなければ現クリップ単独で続ける
            EvaluateNode(skeleton, *curClip, skeleton.rootNodeIndex,
                         math::Matrix4::Identity(), ticksA,
                         animator.boneMatrices, animator.nodeGlobalTransforms);
            ApplyAnimatedPoseToBones(scene, skeleton, *curClip, ticksA, smr);
        }
    } else {
        // ── 単一クリップモード ───────────────────────────────────────────
        EvaluateNode(skeleton, *curClip, skeleton.rootNodeIndex,
                     math::Matrix4::Identity(), ticksA,
                     animator.boneMatrices, animator.nodeGlobalTransforms);
        ApplyAnimatedPoseToBones(scene, skeleton, *curClip, ticksA, smr);
    }

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

        // clipsLoaded でも clips が空かつ clipSources がある場合は再試行する。
        // WHY: インポート前に LoadClips が失敗しても clipsLoaded=true になるため、
        //      FlushFailed() 後のキャッシュクリアに追従できるよう再試行が必要。
        if (!animator->clipsLoaded ||
            (animator->clips.empty() && !animator->clipSources.empty()))
            LoadClips(*animator);

        if (!animator->skinningBuffer.IsValid())
            animator->skinningBuffer = resources.CreateConstantBuffer(sizeof(SkinningCB));

        auto* smr = go.GetComponent<SkinnedMeshRenderer>();
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
