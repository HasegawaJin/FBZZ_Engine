/// @file    GeometryPipeline.cpp
/// @brief   Forward / Deferred の登録を分離し、共通パスを同じグラフへ組む。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#include <Graphics/Pipeline/GeometryPipeline.hpp>
#include <Graphics/Pipeline/RenderPipeline.hpp>
#include <Graphics/Passes/Geometry/FiberRenderPass.hpp>
#include <Graphics/Passes/Geometry/TerrainRenderPass.hpp>
#include <Graphics/Passes/Geometry/WaterRenderPass.hpp>
#include <Graphics/Renderer/OpaqueRenderPlan.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>

namespace fbzz::renderer {
namespace {

void BuildScreenSpaceOcclusion(RenderPipeline& pipeline)
{
    pipeline.AddPass<GTAOPass>();
    pipeline.AddPass<ContactShadowsPass>();
    pipeline.AddPass<SSAOPass>();
}

void BuildSkyComposition(RenderPipeline& pipeline)
{
    pipeline.AddPass<SkyPass>();
    pipeline.AddPass<SunMoonPass>();
    pipeline.AddPass<VolumetricCloudPass>();
}

void BuildForwardOpaque(RenderPipeline& pipeline, bool depthNormalPrepass)
{
    if (depthNormalPrepass) {
        pipeline.AddPass<GBufferPass>(GBufferPassMode::DepthNormalPrepass);
        /// @note 地形も AO の遮蔽者・受け手になるためプリパスへ含める。
        pipeline.AddPass<TerrainRenderPass>(TerrainDrawMode::GBuffer);
        /// @note Forward は各材質が遮蔽を読むため、本描画前に結果を揃える。
        BuildScreenSpaceOcclusion(pipeline);
    }

    pipeline.AddPass<ForwardOpaquePass>();
    pipeline.AddPass<TerrainRenderPass>(TerrainDrawMode::Forward);
    BuildSkyComposition(pipeline);

    if (depthNormalPrepass)
        pipeline.AddPass<SSRPass>();
}

void BuildDeferredOpaque(RenderPipeline& pipeline)
{
    pipeline.AddPass<GBufferPass>(GBufferPassMode::Deferred);
    pipeline.AddPass<TerrainRenderPass>(TerrainDrawMode::GBuffer);
    pipeline.AddPass<DeferredDepthCopyPass>();
    BuildScreenSpaceOcclusion(pipeline);
    pipeline.AddPass<DeferredLightingPass>();
    /// @note DepthCopy の HDR クリアに空を消されないよう、Lighting 後に合成する。
    BuildSkyComposition(pipeline);
    pipeline.AddPass<DeferredSkinnedForwardPass>();
    pipeline.AddPass<FiberRenderPass>();
    pipeline.AddPass<DeferredForwardTransparentPass>();
    /// @note 透明描画後の SSR は現行の契約。段境界の是正時に別途変更する。
    pipeline.AddPass<SSRPass>();
}

} /// @note namespace

void BuildGeometryPreparation(RenderPipeline& pipeline, bool clusteredEnabled)
{
    /// @note スキニング出力は現在グラフの論理資源でないため、影より先の登録を保つ。
    pipeline.AddPass<SkinningComputePass>();
    if (clusteredEnabled)
        pipeline.AddPass<ClusterLightCullPass>();
    pipeline.AddPass<LightCookiePass>();
    pipeline.AddPass<ShadowPass>();
}

void BuildGeometryPipeline(RenderPipeline& pipeline, const renderer::OpaqueRenderPlan& plan)
{
    if (plan.UsesDeferredLighting())
        BuildDeferredOpaque(pipeline);
    else
        BuildForwardOpaque(pipeline, plan.HasScreenSpaceInputs());
}

void BuildWaterComposition(RenderPipeline& pipeline)
{
    /// @note 水面の後で光芒・コースティクスを足すと、水底への効果が水面を覆う。
    pipeline.AddPass<VolumetricLightPass>();
    pipeline.AddPass<WaterCausticsPass>();
    pipeline.AddPass<WaterRenderPass>();
}

} /// @note namespace fbzz::renderer
