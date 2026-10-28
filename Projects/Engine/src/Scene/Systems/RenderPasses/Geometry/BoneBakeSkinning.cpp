/// @file    BoneBakeSkinning.cpp
/// @brief   Species-shared resident local TRS replay connected to existing skinning batches.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#include "BoneBakeSkinning.hpp"
#include <Engine/Scene/BoneBakeRuntime.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model.hpp>
#include <Graphics/Effects/BakedBoneReplay.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

namespace fbzz::scene {
namespace {
using BoneBakeSkinPairKey = std::pair<const asset::BakedBoneAnimation*, const asset::BakedBoneAnimation*>;
struct BoneBakeSkinPairAsset {
    std::shared_ptr<const asset::BakedBoneAnimation> source, target;
    std::unique_ptr<renderer::BoneReplayGpuAsset> gpu;
    uint32_t targetOffset = 0;
};
std::map<BoneBakeSkinPairKey, std::shared_ptr<BoneBakeSkinPairAsset>> s_boneBakeSkinAssets;
std::map<std::pair<const asset::Model*, const asset::BakedBoneAnimation*>, bool> s_boneBakeLodCompatibility;
uint64_t s_boneBakeSkinOwner = 0, s_boneBakeSkinEpoch = 0;
int s_boneBakeSkinGeneration = -1;
uint64_t s_boneBakeResidentBytes = 0;
constexpr uint64_t BONE_BAKE_GPU_CACHE_BUDGET = 256ull * 1024ull * 1024ull;

bool BoneBakeLodCompatible(const BoneBakeSkinRequest& request)
{
    if (!request.model || !request.model->skeleton) return false;
    const auto& source = *request.runtime->source;
    const auto key = std::pair{request.model, &source};
    if (const auto it = s_boneBakeLodCompatibility.find(key); it != s_boneBakeLodCompatibility.end()) return it->second;
    bool compatible = asset::BoneBakeSkeletonFingerprint(*request.model->skeleton) == source.skeletonFingerprint;
    for (const auto& mesh : request.model->meshes) {
        if (!mesh || mesh->cpuSkinnedVertices.empty()) { compatible = false; break; }
        for (const auto& vertex : mesh->cpuSkinnedVertices) {
            const auto radius = vertex.position.Length();
            if (!std::isfinite(radius) || radius > source.sourceMeshRadius) compatible = false;
            for (uint32_t bone : vertex.boneIndices) if (bone >= source.bones.size()) compatible = false;
            float sum = 0.0f;
            for (float weight : vertex.boneWeights) {
                if (!std::isfinite(weight) || weight < 0.0f) compatible = false;
                sum += weight;
            }
            if (std::abs(sum - 1.0f) > 0.00001f) compatible = false;
        }
    }
    s_boneBakeLodCompatibility.emplace(key, compatible); return compatible;
}

std::shared_ptr<BoneBakeSkinPairAsset> GetBoneBakeSkinAsset(RenderPassContext& ctx, const BoneBakePoseState& runtime)
{
    const BoneBakeSkinPairKey key{runtime.source.get(), runtime.target.get()};
    if (const auto it = s_boneBakeSkinAssets.find(key); it != s_boneBakeSkinAssets.end()) return it->second;
    if (!runtime.source || asset::ValidateBakedBoneAnimation(*runtime.source) != asset::BoneBakeStatus::OK
        || (runtime.target && asset::ValidateBakedBoneAnimation(*runtime.target) != asset::BoneBakeStatus::OK)) return nullptr;
    const int creationGeneration = asset::AssetManager::GetAssetGeneration();
    auto samples = runtime.source->samples;
    auto entry = std::make_shared<BoneBakeSkinPairAsset>(); entry->source = runtime.source; entry->target = runtime.target;
    if (runtime.target) {
        if (samples.size() > UINT32_MAX || runtime.target->samples.size() > UINT32_MAX - samples.size()) return nullptr;
        entry->targetOffset = static_cast<uint32_t>(samples.size());
        samples.insert(samples.end(), runtime.target->samples.begin(), runtime.target->samples.end());
    }
    const renderer::BoneReplaySource upload{
        std::as_bytes(std::span(samples)), std::as_bytes(std::span(runtime.source->nodes)), std::as_bytes(std::span(runtime.source->bones)),
        static_cast<uint32_t>(runtime.source->nodes.size()), static_cast<uint32_t>(runtime.source->bones.size()), runtime.source->rootInverse};
    const uint64_t bytes = upload.samples.size() + upload.nodes.size() + upload.bindings.size();
    /// @note A bounded clip-pair cache avoids unbounded GPU residency; budget overflow uses the existing CPU palette.
    if (bytes > BONE_BAKE_GPU_CACHE_BUDGET - s_boneBakeResidentBytes) return nullptr;
    renderer::BoneReplayStatus status;
    entry->gpu = renderer::UploadBakedBoneReplay(ctx.resources, upload, status);
    if (!entry->gpu) return nullptr;
    /// @note A local entry lease survives cache invalidation while native creation/replay pumps an editor callback.
    if (s_boneBakeSkinOwner != ctx.resources.GetInstanceId() || s_boneBakeSkinEpoch != ctx.resources.GetResetVersion()
        || s_boneBakeSkinGeneration != creationGeneration || creationGeneration != asset::AssetManager::GetAssetGeneration()
        || entry->gpu->owner != s_boneBakeSkinOwner || entry->gpu->epoch != s_boneBakeSkinEpoch) return {};
    if (const auto found = s_boneBakeSkinAssets.find(key); found != s_boneBakeSkinAssets.end()) return found->second;
    if (bytes > BONE_BAKE_GPU_CACHE_BUDGET - s_boneBakeResidentBytes) return {};
    s_boneBakeResidentBytes += entry->gpu->residentBytes;
    ctx.statsSkinningPaletteUploadBytes += entry->gpu->residentBytes;
    ctx.statsSkinningPaletteUploads += 3;
    s_boneBakeSkinAssets.emplace(key, entry); return entry;
}
renderer::BoneReplayRow MakeBoneBakeSkinRow(const BoneBakePoseState& runtime, const BoneBakeSkinPairAsset& asset)
{
    const uint32_t nodes = static_cast<uint32_t>(runtime.source->nodes.size());
    return {runtime.frame.first * nodes, runtime.frame.second * nodes, runtime.frame.alpha, 0,
        asset.targetOffset + runtime.targetFrame.first * nodes, asset.targetOffset + runtime.targetFrame.second * nodes,
        runtime.targetFrame.alpha, runtime.blendWeight};
}
}

void PrepareBakedSkinningRequests(RenderPassContext& ctx, std::span<const BoneBakeSkinRequest> requests)
{
    if (requests.empty()) return;
    if (!ctx.renderer.GetCapabilities().bindless) {
        for (const auto& request : requests)
            if (request.runtime) request.runtime->fallback = BoneBakeFallback::GPU_RESOURCE;
        return;
    }
    const int generation = asset::AssetManager::GetAssetGeneration();
    if (s_boneBakeSkinOwner != ctx.resources.GetInstanceId() || s_boneBakeSkinEpoch != ctx.resources.GetResetVersion() || s_boneBakeSkinGeneration != generation) {
        ReleaseBoneBakeSkinningCaches(); s_boneBakeSkinOwner = ctx.resources.GetInstanceId(); s_boneBakeSkinEpoch = ctx.resources.GetResetVersion(); s_boneBakeSkinGeneration = generation;
    }
    struct RuntimeWitness { EntityID entity; uint64_t lifetime; BoneBakePoseState pose; };
    std::map<BoneBakeRuntime*, RuntimeWitness> witnesses;
    const auto sceneGeneration = ctx.scene.GetRenderSceneGeneration();
    const auto owner = ctx.resources.GetInstanceId(), epoch = ctx.resources.GetResetVersion(), frame = ctx.resources.FrameStamp();
    const auto sameFrame = [](const auto& a, const auto& b) {
        return a.first == b.first && a.second == b.second && std::bit_cast<uint32_t>(a.alpha) == std::bit_cast<uint32_t>(b.alpha);
    };
    /// @note Native resource operations may pump edits; no component address is trusted across a resource call.
    const auto resolve = [&](BoneBakeRuntime* identity) -> BoneBakeRuntime* {
        const auto found = witnesses.find(identity);
        if (found == witnesses.end() || ctx.scene.GetRenderSceneGeneration() != sceneGeneration
            || ctx.resources.GetInstanceId() != owner || ctx.resources.GetResetVersion() != epoch || ctx.resources.FrameStamp() != frame
            || asset::AssetManager::GetAssetGeneration() != generation) return nullptr;
        const auto& witness = found->second;
        auto* animator = ctx.scene.GetComponent<AnimatorComponent>(witness.entity);
        if (!animator || ctx.scene.GetComponentLifetime<AnimatorComponent>(witness.entity) != witness.lifetime) return nullptr;
        auto& live = animator->boneBake;
        if (&live != identity || !live.active || live.generation != generation || live.source != witness.pose.source
            || live.target != witness.pose.target || !sameFrame(live.frame, witness.pose.frame)
            || !sameFrame(live.targetFrame, witness.pose.targetFrame)
            || std::bit_cast<uint32_t>(live.blendWeight) != std::bit_cast<uint32_t>(witness.pose.blendWeight)) return nullptr;
        return &live;
    };

    struct PoseInput { BoneBakeRuntime* runtime; bool previous; BoneBakePoseState pose; };
    std::map<BoneBakeSkinPairKey, std::vector<PoseInput>> groups;
    std::map<BoneBakeRuntime*, bool> eligible;
    for (const auto& request : requests) {
        if (!request.runtime || !request.runtime->active || !request.runtime->source
            || request.runtime->generation != generation) continue;
        witnesses.try_emplace(request.runtime, RuntimeWitness{request.animatorEntity, request.animatorLifetime,
            {request.runtime->source, request.runtime->target, request.runtime->frame, request.runtime->targetFrame, request.runtime->blendWeight}});
        const bool compatible = BoneBakeLodCompatible(request);
        const auto [entry, inserted] = eligible.emplace(request.runtime, compatible);
        if (!inserted) entry->second &= compatible;
    }
    for (const auto& [runtime, compatible] : eligible) {
        runtime->gpuPalette = {}; runtime->gpuPreviousPalette = {};
        runtime->gpuOwner = ctx.resources.GetInstanceId(); runtime->gpuEpoch = ctx.resources.GetResetVersion();
        runtime->gpuFrame = ctx.resources.FrameStamp();
        if (!compatible) {
            runtime->fallback = BoneBakeFallback::INCOMPATIBLE_LOD;
            continue;
        }
        const auto add = [&](bool previous, const auto& source, const auto& target,
                             const auto& frame, const auto& targetFrame, float blend) {
            PoseInput row; row.runtime = runtime; row.previous = previous;
            row.pose.source = source; row.pose.target = target; row.pose.frame = frame;
            row.pose.targetFrame = targetFrame; row.pose.blendWeight = blend;
            groups[{source.get(), target.get()}].push_back(std::move(row));
        };
        add(false, runtime->source, runtime->target, runtime->frame, runtime->targetFrame, runtime->blendWeight);
        if (runtime->previousPoseValid && runtime->previous.source
            && runtime->previous.source->skeletonFingerprint == runtime->source->skeletonFingerprint) {
            const auto& previous = runtime->previous;
            add(true, previous.source, previous.target, previous.frame, previous.targetFrame, previous.blendWeight);
        }
    }
    struct ReplayJob {
        std::shared_ptr<BoneBakeSkinPairAsset> asset;
        std::vector<PoseInput>* group = nullptr;
        std::vector<uint32_t> rowIndices;
        std::vector<renderer::BoneReplayRow> rows;
        renderer::BoneReplayResult result;
    };
    std::vector<ReplayJob> replayJobs;
    replayJobs.reserve(groups.size());
    for (auto& [key, group] : groups) {
        const auto asset = GetBoneBakeSkinAsset(ctx, group.front().pose);
        if (!asset) { for (auto& input : group) if (auto* live = resolve(input.runtime)) live->fallback = BoneBakeFallback::GPU_RESOURCE; continue; }
        std::map<std::array<uint32_t,8>,uint32_t> poseIndices;
        std::vector<uint32_t> rowIndices;
        std::vector<renderer::BoneReplayRow> rows;
        for (const auto& input : group) {
            const auto row = MakeBoneBakeSkinRow(input.pose, *asset);
            const std::array<uint32_t,8> poseKey{row.first,row.second,std::bit_cast<uint32_t>(row.alpha),row.padding,
                row.targetFirst,row.targetSecond,std::bit_cast<uint32_t>(row.targetAlpha),std::bit_cast<uint32_t>(row.blendWeight)};
            const auto [pose, inserted] = poseIndices.emplace(poseKey, static_cast<uint32_t>(rows.size()));
            rowIndices.push_back(pose->second);
            if (inserted) rows.push_back(row);
        }
        /// @note The 65535 row and 128 bone limits keep the float object offset exact as well as meeting Dispatch Y limits.
        replayJobs.push_back({asset, &group, std::move(rowIndices), std::move(rows), {}});
    }
    /// @note Replay requires the render thread. The callback contains only immutable GPU jobs; all Scene publication stays on the caller thread.
    if (!replayJobs.empty()) ctx.renderer.ExecuteResourceOperation([&] {
        for (auto& job : replayJobs) job.result = renderer::ReplayBakedBones(ctx, *job.asset->gpu, job.rows);
    });
    for (const auto& job : replayJobs) {
        auto& group = *job.group;
        const auto& asset = job.asset;
        const auto& rowIndices = job.rowIndices;
        const auto& replay = job.result;
        if (replay.status != renderer::BoneReplayStatus::OK) {
            for (auto& input : group) if (auto* live = resolve(input.runtime)) live->fallback = BoneBakeFallback::GPU_DISPATCH;
            continue;
        }
        ctx.statsSkinningPaletteUploadBytes += replay.rowUploadBytes;
        ++ctx.statsSkinningPaletteUploads; ctx.statsSkinningDispatchCount += replay.dispatchCount;
        for (size_t index = 0; index < group.size(); ++index) {
            auto& input = group[index];
            auto* live = resolve(input.runtime);
            if (!live) continue;
            auto& runtime = *live;
            const uint32_t offset = rowIndices[index] * asset->gpu->boneCount;
            if (input.previous) { runtime.gpuPreviousPalette = replay.palette; runtime.gpuPreviousOffset = offset; }
            else { runtime.gpuPalette = replay.palette; runtime.gpuOffset = offset; }
        }
    }
    for (const auto& [identity, compatible] : eligible) {
        auto* runtime = resolve(identity);
        if (!runtime) continue;
        runtime->gpuReady = compatible && runtime->gpuPalette.IsValid()
            && (!runtime->previousPoseValid || runtime->gpuPreviousPalette.IsValid());
        if (runtime->gpuReady || !runtime->cpuDeferred) continue;
        const auto owner = std::find_if(requests.begin(), requests.end(),
            [&](const auto& request) { return request.runtime == runtime; });
        if (owner != requests.end()) {
            if (auto* animator = ctx.scene.GetComponent<AnimatorComponent>(owner->animatorEntity))
                MaterializeBoneBakeCpu(*animator, ctx.resources, true);
        }
        runtime = resolve(runtime);
        if (!runtime) continue;
        runtime->gpuPalette = {}; runtime->gpuPreviousPalette = {};
        /// @note Packed CPU ranges were captured before replay; repair them before the existing batch consumes a fallback.
        for (const auto& request : requests) {
            if (request.runtime != runtime || request.requestIndex >= ctx.skinningRequests.size()) continue;
            const auto* animator = ctx.scene.GetComponent<AnimatorComponent>(request.animatorEntity);
            if (!animator || ctx.scene.GetComponentLifetime<AnimatorComponent>(request.animatorEntity) != request.animatorLifetime) continue;
            const auto& input = ctx.skinningRequests[request.requestIndex];
            const auto& matrices = animator->boneMatrices;
            if (input.boneCount == matrices.size() && input.boneOffset <= ctx.skinningMatrices.size()
                && matrices.size() <= ctx.skinningMatrices.size() - input.boneOffset)
                std::copy(matrices.begin(), matrices.end(), ctx.skinningMatrices.begin() + input.boneOffset);
        }
    }
    for (const auto& request : requests) {
        const auto* runtime = resolve(request.runtime);
        if (!runtime || !runtime->gpuPalette.IsValid() || request.requestIndex >= ctx.skinningRequests.size()) continue;
        auto& input = ctx.skinningRequests[request.requestIndex];
        input.bonePalette = runtime->gpuPalette; input.boneOffset = runtime->gpuOffset;
        input.boneCount = static_cast<uint32_t>(runtime->source->bones.size()); input.gpuBonePalette = true;
    }

    std::map<std::pair<uint32_t, uint32_t>, uint32_t> offsets;
    std::vector<std::pair<size_t, uint32_t>> rewrites;
    std::vector<math::Matrix4> remaining;
    for (size_t index = 0; index < ctx.skinningRequests.size(); ++index) {
        const auto& input = ctx.skinningRequests[index];
        if (input.gpuBonePalette || !input.boneCount) continue;
        const uint32_t old = input.boneOffset;
        /// @note Keep every CPU offset unchanged if compaction cannot validate the complete input set.
        if (old > ctx.skinningMatrices.size() || input.boneCount > ctx.skinningMatrices.size() - old) return;
        const auto range = std::pair{old, input.boneCount};
        if (const auto it = offsets.find(range); it != offsets.end()) rewrites.emplace_back(index, it->second);
        else {
            if (remaining.size() > UINT32_MAX - input.boneCount) return;
            const auto offset = static_cast<uint32_t>(remaining.size()); offsets.emplace(range, offset);
            rewrites.emplace_back(index, offset);
            remaining.insert(remaining.end(), ctx.skinningMatrices.begin() + old, ctx.skinningMatrices.begin() + old + input.boneCount);
        }
    }
    for (const auto& [index, offset] : rewrites) ctx.skinningRequests[index].boneOffset = offset;
    ctx.skinningMatrices = std::move(remaining);
}

void ReleaseBoneBakeSkinningCaches()
{
    /// @note Publish an empty cache before resource retirement can enter the executor.
    auto retiredAssets = std::move(s_boneBakeSkinAssets);
    s_boneBakeSkinAssets.clear(); s_boneBakeLodCompatibility.clear(); s_boneBakeResidentBytes = 0;
    s_boneBakeSkinOwner = s_boneBakeSkinEpoch = 0; s_boneBakeSkinGeneration = -1;
}
}
