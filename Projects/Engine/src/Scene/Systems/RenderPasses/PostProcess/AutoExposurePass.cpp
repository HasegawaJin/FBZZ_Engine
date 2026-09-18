/// @file    AutoExposurePass.cpp
/// @brief   HDR バッファの輝度ヒストグラムから露出を決めて時間順応させる
/// @author  Hasegawa Jin
/// @date    2026-08-25
///
/// @note ExposureHistogram (輝度ヒストグラム作成) → ExposureAverage (パーセンタイル平均+順応)
///       の 2 Dispatch。結果は 1 要素の StructuredBuffer に残ったまま Composite が t29 で読み、
///       CPU へ読み戻さない (GPU 完了待ちのストールを避けるため。露出の 1 フレーム遅延は無害)。
#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/ComputeCall.hpp>
#include <Engine/Core/Time.hpp>
#include <algorithm>

namespace fbzz::scene {

namespace {
/// @brief リセット要求の世代。各ビューが `handles.exposureResetGeneration` へ写し、食い違う
///        フレームで 1 度だけ順応を捨てて合わせる。
/// @note bool でなく世代にするのは、順応の状態がビュー単位 (SceneView/GameView) のため。
///       1 本の bool だと先に走ったビューが消費し、もう片方が古い順応を引きずる。初期値 0 は
///       世代 1 と食い違うので初回フレームは必ずリセットになる。
uint32_t s_resetGeneration = 1u;
} // namespace

void RequestAutoExposureReset()
{
    ++s_resetGeneration;
}

void ExecuteAutoExposurePass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& ae  = ctx.settings.autoExposure;

    if (!ae.enabled) {
        /// @note 無効な間は順応の状態を捨てる。次に有効化したとき、前回の値から
        ///       ゆっくり動き出すのではなく、その場の明るさへ即座に合う。
        h.exposureResetGeneration = 0u;
        return;
    }
    if (!h.exposureHistogramCS.IsValid() || !h.exposureAverageCS.IsValid()
        || !h.exposureHistogram.IsValid() || !h.exposureResult.IsValid()
        || !h.exposureCB.IsValid() || !ctx.Res().Target("HDR").IsValid()) {
        return;
    }

    AutoExposureCB data{};
    data.minEV        = ae.minEV;
    data.evRange      = (std::max)(ae.maxEV - ae.minEV, 0.1f);
    /// @note 下側の切り捨て率が上側を追い越すと、採用される帯が消えて露出が固まる。
    data.lowPercent   = std::clamp(ae.lowPercent, 0.0f, 0.99f);
    data.highPercent  = std::clamp(ae.highPercent, data.lowPercent + 0.01f, 1.0f);
    data.speedUp      = (std::max)(ae.speedUp, 0.0f);
    data.speedDown    = (std::max)(ae.speedDown, 0.0f);
    /// @note Play/Stop やエディタのポーズで dt が跳ねると順応が一気に飛ぶ。上限を切る。
    data.deltaTime    = std::clamp(Time::deltaTime, 0.0f, 0.1f);
    data.compensation = ae.compensation;
    data.minEVClamp   = ae.minExposureEV;
    data.maxEVClamp   = (std::max)(ae.maxExposureEV, ae.minExposureEV);
    data.reset        = (h.exposureResetGeneration != s_resetGeneration) ? 1u : 0u;
    resources.Update(h.exposureCB, &data, sizeof(AutoExposureCB));

    /// @name 1. ヒストグラム
    renderer::ComputeCall histogram;
    histogram.shader             = h.exposureHistogramCS;
    histogram.constantBuffers[2] = h.exposureCB;
    /// @note t5: TEX_GBUFFER0
    histogram.srvInputs[5]       = resources.GetColorTexture(ctx.Res().Target("HDR"), 0);
    /// @note u2
    histogram.uavBuffers[0]      = h.exposureHistogram;
    histogram.dispatchX          = (ctx.width  + 15) / 16;
    histogram.dispatchY          = (ctx.height + 15) / 16;
    histogram.dispatchZ          = 1;
    renderer.Dispatch(histogram, resources);

    /// @name 2. 平均と順応
    /// @note 1 グループにするのはビン数 = スレッド数でグループ間の同期が不要なため。
    ///       複数グループへ割ると累積和のためにもう 1 パス増える。
    renderer::ComputeCall average;
    average.shader             = h.exposureAverageCS;
    average.constantBuffers[2] = h.exposureCB;
    /// @note u2
    average.uavBuffers[0]      = h.exposureHistogram;
    /// @note u3
    average.uavBuffers[1]      = h.exposureResult;
    average.dispatchX          = 1;
    average.dispatchY          = 1;
    average.dispatchZ          = 1;
    renderer.Dispatch(average, resources);

    h.exposureResetGeneration = s_resetGeneration;
}


void AutoExposurePass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("HDR");
}

bool AutoExposurePass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.autoExposure.enabled;
}

void AutoExposurePass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteAutoExposurePass(ctx);
}
} // namespace fbzz::scene
