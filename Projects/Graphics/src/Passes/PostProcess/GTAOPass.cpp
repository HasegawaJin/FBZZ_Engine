/// @file    GTAOPass.cpp
/// @brief   Ground Truth Ambient Occlusion (Horizon-Based AO) パス。
/// @author  Hasegawa Jin
/// @date    2026-06-23

/// @note SSAO より高品質で接触部・コーナー部の陰が自然に締まる。SSAO は半球上のランダム
/// @note サンプルで AO を近似するため法線方向と無関係なアーティファクトが出やすいのに対し、
/// @note GTAO はスライスごとに水平線角度を積分するため物理的に正確な AO が得られる。
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Pipeline/Passes/RenderPassContext.hpp>
#include <Graphics/Renderer/Commands/ComputeCall.hpp>

namespace fbzz::renderer {

void ExecuteGTAOPass(RenderPassContext& ctx)
{
    auto& r         = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& gtao = ctx.settings.gtao;

    if (!gtao.enabled ||
        !h.gtaoShader.IsValid()     || !h.gtaoBlurShader.IsValid() ||
        !h.gtaoRaw.IsValid()        || !ctx.Res().Texture("GTAOResult").IsValid()       ||
        !ctx.Res().Target("GBuffer").IsValid()) {
        return;
    }

    // 入出力がこのパスに閉じているので、非同期コンピュートキューへ出せる。
    // 使えなければ false が返り、そのまま描画キューで記録される (絵は変わらない)。
    /// @see Docs/design/async-compute.md
    const bool async = ctx.settings.asyncCompute && r.BeginAsyncCompute(resources);

    // GTAO は b8 のパラメータに加え、b5 の texelSize/screenSize でホライゾンの UV
    // オフセットを、time でスライス位相のディザを決める。b5 を更新するパスはこれより
    // 後 (SSAO/DeferredLighting/Composite) にしかなく、束縛だけして更新しないと前
    // フレームの残りを読み初回フレームは UV オフセットが消えて AO が真っ白になるため、
    // 自前で入れて他パスに依存しない。
    PostProcCB gtaoData = MakeScreenPostProcCB(ctx.width, ctx.height);
    gtaoData.time = ctx.time;
    resources.Update(h.postprocCB, &gtaoData, sizeof(PostProcCB));

    // GTAO RAW パス
    // GBuffer1 (法線) と深度から Horizon-Based AO を計算し、UAV_GTAO_RAW (u6) に出力する。
    renderer::ComputeCall gtaoDC;
    gtaoDC.shader            = h.gtaoShader;
    // b0: CameraConstants
    gtaoDC.constantBuffers[cb::CAMERA] = h.frameCB;
    // b5: PostProcConstants (texelSize, screenSize)
    gtaoDC.constantBuffers[cb::POSTPROC] = h.postprocCB;
    // b8: AdvancedGraphicsConstants
    gtaoDC.constantBuffers[cb::ADVANCED_GRAPHICS] = h.advancedGraphicsCB;
    // t6: GBuffer1 (normal)
    gtaoDC.srvInputs[srv::GBUFFER1]      = resources.GetColorTexture(ctx.Res().Target("GBuffer"), 1);
    // t7: Depth
    gtaoDC.srvInputs[srv::DEPTH]      = resources.GetDepthTexture(ctx.Res().Target("GBuffer"));
    // u6: UAV_GTAO_RAW
    gtaoDC.uavOutputs[uav::GTAO_RAW]     = h.gtaoRaw;
    gtaoDC.dispatchX         = (ctx.width  / 2 + 7) / 8;
    gtaoDC.dispatchY         = (ctx.height / 2 + 7) / 8;
    gtaoDC.dispatchZ         = 1;
    r.Dispatch(gtaoDC, resources);

    // GTAO Blur パス
    // 奥行きで重みを付けた 5×5 のぼかしでノイズを除去し、UAV_GTAO_BLUR (u7) に出力する。
    renderer::ComputeCall blurDC;
    blurDC.shader            = h.gtaoBlurShader;
    // b5: texelSize
    blurDC.constantBuffers[cb::POSTPROC] = h.postprocCB;
    // b0 + t7: 奥行きの差で重みを落とし、輪郭の向こうへ AO がにじまないようにする。
    blurDC.constantBuffers[cb::CAMERA] = h.frameCB;
    blurDC.srvInputs[srv::DEPTH]      = resources.GetDepthTexture(ctx.Res().Target("GBuffer"));
    // t23: TEX_GTAO (raw)
    blurDC.srvInputs[srv::GTAO]     = h.gtaoRaw;
    // u7: UAV_GTAO_BLUR
    blurDC.uavOutputs[uav::GTAO_BLUR]     = ctx.Res().Texture("GTAOResult");
    blurDC.dispatchX         = (ctx.width  / 2 + 7) / 8;
    blurDC.dispatchY         = (ctx.height / 2 + 7) / 8;
    blurDC.dispatchZ         = 1;
    r.Dispatch(blurDC, resources);

    if (async) {
        r.EndAsyncCompute();
    }
}


std::string_view GTAOPass::Name() const { return "GTAO"; }

void GTAOPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    // HDR は申告しない。GBuffer の法線と深度だけから作るので、本描画前の
    // HDR へ依存を張ると偽の順序制約になる。
    builder.Read("GBuffer").Write("GTAOResult");
}

bool GTAOPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.IsGtaoActive();
}

void GTAOPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteGTAOPass(ctx);
}

} // namespace fbzz::renderer
