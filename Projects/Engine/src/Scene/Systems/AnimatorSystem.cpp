// FBZZ Engine
// AnimatorSystem.cpp | fbzz::scene
// スケルタルアニメーションのサンプリングと骨行列転送
// AnimationClip を評価し、SkinnedMeshRenderer 用の行列パレットを更新する。
// 描画自体は RenderSystem が担当する。
// スケルトン取得元: SkinnedMeshRenderer.model->skeleton
// アニメーションクリップ取得元: AnimatorComponent.clipSources (1 つの参照元につき 1 つの FBX)
#include <Engine/Scene/Systems/AnimatorSystem.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {

namespace {

struct SkinningCB {
    math::Matrix4 boneMatrices[asset::MAX_SKINNING_BONES];
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
    return nullptr;
}

math::Matrix4 SampleNodeLocal(const asset::SkeletonNode& node,
                              const asset::AnimationClip& clip,
                              double ticks)
{
    const auto* track = FindTrack(clip, node.name);
    if (!track) return node.localBindTransform;

    math::Vector3 bindTranslation = {
        node.localBindTransform.m[0][3],
        node.localBindTransform.m[1][3],
        node.localBindTransform.m[2][3]
    };
    math::Vector3 bindScale = {
        math::Vector3{ node.localBindTransform.m[0][0], node.localBindTransform.m[1][0], node.localBindTransform.m[2][0] }.Length(),
        math::Vector3{ node.localBindTransform.m[0][1], node.localBindTransform.m[1][1], node.localBindTransform.m[2][1] }.Length(),
        math::Vector3{ node.localBindTransform.m[0][2], node.localBindTransform.m[1][2], node.localBindTransform.m[2][2] }.Length()
    };
    math::Quaternion bindRotation = math::Quaternion::FromMatrix4(node.localBindTransform);

    const math::Vector3    translation = SampleVectorKeys(track->positions, ticks, bindTranslation);
    const math::Quaternion rotation    = SampleQuaternionKeys(track->rotations, ticks, bindRotation);
    const math::Vector3    scale       = SampleVectorKeys(track->scales, ticks, bindScale);
    return math::Matrix4::TRS(translation, rotation, scale);
}

void EvaluateNode(const asset::Skeleton& skeleton,
                  const asset::AnimationClip& clip,
                  int nodeIndex,
                  const math::Matrix4& parentGlobal,
                  double ticks,
                  std::vector<math::Matrix4>& palette,
                  std::vector<math::Matrix4>& nodeGlobals)
{
    const auto& node   = skeleton.nodes[static_cast<size_t>(nodeIndex)];
    const math::Matrix4 local  = SampleNodeLocal(node, clip, ticks);
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

// clipSources すべてからクリップを読み込み、animator.clips へ統合する
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

} // namespace

void AnimatorSystem(Scene& scene, renderer::ResourceManager& resources, float dt)
{
    for (auto& go : scene.GameObjects()) {
        auto* animator = go.GetComponent<AnimatorComponent>();
        if (!animator || !animator->enabled) continue;

        // clipSources から必要になった時点でクリップを読み込む
        if (!animator->clipsLoaded)
            LoadClips(*animator);

        // スキニング用定数バッファがなければ作成する
        if (!animator->skinningBuffer.IsValid())
            animator->skinningBuffer = resources.CreateConstantBuffer(sizeof(SkinningCB));

        // 同じ GameObject の SkinnedMeshRenderer から Skeleton を取得する
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

        const auto* clip = ResolveClip(*animator);
        if (!clip) {
            UploadBindPose(*animator, resources);
            continue;
        }

        const double ticksPerSecond  = clip->ticksPerSecond > 0.0 ? clip->ticksPerSecond : 30.0;
        const float  durationSeconds = static_cast<float>(clip->durationTicks / ticksPerSecond);
        if (animator->playing) {
            animator->time += dt * animator->speed;
            animator->time = animator->loop
                ? WrapTime(animator->time, durationSeconds)
                : std::clamp(animator->time, 0.0f, durationSeconds);
        }

        const size_t boneCount = std::min(skeleton->bones.size(),
                                          static_cast<size_t>(asset::MAX_SKINNING_BONES));
        animator->boneMatrices.assign(boneCount, math::Matrix4::Identity());
        animator->nodeGlobalTransforms.assign(skeleton->nodes.size(), math::Matrix4::Identity());

        if (skeleton->rootNodeIndex >= 0 && !skeleton->nodes.empty()) {
            EvaluateNode(*skeleton, *clip, skeleton->rootNodeIndex, math::Matrix4::Identity(),
                         static_cast<double>(animator->time) * ticksPerSecond,
                         animator->boneMatrices, animator->nodeGlobalTransforms);
        }

        SkinningCB cb{};
        for (int i = 0; i < asset::MAX_SKINNING_BONES; ++i)
            cb.boneMatrices[i] = math::Matrix4::Identity();
        for (size_t i = 0; i < animator->boneMatrices.size(); ++i)
            cb.boneMatrices[i] = animator->boneMatrices[i];
        resources.Update(animator->skinningBuffer, &cb, sizeof(SkinningCB));
    }
}

} // namespace fbzz::scene
