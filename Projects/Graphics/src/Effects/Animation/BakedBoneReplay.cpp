/// @file    BakedBoneReplay.cpp
/// @brief   GPU-resident skeletal samples and independent palette replay using existing RHI pools.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include <Graphics/Effects/Animation/BakedBoneReplay.hpp>
#include <Graphics/Pipeline/Passes/RenderPassContext.hpp>
#include <Graphics/Renderer/Commands/ComputeCall.hpp>
#include <Graphics/Renderer/Resources/Buffers/DynamicBufferPool.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/Resources/Buffers/IStructuredBuffer.hpp>
#include <cmath>
#include <limits>
#include <mutex>

namespace fbzz::renderer {
namespace {
/// @note Cache retirement can run from a pumped main-thread edit while the render executor is replaying.
std::recursive_mutex s_boneReplayMutex;
DynamicRWStructuredBufferPool s_boneReplayPalettePool;
DynamicStructuredBufferPool s_boneReplayRowPool;
struct ReplayProgram {
    uint64_t owner = 0, epoch = 0;
    ResourceHandle<ShaderTag> shader;
    ResourceHandle<ConstantBufferTag> constants;
};
ReplayProgram s_boneReplayProgram;
struct ReplayConstants {
    math::Matrix4 rootInverse;
    uint32_t nodes, bones, samples, rows;
};
static_assert(sizeof(ReplayConstants) == 80);
bool BoneReplayValidBuffer(ResourceManager& resources, ResourceHandle<StructuredBufferTag> h, uint32_t count, uint32_t stride)
{
    const auto* b = resources.Get(h);
    return b && b->GetStride() == stride && b->GetElementCount() >= count && b->GetSize() / stride >= count;
}
bool BoneReplayValidOffset(uint32_t offset, uint32_t nodes, uint32_t samples)
{
    return samples >= nodes && offset <= samples - nodes && offset % nodes == 0;
}
}

BoneReplayGpuAsset::~BoneReplayGpuAsset()
{
    auto* resources = ResourceManager::FindOwner(owner);
    if (!resources || resources->GetInstanceId() != owner || resources->GetResetVersion() != epoch) {
        return;
    }
    if (samples.IsValid()) {
        resources->Release(samples);
    }
    if (nodes.IsValid()) {
        resources->Release(nodes);
    }
    if (bindings.IsValid()) {
        resources->Release(bindings);
    }
}

std::unique_ptr<BoneReplayGpuAsset> UploadBakedBoneReplay(ResourceManager& resources,
    const BoneReplaySource& source, BoneReplayStatus& status)
{
    status = BoneReplayStatus::INVALID_SOURCE;
    // サンプル数をノード数で割る前に、GPU の骨格上限内であることを確認する。
    if (!source.nodeCount || source.nodeCount > 1024 || !source.boneCount || source.boneCount > 128) {
        return {};
    }
    const size_t sampleCount = source.samples.size() / 48;
    const bool wholeFrames = !source.samples.empty() && source.samples.size() % 48 == 0
        && sampleCount <= UINT32_MAX && sampleCount % source.nodeCount == 0;
    if (!wholeFrames) {
        return {};
    }
    if (source.nodes.size() != static_cast<uint64_t>(source.nodeCount) * 16
        || source.bindings.size() != static_cast<uint64_t>(source.boneCount) * 80) {
        return {};
    }

    // 三つの表を受け渡す所有元と世代を記録し、すべてのアップロード後に照合する。
    auto asset = std::make_unique<BoneReplayGpuAsset>();
    asset->owner = resources.GetInstanceId(); asset->epoch = resources.GetResetVersion();
    asset->nodeCount = source.nodeCount; asset->boneCount = source.boneCount;
    asset->sampleCount = static_cast<uint32_t>(sampleCount); asset->rootInverse = source.rootInverse;
    asset->samples = resources.CreateGpuLocalStructuredBuffer(source.samples.data(), asset->sampleCount, 48);
    asset->nodes = resources.CreateGpuLocalStructuredBuffer(source.nodes.data(), asset->nodeCount, 16);
    asset->bindings = resources.CreateGpuLocalStructuredBuffer(source.bindings.data(), asset->boneCount, 80);
    const auto matchesResourceEpoch = [&] {
        return asset->owner == resources.GetInstanceId() && asset->epoch == resources.GetResetVersion();
    };
    const bool readyInSameEpoch = matchesResourceEpoch()
        && BoneReplayValidBuffer(resources, asset->samples, asset->sampleCount, 48)
        && BoneReplayValidBuffer(resources, asset->nodes, asset->nodeCount, 16)
        && BoneReplayValidBuffer(resources, asset->bindings, asset->boneCount, 80)
        && matchesResourceEpoch();
    if (!readyInSameEpoch) {
        // 生成中にリセットされると新世代の表も混ざり得るため、返されたハンドルを個別に解放する。
        resources.Release(asset->samples); resources.Release(asset->nodes); resources.Release(asset->bindings);
        asset->samples = {}; asset->nodes = {}; asset->bindings = {};
        status = BoneReplayStatus::RESOURCE_FAILED; return {};
    }
    asset->residentBytes = source.samples.size() + source.nodes.size() + source.bindings.size();
    status = BoneReplayStatus::OK; return asset;
}

BoneReplayResult ReplayBakedBones(RenderPassContext& ctx, const BoneReplayGpuAsset& asset,
                                 std::span<const BoneReplayRow> rows, ResourceHandle<ShaderTag> shaderOverride)
{
    std::lock_guard lock(s_boneReplayMutex);
    BoneReplayResult result;
    if (!ctx.renderer.GetCapabilities().bindless) {
        return result;
    }
    result.status = BoneReplayStatus::INVALID_SOURCE;
    if (asset.owner != ctx.resources.GetInstanceId() || asset.epoch != ctx.resources.GetResetVersion()) {
        return result;
    }
    if (!asset.nodeCount || asset.nodeCount > 1024 || !asset.boneCount || asset.boneCount > 128) {
        return result;
    }
    if (!BoneReplayValidBuffer(ctx.resources, asset.samples, asset.sampleCount, 48)
        || !BoneReplayValidBuffer(ctx.resources, asset.nodes, asset.nodeCount, 16)
        || !BoneReplayValidBuffer(ctx.resources, asset.bindings, asset.boneCount, 80)) {
        return result;
    }

    // 各行で参照するフレームと補間係数を検証してから、GPU の出力を用意する。
    result.status = BoneReplayStatus::INVALID_ROW;
    if (rows.empty() || rows.size() > 65535 || rows.size() > UINT32_MAX / asset.boneCount) {
        return result;
    }
    const auto isUnitWeight = [](float value) {
        return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
    };
    for (const auto& row : rows) {
        const bool validSourceFrames = BoneReplayValidOffset(row.first, asset.nodeCount, asset.sampleCount)
            && BoneReplayValidOffset(row.second, asset.nodeCount, asset.sampleCount);
        if (!validSourceFrames) {
            return result;
        }
        const bool validTargetFrames = BoneReplayValidOffset(row.targetFirst, asset.nodeCount, asset.sampleCount)
            && BoneReplayValidOffset(row.targetSecond, asset.nodeCount, asset.sampleCount);
        if (!validTargetFrames) {
            return result;
        }
        if (row.padding || !isUnitWeight(row.alpha) || !isUnitWeight(row.targetAlpha) || !isUnitWeight(row.blendWeight)) {
            return result;
        }
    }
    if (s_boneReplayProgram.owner != asset.owner || s_boneReplayProgram.epoch != asset.epoch) {
        s_boneReplayProgram = {}; s_boneReplayProgram.owner = asset.owner; s_boneReplayProgram.epoch = asset.epoch;
    }
    if (!shaderOverride.IsValid() && !ctx.resources.Get(s_boneReplayProgram.shader)) {
        s_boneReplayProgram.shader = ctx.resources.LoadShader("Assets/Shaders/Pipeline/Skinning/BakedBoneReplay.cs.hlsl");
    }
    const auto shader = shaderOverride.IsValid() ? shaderOverride : s_boneReplayProgram.shader;
    if (!ctx.resources.Get(s_boneReplayProgram.constants)) {
        s_boneReplayProgram.constants = ctx.resources.CreateConstantBuffer(sizeof(ReplayConstants));
    }
    const auto palette = s_boneReplayPalettePool.Acquire(ctx.resources, rows.size() * asset.boneCount, sizeof(math::Matrix4));
    const auto rowBuffer = s_boneReplayRowPool.Acquire(ctx.resources, rows.size(), sizeof(BoneReplayRow));
    result.status = BoneReplayStatus::RESOURCE_FAILED;
    const auto* output = ctx.resources.Get(palette);
    if (!ctx.resources.Get(shader) || !ctx.resources.Get(s_boneReplayProgram.constants) || !rowBuffer.IsValid()
        || !output || output->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX) {
        return result;
    }
    const ReplayConstants constants{asset.rootInverse, asset.nodeCount, asset.boneCount, asset.sampleCount, static_cast<uint32_t>(rows.size())};
    ctx.resources.Update(s_boneReplayProgram.constants, &constants, sizeof(constants));
    ctx.resources.Update(rowBuffer, rows.data(), rows.size_bytes());
    ComputeCall call;
    call.shader = shader; call.constantBuffers[cb::BAKE_REPLAY] = s_boneReplayProgram.constants;
    call.srvBuffers[srv::BAKE_SAMPLES] = asset.samples; call.srvBuffers[srv::BAKE_NODES] = asset.nodes;
    call.srvBuffers[srv::BAKE_BINDINGS] = asset.bindings; call.srvBuffers[srv::BAKE_ROWS] = rowBuffer;
    call.uavBuffers[StructuredUavIndex(uav::BAKE_PALETTE)] = palette;
    call.dispatchX = (asset.boneCount - 1) / 64 + 1; call.dispatchY = static_cast<uint32_t>(rows.size());
    result.status = BoneReplayStatus::DISPATCH_FAILED;
    if (!ctx.renderer.TryDispatch(call, ctx.resources)) {
        return result;
    }
    result.status = BoneReplayStatus::OK; result.palette = palette;
    result.rowUploadBytes = rows.size_bytes() + sizeof(constants); result.dispatchCount = 1;
    return result;
}

void ReleaseBakedBoneReplayCaches(ResourceManager& resources)
{
    DynamicRWStructuredBufferPool palettes;
    DynamicStructuredBufferPool rows;
    ReplayProgram program;
    {
        std::lock_guard lock(s_boneReplayMutex);
        palettes = std::move(s_boneReplayPalettePool); s_boneReplayPalettePool = DynamicRWStructuredBufferPool{};
        rows = std::move(s_boneReplayRowPool); s_boneReplayRowPool = DynamicStructuredBufferPool{};
        program = s_boneReplayProgram; s_boneReplayProgram = {};
    }
    // Resource retirement can synchronously enter the render executor. Never hold its replay mutex across that wait.
    palettes.ReleaseAll(resources); rows.ReleaseAll(resources);
    if (program.owner == resources.GetInstanceId() && program.epoch == resources.GetResetVersion()) {
        if (program.shader.IsValid()) {
            resources.Release(program.shader);
        }
        if (program.constants.IsValid()) {
            resources.Release(program.constants);
        }
    }
}
}
