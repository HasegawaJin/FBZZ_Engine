/// @file    TAAPass.cpp
/// @brief   深度付きの同一履歴世代へ TAA を提出する。
/// @author  Hasegawa Jin
/// @date    2026-06-23

#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Pipeline/Passes/RenderPassContext.hpp>
#include <Graphics/Renderer/Commands/DrawCall.hpp>
#include <array>
#include <span>

namespace fbzz::renderer {

bool ExecuteTAAPass(RenderPassContext& ctx, bool staticView)
{
    auto& r         = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    if (!ctx.settings.IsTaaActive() ||
        !h.taaShader.IsValid()    || !h.taaPSO.IsValid()    ||
        !h.taaHistoryA.IsValid()  || !h.taaHistoryB.IsValid()) {
        return false;
    }

    // ping-pong: taaFlip=false → A が前フレーム履歴、B に書く。true ならその逆。
    // 同一リソースを SRV/RTV に同時束縛できないため毎フレーム入れ替える。
    auto& historyRead  = h.taaFlip ? h.taaHistoryB : h.taaHistoryA;
    auto& historyWrite = h.taaFlip ? h.taaHistoryA : h.taaHistoryB;

    TaaCycleCB cycle;
    cycle.phaseIndex = static_cast<uint32_t>(ctx.frameStamp % 8u);
    bool cycleReady = h.taaCycleCB.IsValid();
    for (const auto target : h.taaRawHistory) {
        cycleReady = cycleReady && resources.Get(target) != nullptr;
    }
    if (cycleReady) {
        const std::array<RenderTargetColorAttachment, 3> attachments{{
            {historyWrite, 0u}, {historyWrite, 1u}, {h.taaRawHistory[cycle.phaseIndex], 0u}
        }};
        cycleReady = r.SetRenderTargetColors(attachments, resources);
    }
    if (!cycleReady) {
        r.SetRenderTarget(historyWrite, resources);
    }
    cycle.enabled = cycleReady ? 1u : 0u;
    cycle.reserved[0] = cycleReady && staticView ? 1u : 0u;
    if (h.taaCycleCB.IsValid()) {
        resources.Update(h.taaCycleCB, &cycle, sizeof(cycle));
    }

    // Composite が確定した b5 を保ち、HDR 平均後も同じ色調整・明るさ・フェードを適用する。
    const auto frame = MakeCameraFrameCB(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    resources.Update(h.frameCB, &frame, sizeof(frame));

    // t5 は LDR、t8 は確率照明専用の露出済み HDR。画面出力は常に LDR のまま。
    renderer::DrawCall taaDC;
    taaDC.shader          = h.taaShader;
    taaDC.pipelineState   = h.taaPSO;
    // フルスクリーントライアングル
    taaDC.vertexCount     = 3;
    // b0: CameraConstants (inv matrices)
    taaDC.constantBuffers[cb::CAMERA] = h.frameCB;
    taaDC.constantBuffers[cb::TAA_CYCLE] = h.taaCycleCB;
    // b5: Composite と共有する画面寸法と色調整。
    taaDC.constantBuffers[cb::POSTPROC] = h.postprocCB;
    // b8: taaFeedback, prevViewProjection
    taaDC.constantBuffers[cb::ADVANCED_GRAPHICS] = h.advancedGraphicsCB;
    taaDC.constantBuffers[cb::CLUSTER] = h.clusterCB;
    // t5: 現フレーム LDR (Composite 出力)
    taaDC.textures[srv::POST_COLOR]        = resources.GetColorTexture(ctx.Res().Target("LDR"), 0);
    const auto* composite = resources.Get(ctx.Res().Target("LDR"));
    if (composite && composite->GetColorCount() > 1u) {
        taaDC.textures[srv::TAA_RADIANCE] = resources.GetColorTexture(ctx.Res().Target("LDR"), 1);
    }
    if (ctx.settings.lutColorGrading.enabled && h.proceduralColorLut.IsValid()) {
        taaDC.textures[srv::LUT_COLOR_GRADE] = h.proceduralColorLut;
    }
    if (ctx.settings.UsesGBuffer() && !ctx.settings.IsUnlit() && !ctx.settings.IsWireframe()) {
        const auto gbuffer = ctx.Res().Target("GBuffer");
        if (resources.Get(gbuffer)) {
            taaDC.textures[srv::TAA_SURFACE_MATERIAL] = resources.GetColorTexture(gbuffer, 0);
            taaDC.textures[srv::TAA_SURFACE_NORMAL] = resources.GetColorTexture(gbuffer, 1);
            taaDC.textures[srv::TAA_SURFACE_DEPTH] = resources.GetDepthTexture(gbuffer);
        }
    }
    // t21: TEX_TAA_HISTORY (前フレーム)
    taaDC.textures[srv::TAA_HISTORY]       = resources.GetColorTexture(historyRead, 0);
    // t6 は同じ履歴世代の対数深度。色だけ過去へ戻して別フレームの深度を使わない。
    taaDC.textures[srv::TAA_HISTORY_DEPTH]        = resources.GetColorTexture(historyRead, 1);
    // t7: シーン深度。速度を書かない画素 (空・静止物) はこれでカメラの動きだけ再投影する。
    // 束縛しないと無効な添字のディスクリプタを読む。
    taaDC.textures[srv::DEPTH]        = resources.GetDepthTexture(ctx.Res().Target("HDR"));
    // t26: モーションベクター。VelocityPass が動かなかったフレームは無効ハンドルのままで、
    // シェーダー側は B=0 を読んで従来の深度再投影へフォールバックする。
    if (ctx.velocityWritten && ctx.Res().Target("Velocity").IsValid()) {
        taaDC.textures[srv::VELOCITY]   = resources.GetColorTexture(ctx.Res().Target("Velocity"), 0);
    }
    // t9: 粒子の反応マスク。粒子は速度を書かないので、覆われた画素は履歴を信じる割合を下げる。
    if (h.particleReactiveValid && h.particleReactiveRT != nullptr && h.particleReactiveRT->IsValid()) {
        taaDC.textures[srv::TAA_REACTIVE]    = resources.GetColorTexture(*h.particleReactiveRT, 0);
    }
    if (cycleReady) {
        constexpr std::array<uint32_t, 8> PHASE_SLOTS{
        srv::TAA_PHASE0, srv::TAA_PHASE1, srv::TAA_PHASE2, srv::TAA_PHASE3,
        srv::TAA_PHASE4, srv::TAA_PHASE5, srv::TAA_PHASE6, srv::TAA_PHASE7 };
        for (uint32_t phase = 0; phase < PHASE_SLOTS.size(); ++phase) {
            if (phase != cycle.phaseIndex) {
                taaDC.textures[PHASE_SLOTS[phase]] = resources.GetColorTexture(h.taaRawHistory[phase], 0u);
            }
        }
    }
    r.Submit(taaDC, resources);

    // フレーム終了後に ping-pong フラグを反転
    h.taaFlip = !h.taaFlip;
    return true;
}

} // namespace fbzz::renderer
