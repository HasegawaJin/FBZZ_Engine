/// @file    BoneBakeRuntime.cpp
/// @brief   Generation-scoped shared bone bake cache and CPU compatibility publication.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#include <Engine/Scene/BoneBakeRuntime.hpp>
#include "BoneBakePrepared.hpp"
#include "AnimationWorkTrace.hpp"
#include "BonePoseSharing.hpp"
#include "BoneCbBatch.hpp"
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Core/Logger.hpp>
#include <Core/Util/FileSystem.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <tuple>

namespace fbzz::scene {
namespace {
struct BoneBakeCachedValue {
    asset::BoneBakeStatus status = asset::BoneBakeStatus::INVALID_DATA;
    std::shared_ptr<const asset::BakedBoneAnimation> bake;
};
/// @note AssetManager owns the pointers and bumps generation before reimport/reload; mutable runtime clips are excluded.
using BoneBakeLookupKey = std::tuple<const asset::Model*, const asset::AnimationClip*, uint32_t>;
using BoneBakeContentKey = std::tuple<uint64_t, uint64_t, uint32_t, uint32_t>;
std::map<BoneBakeLookupKey, BoneBakeCachedValue> s_boneBakeLookup;
std::map<BoneBakeContentKey, BoneBakeCachedValue> s_boneBakeContent;
std::map<std::pair<const asset::BakedBoneAnimation*, const asset::BakedBoneAnimation*>, float> s_boneBakeBlendRadii;
int s_boneBakeGeneration = -1;

std::string BoneBakeCachePath(const BoneBakeContentKey& key)
{
    const auto directory = asset::AssetManager::StreamCacheDir();
    if (directory.empty()) return {};
    return directory + "/BoneBakes/v" + std::to_string(asset::BakedBoneAnimation::FORMAT_VERSION) + "_"
        + std::to_string(std::get<0>(key)) + "_" + std::to_string(std::get<1>(key)) + "_"
        + std::to_string(std::get<2>(key)) + "_" + std::to_string(std::get<3>(key)) + ".bba";
}

bool LoadBoneBakeCache(const std::string& path, const asset::Skeleton& skeleton,
                       const asset::AnimationClip& clip, float rate, float radius,
                       asset::BakedBoneAnimation& output)
{
    if (path.empty()) return false;
    const auto file = util::FileSystem::PathFromUtf8(path);
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    /// @note Bound before FileSystem allocates the byte vector; invalid caches are rebuilt from the source asset.
    if (error || size > 257ull * 1024ull * 1024ull) return false;
    std::vector<uint8_t> bytes;
    if (!util::FileSystem::ReadBinary(file, bytes)
        || asset::DecodeBoneBake(std::as_bytes(std::span(bytes)), output) != asset::BoneBakeStatus::OK) return false;
    return std::bit_cast<uint32_t>(output.sampleRate) == std::bit_cast<uint32_t>(rate)
        && asset::ValidateBoneBakeSource(output, skeleton, clip, radius) == asset::BoneBakeStatus::OK;
}

void SaveBoneBakeCache(const std::string& path, const asset::BakedBoneAnimation& bake)
{
    if (path.empty()) return;
    std::vector<std::byte> bytes;
    if (asset::EncodeBoneBake(bake, bytes) != asset::BoneBakeStatus::OK) return;
    std::string payload(bytes.size(), '\0');
    for (size_t i = 0; i < bytes.size(); ++i) payload[i] = std::bit_cast<char>(bytes[i]);
    /// @note The existing atomic writer preserves exact bytes (including NUL) and retires its own temporary file.
    if (!util::FileSystem::WriteTextAtomic(path, payload))
        FBZZ_LOG_WARN("BoneBake: unable to save derived cache '%s'; using in-memory bake", path.c_str());
}

BoneBakeCachedValue GetCachedBoneBake(const asset::Model& model, const asset::AnimationClip& clip, float rate, int generation)
{
    if (s_boneBakeGeneration != generation) { s_boneBakeBlendRadii.clear(); s_boneBakeLookup.clear(); s_boneBakeContent.clear(); s_boneBakeGeneration = generation; }
    const BoneBakeLookupKey lookup{&model, &clip, std::bit_cast<uint32_t>(rate)};
    if (const auto it = s_boneBakeLookup.find(lookup); it != s_boneBakeLookup.end()) return it->second;
    /// @note Asset models are immutable within their import generation; remember rejected meshes as well as successful bakes.
    const auto reject = [&]() { const BoneBakeCachedValue invalid; s_boneBakeLookup.emplace(lookup, invalid); return invalid; };
    float radius = 0.0f;
    for (const auto& mesh : model.meshes) {
        if (!mesh || mesh->cpuSkinnedVertices.empty()) return reject();
        for (const auto& v : mesh->cpuSkinnedVertices) {
            const float length = v.position.Length();
            if (!std::isfinite(length)) return reject();
            radius = (std::max)(radius, length);
            for (uint32_t bone : v.boneIndices)
                if (bone >= model.skeleton->bones.size()) return reject();
            float sum = 0.0f;
            for (float weight : v.boneWeights) {
                if (!std::isfinite(weight) || weight < 0.0f) return reject();
                sum += weight;
            }
            if (std::abs(sum - 1.0f) > 0.00001f) return reject();
        }
    }
    const BoneBakeContentKey key{asset::BoneBakeSkeletonFingerprint(*model.skeleton), asset::BoneBakeClipFingerprint(clip),
        std::bit_cast<uint32_t>(rate), std::bit_cast<uint32_t>(radius)};
    BoneBakeCachedValue result;
    if (const auto it = s_boneBakeContent.find(key); it != s_boneBakeContent.end()) result = it->second;
    else {
        asset::BakedBoneAnimation bake;
        const auto cache = BoneBakeCachePath(key);
        const bool loaded = LoadBoneBakeCache(cache, *model.skeleton, clip, rate, radius, bake);
        result.status = loaded ? asset::BoneBakeStatus::OK : asset::BakeBoneAnimation(*model.skeleton, clip, {rate, radius}, bake);
        if (result.status == asset::BoneBakeStatus::OK) {
            if (!loaded) SaveBoneBakeCache(cache, bake);
            result.bake = std::make_shared<const asset::BakedBoneAnimation>(std::move(bake));
        }
        else FBZZ_LOG_WARN("BoneBake: clip '%s' rejected, status=%d; continuing ordinary Animator", clip.name.c_str(), static_cast<int>(result.status));
        s_boneBakeContent.emplace(key, result);
    }
    s_boneBakeLookup.emplace(lookup, result); return result;
}
}

const char* BoneBakeFallbackName(BoneBakeFallback reason)
{
    switch (reason) {
    case BoneBakeFallback::NONE: return "None";
    case BoneBakeFallback::DISABLED: return "Legacy mode";
    case BoneBakeFallback::NEAR_CAMERA: return "Near camera or no camera";
    case BoneBakeFallback::CPU_BONES_REQUIRED: return "CPU bone transforms required";
    case BoneBakeFallback::RIG_CONTROL: return "IK, spring bone, external pose or motion warp";
    case BoneBakeFallback::ROOT_MOTION: return "Root motion or stripping";
    case BoneBakeFallback::LAYERS_OR_MASK: return "Layers or mask";
    case BoneBakeFallback::MORPH: return "Morph weights";
    case BoneBakeFallback::MUTABLE_CLIP: return "Mutable runtime clip";
    case BoneBakeFallback::CLIP_COUNT: return "Unsupported blend or clip weights";
    case BoneBakeFallback::INVALID_SOURCE: return "Invalid or unsupported bake source";
    case BoneBakeFallback::INVALID_CLOCK: return "Invalid sample clock";
    case BoneBakeFallback::INCOMPATIBLE_LOD: return "LOD skeleton or vertex bounds mismatch; CPU palette used";
    case BoneBakeFallback::GPU_RESOURCE: return "GPU resource unavailable; CPU palette used";
    case BoneBakeFallback::GPU_DISPATCH: return "GPU replay failed; CPU palette used";
    }
    return "Unknown";
}

bool AreBoneBakeAssetsPrepared(const AnimatorComponent& animator, const asset::Model& model)
{
    if (!model.skeleton || model.meshes.empty() || !animator.clips.empty() || animator.sharedClips.empty()
        || s_boneBakeGeneration != asset::AssetManager::GetAssetGeneration()) return false;
    for (const auto& clip : animator.Clips()) {
        const BoneBakeLookupKey key{&model, &clip, std::bit_cast<uint32_t>(animator.boneBakeSampleRate)};
        if (s_boneBakeLookup.find(key) == s_boneBakeLookup.end()) return false;
    }
    return true;
}

void PrepareBoneBakeAssets(AnimatorComponent& animator, const asset::Model& model)
{
    if (!model.skeleton || animator.sharedClips.empty() || model.meshes.empty()
        || (animator.boneBakeMode != static_cast<int>(BoneBakeMode::CPU_BAKED)
            && animator.boneBakeMode != static_cast<int>(BoneBakeMode::GPU_SHARED)
            && animator.boneBakeMode != static_cast<int>(BoneBakeMode::GPU_POSE_SHARED)) || animator.boneBakeRequireCpuBones) return;
    const int generation = asset::AssetManager::GetAssetGeneration();
    for (const auto& clip : animator.Clips())
        GetCachedBoneBake(model, clip, animator.boneBakeSampleRate, generation);
}

bool CanHoldBoneBake(const AnimatorComponent& animator, const asset::Model& model,
                     std::span<const BoneBakeClipInput> clips)
{
    const auto& runtime = animator.boneBake;
    const int generation = asset::AssetManager::GetAssetGeneration();
    if (!runtime.active || runtime.fallback != BoneBakeFallback::NONE || !runtime.source
        || runtime.bakeStatus != asset::BoneBakeStatus::OK || runtime.generation != generation
        || s_boneBakeGeneration != generation || !model.skeleton || model.meshes.empty()
        || animator.sharedClips.empty() || clips.empty() || clips.size() > 2
        || std::bit_cast<uint32_t>(runtime.sampleRate) != std::bit_cast<uint32_t>(animator.boneBakeSampleRate)) return false;
    float sum = 0.0f;
    for (size_t index = 0; index < clips.size(); ++index) {
        const auto& input = clips[index];
        if (!input.clip || !std::isfinite(input.weight) || input.weight <= 0.0f) return false;
        sum += input.weight;
        bool shared = false;
        for (const auto& clip : animator.Clips()) shared |= &clip == input.clip;
        if (!shared) return false;
        const BoneBakeLookupKey key{&model, input.clip, std::bit_cast<uint32_t>(animator.boneBakeSampleRate)};
        const auto cached = s_boneBakeLookup.find(key);
        const auto& published = index == 0 ? runtime.source : runtime.target;
        if (cached == s_boneBakeLookup.end() || !published || cached->second.bake != published) return false;
        asset::BakedBoneFrame frame;
        if (asset::ResolveBakedBoneFrame(*published, input.ticks / input.clip->ticksPerSecond,
                1.0, 0.0, false, frame) != asset::BoneBakeStatus::OK) return false;
    }
    return std::isfinite(sum) && std::abs(sum - 1.0f) <= 0.0001f
        && (clips.size() == 2 || !runtime.target);
}

bool MaterializeBoneBakeCpu(AnimatorComponent& animator, renderer::ResourceManager& resources, bool current)
{
    auto& runtime = animator.boneBake;
    bool valid = true;
    const bool previousHistoryValid = animator.prevBoneMatricesValid;
    std::vector<math::Matrix4> previousMatrices, currentMatrices, globals;
    bool publishPrevious = false, publishCurrent = false;
    if (runtime.previousCpuDeferred && runtime.previousPoseValid && runtime.previous.source) {
        const auto& previous = runtime.previous;
        valid = asset::EvaluateBakedBoneAnimation(*previous.source, previous.frame, previousMatrices, globals,
            previous.target.get(), previous.target ? &previous.targetFrame : nullptr, previous.blendWeight, &runtime.workspace)
            == asset::BoneBakeStatus::OK;
        animator.prevBoneMatricesValid = previousHistoryValid && valid;
        if (valid) { animator.prevBoneMatrices = previousMatrices; publishPrevious = true; }
    }
    if (current && runtime.cpuDeferred && runtime.active && runtime.source) {
        const bool evaluated = asset::EvaluateBakedBoneAnimation(*runtime.source, runtime.frame,
            currentMatrices, globals, runtime.target.get(),
            runtime.target ? &runtime.targetFrame : nullptr, runtime.blendWeight, &runtime.workspace)
            == asset::BoneBakeStatus::OK;
        valid &= evaluated;
        if (evaluated) { animator.boneMatrices = currentMatrices; animator.nodeGlobalTransforms = globals; publishCurrent = true; }
    }
    runtime.previousCpuDeferred = false;
    if (current) runtime.cpuDeferred = false;
    if (!valid) animator.prevBoneMatricesValid = false;
    const auto currentBuffer = animator.skinningBuffer, previousBuffer = animator.prevSkinningBuffer;
    /// @note All component publication precedes resource calls; local packets remain valid if the executor pumps a Scene edit.
    if (publishCurrent) BoneCbBatch::UpdatePalette(resources, currentBuffer, currentMatrices);
    if (publishPrevious) BoneCbBatch::UpdatePalette(resources, previousBuffer, previousMatrices);
    return valid;
}

bool TryEvaluateBoneBake(AnimatorComponent& animator, const asset::Model& model,
                         std::span<const BoneBakeClipInput> clips)
{
    auto& runtime = animator.boneBake;
    runtime.active = false;
    runtime.fallback = BoneBakeFallback::INVALID_SOURCE;
    if (!model.skeleton || model.meshes.empty()) return false;
    if (animator.sharedClips.empty()) { runtime.fallback = BoneBakeFallback::MUTABLE_CLIP; return false; }
    if (clips.empty() || clips.size() > 2) { runtime.fallback = BoneBakeFallback::CLIP_COUNT; return false; }
    float sum = 0.0f;
    for (const auto& input : clips) {
        if (!input.clip || !std::isfinite(input.weight) || input.weight <= 0.0f) { runtime.fallback = BoneBakeFallback::CLIP_COUNT; return false; }
        sum += input.weight;
        bool shared = false;
        for (const auto& clip : animator.Clips()) shared |= &clip == input.clip;
        if (!shared) { runtime.fallback = BoneBakeFallback::MUTABLE_CLIP; return false; }
    }
    if (!std::isfinite(sum) || std::abs(sum - 1.0f) > 0.0001f) { runtime.fallback = BoneBakeFallback::CLIP_COUNT; return false; }
    const int generation = asset::AssetManager::GetAssetGeneration();
    const auto source = GetCachedBoneBake(model, *clips[0].clip, animator.boneBakeSampleRate, generation);
    runtime.bakeStatus = source.status;
    if (!source.bake) return false;
    BoneBakeCachedValue target;
    if (clips.size() == 2) {
        target = GetCachedBoneBake(model, *clips[1].clip, animator.boneBakeSampleRate, generation);
        runtime.bakeStatus = target.status;
        if (!target.bake) return false;
    }
    asset::BakedBoneFrame frame, targetFrame;
    /// @note State machine already wraps/clamps and applies speeds; do not apply clip.loop or animator.speed a second time.
    if (asset::ResolveBakedBoneFrame(*source.bake, clips[0].ticks / clips[0].clip->ticksPerSecond, 1.0, 0.0, false, frame) != asset::BoneBakeStatus::OK
        || (target.bake && asset::ResolveBakedBoneFrame(*target.bake, clips[1].ticks / clips[1].clip->ticksPerSecond, 1.0, 0.0, false, targetFrame) != asset::BoneBakeStatus::OK)) {
        runtime.fallback = BoneBakeFallback::INVALID_CLOCK; return false;
    }
    const float weight = target.bake ? clips[1].weight / sum : 0.0f;
    if (runtime.source && (runtime.generation != generation
        || runtime.source->skeletonFingerprint != source.bake->skeletonFingerprint)) animator.prevBoneMatricesValid = false;
    const bool deferCpu = animator.boneBakeMode == static_cast<int>(BoneBakeMode::GPU_POSE_SHARED)
        && !animator.boneBakeRequireCpuBones && runtime.gpuReady && runtime.previousPoseValid
        && runtime.previous.source && runtime.generation == generation
        && runtime.previous.source->skeletonFingerprint == source.bake->skeletonFingerprint
        && animator.boneMatrices.size() == source.bake->bones.size();
    asset::BoneBakeStatus status = asset::BoneBakeStatus::OK;
    if (!deferCpu) {
    AnimationWorkScope bakedWork(AnimationWorkPart::BAKED_FK);
    thread_local BonePoseSharing sharing;
    bool cacheHit = false;
    status = sharing.Evaluate(Time::frameCount,generation,AnimationPoseShareRate(),source.bake,frame,target.bake,targetFrame,
                              weight,animator.boneMatrices,animator.nodeGlobalTransforms,runtime.workspace,cacheHit);
    if (auto* row = CurrentAnimationWorkRow()) {
        if (cacheHit) ++row->poseCacheHits; else ++row->poseCacheMisses;
    }
    }
    runtime.bakeStatus = status;
    if (status != asset::BoneBakeStatus::OK) return false;
    float radius = source.bake->conservativeRadius;
    if (target.bake) {
        const auto key = std::pair{source.bake.get(), target.bake.get()};
        if (const auto it = s_boneBakeBlendRadii.find(key); it != s_boneBakeBlendRadii.end()) radius = it->second;
        else {
            if (asset::BoneBakeBlendRadius(*source.bake, *target.bake, radius) != asset::BoneBakeStatus::OK) return false;
            s_boneBakeBlendRadii.emplace(key, radius);
        }
    }
    runtime.source = source.bake; runtime.target = target.bake;
    runtime.frame = frame; runtime.targetFrame = targetFrame; runtime.blendWeight = weight;
    runtime.radius = radius; runtime.generation = generation; runtime.sampleRate = animator.boneBakeSampleRate;
    runtime.active = true; runtime.cpuDeferred = deferCpu; runtime.fallback = BoneBakeFallback::NONE;
    animator.skinnedBoundsCenter = math::Vector3::ZERO; animator.skinnedBoundsRadius = radius;
    return true;
}
}
