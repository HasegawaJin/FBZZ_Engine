// FBZZ Engine
// RenderPasses/ShadowPass.cpp | fbzz::scene
// シャドウマップ描画 (静的メッシュ + スキンドメッシュ)
#include "GeometryPasses.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Renderer/Mesh.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Renderer/DrawCall.hpp"

namespace fbzz::scene {

void ExecuteShadowPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& rs  = ctx.settings;

    renderer.SetRenderTarget(h.shadowMapRT, resources);
    renderer.ClearDepth();

    if (!rs.shadowEnabled) return;

    PerFrameCB lightFrameData{};
    lightFrameData.viewProjection = ctx.lightVP;
    resources.Update(h.frameCB, &lightFrameData, sizeof(PerFrameCB));

    // 静的メッシュのシャドウ
    // ライト視錐台カリング: シャドウマップに映らないオブジェクトのシャドウ DrawCall を省く。
    // WHY: シャドウマップは平行投影のため視錐台が直方体形状になる。
    //      光源から見えないジオメトリはシャドウを落とさないため除外して安全。
    const auto& lightFrustum = ctx.lightFrustum;

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* mr  = go.GetComponent<MeshRenderer>();
        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mr || !mr->enabled || !mr->mesh || !mat || !mat->enabled) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;

        // ライトフラスタムカリング
        if (!IsVisibleInFrustum(lightFrustum, go.transform, *mr->mesh)) continue;

        PerObjectCB objData{};
        objData.world = go.transform.GetWorldMatrix();
        resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));

        renderer::DrawCall dc;
        dc.vertexBuffer       = mr->mesh->vertexBuffer;
        dc.indexBuffer        = mr->mesh->indexBuffer;
        dc.indexCount         = mr->mesh->indexCount;
        dc.shader             = h.shadowShader;
        dc.pipelineState      = h.defaultPSO;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[1] = h.objectCB;
        renderer.Submit(dc, resources);
    }

    if (!h.shadowSkinnedShader.IsValid()) return;

    // スキンドメッシュのシャドウ
    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* smr  = go.GetComponent<SkinnedMeshRenderer>();
        auto* mat  = go.GetComponent<MaterialComponent>();
        auto* anim = go.GetComponent<AnimatorComponent>();
        if (!smr || !smr->enabled || !smr->model) continue;
        if (!mat || !mat->enabled) continue;

        PerObjectCB objData{};
        objData.world = go.transform.GetWorldMatrix();
        resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));

        const auto skinCB = (anim && anim->skinningBuffer.IsValid())
            ? anim->skinningBuffer : h.bindPoseSkinningCB;

        for (const auto& meshPtr : smr->model->meshes) {
            if (!meshPtr) continue;
            if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid()) continue;

            renderer::DrawCall dc;
            dc.vertexBuffer       = meshPtr->vertexBuffer;
            dc.indexBuffer        = meshPtr->indexBuffer;
            dc.indexCount         = meshPtr->indexCount;
            dc.shader             = h.shadowSkinnedShader;
            dc.pipelineState      = h.defaultPSO;
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[1] = h.objectCB;
            dc.constantBuffers[7] = skinCB;
            renderer.Submit(dc, resources);
        }
    }
}

} // namespace fbzz::scene
