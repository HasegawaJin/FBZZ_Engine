/// @file    BakedBoneReplay.cpp
/// @brief   GPU-resident skeletal samples and independent palette replay using existing RHI pools.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#include <Graphics/Effects/BakedBoneReplay.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/DynamicBufferPool.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include <cmath>
#include <limits>

namespace fbzz::renderer {
namespace {
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
    auto* resources = ResourceManager::Active();
    if (!resources || resources->GetInstanceId() != owner || resources->GetResetVersion() != epoch) return;
    if (samples.IsValid()) resources->Release(samples);
    if (nodes.IsValid()) resources->Release(nodes);
    if (bindings.IsValid()) resources->Release(bindings);
}

std::unique_ptr<BoneReplayGpuAsset> UploadBakedBoneReplay(ResourceManager& resources,
    const BoneReplaySource& source, BoneReplayStatus& status)
{
    status = BoneReplayStatus::INVALID_SOURCE;
    if (!source.nodeCount || source.nodeCount > 1024 || !source.boneCount || source.boneCount > 128
        || source.samples.empty() || source.samples.size() % 48 != 0 || source.samples.size() / 48 > UINT32_MAX
        || source.samples.size() / 48 % source.nodeCount != 0
        || source.nodes.size() != static_cast<uint64_t>(source.nodeCount) * 16
        || source.bindings.size() != static_cast<uint64_t>(source.boneCount) * 80) return {};
    auto asset = std::make_unique<BoneReplayGpuAsset>();
    asset->owner = resources.GetInstanceId(); asset->epoch = resources.GetResetVersion();
    asset->nodeCount = source.nodeCount; asset->boneCount = source.boneCount;
    asset->sampleCount = static_cast<uint32_t>(source.samples.size() / 48); asset->rootInverse = source.rootInverse;
    asset->samples = resources.CreateGpuLocalStructuredBuffer(source.samples.data(), asset->sampleCount, 48);
    asset->nodes = resources.CreateGpuLocalStructuredBuffer(source.nodes.data(), asset->nodeCount, 16);
    asset->bindings = resources.CreateGpuLocalStructuredBuffer(source.bindings.data(), asset->boneCount, 80);
    if (!BoneReplayValidBuffer(resources, asset->samples, asset->sampleCount, 48)
        || !BoneReplayValidBuffer(resources, asset->nodes, asset->nodeCount, 16)
        || !BoneReplayValidBuffer(resources, asset->bindings, asset->boneCount, 80)) { status = BoneReplayStatus::RESOURCE_FAILED; return {}; }
    asset->residentBytes = source.samples.size() + source.nodes.size() + source.bindings.size();
    status = BoneReplayStatus::OK; return asset;
}

BoneReplayResult ReplayBakedBones(RenderPassContext& ctx, const BoneReplayGpuAsset& asset,
                                 std::span<const BoneReplayRow> rows, ResourceHandle<ShaderTag> shaderOverride)
{
    BoneReplayResult result;
    if (!ctx.renderer.GetCapabilities().bindless) return result;
    result.status = BoneReplayStatus::INVALID_SOURCE;
    if (asset.owner != ctx.resources.GetInstanceId() || asset.epoch != ctx.resources.GetResetVersion()
        || !asset.nodeCount || asset.nodeCount > 1024 || !asset.boneCount || asset.boneCount > 128
        || !BoneReplayValidBuffer(ctx.resources, asset.samples, asset.sampleCount, 48)
        || !BoneReplayValidBuffer(ctx.resources, asset.nodes, asset.nodeCount, 16)
        || !BoneReplayValidBuffer(ctx.resources, asset.bindings, asset.boneCount, 80)) return result;
    result.status = BoneReplayStatus::INVALID_ROW;
    if (rows.empty() || rows.size() > 65535 || rows.size() > UINT32_MAX / asset.boneCount) return result;
    for (const auto& row : rows) {
        if (!BoneReplayValidOffset(row.first, asset.nodeCount, asset.sampleCount) || !BoneReplayValidOffset(row.second, asset.nodeCount, asset.sampleCount)
            || !BoneReplayValidOffset(row.targetFirst, asset.nodeCount, asset.sampleCount) || !BoneReplayValidOffset(row.targetSecond, asset.nodeCount, asset.sampleCount)
            || row.padding || !std::isfinite(row.alpha) || row.alpha < 0.0f || row.alpha > 1.0f
            || !std::isfinite(row.targetAlpha) || row.targetAlpha < 0.0f || row.targetAlpha > 1.0f
            || !std::isfinite(row.blendWeight) || row.blendWeight < 0.0f || row.blendWeight > 1.0f) return result;
    }
    if (s_boneReplayProgram.owner != asset.owner || s_boneReplayProgram.epoch != asset.epoch) {
        s_boneReplayProgram = {}; s_boneReplayProgram.owner = asset.owner; s_boneReplayProgram.epoch = asset.epoch;
    }
    if (!shaderOverride.IsValid() && !ctx.resources.Get(s_boneReplayProgram.shader))
        s_boneReplayProgram.shader = ctx.resources.LoadShader("Assets/Shaders/Pipeline/Skinning/BakedBoneReplay.cs.hlsl");
    const auto shader = shaderOverride.IsValid() ? shaderOverride : s_boneReplayProgram.shader;
    if (!ctx.resources.Get(s_boneReplayProgram.constants)) s_boneReplayProgram.constants = ctx.resources.CreateConstantBuffer(sizeof(ReplayConstants));
    const auto palette = s_boneReplayPalettePool.Acquire(ctx.resources, rows.size() * asset.boneCount, sizeof(math::Matrix4));
    const auto rowBuffer = s_boneReplayRowPool.Acquire(ctx.resources, rows.size(), sizeof(BoneReplayRow));
    result.status = BoneReplayStatus::RESOURCE_FAILED;
    const auto* output = ctx.resources.Get(palette);
    if (!ctx.resources.Get(shader) || !ctx.resources.Get(s_boneReplayProgram.constants) || !rowBuffer.IsValid()
        || !output || output->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX) return result;
    const ReplayConstants constants{asset.rootInverse, asset.nodeCount, asset.boneCount, asset.sampleCount, static_cast<uint32_t>(rows.size())};
    ctx.resources.Update(s_boneReplayProgram.constants, &constants, sizeof(constants));
    ctx.resources.Update(rowBuffer, rows.data(), rows.size_bytes());
    ComputeCall call;
    call.shader = shader; call.constantBuffers[0] = s_boneReplayProgram.constants;
    call.srvBuffers[14] = asset.samples; call.srvBuffers[15] = asset.nodes;
    call.srvBuffers[29] = asset.bindings; call.srvBuffers[30] = rowBuffer;
    call.uavBuffers[0] = palette;
    call.dispatchX = (asset.boneCount - 1) / 64 + 1; call.dispatchY = static_cast<uint32_t>(rows.size());
    result.status = BoneReplayStatus::DISPATCH_FAILED;
    if (!ctx.renderer.TryDispatch(call, ctx.resources)) return result;
    result.status = BoneReplayStatus::OK; result.palette = palette;
    result.rowUploadBytes = rows.size_bytes() + sizeof(constants); result.dispatchCount = 1;
    return result;
}

void ReleaseBakedBoneReplayCaches(ResourceManager& resources)
{
    s_boneReplayPalettePool.ReleaseAll(resources); s_boneReplayRowPool.ReleaseAll(resources);
    if (s_boneReplayProgram.owner == resources.GetInstanceId() && s_boneReplayProgram.epoch == resources.GetResetVersion()) {
        if (s_boneReplayProgram.shader.IsValid()) resources.Release(s_boneReplayProgram.shader);
        if (s_boneReplayProgram.constants.IsValid()) resources.Release(s_boneReplayProgram.constants);
    }
    s_boneReplayProgram = {};
}
}
