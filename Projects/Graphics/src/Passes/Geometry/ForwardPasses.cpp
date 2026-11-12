/// @file    ForwardPasses.cpp
/// @brief   抽出済み RenderScene の不透明・透明メッシュを前方描画する。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "RenderScenePassHelpers.hpp"
#include <Graphics/Passes/Geometry/FiberRenderPass.hpp>
#include <Graphics/Passes/Geometry/InstanceRenderPasses.hpp>
#include <Graphics/Pipeline/GpuHiZ.hpp>
#include <algorithm>
#include <vector>
#include <optional>
#include <chrono>
#include <Core/Profiler/ProfileScope.hpp>

namespace fbzz::renderer {
namespace {
struct Entry {
    const renderer::RenderObject* object;
    const renderer::RenderMeshItem* item;
    float distanceSquared;
};
}

void ExecuteForwardPasses(RenderPassContext& ctx)
{
    /// @note Resource callbacks may replace ctx.renderScene; retain the input until all local entries are destroyed.
    const auto sceneOwner = ctx.renderScene;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    const auto& input = sceneOwner ? *sceneOwner : SceneInput(ctx);
    const auto arguments = ctx.gpuOcclusionEnabled
        ? ctx.Res().StructuredBuffer(OCCLUSION_DRAW_ARGUMENTS) : ResourceHandle<StructuredBufferTag>{};
    ctx.renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
    ClearForCamera(ctx.renderer, ctx.camera);
    const auto frame = MakeCameraFrameCB(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    resources.Update(h.frameCB, &frame, sizeof(frame));
    resources.Update(h.lightCB, &ctx.lightData, sizeof(renderer::LightConstantsCB));
    UpdateShadowConstants(ctx);
    UpdatePunctualShadowConstants(ctx);
    std::vector<Entry> staticOpaque, skinnedOpaque;
    const bool detailed = profiler::Profiler::IsEnabled()
        && profiler::Profiler::GetRequestedCaptureFilter().scope == "ForwardOpaque";
    double cullingMs = 0, objectUploadMs = 0, drawPreparationMs = 0, submitCallMs = 0;
    const auto gather = [&]<bool DETAILED>() {
        /// @note 同距離の旧来の投入順を保つため、静的候補を先に、スキンド候補を後に収集する。
        for (bool skinned : { false, true }) {
            for (const auto& object : input.Objects()) {
                if (object.skinned != skinned || !object.colorEligible || !MatchesView(ctx, object)) continue;
                if (!skinned && !HasColorGeometry(input.items[object.firstItem])) continue;
                ++ctx.statsTotalObjects;
                std::chrono::steady_clock::time_point visibilityBegin;
                if constexpr (DETAILED) visibilityBegin = std::chrono::steady_clock::now();
                const bool visible = IsRenderObjectVisible(ctx, object);
                if constexpr (DETAILED) cullingMs += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - visibilityBegin).count();
                if (!visible) continue;
                const float distance = (object.sortPosition - ctx.camera.m_position).LengthSq();
                for (uint32_t i = object.firstItem; i < object.firstItem + object.itemCount; ++i) {
                    const auto& item = input.items[i];
                    if (!HasColorGeometry(item)) continue;
                    Entry entry{ &object, &item, distance };
                    if (renderer::ResolveGeometryRoute(item.material.capabilities, false)
                        == renderer::GeometryRoute::ForwardTransparent) continue;
                    else if (skinned) skinnedOpaque.push_back(entry);
                    else staticOpaque.push_back(entry);
                }
            }
        }
    };
    const auto gatherProfile = profiler::Profiler::BeginScope({"ForwardOpaque::Gather", "Rendering"});
    if (detailed) gather.template operator()<true>();
    else gather.template operator()<false>();
    profiler::Profiler::EndScope(gatherProfile);
    const auto frontToBack = [](const Entry& a, const Entry& b) {
        if (a.item->material.renderQueue != b.item->material.renderQueue)
            return a.item->material.renderQueue < b.item->material.renderQueue;
        return a.distanceSquared < b.distanceSquared;
    };
    {
        FBZZ_PROFILE_SCOPE("ForwardOpaque::SortOpaque");
        std::sort(staticOpaque.begin(), staticOpaque.end(), frontToBack);
        std::sort(skinnedOpaque.begin(), skinnedOpaque.end(), frontToBack);
    }
    std::optional<DrawCall> bindings;
    auto submit = [&]<bool DETAILED>(const Entry& entry) {
        if (!entry.item->forwardMaterial.valid || !entry.item->forwardMaterial.shader.IsValid()) return;
        if (!bindings) bindings = ForwardPassBindings(ctx);
        if constexpr (DETAILED) {
            const auto begin = std::chrono::steady_clock::now();
            const auto constants = ObjectConstants(*entry.object);
            resources.Update(h.objectCB, &constants, sizeof(constants));
            const auto uploaded = std::chrono::steady_clock::now();
            auto draw = ForwardDrawWithBindings(ctx, *entry.object, *entry.item, *bindings);
            (void)BindMeshOcclusionArguments(ctx, input, entry.item - input.items.data(), arguments, draw);
            const auto prepared = std::chrono::steady_clock::now();
            SubmitCounted(ctx, draw);
            const auto submitted = std::chrono::steady_clock::now();
            objectUploadMs += std::chrono::duration<double, std::milli>(uploaded - begin).count();
            drawPreparationMs += std::chrono::duration<double, std::milli>(prepared - uploaded).count();
            submitCallMs += std::chrono::duration<double, std::milli>(submitted - prepared).count();
        } else {
            const auto constants = ObjectConstants(*entry.object);
            resources.Update(h.objectCB, &constants, sizeof(constants));
            auto draw = ForwardDrawWithBindings(ctx, *entry.object, *entry.item, *bindings);
            (void)BindMeshOcclusionArguments(ctx, input, entry.item - input.items.data(), arguments, draw);
            SubmitCounted(ctx, draw);
        }
    };
    for (const auto& entry : staticOpaque) {
        if (!entry.item->forwardMaterial.valid || !entry.item->forwardMaterial.shader.IsValid()) continue;
        if (detailed) submit.template operator()<true>(entry);
        else submit.template operator()<false>(entry);
    }
    for (const auto& entry : skinnedOpaque) {
        if (detailed) submit.template operator()<true>(entry);
        else submit.template operator()<false>(entry);
    }
    const auto hierarchy = ReadViewGpuHiZ(ctx);
    if (hierarchy.depths.IsValid()) ExecuteInstanceForward(ctx, hierarchy);
    else ExecuteInstanceForward(ctx);
    ExecuteFiberPass(ctx);
    if (detailed) {
        /// @note These sums are diagnostic aggregates, not contiguous child spans; visibility overlaps Gather, and per-draw clocks affect this run only.
        profiler::Profiler::PushSample({"ForwardOpaque::CullingAggregate", "Rendering"}, cullingMs);
        profiler::Profiler::PushSample({"ForwardOpaque::ObjectUploadAggregate", "Rendering"}, objectUploadMs);
        profiler::Profiler::PushSample({"ForwardOpaque::DrawPreparationAggregate", "Rendering"}, drawPreparationMs);
        profiler::Profiler::PushSample({"ForwardOpaque::SubmitCallAggregate", "Rendering"}, submitCallMs);
    }
}

void ForwardOpaquePass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    builder.Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas").Write("HDR");
    if (ctx.gpuOcclusionEnabled)
        builder.Read(OCCLUSION_HIZ, PassBuilder::Purpose::SHADER_READ).Read(OCCLUSION_DRAW_ARGUMENTS);
    /// @note GBuffer プリパスが走ったフレームだけ画面空間の遮蔽が存在する。
    /// @note 走っていないのに申告すると、誰も書かない名前を読むことになり Plan が落ちる。
    if (ctx.gbufferDepthReady)
        DeclareScreenSpaceOcclusionReads(builder, ctx);
    if (ctx.waterMediumPassActive)
        builder.Read("WaterMediumAtlas");
}

void ForwardOpaquePass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteForwardPasses(ctx);
}
}
