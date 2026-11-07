/// @file    GpuHiZ.cpp
/// @brief   実描画深度から最遠値の GPU 階層を構築する。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <Graphics/Pipeline/GpuHiZ.hpp>
#include "../Renderer/GpuCullDrawQueue.hpp"
#include <Core/Profiler/ProfileScope.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/DynamicBufferPool.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/IRenderTarget.hpp>
#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include <Graphics/Renderer/ITexture.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <array>
#include <limits>

namespace fbzz::renderer {
namespace {

constexpr uint32_t BASE_TILE = 4;
constexpr uint32_t THREAD_GROUP_SIDE = 8;
constexpr uint32_t LEVELS_PER_DISPATCH = 4;
constexpr uint32_t MAX_DISPATCH_GROUPS = 65535;
constexpr uint32_t MAX_LEVELS = std::numeric_limits<uint32_t>::digits;
constexpr uint32_t MAX_DISPATCHES = (MAX_LEVELS + LEVELS_PER_DISPATCH - 1) / LEVELS_PER_DISPATCH;

struct PyramidLevel {
    uint32_t width;
    uint32_t height;
    uint32_t offset;
};

struct HiZBuildConstants {
    std::array<uint32_t, 4> sourceRect;
    std::array<uint32_t, LEVELS_PER_DISPATCH> outputOffsets;
    uint32_t sourceOffset;
    uint32_t levelCount;
    uint32_t readDepth;
    uint32_t reversedZ;
};
static_assert(sizeof(HiZBuildConstants) == 48);

struct HiZResources {
    DynamicRWStructuredBufferPool depths;
    std::array<ResourceHandle<ConstantBufferTag>, MAX_DISPATCHES> constants;
    ResourceHandle<ShaderTag> shader;
    uint64_t owner = 0;
    uint64_t epoch = 0;
};

uint32_t RoundUpDivide(uint32_t value, uint32_t divisor)
{
    return value / divisor + (value % divisor != 0);
}

} /// @note namespace

GpuHiZ BuildGpuHiZ(IRenderer& renderer, ResourceManager& resources, ResourceHandle<RenderTargetTag> source,
    uint32_t viewportX, uint32_t viewportY, uint32_t viewportWidth, uint32_t viewportHeight)
{
    FBZZ_PROFILE_SCOPE("ViewOcclusion::GpuHiZ");
    if (!renderer.SupportsIndirectDraw() || !source.IsValid()) return {};
    const auto epoch = resources.GetResetVersion();
    const auto frame = resources.FrameStamp();
    const auto* target = resources.Get(source);
    if (!target) return {};
    const uint32_t targetWidth = target->GetWidth(), targetHeight = target->GetHeight();
    const bool reversedZ = target->IsReversedZ();
    if (viewportWidth == 0 && viewportHeight == 0 && viewportX == 0 && viewportY == 0) {
        viewportWidth = targetWidth;
        viewportHeight = targetHeight;
    }
    if (viewportWidth == 0 || viewportHeight == 0 || viewportX >= targetWidth || viewportY >= targetHeight
        || viewportWidth > targetWidth - viewportX || viewportHeight > targetHeight - viewportY) return {};

    /// @note 登録済みの通常 2D RT 深度だけを読む。DX12 の SRV は単一サンプルの R32_FLOAT。
    const auto depth = resources.GetDepthTexture(source);
    const auto* depthTexture = resources.Get(depth);
    if (!depthTexture || depthTexture->GetBindlessIndex() == INVALID_BINDLESS_INDEX
        || depthTexture->GetWidth() != targetWidth || depthTexture->GetHeight() != targetHeight
        || depthTexture->GetDepth() != 1) return {};

    uint32_t width = RoundUpDivide(viewportWidth, BASE_TILE), height = RoundUpDivide(viewportHeight, BASE_TILE);
    if (RoundUpDivide(width, THREAD_GROUP_SIDE) > MAX_DISPATCH_GROUPS
        || RoundUpDivide(height, THREAD_GROUP_SIDE) > MAX_DISPATCH_GROUPS) return {};
    std::array<PyramidLevel, MAX_LEVELS> levels;
    uint32_t levelCount = 0, depthCount = 0;
    for (;;) {
        const auto size = uint64_t{width} * height;
        if (size + depthCount > (std::numeric_limits<uint32_t>::max)() / sizeof(float)) return {};
        levels[levelCount++] = {width, height, depthCount};
        depthCount += static_cast<uint32_t>(size);
        if (width == 1 && height == 1) break;
        width = RoundUpDivide(width, 2);
        height = RoundUpDivide(height, 2);
    }

    /// @note 同じ CPU フレームの別ビュー・別光源には別のバッファを貸し出す。
    static HiZResources cache;
    if (cache.owner != resources.GetInstanceId() || cache.epoch != epoch) {
        if (auto* previous = ResourceManager::FindOwner(cache.owner);
            previous && previous->GetResetVersion() == cache.epoch) {
            for (const auto constants : cache.constants)
                if (constants.IsValid()) previous->Release(constants);
        }
        cache.constants = {};
        cache.shader = {};
        cache.owner = resources.GetInstanceId();
        cache.epoch = epoch;
    }
    if (!resources.Get(cache.shader))
        cache.shader = resources.LoadShader("Assets/Shaders/Pipeline/GpuHiZ.cs.hlsl");
    if (!resources.Get(cache.shader)) return {};
    const auto output = cache.depths.Acquire(resources, depthCount, sizeof(float));
    const auto* buffer = resources.Get(output);
    if (!buffer || buffer->GetBindlessIndex() == INVALID_BINDLESS_INDEX
        || buffer->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX || resources.GetResetVersion() != epoch
        || resources.FrameStamp() != frame || resources.GetDepthTexture(source) != depth) return {};

    /// @note 段ごとの定数を分離し、依存する Dispatch の記録結果を 1 回の同期で受け取る。
    const uint32_t dispatchCount = RoundUpDivide(levelCount, LEVELS_PER_DISPATCH);
    std::array<ComputeCall, MAX_DISPATCHES> calls;
    for (uint32_t index = 0; index < dispatchCount; ++index) {
        auto& bufferHandle = cache.constants[index];
        if (!resources.Get(bufferHandle)) bufferHandle = resources.CreateConstantBuffer(sizeof(HiZBuildConstants));
        if (!resources.Get(bufferHandle)) return {};
        auto& compute = calls[index];
        compute.shader = cache.shader;
        compute.constantBuffers[0] = bufferHandle;
        compute.uavBuffers[0] = output;
        const uint32_t first = index * LEVELS_PER_DISPATCH;
        HiZBuildConstants constants{};
        constants.levelCount = (std::min)(LEVELS_PER_DISPATCH, levelCount - first);
        constants.reversedZ = reversedZ;
        if (first == 0) {
            constants.sourceRect = {viewportX, viewportY, viewportWidth, viewportHeight};
            constants.readDepth = 1;
            compute.srvInputs[7] = depth;
        } else {
            const auto& previous = levels[first - 1];
            constants.sourceRect = {0, 0, previous.width, previous.height};
            constants.sourceOffset = previous.offset;
        }
        for (uint32_t i = 0; i < constants.levelCount; ++i) constants.outputOffsets[i] = levels[first + i].offset;
        resources.Update(bufferHandle, &constants, sizeof(constants));
        compute.dispatchX = RoundUpDivide(levels[first].width, THREAD_GROUP_SIDE);
        compute.dispatchY = RoundUpDivide(levels[first].height, THREAD_GROUP_SIDE);
    }
    if (resources.GetResetVersion() != epoch || resources.FrameStamp() != frame
        || resources.GetDepthTexture(source) != depth) return {};

    /// @note 読み取り中は source を DSV として束縛しない。失敗時も描画先を復元する。
    renderer.SetRenderTarget({}, resources);
    std::array<uint8_t, MAX_DISPATCHES> receipts{};
    auto recorded = std::span{receipts}.first(dispatchCount);
    gpu_cull_detail::TryDispatchSequence(renderer, resources, std::span{calls}.first(dispatchCount), recorded, true);
    renderer.SetRenderTarget(source, resources);
    if (viewportX != 0 || viewportY != 0 || viewportWidth != targetWidth || viewportHeight != targetHeight)
        renderer.SetViewport(viewportX, viewportY, viewportWidth, viewportHeight);
    if (std::find(recorded.begin(), recorded.end(), uint8_t{0}) != recorded.end()
        || resources.GetResetVersion() != epoch || resources.FrameStamp() != frame) return {};
    return {output, viewportWidth, viewportHeight, reversedZ};
}

} /// @note namespace fbzz::renderer
