/// @file    DecalPass.cpp
/// @brief   Deferred デカールパス
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// @note 各 DecalComponent に対してフルスクリーントライアングルを 1 draw 発行する。
/// @note PS が深度バッファからワールド座標を復元し、デカール OBB 外のフラグメントを
/// @note discard することで表面への投影を実現する。
///
/// @note マテリアルは 2 経路ある:
/// @note materialPath 空  → 組み込み Decal.hlsl + コンポーネントの色・テクスチャ
/// @note materialPath 有り → .mat (render_path = "decal") のシェーダーとパラメータ
/// @note どちらの経路でも投影ボリューム・角度フェード・ライフタイムは b10 の
/// @note DecalConstants がエンジン側から埋める (DecalCommon.hlsli 参照)。
#include "RenderScenePassHelpers.hpp"
#include <algorithm>
namespace fbzz::renderer {
namespace {
bool ContainsLayer(uint32_t mask, uint32_t layer) { return (mask & (1u << (layer & 31u))) != 0; }
bool RenderDecalReceiverLayers(RenderPassContext& ctx, uint32_t markMask)
{
    auto& r         = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!h.decalMaskRT.IsValid() || !h.decalMaskShader.IsValid() ||
        !h.decalMaskPSO.IsValid() || !h.decalReceiverCB.IsValid())
        return false;

    const auto depthTex = resources.GetDepthTexture(ctx.Res().Target("DecalDepth"));
    if (!depthTex.IsValid()) return false;

    r.SetRenderTarget(h.decalMaskRT, resources);
    r.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });

    const auto& input = SceneInput(ctx);
    for (const auto& object : input.objects) {
        if (!MatchesView(ctx, object)) continue;
        const bool markStatic = !object.skinned && ContainsLayer(markMask, 9);
        bool staticSiblingMarked = false;
        if (object.skinned && ContainsLayer(markMask, 9)) {
            for (const auto& sibling : input.objects)
                if (!sibling.skinned && sibling.sourceIndex == object.sourceIndex && MatchesView(ctx, sibling)) {
                    const auto& mesh = input.items[sibling.firstItem];
                    staticSiblingMarked = mesh.vertexBuffer.IsValid() && mesh.indexBuffer.IsValid();
                    break;
                }
        }
        const bool markSkinned = object.skinned && !staticSiblingMarked &&
            h.decalMaskSkinnedShader.IsValid() && ContainsLayer(markMask, object.layer);
        if (!markStatic && !markSkinned) continue;
        auto constants = ObjectConstants(object);
        constants.objectParams = {};
        resources.Update(h.objectCB, &constants, sizeof(constants));
        DecalReceiverCB layerData{};
        const int receiverLayer = markStatic ? 9 : static_cast<int>(object.layer);
        layerData.layerEncoded = static_cast<float>((receiverLayer & 31) + 1);
        resources.Update(h.decalReceiverCB, &layerData, sizeof(layerData));
        for (uint32_t i = object.firstItem; i < object.firstItem + object.itemCount; ++i) {
            const auto& item = input.items[i];
            if (!item.vertexBuffer.IsValid() || !item.indexBuffer.IsValid() || (object.skinned && !item.slotVisible)) continue;
            renderer::DrawCall dc;
            dc.vertexBuffer = object.skinned ? item.skinningVertexBuffer : item.vertexBuffer;
            dc.indexBuffer = item.indexBuffer;
            dc.indexCount = item.indexCount;
            dc.shader = object.skinned ? h.decalMaskSkinnedShader : h.decalMaskShader;
            dc.pipelineState = h.decalMaskPSO;
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[1] = h.objectCB;
            if (object.skinned) dc.constantBuffers[7] = object.skinningPalette;
            dc.constantBuffers[10] = h.decalReceiverCB;
            dc.textures[7] = depthTex;
            r.Submit(dc, resources);
        }
    }
    return true;
}
}
void ExecuteDecalPass(RenderPassContext& ctx) {
    auto& r = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    r.SetRenderTarget(ctx.Res().Target("HDR"), resources);
    if (!ctx.renderScene || !h.decalShader.IsValid() || !h.decalPSO.IsValid() || !h.decalCB.IsValid() || !h.decalMaterialCB.IsValid()) return;
    const auto depthTex = resources.GetDepthTexture(ctx.Res().Target("DecalDepth"));
    if (!depthTex.IsValid()) return;
    std::vector<const RenderDecalInput*> pending;
    uint32_t alwaysReceive = ~0u;
    bool anyFiltered = false;
    for (const auto& input : ctx.renderScene->decals) {
        if (!ContainsLayer(ctx.cullingMask, input.layer)) continue;
        if (input.projection.receiverLayerMask != ~0u) { alwaysReceive &= input.projection.receiverLayerMask; anyFiltered = true; }
        pending.push_back(&input);
    }
    std::stable_sort(pending.begin(), pending.end(), [](const auto* a, const auto* b) { return a->sortOrder < b->sortOrder; });
    const bool receiverBufferReady = anyFiltered && RenderDecalReceiverLayers(ctx, ~alwaysReceive);
    if (receiverBufferReady) r.SetRenderTarget(ctx.Res().Target("HDR"), resources);
    for (const auto* input : pending) {
        auto projection = input->projection;
        const bool filtered = receiverBufferReady && projection.receiverLayerMask != ~0u;
        projection.flags = filtered ? kDecalFlagReceiverFilter : 0u;
        resources.Update(h.decalCB, &projection, sizeof(projection));
        auto drawCall = input->binding;
        drawCall.vertexCount = 3;
        drawCall.constantBuffers[0] = h.frameCB;
        drawCall.constantBuffers[3] = h.lightCB;
        drawCall.constantBuffers[10] = h.decalCB;
        drawCall.textures[7] = depthTex;
        if (filtered) drawCall.textures[13] = resources.GetColorTexture(h.decalMaskRT, 0);
        if (!input->customMaterial) resources.Update(h.decalMaterialCB, &input->material, sizeof(input->material));
        else if (!input->materialParameters.empty()) resources.Update(drawCall.constantBuffers[2], input->materialParameters.data(), input->materialParameters.size());
        SubmitCounted(ctx, drawCall);
    }
}
void ExecuteDecalDepthCopyPass(RenderPassContext& ctx)
{
    auto& r         = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    r.SetRenderTarget(ctx.Res().Target("DecalDepth"), resources);
    r.ClearDepth();
    if (!h.depthCopyShader.IsValid()) return;

    renderer::DrawCall dc;
    dc.shader        = h.depthCopyShader;
    dc.pipelineState = h.defaultPSO;
    dc.vertexCount   = 3;
    dc.textures[7]   = ctx.isDeferred ? resources.GetDepthTexture(ctx.Res().Target("GBuffer"))
                                      : resources.GetDepthTexture(ctx.Res().Target("HDR"));
    r.Submit(dc, resources);
}


void DecalDepthCopyPass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    builder.Read(ctx.isDeferred ? "GBuffer" : "HDR").Write("DecalDepth");
}

void DecalDepthCopyPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteDecalDepthCopyPass(ctx);
}

void DecalPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("DecalDepth").ReadWrite("HDR");
}

void DecalPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteDecalPass(ctx);
}
} /// @note namespace fbzz::renderer
