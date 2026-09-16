/// @file    AutoExposurePass.cpp
/// @brief   HDR バッファの輝度ヒストグラムから露出を決めて時間順応させる
/// @author  Hasegawa Jin
/// @date    2026-08-25
//
// 2 つの Dispatch で完結する。
//   1. ExposureHistogram : 画面を走査して対数輝度のヒストグラムを作る
//   2. ExposureAverage   : 上下のパーセンタイルを捨てて平均を取り、前フレームから順応させる
//
// 結果 (順応済みの平均輝度) は 1 要素の StructuredBuffer に残り、Composite が t29 で読む。
//
// WHY 結果を GPU 側に置いたままにするか: CPU へ読み戻すと GPU の完了待ちが要る。
//     露出は 1 フレーム遅れても誰も気づかないが、パイプラインのストールは全体に効く。
#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/ComputeCall.hpp>
#include <Engine/Core/Time.hpp>
#include <algorithm>

namespace fbzz::scene {

namespace {
// リセット要求の世代。各ビューは handles.exposureResetGeneration にこの値を写し、
// 食い違っているフレームで 1 度だけ順応を捨てて即座に合わせる。
// WHY 必要か: 起動直後は「前フレームの露出」が存在しない。順応させると、
//     暗い初期値から正しい露出まで数秒かけて明るくなる立ち上がりが毎回入る。
// WHY bool でないか: 順応の状態はビュー単位 (SceneView / GameView)。1 本の bool を
//     最初に走ったビューが消費すると、もう片方はリセットされないまま古い順応を引きずる。
//     ビュー側の初期値 0 は世代 1 と食い違うので、初回フレームは必ずリセットになる。
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
        // 無効な間は順応の状態を捨てる。次に有効化したとき、前回の値から
        // ゆっくり動き出すのではなく、その場の明るさへ即座に合う。
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
    // 下側の切り捨て率が上側を追い越すと、採用される帯が消えて露出が固まる。
    data.lowPercent   = std::clamp(ae.lowPercent, 0.0f, 0.99f);
    data.highPercent  = std::clamp(ae.highPercent, data.lowPercent + 0.01f, 1.0f);
    data.speedUp      = (std::max)(ae.speedUp, 0.0f);
    data.speedDown    = (std::max)(ae.speedDown, 0.0f);
    // Play/Stop やエディタのポーズで dt が跳ねると順応が一気に飛ぶ。上限を切る。
    data.deltaTime    = std::clamp(Time::deltaTime, 0.0f, 0.1f);
    data.compensation = ae.compensation;
    data.minEVClamp   = ae.minExposureEV;
    data.maxEVClamp   = (std::max)(ae.maxExposureEV, ae.minExposureEV);
    data.reset        = (h.exposureResetGeneration != s_resetGeneration) ? 1u : 0u;
    resources.Update(h.exposureCB, &data, sizeof(AutoExposureCB));

    // ---- 1. ヒストグラム ----
    renderer::ComputeCall histogram;
    histogram.shader             = h.exposureHistogramCS;
    histogram.constantBuffers[2] = h.exposureCB;
    histogram.srvInputs[5]       = resources.GetColorTexture(ctx.Res().Target("HDR"), 0); // t5: TEX_GBUFFER0
    histogram.uavBuffers[0]      = h.exposureHistogram;                   // u2
    histogram.dispatchX          = (ctx.width  + 15) / 16;
    histogram.dispatchY          = (ctx.height + 15) / 16;
    histogram.dispatchZ          = 1;
    renderer.Dispatch(histogram, resources);

    // ---- 2. 平均と順応 ----
    // WHY 1 グループか: ビン数 = スレッド数なので、グループ間の同期が要らない。
    //     複数グループへ割ると累積和のためにもう 1 パス増える。
    renderer::ComputeCall average;
    average.shader             = h.exposureAverageCS;
    average.constantBuffers[2] = h.exposureCB;
    average.uavBuffers[0]      = h.exposureHistogram; // u2
    average.uavBuffers[1]      = h.exposureResult;    // u3
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
