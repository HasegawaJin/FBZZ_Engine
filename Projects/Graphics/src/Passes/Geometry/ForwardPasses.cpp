/// @file    ForwardPasses.cpp
/// @brief   抽出済み RenderScene の不透明・透明メッシュを前方描画する。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "RenderScenePassHelpers.hpp"
#include <Graphics/Passes/Geometry/FiberRenderPass.hpp>
#include <algorithm>
#include <vector>

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
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    const auto& input = SceneInput(ctx);
    ctx.renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
    ClearForCamera(ctx.renderer, ctx.camera);
    const auto frame = MakeCameraFrameCB(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    resources.Update(h.frameCB, &frame, sizeof(frame));
    resources.Update(h.lightCB, &ctx.lightData, sizeof(renderer::LightConstantsCB));
    UpdateShadowConstants(ctx);
    UpdatePunctualShadowConstants(ctx);
    std::vector<Entry> staticOpaque, skinnedOpaque, transparent;
    /// @note 同距離の旧来の投入順を保つため、静的候補を先に、スキンド候補を後に収集する。
    for (bool skinned : { false, true }) {
        for (const auto& object : input.objects) {
            if (object.skinned != skinned || !object.colorEligible || !MatchesView(ctx, object)) continue;
            if (!skinned && !HasColorGeometry(input.items[object.firstItem])) continue;
            ++ctx.statsTotalObjects;
            if (!IsRenderObjectVisible(ctx, object)) continue;
            const float distance = (object.sortPosition - ctx.camera.m_position).LengthSq();
            for (uint32_t i = object.firstItem; i < object.firstItem + object.itemCount; ++i) {
                const auto& item = input.items[i];
                if (!HasColorGeometry(item)) continue;
                Entry entry{ &object, &item, distance };
                if (renderer::ResolveGeometryRoute(item.material.capabilities, false)
                    == renderer::GeometryRoute::ForwardTransparent) transparent.push_back(entry);
                else if (skinned) skinnedOpaque.push_back(entry);
                else staticOpaque.push_back(entry);
            }
        }
    }
    const auto frontToBack = [](const Entry& a, const Entry& b) {
        if (a.item->material.renderQueue != b.item->material.renderQueue)
            return a.item->material.renderQueue < b.item->material.renderQueue;
        return a.distanceSquared < b.distanceSquared;
    };
    std::sort(staticOpaque.begin(), staticOpaque.end(), frontToBack);
    std::sort(skinnedOpaque.begin(), skinnedOpaque.end(), frontToBack);
    const bool occlusion = ctx.occlusionCuller && ctx.occlusionCullingEnabled;
    if (occlusion) ctx.occlusionCuller->Reset(ctx.camera);
    auto submit = [&](const Entry& entry, bool bindIbl) {
        if (!entry.item->forwardMaterial.valid || !entry.item->forwardMaterial.shader.IsValid()) return;
        const auto constants = ObjectConstants(*entry.object);
        resources.Update(h.objectCB, &constants, sizeof(constants));
        SubmitCounted(ctx, ForwardDraw(ctx, *entry.object, *entry.item, bindIbl));
    };
    for (const auto& entry : staticOpaque) {
        if (!entry.item->forwardMaterial.valid || !entry.item->forwardMaterial.shader.IsValid()) continue;
        const auto bounds = RenderBounds(ctx, *entry.object);
        if (occlusion && !ctx.occlusionCuller->TestAndRaster(bounds.center, bounds.radius,
                                                            entry.item->reliableOccluder)) {
            ++ctx.statsOcclusionCulled;
            continue;
        }
        submit(entry, true);
    }
    /// @note 既存 Forward の不透明スキンドは IBL テクスチャを束縛しない。構造移行中は挙動を維持する。
    for (const auto& entry : skinnedOpaque) submit(entry, false);
    ExecuteFiberPass(ctx);
    std::sort(transparent.begin(), transparent.end(), [](const Entry& a, const Entry& b) {
        if (a.item->material.renderQueue != b.item->material.renderQueue)
            return a.item->material.renderQueue < b.item->material.renderQueue;
        return a.distanceSquared > b.distanceSquared;
    });
    for (const auto& entry : transparent) submit(entry, true);
}

void ForwardOpaquePass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    builder.Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas").Write("HDR");
    /// @note GBuffer プリパスが走ったフレームだけ画面空間の遮蔽が存在する。
    /// @note 走っていないのに申告すると、誰も書かない名前を読むことになり Plan が落ちる。
    if (ctx.gbufferDepthReady)
        DeclareScreenSpaceOcclusionReads(builder, ctx);
}

void ForwardOpaquePass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteForwardPasses(ctx);
}
}
