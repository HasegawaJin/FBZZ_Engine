// FBZZ Engine
// DecalPass.cpp | fbzz::scene
// Deferred デカールパス
//
// 各 DecalComponent に対してフルスクリーントライアングルを 1 draw 発行する。
// PS が深度バッファからワールド座標を復元し、デカール OBB 外のフラグメントを
// discard することで表面への投影を実現する。
//
// receiverLayerMask が Layer::Everything でない場合:
//   除外レイヤーのオブジェクトを decalMaskRT に白く描画し、
//   textureMask bit3 を立てて PS 側でそのピクセルを discard させる。
#include "GeometryPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Math/Matrix4.hpp>
#include <Physics/Layer.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace fbzz::scene {

// 除外オブジェクトを decalMaskRT に描画するヘルパー。
// receiverLayerMask に含まれない GameObject を白で描画し、
// Decal PS が textureMask bit3 でサンプルして discard する。
static void RenderDecalMask(RenderPassContext& ctx, fbzz::LayerMask receiverLayerMask)
{
    auto& r         = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!h.decalMaskRT.IsValid() || !h.decalMaskShader.IsValid() || !h.decalMaskPSO.IsValid())
        return;

    r.SetRenderTarget(h.decalMaskRT, resources);
    r.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });

    for (auto& go : ctx.scene.GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        if (!fbzz::Layer::Contains(ctx.cullingMask, go.layer)) continue;
        // receiverLayerMask に含まれているレイヤーはデカールを受け取る → マスクに描かない
        if (fbzz::Layer::Contains(receiverLayerMask, go.layer)) continue;

        PerObjectCB objData{};
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(objData.world));
        resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));

        // 静的メッシュ
        auto* mr = go.GetComponent<MeshRenderer>();
        if (mr && mr->enabled && mr->mesh && !mr->mesh->isSkinned &&
            mr->mesh->vertexBuffer.IsValid() && mr->mesh->indexBuffer.IsValid())
        {
            renderer::DrawCall dc;
            dc.vertexBuffer       = mr->mesh->vertexBuffer;
            dc.indexBuffer        = mr->mesh->indexBuffer;
            dc.indexCount         = mr->mesh->indexCount;
            dc.shader             = h.decalMaskShader;
            dc.pipelineState      = h.decalMaskPSO;
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[1] = h.objectCB;
            r.Submit(dc, resources);
        }

        // スキンドメッシュ — selectionMaskSkinnedShader を流用 (出力は同じ白)
        if (h.selectionMaskSkinnedShader.IsValid()) {
            auto* smr  = go.GetComponent<SkinnedMeshRenderer>();
            auto* anim = go.GetComponent<AnimatorComponent>();
            if (smr && smr->enabled && smr->model) {
                const auto skinCB = (anim && anim->skinningBuffer.IsValid())
                    ? anim->skinningBuffer : h.bindPoseSkinningCB;

                for (const auto& meshPtr : smr->model->meshes) {
                    if (!meshPtr) continue;
                    if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid()) continue;

                    renderer::DrawCall dc;
                    dc.vertexBuffer       = meshPtr->vertexBuffer;
                    dc.indexBuffer        = meshPtr->indexBuffer;
                    dc.indexCount         = meshPtr->indexCount;
                    dc.shader             = h.selectionMaskSkinnedShader;
                    dc.pipelineState      = h.decalMaskPSO;
                    dc.constantBuffers[0] = h.frameCB;
                    dc.constantBuffers[1] = h.objectCB;
                    dc.constantBuffers[7] = skinCB;
                    r.Submit(dc, resources);
                }
            }
        }
    }
}

