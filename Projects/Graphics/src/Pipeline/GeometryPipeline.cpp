/// @file    GeometryPipeline.cpp
/// @brief   Forward / Deferred の登録を分離し、共通パスを同じグラフへ組む。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#include <Graphics/Pipeline/GeometryPipeline.hpp>
#include <Graphics/Pipeline/GpuHiZ.hpp>
#include <Graphics/Pipeline/RenderPipeline.hpp>
#include <Graphics/Passes/Geometry/FiberRenderPass.hpp>
#include <Graphics/Passes/Geometry/TerrainRenderPass.hpp>
#include <Graphics/Passes/Geometry/WaterRenderPass.hpp>
#include <Graphics/Renderer/OpaqueRenderPlan.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>

namespace fbzz::renderer {
namespace {

class OccluderDepthPass final : public IRenderPass {
public:
    std::string_view Name() const override { return DEPTH_PREPASS; }
    bool IsEnabled(const RenderPassContext& ctx) const override { return ctx.gpuOcclusionEnabled; }
    void Setup(PassBuilder& builder, const RenderPassContext&) const override
    {
        builder.Write(OCCLUDER_DEPTH).SetAutoTarget(OCCLUDER_DEPTH);
    }

    void Execute(PassResources& resources, RenderPassContext& ctx) override
    {
        const auto target = resources.Target(OCCLUDER_DEPTH);
        if (!ctx.resources.Get(target) || !IsViewOcclusionCurrent(ctx)) return;
        ctx.renderer.ClearDepth();
        const bool terrainSubmitted = DrawTerrainOccluderDepth(ctx);
        const bool meshSubmitted = DrawOpaqueOccluderDepth(ctx);
        ctx.gpuOccluderDepthReady = (terrainSubmitted || meshSubmitted) && IsViewOcclusionCurrent(ctx);
    }
};

class OcclusionHiZPass final : public IRenderPass {
public:
    std::string_view Name() const override { return BUILD_OCCLUSION_HIZ; }
    bool IsEnabled(const RenderPassContext& ctx) const override { return ctx.gpuOcclusionEnabled; }
    void Setup(PassBuilder& builder, const RenderPassContext&) const override
    {
        builder.Read(OCCLUDER_DEPTH, PassBuilder::Purpose::SHADER_READ).Write(OCCLUSION_HIZ, PassBuilder::Purpose::UAV);
    }

    void Execute(PassResources& resources, RenderPassContext& ctx) override
    {
        if (!ctx.gpuOccluderDepthReady || !IsViewOcclusionCurrent(ctx)) return;
        const auto hierarchy = BuildGpuHiZ(ctx.renderer, ctx.resources, resources.Target(OCCLUDER_DEPTH));
        if (hierarchy.reversedZ && hierarchy.width == ctx.width && hierarchy.height == ctx.height
            && hierarchy.depths.IsValid() && IsViewOcclusionCurrent(ctx))
            ctx.resourceRegistry.BindStructuredBuffer(OCCLUSION_HIZ, hierarchy.depths);
    }
};

class MeshOcclusionPass final : public IRenderPass {
public:
    std::string_view Name() const override { return CULL_OCCLUDED_MESHES; }
    bool IsEnabled(const RenderPassContext& ctx) const override { return ctx.gpuOcclusionEnabled; }
    void Setup(PassBuilder& builder, const RenderPassContext&) const override
    {
        builder.Read(OCCLUSION_HIZ, PassBuilder::Purpose::SHADER_READ)
            .Write(OCCLUSION_DRAW_ARGUMENTS, PassBuilder::Purpose::UAV);
    }
    void Execute(PassResources&, RenderPassContext& ctx) override { CullOccludedMeshes(ctx, ReadViewGpuHiZ(ctx)); }
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
    pipeline.AddPass<OccluderDepthPass>();
    pipeline.AddPass<OcclusionHiZPass>();
    pipeline.AddPass<MeshOcclusionPass>();
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
    pipeline.AddPass<ForwardTransparentPass>(false, true);
}

} /// @note namespace fbzz::renderer
