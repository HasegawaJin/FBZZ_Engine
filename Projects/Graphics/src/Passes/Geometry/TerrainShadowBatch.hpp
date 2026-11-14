/// @file    TerrainShadowBatch.hpp
/// @brief   地形の影ビュー選別をアトラス描画より前に記録する。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/GpuGeometrySelection.hpp>
#include <Graphics/Renderer/RenderSettings.hpp>
#include <array>
#include <memory>
#include <span>
#include <vector>

namespace fbzz::math { struct Frustum; struct Matrix4; }

namespace fbzz::renderer {

struct InstanceShadowView;
struct RenderPassContext;
struct RenderScene;

enum class TerrainShadowCasterFilter { ALL, STATIC_ONLY, DYNAMIC_ONLY };

/// @note CPU LOD とメッシュレットは同じインデックスを使う。GPU 選択の LOD は深度の再利用対象外。
/// @note カメラ移動による LOD の変更は、そのパッチを描くカスケードにだけ反映する。
[[nodiscard]] std::array<uint64_t, kMaxShadowCascades> TerrainShadowCacheContentKeys(
    const RenderPassContext& ctx, uint32_t mask);

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
    bool usesBakedStaticDepth, TerrainShadowCasterFilter filter = TerrainShadowCasterFilter::ALL);

} /// @note namespace fbzz::renderer