void ExecuteDecalPass(RenderPassContext& ctx)
{
    auto& r         = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!h.decalShader.IsValid() || !h.decalPSO.IsValid() || !h.decalCB.IsValid())
        return;

    // decalDepthRT はデカールパス直前にコピーされた深度専用 RT。
    // hdrRT の深度をそのまま使うと DX11 が DSV/SRV 競合で SRV をサイレント解除する。
    const auto depthTex = resources.GetDepthTexture(h.decalDepthRT);
    if (!depthTex.IsValid())
        return;

    const float dt = Time::deltaTime;
    std::vector<EntityID> expiredDecals;

    for (auto& go : ctx.scene.GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        if (!fbzz::Layer::Contains(ctx.cullingMask, go.layer)) continue;
        auto* dc = go.GetComponent<DecalComponent>();
        if (!dc || !dc->enabled) continue;

        // ライフタイム更新
        if (dc->lifetime >= 0.0f) {
            dc->age += dt;
            if (dc->age >= dc->lifetime) {
                dc->enabled = false;
                expiredDecals.push_back(go.GetID());
                continue;
            }
        }

        // フェードアルファ算出
        float alpha = dc->albedoColor[3];
        if (dc->lifetime >= 0.0f && dc->fadeTime > 0.0f) {
            const float remaining = dc->lifetime - dc->age;
            alpha *= std::min(1.0f, remaining / dc->fadeTime);
        }
        if (alpha < 0.001f) continue;

        // receiverLayerMask が全許可でない場合はマスクを生成する
        const bool needsMask = (dc->receiverLayerMask != fbzz::Layer::Everything);
        if (needsMask)
            RenderDecalMask(ctx, dc->receiverLayerMask);

        // HDR RT に戻してデカールを描画
        r.SetRenderTarget(h.hdrRT, resources);

        auto loadTex = [&](const std::string& path) {
            return path.empty()
                ? renderer::ResourceHandle<renderer::TextureTag>{}
                : resources.LoadTexture(path);
        };
        const auto albedoTex   = loadTex(dc->albedoTexPath);
        const auto normalTex   = loadTex(dc->normalTexPath);
        const auto emissiveTex = loadTex(dc->emissiveTexPath);

        // 定数バッファ更新
        DecalCB data{};
        data.invDecalWorld    = math::Matrix4::Inverse(go.transform.GetWorldMatrix());
        data.albedo[0]        = dc->albedoColor[0];
        data.albedo[1]        = dc->albedoColor[1];
        data.albedo[2]        = dc->albedoColor[2];
        data.albedo[3]        = 1.0f;
        data.emissiveColor[0] = dc->emissiveColor[0];
        data.emissiveColor[1] = dc->emissiveColor[1];
        data.emissiveColor[2] = dc->emissiveColor[2];
        data.emissiveScale    = dc->emissiveScale;
        data.normalStrength   = dc->normalStrength;
        data.alpha            = alpha;
        data.textureMask      = (albedoTex.IsValid()          ? 1u : 0u)
                              | (normalTex.IsValid()          ? 2u : 0u)
                              | (emissiveTex.IsValid()        ? 4u : 0u)
                              | (needsMask                    ? 8u : 0u);
        data.decalTangent     = go.transform.right.Normalized();
        data.decalBitangent   = go.transform.forward.Normalized();
        data.decalNormal      = go.transform.up.Normalized();
        resources.Update(h.decalCB, &data, sizeof(DecalCB));

        renderer::DrawCall drawCall;
        drawCall.shader             = h.decalShader;
        drawCall.pipelineState      = h.decalPSO;
        drawCall.vertexCount        = 3;
        drawCall.constantBuffers[0] = h.frameCB;
        drawCall.constantBuffers[2] = h.decalCB;
        drawCall.constantBuffers[3] = h.lightCB;
        if (albedoTex.IsValid())   drawCall.textures[0] = albedoTex;
        if (normalTex.IsValid())   drawCall.textures[1] = normalTex;
        if (emissiveTex.IsValid()) drawCall.textures[3] = emissiveTex;
        drawCall.textures[7] = depthTex;
        if (needsMask && h.decalMaskRT.IsValid())
            drawCall.textures[13] = resources.GetColorTexture(h.decalMaskRT, 0);
        r.Submit(drawCall, resources);
    }

    // WHY: GameObjects() の走査中に即時削除すると iterator が無効化されるため、pass 後に破棄キューへ積む。
    for (EntityID id : expiredDecals) {
        ctx.scene.DestroyGameObject(id);
    }
}

} // namespace fbzz::scene
