/// @file    VelocityPass.cpp
/// @brief   不透明ジオメトリのモーションベクターを専用 RT へ描く
/// @author  Hasegawa Jin
/// @date    2026-08-25
///
/// @note TAA とモーションブラーはこれまで深度 + prevViewProjection の再投影だけを使っており、
/// @note 復元できるのはカメラの動きに限られていた。動くオブジェクトは「静止している」と
/// @note 判定されるため、TAA では輪郭に尾を引き、カメラを止めるとモーションブラーが
/// @note 一切かからなかった。このパスが両者の欠けていた入力を供給する。
///
/// @note Forward / Deferred の両経路へ同じ速度入力を供給するため、専用 RT を使う。

#include "RenderScenePassHelpers.hpp"
#include <Graphics/Passes/Geometry/FiberRenderPass.hpp>
#include <Graphics/Pipeline/InstanceBatch.hpp>

#include "Graphics/Renderer/DrawCall.hpp"
#include "Graphics/Renderer/ResourceManager.hpp"
#include <Math/Matrix4.hpp>
#include <Math/Vector4.hpp>

#include <cstddef>
#include <cstdint>

namespace fbzz::renderer {

namespace {

/// @note Velocity.hlsl / VelocitySkinned.hlsl の b1。ObjectConstants と同じ大きさで、
/// @note 2 枠目の意味だけが worldInvTranspose から prevWorld へ変わる。
///
/// @note b1 の末尾に前パスの値を残さないよう、PerObjectCB と同じサイズで更新する。
struct VelocityObjectCB {
    math::Matrix4 world;
    math::Matrix4 prevWorld;
    math::Vector4 objectParams;
};
static_assert(sizeof(VelocityObjectCB) == sizeof(PerObjectCB),
              "VelocityObjectCB must fit the ObjectConstants (b1) slot");

bool SameMatrix(const math::Matrix4& a, const math::Matrix4& b)
{
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            if (a.m[row][col] != b.m[row][col]) return false;
    return true;
}

} /// @note namespace

void ExecuteVelocityPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!ctx.Res().Target("Velocity").IsValid() || !h.velocityShader.IsValid()) return;

    renderer.SetRenderTarget(ctx.Res().Target("Velocity"), resources);
    /// @note B チャンネルが 0 の画素は「Velocity パスが触っていない」= 空・未描画。
    /// @note TAA / MotionBlur はそこだけ従来の深度再投影へ落ちる。
    renderer.Clear(math::Vector4{ 0.0f, 0.0f, 0.0f, 0.0f });

    /// @note ジッターを載せないこと。prevViewProjection (b8) もジッター無しで保存されており、
    /// @note 片側だけジッターを載せると半ピクセルの揺れがそのまま「動き」として出力される。
    const PerFrameCB frameData = MakeCameraFrameCB(ctx.camera, 0.0f, 0.0f);
    resources.Update(h.frameCB, &frameData, sizeof(PerFrameCB));

    const auto opaquePSO =
        GetOrCreateMaterialPSO(resources, renderer::BlendMode::OPAQUE_BLEND, false);
    const auto sceneDepth = resources.GetDepthTexture(ctx.Res().Target("HDR"));

    /// @note 本描画パスと同じ物体をもう一度判定するので、カリング統計はパスを抜けるときに打ち消す。
    const CullStatsRollback rollback(ctx);

    /// @note 速度を描くのは «動いた物体» だけなので、束ねられるのは同じメッシュが揃って動いて
    /// @note いるとき (回る歯車・同じ動きの群れ) に限られる。
    /// @see Docs/design/gpu-instancing.md
    InstanceBatcher batcher(ctx, h.velocityShader,
                            ctx.settings.gpuInstancing
                                ? h.velocityInstancedShader
                                : renderer::ResourceHandle<renderer::ShaderTag>{},
                            false);

    const auto& input = SceneInput(ctx);
    for (const auto& object : input.objects) {
        if (object.skinned || !MatchesView(ctx, object) || !IsRenderObjectVisible(ctx, object)) continue;
        const auto& item = input.items[object.firstItem];
        if (!item.vertexBuffer.IsValid() || !item.indexBuffer.IsValid() ||
            item.material.capabilities.blend != renderer::BlendMode::OPAQUE_BLEND) continue;
        if (SameMatrix(object.world, object.previousWorld)) continue;
        renderer::DrawCall dc;
        dc.vertexBuffer = item.vertexBuffer;
        dc.indexBuffer = item.indexBuffer;
        dc.indexCount = item.indexCount;
        dc.vertexCount = item.vertexCount;
        dc.shader = h.velocityShader;
        dc.pipelineState = opaquePSO;
        dc.layer = renderer::RenderLayer::OPAQUE_LAYER;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[8] = h.advancedGraphicsCB;
        dc.textures[7] = sceneDepth;
        PerObjectCB constants{};
        constants.world = object.world;
        constants.worldInvTranspose = object.previousWorld;
        batcher.Add(dc, constants);
    }
    /// @note 繊維・スキンドへ進む前に吐き出す。b1 の意味 (2 枠目) が変わるので跨がせない。
    batcher.Flush();

    /// @name スキンドメッシュ
    /// @note world が静止していても風だけで繊維が動く。土台の SameMatrix 判定とは独立して提出する。
    ExecuteFiberVelocityPass(ctx);
    resources.Update(h.frameCB, &frameData, sizeof(frameData));

    if (!h.velocitySkinnedShader.IsValid()) return;

    for (const auto& object : input.objects) {
        if (!object.skinned || !MatchesView(ctx, object) || !object.previousSkinningValid ||
            !IsRenderObjectVisible(ctx, object)) continue;
        const VelocityObjectCB constants{ object.world, object.previousWorld, {} };
        resources.Update(h.objectCB, &constants, sizeof(constants));
        for (uint32_t i = object.firstItem; i < object.firstItem + object.itemCount; ++i) {
            const auto& item = input.items[i];
            if (!item.slotVisible || !item.vertexBuffer.IsValid() || !item.indexBuffer.IsValid() ||
                item.material.capabilities.blend != renderer::BlendMode::OPAQUE_BLEND) continue;
            renderer::DrawCall dc;
            /// @note 前フレームの頂点位置は計算済み VB に無いため、現在・過去のパレットを VS で評価する。
            dc.vertexBuffer = item.skinningVertexBuffer;
            dc.indexBuffer = item.indexBuffer;
            dc.indexCount = item.indexCount;
            dc.vertexCount = item.vertexCount;
            dc.shader = h.velocitySkinnedShader;
            dc.pipelineState = opaquePSO;
            dc.layer = renderer::RenderLayer::OPAQUE_LAYER;
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[1] = h.objectCB;
            dc.constantBuffers[2] = object.previousSkinningPalette;
            dc.constantBuffers[7] = object.skinningPalette;
            dc.constantBuffers[8] = h.advancedGraphicsCB;
            dc.textures[7] = sceneDepth;
            SubmitCounted(ctx, dc);
        }
    }
}


void VelocityPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("HDR").Write("Velocity");
}

void VelocityPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteVelocityPass(ctx);
}
} /// @note namespace fbzz::renderer
