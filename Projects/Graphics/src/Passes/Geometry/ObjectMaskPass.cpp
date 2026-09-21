/// @file    ObjectMaskPass.cpp
/// @brief   申告された GameObject のシルエットを、値つきでマスクへ描くパス
/// @author  Hasegawa Jin
/// @date    2026-08-28

/// @note マスクの RGBA はエンジンが意味を決めない。申告した側 (スクリプト) と読む側
/// @note (カスタムパスのシェーダー) の取り決めで、輪郭の色にも強さにもボカし量にもなる。
#include "RenderScenePassHelpers.hpp"
#include <Graphics/Pipeline/InstanceBatch.hpp>
#include <Core/Logger.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Math/Matrix4.hpp>
#include <algorithm>

namespace fbzz::renderer {

namespace {

/// @note このフレームでマスクへ描くと決まったメッシュの数。申告・描画・後段のどこで切れても
/// @note 症状が同じ「何も見えない」になるため、RenderDoc を開かずに切り分けられるよう数える。
/// @note 実際に GPU へ出た回数とは一致しない (束ねると 1 回に畳まれる)。発行回数が要るときは
/// @note Analysis パネルの Draw calls / Draws saved を見ること。
int g_objectMaskDraws = 0;

/// @note 遮蔽判定はこのパスで完結させる。別 RT へ描くのでシーン深度 (HDR の深度) を自由に
/// @note 読める。visibleOnly が偽の申告はこの深度テストを素通しにし、壁越しシルエットを許す。
void DrawObjectMask(const renderer::RenderObject& object, RenderPassContext& ctx, InstanceBatcher& batcher)
{
    if (!MatchesView(ctx, object)) return;
    auto& h = ctx.handles;
    const auto& input = SceneInput(ctx);
    const auto shader = object.skinned ? h.objectMaskSkinnedShader : h.objectMaskShader;
    if (!shader.IsValid()) return;
    auto constants = ObjectConstants(object);
    constants.objectParams = {};
    for (uint32_t i = object.firstItem; i < object.firstItem + object.itemCount; ++i) {
        const auto& item = input.items[i];
        if (!item.vertexBuffer.IsValid() || !item.indexBuffer.IsValid() || (object.skinned && !item.slotVisible)) continue;
        renderer::DrawCall dc;
        dc.vertexBuffer = object.skinned ? item.skinningVertexBuffer : item.vertexBuffer;
        dc.indexBuffer = item.indexBuffer;
        dc.indexCount = item.indexCount;
        dc.vertexCount = item.vertexCount;
        dc.shader = shader;
        dc.pipelineState = h.selectionMaskPSO;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[2] = h.objectMaskCB;
        dc.textures[7] = ctx.resources.GetDepthTexture(ctx.Res().Target("HDR"));
        if (object.skinned) {
            batcher.Flush();
            ctx.resources.Update(h.objectCB, &constants, sizeof(constants));
            batcher.InvalidateObjectConstants();
            dc.constantBuffers[1] = h.objectCB;
            dc.constantBuffers[7] = object.skinningPalette;
            ctx.renderer.Submit(dc, ctx.resources);
        } else batcher.Add(dc, constants);
        ++g_objectMaskDraws;
    }
}

} /// @note namespace

void ExecuteObjectMaskPass(RenderPassContext& ctx)
{
    if (!ctx.objectMaskEnabled || !ctx.Res().Target("ObjectMask").IsValid()) return;

    auto& r = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;

    r.SetRenderTarget(ctx.Res().Target("ObjectMask"), resources);
    r.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });

    g_objectMaskDraws = 0;
    const auto& input = SceneInput(ctx);
    const int live = static_cast<int>(input.liveMaskRequests);
    const int resolved = static_cast<int>(input.maskGroups.size());
    for (const auto& group : input.maskGroups) {
        ObjectMaskCB maskData{};
        maskData.payload = group.payload;
        maskData.flags = { group.visibleOnly ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
        resources.Update(h.objectMaskCB, &maskData, sizeof(maskData));

        /// @note 束ねる範囲は申告 1 件ぶん。b2 (objectMaskCB) は申告ごとに書き換わるハンドル
        /// @note 共有の CB なので、跨いで束ねると先に積んだシルエットまで後の色で描かれる。
        /// @note スコープを抜けるときにデストラクタが吐き出す。
        /// @see Docs/design/gpu-instancing.md
        InstanceBatcher batcher(ctx, h.objectMaskShader,
                                ctx.settings.gpuInstancing
                                    ? h.objectMaskInstancedShader
                                    : renderer::ResourceHandle<renderer::ShaderTag>{},
                                false);
        for (uint32_t index : group.objectIndices) DrawObjectMask(input.objects[index], ctx, batcher);
    }

    /// @note 内訳が変わったときだけ 1 行。毎フレーム出すとログが埋まって本当のエラーが見えなくなる
    /// @note (RenderSystem の «無視される設定» の報告と同じ扱い)。
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
} /// @note namespace fbzz::renderer
