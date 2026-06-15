// FBZZ Engine
// RenderPasses/ShadowPass.cpp | fbzz::scene
// シャドウマップ描画 (静的メッシュ + スキンドメッシュ + Terrain)
#include "GeometryPasses.hpp"
#include "Engine/Scene/Systems/TerrainRenderSystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Renderer/Mesh.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Renderer/DrawCall.hpp"

namespace fbzz::scene {

namespace {

// SubmitStaticMeshShadowCasters — MeshRenderer の静的メッシュをシャドウマップへ提出する。
// WHY: ShadowPass は「どのライトのどの影面へ描くか」を管理し、MeshRenderer 固有の
//      DrawCall 組み立ては caster 提出関数へ分離する。Spot / Point / CSM を追加するときも
//      ライトループから同じ提出関数を再利用できる。
void SubmitStaticMeshShadowCasters(RenderPassContext& ctx, const math::Frustum& lightFrustum)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* mr  = go.GetComponent<MeshRenderer>();
        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mr || !mr->enabled || !mr->mesh || !mat || !mat->enabled || !mat->EnsureMaterialAsset()) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;

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
}

// SubmitSkinnedMeshShadowCasters — SkinnedMeshRenderer をスキニング CB 付きで提出する。
// WHAT: Animator の skinningBuffer が未生成のフレームでは bind pose CB にフォールバックする。
void SubmitSkinnedMeshShadowCasters(RenderPassContext& ctx, const math::Frustum& lightFrustum)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!h.shadowSkinnedShader.IsValid())
        return;

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* smr  = go.GetComponent<SkinnedMeshRenderer>();
        auto* mat  = go.GetComponent<MaterialComponent>();
        auto* anim = FindAnimator(go);
        if (!smr || !smr->enabled || !smr->model) continue;
        if (!mat || !mat->enabled || !mat->EnsureMaterialAsset()) continue;
        if (!IsSkinnedVisibleInFrustum(lightFrustum, go.transform, *smr)) continue;

        PerObjectCB objData{};
        objData.world = go.transform.GetWorldMatrix();
        resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));

        const auto skinCB = (anim && anim->skinningBuffer.IsValid())
            ? anim->skinningBuffer : h.bindPoseSkinningCB;

        const int meshStart = (smr->meshIndex < 0) ? 0 : smr->meshIndex;
        const int meshEnd   = (smr->meshIndex < 0) ? static_cast<int>(smr->model->meshes.size()) : smr->meshIndex + 1;
        for (int mi = meshStart; mi < meshEnd; ++mi) {
            const auto& meshPtr = smr->model->meshes[mi];
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

// SubmitAllShadowCasters — 現在のライト視錐台に入る caster を種類別に提出する。
// WHY: ShadowPass 本体から caster 種別の詳細を追い出し、将来のライト別 shadow map 生成を
//      「ライト面を選ぶ → frameCB を更新 → caster を提出」の形に単純化する。
void SubmitAllShadowCasters(RenderPassContext& ctx, const math::Frustum& lightFrustum)
{
    SubmitStaticMeshShadowCasters(ctx, lightFrustum);
    SubmitSkinnedMeshShadowCasters(ctx, lightFrustum);
    SubmitTerrainShadowCasters(ctx.scene, ctx.renderer, ctx.resources, lightFrustum,
                               ctx.handles.shadowShader, ctx.handles.defaultPSO,
                               ctx.handles.frameCB, ctx.handles.objectCB);
}

} // namespace

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

    // ライト視錐台カリング: シャドウマップに映らないオブジェクトのシャドウ DrawCall を省く。
    // WHY: シャドウマップは平行投影のため視錐台が直方体形状になる。
    //      光源から見えないジオメトリはシャドウを落とさないため除外して安全。
    const auto& lightFrustum = *ctx.lightFrustum;
    SubmitAllShadowCasters(ctx, lightFrustum);
}

} // namespace fbzz::scene
