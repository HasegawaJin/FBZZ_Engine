/// @file    ObjectMaskPass.cpp
/// @brief   申告された GameObject のシルエットを、値つきでマスクへ描くパス
/// @author  Hasegawa Jin
/// @date    2026-08-28
///
/// マスクの RGBA はエンジンが意味を決めない。申告した側 (スクリプト) と読む側
/// (カスタムパスのシェーダー) の取り決めで、輪郭の色にも強さにもボカし量にもなる。
#include "GeometryPasses.hpp"
#include <Engine/Core/Logger.hpp>
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

/// このフレームでマスクへ実際に発行したドローコール数。申告・描画・後段のどこで切れても
/// 症状が同じ「何も見えない」になるため、RenderDoc を開かずに切り分けられるよう数える。
int g_objectMaskDraws = 0;

/// @note 遮蔽判定はこのパスで完結させる。別 RT へ描くのでシーン深度 (HDR の深度) を自由に
///       読める。visibleOnly が偽の申告はこの深度テストを素通しにし、壁越しシルエットを許す。
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
            dc.textures[7] = resources.GetDepthTexture(ctx.Res().Target("HDR"));
            r.Submit(dc, resources);
            ++g_objectMaskDraws;
        }
    }

    if (!h.objectMaskSkinnedShader.IsValid()) return;

    auto* smr = go.GetComponent<SkinnedMeshRenderer>();
    if (!smr || !smr->enabled || !smr->lodVisible || !smr->model) return;

    /// @note Animator はモデルルート側、SkinnedMeshRenderer は submesh の子 GO に分かれる構成が
    ///       普通なので、通常描画と同じ親方向探索で解決する。自 GO だけを見ると bind pose へ
    ///       落ちて、輪郭だけが T ポーズで止まる。
    auto* anim = FindAnimator(go);
    const auto skinCB = ResolveSkinningCB(
        anim ? anim->skinningBuffer : decltype(anim->skinningBuffer){},
        smr->model, h.bindPoseSkinningCB);

    const auto* mat = go.GetComponent<MaterialComponent>();
    for (size_t mi = 0; mi < smr->SubmeshCount(); ++mi) {
        renderer::Mesh* meshPtr = smr->SubmeshMesh(mi);
        if (!meshPtr) continue;
        if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid()) continue;
        /// @note 非表示スロットまで描くと、隠してあるメッシュの分だけ輪郭が膨らむ。
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
        dc.textures[7] = resources.GetDepthTexture(ctx.Res().Target("HDR"));
        r.Submit(dc, resources);
        ++g_objectMaskDraws;
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
    if (!ctx.objectMaskEnabled || !ctx.Res().Target("ObjectMask").IsValid()) return;

    auto& r = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;

    r.SetRenderTarget(ctx.Res().Target("ObjectMask"), resources);
    r.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });

    g_objectMaskDraws = 0;
    int live    = 0;
    int resolved = 0;

    for (const renderer::RenderObjectMaskRequest& request : ctx.settings.objectMaskRequests) {
        if (!renderer::IsObjectMaskRequestLive(request, Time::frameCount)) continue;
        ++live;

        GameObject* root = ctx.scene.GetGameObject(
            EntityID{ request.id.index, request.id.generation });
        if (!root) continue;
        ++resolved;

        /// @note 値は申告 (GameObject) ごとに持つ。パス共通の定数にすると、同一フレームに
        ///       並ぶ複数のシルエットを色や強さで区別できなくなる。
        ObjectMaskCB maskData{};
        maskData.payload = {
            request.color[0],
            request.color[1],
            request.color[2],
            /// @note 0 は «描かれていない» とクリア値の区別が付かない。値 0 の申告は
            ///       «居ないこと» ではなく «弱いこと» なので、下限を持たせる。
            std::clamp(request.value, 0.02f, 1.0f)
        };
        maskData.flags = { request.visibleOnly ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
        resources.Update(h.objectMaskCB, &maskData, sizeof(ObjectMaskCB));

        DrawSubtreeMask(*root, request.includeChildren, ctx);
    }

    /// @note 内訳が変わったときだけ 1 行。毎フレーム出すとログが埋まって本当のエラーが見えなくなる
    ///       (RenderSystem の «無視される設定» の報告と同じ扱い)。
    {
        static int sLive = -1;
        static int sResolved = -1;
        static int sDraws = -1;
        if (live != sLive || resolved != sResolved || g_objectMaskDraws != sDraws) {
            sLive = live;
            sResolved = resolved;
            sDraws = g_objectMaskDraws;
            FBZZ_LOG_INFO("ObjectMask: live=%d resolved=%d draws=%d "
                          "(live=0 なら申告が届いていない / draws=0 なら描く物が無い)",
                          live, resolved, g_objectMaskDraws);
        }
    }

    r.SetRenderTarget(ctx.Res().Target("HDR"), resources);
}


void ObjectMaskPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("HDR").Write("ObjectMask");
}

bool ObjectMaskPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.objectMaskEnabled;
}

void ObjectMaskPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteObjectMaskPass(ctx);
}
} // namespace fbzz::scene
