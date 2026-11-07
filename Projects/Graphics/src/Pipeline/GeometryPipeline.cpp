/// @file    GeometryPipeline.cpp
/// @brief   Forward / Deferred の登録を分離し、共通パスを同じグラフへ組む。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#include <Graphics/Pipeline/GeometryPipeline.hpp>
#include <Graphics/Pipeline/GpuHiZ.hpp>
#include <Graphics/Pipeline/RenderPipeline.hpp>
#include <Graphics/Passes/Geometry/FiberRenderPass.hpp>
#include <Graphics/Passes/Geometry/InstanceRenderPasses.hpp>
#include <Graphics/Passes/Geometry/TerrainRenderPass.hpp>
#include <Graphics/Passes/Geometry/WaterRenderPass.hpp>
#include <Graphics/Renderer/OpaqueRenderPlan.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <utility>

namespace fbzz::renderer {
namespace {

class TerrainHiZInstancesPass final : public IRenderPass {
public:
    explicit TerrainHiZInstancesPass(bool deferred) : m_deferred(deferred) {}

    std::string_view Name() const override { return "TerrainHiZInstances"; }

    bool IsEnabled(const RenderPassContext& ctx) const override
    {
        if (ctx.isDeferred != m_deferred || !ctx.occlusionCullingEnabled || !ctx.occlusionCuller
            || ctx.settings.IsWireframe() || ctx.width == 0 || ctx.height == 0 || !ctx.renderScene) return false;
        const bool terrainInView = std::any_of(ctx.renderScene->terrains.begin(), ctx.renderScene->terrains.end(),
            [&](const auto& terrain) {
                return terrain.layer < 32 && (ctx.cullingMask & (1u << terrain.layer)) && !terrain.patches.empty();
            });
        return terrainInView && HasTerrainHiZInstanceCandidates(ctx);
    }

    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override
    {
        const auto target = m_deferred ? "GBuffer" : "HDR";
        builder.ReadWrite(target).SetAutoTarget(target);
        if (!m_deferred) {
            builder.Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas");
            if (ctx.gbufferDepthReady) DeclareScreenSpaceOcclusionReads(builder, ctx);
            if (ctx.waterMediumPassActive) builder.Read("WaterMediumAtlas");
        }
    }

    void Execute(PassResources& resources, RenderPassContext& ctx) override
    {
        if (!std::exchange(ctx.terrainHiZInstancesPending, false)) return;
        const auto target = resources.Target(m_deferred ? "GBuffer" : "HDR");
        GpuHiZ hierarchy{};
        /// @note The seed and consumer must use the same immutable view, even if resource calls pump callbacks.
        /// @note The graph binds the full target. A different logical view size must not reinterpret a subrectangle as NDC.
        if (IsViewOcclusionCurrent(ctx))
            hierarchy = BuildGpuHiZ(ctx.renderer, ctx.resources, target);
        if (!IsViewOcclusionCurrent(ctx)) hierarchy = {};
        if (m_deferred) ExecuteInstanceGBuffer(ctx, hierarchy);
        else ExecuteInstanceForward(ctx, hierarchy);
    }

private:
    bool m_deferred;
};

void BuildScreenSpaceOcclusion(RenderPipeline& pipeline)
{
    pipeline.AddPass<GTAOPass>();
    pipeline.AddPass<ContactShadowsPass>();
    pipeline.AddPass<SSAOPass>();
}

void BuildSkyBackground(RenderPipeline& pipeline)
{
    pipeline.AddPass<SkyPass>();
    pipeline.AddPass<SunMoonPass>();
}

void BuildSkyComposition(RenderPipeline& pipeline)
{
    BuildSkyBackground(pipeline);
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
    pipeline.AddPass<TerrainHiZInstancesPass>(false);
    BuildSkyComposition(pipeline);

    if (depthNormalPrepass) {
        pipeline.AddPass<SSRPass>();
        pipeline.AddPass<SSRResolvePass>();
    }
}

void BuildDeferredOpaque(RenderPipeline& pipeline, const std::function<void()>& deferredSurfaceReady,
    bool hybridScreenFirst)
{
    pipeline.AddPass<GBufferPass>(GBufferPassMode::Deferred);
    pipeline.AddPass<TerrainRenderPass>(TerrainDrawMode::GBuffer);
    pipeline.AddPass<TerrainHiZInstancesPass>(true);
    pipeline.AddPass<DeferredDepthCopyPass>();
    BuildScreenSpaceOcclusion(pipeline);
    if (hybridScreenFirst) {
        /// @note SSR の入力は baseline の一度目の照明。最終 resolve の RT/SSR を循環参照しない。
        pipeline.AddPass<DeferredLightingPass>(true);
        BuildSkyBackground(pipeline);
        pipeline.AddPass<SSRPass>();
    }
    if (deferredSurfaceReady) deferredSurfaceReady();
    pipeline.AddPass<DeferredLightingPass>();
    /// @note 雲は前景にも合成するため baseline へ入れず、最終照明後に一度だけ重ねる。
    if (hybridScreenFirst) pipeline.AddPass<VolumetricCloudPass>();
    else BuildSkyComposition(pipeline);
    pipeline.AddPass<DeferredSkinnedForwardPass>();
    pipeline.AddPass<FiberRenderPass>();
    /// @note SSR は不透明 HDR から解決し、透明色を反射元や反射の受け手へ混入させない。
    if (!hybridScreenFirst) {
        pipeline.AddPass<SSRPass>();
        pipeline.AddPass<SSRResolvePass>();
    }
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
    pipeline.AddPass<WaterMediumAtlasPass>();
}

void BuildGeometryPipeline(RenderPipeline& pipeline, const renderer::OpaqueRenderPlan& plan,
    const std::function<void()>& deferredSurfaceReady, bool hybridScreenFirst)
{
    if (plan.UsesDeferredLighting())
        BuildDeferredOpaque(pipeline, deferredSurfaceReady, hybridScreenFirst);
    else
        BuildForwardOpaque(pipeline, plan.HasScreenSpaceInputs());
}

void BuildGeometryTransparency(RenderPipeline& pipeline, const renderer::OpaqueRenderPlan& plan)
{
    pipeline.AddPass<ForwardTransparentPass>(plan.UsesDeferredLighting());
}

void BuildWaterComposition(RenderPipeline& pipeline)
{
    pipeline.AddPass<WaterInterfacePass>();
    /// @note 水面の後で光芒・コースティクスを足すと、水底への効果が水面を覆う。
    pipeline.AddPass<VolumetricLightPass>();
    pipeline.AddPass<WaterCausticsPass>();
    pipeline.AddPass<WaterRenderPass>();
}

} /// @note namespace fbzz::renderer
