/// @file    ObjectMaskPass.cpp
/// @brief   申告された GameObject のシルエットを、値つきでマスクへ描くパス
/// @author  Hasegawa Jin
/// @date    2026-08-28
///
/// マスクの中身にエンジンは意味を持たない。RGBA は申告した側 (スクリプト) と
/// 読む側 (カスタムパスのシェーダー) の取り決めで、輪郭の色にも、光らせる強さにも、
/// «ここはボカすな» の重みにもなる。
///
/// WHY 値を申告ごとに持つか (パス側の定数にせず):
///   同じフレームに違う意味のシルエットが並ぶ。定数 1 本にすると «誰の印か» を
///   後段が区別できず、色や強さで意味を分ける表現が成立しない。
///   マスクの 1 フェッチで全部揃うので、後段は対象を数える必要もなくなる。
///
/// WHY 遮蔽の判定をここで済ませるか:
///   読む側が HDR の段で走ると、描き先 (hdrRT) の深度を同時には読めない。
///   ここは別の RT へ描いているのでシーン深度を自由に読める。«見えている面だけ»
///   にするかどうかは申告ごとの visibleOnly が決める (壁越しシルエットは false)。
#include "GeometryPasses.hpp"
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/Matrix4.hpp>
#include <algorithm>

namespace fbzz::scene {

namespace {

void DrawObjectMask(GameObject& go, RenderPassContext& ctx)
{
    auto& r = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;

    PerObjectCB objData{};
    objData.world = go.transform.GetWorldMatrix();
    objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(objData.world);
    resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));

    if (h.objectMaskShader.IsValid()) {
        auto* mr = go.GetComponent<MeshRenderer>();
        if (mr && mr->enabled && mr->lodVisible && mr->mesh && !mr->mesh->isSkinned &&
            mr->mesh->vertexBuffer.IsValid() && mr->mesh->indexBuffer.IsValid())
        {
            renderer::DrawCall dc;
            dc.vertexBuffer = mr->mesh->vertexBuffer;
            dc.indexBuffer = mr->mesh->indexBuffer;
            dc.indexCount = mr->mesh->indexCount;
            dc.vertexCount = mr->mesh->vertexCount;
            dc.shader = h.objectMaskShader;
            dc.pipelineState = h.selectionMaskPSO;
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[1] = h.objectCB;
            dc.constantBuffers[2] = h.objectMaskCB;
            dc.textures[7] = resources.GetDepthTexture(h.hdrRT);
            r.Submit(dc, resources);
        }
    }

    if (!h.objectMaskSkinnedShader.IsValid()) return;

    auto* smr = go.GetComponent<SkinnedMeshRenderer>();
    if (!smr || !smr->enabled || !smr->lodVisible || !smr->model) return;

    // Animator はモデルルート側、SkinnedMeshRenderer は submesh の子 GO に分かれる構成が
    // 普通なので、通常描画と同じ親方向探索で解決する。自 GO だけを見ると bind pose へ
    // 落ちて、輪郭だけが T ポーズで止まる。
    auto* anim = FindAnimator(go);
    const auto skinCB = ResolveSkinningCB(
        anim ? anim->skinningBuffer : decltype(anim->skinningBuffer){},
        smr->model, h.bindPoseSkinningCB);

    const auto* mat = go.GetComponent<MaterialComponent>();
    for (size_t mi = 0; mi < smr->SubmeshCount(); ++mi) {
        renderer::Mesh* meshPtr = smr->SubmeshMesh(mi);
        if (!meshPtr) continue;
        if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid()) continue;
        // 非表示スロットまで描くと、隠してあるメッシュの分だけ輪郭が膨らむ。
        if (mat && !mat->SlotAt(mi).visible) continue;

        renderer::DrawCall dc;
        dc.vertexBuffer = smr->ResolveSlotVertexBuffer(mi, meshPtr->vertexBuffer);
        dc.indexBuffer = meshPtr->indexBuffer;
        dc.indexCount = meshPtr->indexCount;
        dc.vertexCount = meshPtr->vertexCount;
        dc.shader = h.objectMaskSkinnedShader;
        dc.pipelineState = h.selectionMaskPSO;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[1] = h.objectCB;
        dc.constantBuffers[2] = h.objectMaskCB;
        dc.constantBuffers[7] = skinCB;
        dc.textures[7] = resources.GetDepthTexture(h.hdrRT);
        r.Submit(dc, resources);
    }
}

void DrawSubtreeMask(GameObject& go, bool includeChildren, RenderPassContext& ctx)
{
    if (!go.activeInHierarchy()) return;
    if (!fbzz::Layer::Contains(ctx.cullingMask, go.layer)) return;

    DrawObjectMask(go, ctx);
    if (!includeChildren) return;

    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (GameObject* child = go.GetChild(i))
            DrawSubtreeMask(*child, true, ctx);
    }
}

} // namespace

void ExecuteObjectMaskPass(RenderPassContext& ctx)
{
    if (!ctx.objectMaskEnabled || !ctx.handles.objectMaskRT.IsValid()) return;

    auto& r = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;

    r.SetRenderTarget(h.objectMaskRT, resources);
    r.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });

    for (const renderer::RenderObjectMaskRequest& request : ctx.settings.objectMaskRequests) {
        if (!renderer::IsObjectMaskRequestLive(request, Time::frameCount)) continue;

        GameObject* root = ctx.scene.GetGameObject(
            EntityID{ request.id.index, request.id.generation });
        if (!root) continue;

        ObjectMaskCB maskData{};
        maskData.payload = {
            request.color[0],
            request.color[1],
            request.color[2],
            // 0 は «描かれていない» とクリア値の区別が付かない。値 0 の申告は
            // «居ないこと» ではなく «弱いこと» なので、下限を持たせる。
            std::clamp(request.value, 0.02f, 1.0f)
        };
        maskData.flags = { request.visibleOnly ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
        resources.Update(h.objectMaskCB, &maskData, sizeof(ObjectMaskCB));

        DrawSubtreeMask(*root, request.includeChildren, ctx);
    }

    r.SetRenderTarget(h.hdrRT, resources);
}

} // namespace fbzz::scene
