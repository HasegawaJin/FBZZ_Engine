/// @file    TerrainShadowBatch.hpp
/// @brief   地形の影ビュー選別をアトラス描画より前に記録する。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/GpuGeometrySelection.hpp>
#include <memory>
#include <span>
#include <vector>

namespace fbzz::math { struct Frustum; struct Matrix4; }

namespace fbzz::renderer {

struct InstanceShadowView;
struct RenderPassContext;
struct RenderScene;

struct PreparedTerrainShadowSet {
    ResourceHandle<ShaderTag> shader;
    GpuGeometryDispatch dispatch;
    size_t commandIndex = SIZE_MAX;
};

struct PreparedTerrainShadowBatch {
    std::shared_ptr<const RenderScene> scene;
    uint64_t resourceEpoch = 0;
    uint64_t resourceFrame = 0;
    std::vector<PreparedTerrainShadowSet> sets;
};

[[nodiscard]] PreparedTerrainShadowBatch PrepareTerrainShadowBatch(RenderPassContext& ctx,
    std::span<const InstanceShadowView> views, std::vector<ComputeCall>& commands, size_t bakedViewIndex);

/// @note Invalid or rejected selection falls back to CPU geometry for this tile.
void SubmitTerrainShadowBatchView(RenderPassContext& ctx, const math::Frustum& frustum,
    const math::Matrix4& lightViewProjection,
    ResourceHandle<ShaderTag> shadowShader, ResourceHandle<PipelineStateTag> pipelineState,
    ResourceHandle<ConstantBufferTag> frameCB, ResourceHandle<ConstantBufferTag> objectCB,
    const PreparedTerrainShadowBatch& batch, size_t viewIndex, std::span<const uint8_t> receipts,
    bool usesBakedStaticDepth);

} /// @note namespace fbzz::renderer
