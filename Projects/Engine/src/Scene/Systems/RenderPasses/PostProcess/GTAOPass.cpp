/// @file    GTAOPass.cpp
/// @brief   Ground Truth Ambient Occlusion (Horizon-Based AO) パス。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// SSAO より高品質で、接触部・コーナー部の陰が自然に締まる。
/// WHY: SSAO は半球上のランダムサンプルで AO を近似するため、法線方向と無関係なアーティファクトが出やすい。
/// GTAO (Horizon-Based AO) はスライスごとに水平線角度を積分するため、物理的に正確な AO が得られる。
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

    // GTAO は b8 のパラメータに加え、b5 の texelSize/screenSize でホライゾンの UV オフセットを、
    // time でスライス位相のディザを決める。
    // WHY: b5 を更新するパスはこれより後 (SSAO / DeferredLighting / Composite) にしかない。
    //      束縛だけして更新しないと前フレームの残りを読むことになり、初回フレームは 0 のまま
    //      = UV オフセットが消えて AO が真っ白になる。自前で入れて他パスに依存しない。
    PostProcCB gtaoData = MakeScreenPostProcCB(ctx.width, ctx.height);
    gtaoData.time = Time::time;
    resources.Update(h.postprocCB, &gtaoData, sizeof(PostProcCB));

    // --- GTAO RAW パス ---
    // WHAT: GBuffer1 (法線) と深度から Horizon-Based AO を計算し、UAV_GTAO_RAW (u6) に出力する。
    renderer::ComputeCall gtaoDC;
    gtaoDC.shader            = h.gtaoShader;
    gtaoDC.constantBuffers[0] = h.frameCB;            // b0: CameraConstants
    gtaoDC.constantBuffers[5] = h.postprocCB;         // b5: PostProcConstants (texelSize, screenSize)
    gtaoDC.constantBuffers[8] = h.advancedGraphicsCB; // b8: AdvancedGraphicsConstants
    gtaoDC.srvInputs[6]      = resources.GetColorTexture(ctx.Res().Target("GBuffer"), 1); // t6: GBuffer1 (normal)
    gtaoDC.srvInputs[7]      = resources.GetDepthTexture(ctx.Res().Target("GBuffer"));    // t7: Depth
    gtaoDC.uavOutputs[6]     = h.gtaoRaw;             // u6: UAV_GTAO_RAW
    gtaoDC.dispatchX         = (ctx.width  / 2 + 7) / 8;
    gtaoDC.dispatchY         = (ctx.height / 2 + 7) / 8;
    gtaoDC.dispatchZ         = 1;
    r.Dispatch(gtaoDC, resources);

    // --- GTAO Blur パス ---
    // WHAT: 4×4 ボックスフィルターでノイズを除去し、UAV_GTAO_BLUR (u7) に出力する。
    renderer::ComputeCall blurDC;
    blurDC.shader            = h.gtaoBlurShader;
    blurDC.constantBuffers[5] = h.postprocCB;         // b5: texelSize
    blurDC.srvInputs[23]     = h.gtaoRaw;             // t23: TEX_GTAO (raw)
    blurDC.uavOutputs[7]     = ctx.Res().Texture("GTAOResult");            // u7: UAV_GTAO_BLUR
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
