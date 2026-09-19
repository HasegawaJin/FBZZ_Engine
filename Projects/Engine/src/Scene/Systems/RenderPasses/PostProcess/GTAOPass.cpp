/// @file    GTAOPass.cpp
/// @brief   Ground Truth Ambient Occlusion (Horizon-Based AO) パス。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// @note SSAO より高品質で接触部・コーナー部の陰が自然に締まる。SSAO は半球上のランダム
///       サンプルで AO を近似するため法線方向と無関係なアーティファクトが出やすいのに対し、
///       GTAO はスライスごとに水平線角度を積分するため物理的に正確な AO が得られる。
#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/ComputeCall.hpp>

namespace fbzz::scene {

void ExecuteGTAOPass(RenderPassContext& ctx)
{
    auto& r         = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& gtao = ctx.settings.gtao;

    if (!gtao.enabled ||
        !h.gtaoShader.IsValid()     || !h.gtaoBlurShader.IsValid() ||
        !h.gtaoRaw.IsValid()        || !ctx.Res().Texture("GTAOResult").IsValid()       ||
        !ctx.Res().Target("GBuffer").IsValid())
        return;

    /// @note GTAO は b8 のパラメータに加え、b5 の texelSize/screenSize でホライゾンの UV
    ///       オフセットを、time でスライス位相のディザを決める。b5 を更新するパスはこれより
    ///       後 (SSAO/DeferredLighting/Composite) にしかなく、束縛だけして更新しないと前
    ///       フレームの残りを読み初回フレームは UV オフセットが消えて AO が真っ白になるため、
    ///       自前で入れて他パスに依存しない。
    PostProcCB gtaoData = MakeScreenPostProcCB(ctx.width, ctx.height);
    gtaoData.time = Time::time;
    resources.Update(h.postprocCB, &gtaoData, sizeof(PostProcCB));

    /// @name GTAO RAW パス
    /// @note GBuffer1 (法線) と深度から Horizon-Based AO を計算し、UAV_GTAO_RAW (u6) に出力する。
    renderer::ComputeCall gtaoDC;
    gtaoDC.shader            = h.gtaoShader;
    /// @note b0: CameraConstants
    gtaoDC.constantBuffers[0] = h.frameCB;
    /// @note b5: PostProcConstants (texelSize, screenSize)
    gtaoDC.constantBuffers[5] = h.postprocCB;
    /// @note b8: AdvancedGraphicsConstants
    gtaoDC.constantBuffers[8] = h.advancedGraphicsCB;
    /// @note t6: GBuffer1 (normal)
    gtaoDC.srvInputs[6]      = resources.GetColorTexture(ctx.Res().Target("GBuffer"), 1);
    /// @note t7: Depth
    gtaoDC.srvInputs[7]      = resources.GetDepthTexture(ctx.Res().Target("GBuffer"));
    /// @note u6: UAV_GTAO_RAW
    gtaoDC.uavOutputs[6]     = h.gtaoRaw;
    gtaoDC.dispatchX         = (ctx.width  / 2 + 7) / 8;
    gtaoDC.dispatchY         = (ctx.height / 2 + 7) / 8;
    gtaoDC.dispatchZ         = 1;
    r.Dispatch(gtaoDC, resources);

    /// @name GTAO Blur パス
    /// @note 4×4 ボックスフィルターでノイズを除去し、UAV_GTAO_BLUR (u7) に出力する。
    renderer::ComputeCall blurDC;
    blurDC.shader            = h.gtaoBlurShader;
    /// @note b5: texelSize
    blurDC.constantBuffers[5] = h.postprocCB;
    /// @note t23: TEX_GTAO (raw)
    blurDC.srvInputs[23]     = h.gtaoRaw;
    /// @note u7: UAV_GTAO_BLUR
    blurDC.uavOutputs[7]     = ctx.Res().Texture("GTAOResult");
    blurDC.dispatchX         = (ctx.width  / 2 + 7) / 8;
    blurDC.dispatchY         = (ctx.height / 2 + 7) / 8;
    blurDC.dispatchZ         = 1;
    r.Dispatch(blurDC, resources);
}


std::string_view GTAOPass::Name() const { return "GTAO"; }

void GTAOPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    /// @note HDR は申告しない。GBuffer の法線と深度だけから作るので、本描画前の
    ///       HDR へ依存を張ると偽の順序制約になる。
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

} // namespace fbzz::scene
