/// @file    RayTracingPipeline.hpp
/// @brief   静的 Ray Scene の共有準備とビュー別の交差診断パス。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#pragma once
#include <Graphics/RayTracing/RayGeometryCache.hpp>
#include <Graphics/RayTracing/RayReflectionResources.hpp>
#include <Graphics/RayTracing/RayPathTraceResources.hpp>
#include <Graphics/Renderer/RenderSettings.hpp>

namespace fbzz::renderer {
struct RenderViewResources;
struct RenderSharedResources;
struct OpaqueRenderPlan;
struct ViewPipelineExtensions;

struct RayDebugViewResources {
    ResourceHandle<TextureTag> output;
    ResourceHandle<ConstantBufferTag> constants;
    uint32_t width = 0;
    uint32_t height = 0;
    RayScene scene;
    RaySceneGpu gpu;
};

[[nodiscard]] bool IsRayDebugView(ViewMode mode);
/// @note Enables shared point diffuse only for a typed finite b8 snapshot and live, dimension-correct irradiance/SH inputs.
[[nodiscard]] bool IsRayDiffuseIndirectReady(const RenderPassContext& context, const RenderViewResources& view);
/// @note 実験 RT のセッション許可なしでは資源を生成せず false。未対応 GPU / shader / 入力失敗でも false。
[[nodiscard]] bool PrepareRayDebugView(RenderPassContext& context,
    RenderViewResources& view, RenderSharedResources& shared);
/// @pre PrepareRayDebugView が成功済み。outputName は当該ビューの LDR チェーン終端。
void BuildRayDebugPipeline(RenderPipeline& pipeline, RenderViewResources& view,
    RenderSharedResources& shared, ResourceManager& resources, std::string_view outputName);
/// @note 保証済み PBR と滑らかな Hybrid 誘電体の被覆を確認し、未対応表面・照明・資源失敗では従来反射へ戻す。
/// @note 実験 RT のセッション許可なしでは Scene 走査・AS 構築・専用資源生成を行わない。
[[nodiscard]] bool PrepareRayReflectionView(RenderPassContext& context,
    RenderViewResources& view, RenderSharedResources& shared, const OpaqueRenderPlan& opaquePlan);
/// @pre PrepareRayReflectionView が成功済み。GBuffer の書き込み後、DeferredLighting より前に登録する。
void BuildRayReflectionPipeline(RenderPipeline& pipeline, RenderViewResources& view,
    RenderSharedResources& shared, RenderPassContext& context);
/// @note 初期 REFERENCE の静的 PBR・発光三角形・方向光・定数環境だけを許可する。
/// @note 実験 RT のセッション許可なしでは Scene 走査・AS 構築・専用資源生成を行わない。
[[nodiscard]] bool PrepareRayPathView(RenderPassContext& context,
    RenderViewResources& view, RenderSharedResources& shared);
/// @pre PrepareRayPathView が成功済み。Raster 照明・TAA・HDR 拡張を登録しない。
void BuildRayPathViewPipeline(RenderPipeline& pipeline, RenderPassContext& context,
    RenderViewResources& view, RenderSharedResources& shared, const ViewPipelineExtensions& extensions);
} /// @note namespace fbzz::renderer
