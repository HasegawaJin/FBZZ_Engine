// FBZZ Engine
// PreviewMetrics.hpp | fbzz::editor::ai
// VFX プレビュー画 1 枚から、視覚判断に依らない評価指標を取り出す
//
// WHY: vfx.preview は PNG を返すだけで、良し悪しの判断は全て MCP クライアント側の視覚に委ねられていた。
//      同じ画を見ても毎回違う結論が出るため「少し暗い」の“少し”に基準が無く、直す量を決められず
//      反復が振動する。ここで測れるものを数値にしておくと、機械的に判る破綻 (白飛び・覆いすぎ・
//      動いていない) を先に潰し、残った「らしさ」の判断だけを画像へ委ねられる。
//      指標は視覚判断の置き換えではなく、視覚判断が必要な範囲を狭めるためのもの。
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::editor::ai {

// 輝度ヒストグラムの階級数。log2 輝度 [-8, +8] を等分する
// (1/256 〜 256。HDR プレビューで実際に出る範囲をほぼ覆う)。
inline constexpr int kPreviewHistogramBuckets = 16;

// プレビュー画 1 枚の指標。全て線形輝度ベースで、トーンマップは通していない。
struct PreviewMetrics {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    // --- 露出 ---
    // 画面全体の平均輝度。背景を含むので、coverage が小さいエフェクトでは自然に小さくなる。
    float luminanceMean = 0.0f;
    // 描画された画素だけの平均輝度。「エフェクト自体が明るいのか」を coverage と切り離して見る。
    float coveredLuminanceMean = 0.0f;
    float luminanceMax = 0.0f;
    // 上位 1% 点。単発のハイライト 1 画素に引きずられない実効的な上限。
    float luminanceP99 = 0.0f;
    // 輝度 1.0 以上 = トーンマップ後に白へ寄る画素の割合。
    float clippedRatio = 0.0f;
    // 輝度 4.0 以上 = 明確な白飛び。ここが数 % を超えると形が消える。
    float blownOutRatio = 0.0f;
    std::array<float, kPreviewHistogramBuckets> histogram{};

    // --- 画面占有 ---
    // 背景色から有意に変化した画素の割合。1.0 に近いほど画面を覆っている。
    float coverage = 0.0f;
    // 占有画素の重心 (0..1、左上原点)。0.5,0.5 から大きく外れていれば画角がずれている。
    float centroidX = 0.5f;
    float centroidY = 0.5f;
    // 占有画素のバウンディングボックス (0..1)。coverage が 0 のときは全て 0。
    float boundsMinX = 0.0f;
    float boundsMinY = 0.0f;
    float boundsMaxX = 0.0f;
    float boundsMaxY = 0.0f;

    // --- 動き ---
    // 直前に測ったフレームとの比較。サイズが違う / 前回が無い場合は false。
    bool  hasPrevious = false;
    // 輝度の平均絶対差。0 に近ければ「時間を進めても画が変わっていない」。
    float motion = 0.0f;
    // 有意に変化した画素の割合。motion が小さくても、ここが大きければ薄く全体が動いている。
    float changedRatio = 0.0f;
};

// rgba (width*height*4 の線形 float) から指標を計算する。
// background は占有判定の基準になる Clear 色 (RGB)。
// previousLuminance が非 null かつ画素数が一致すれば動き指標を埋める。
// outLuminance が非 null なら、次回の比較用に今回の輝度バッファを書き出す。
[[nodiscard]] PreviewMetrics ComputePreviewMetrics(const std::vector<float>& rgba,
                                                   std::uint32_t width, std::uint32_t height,
                                                   const float background[3],
                                                   const std::vector<float>* previousLuminance,
                                                   std::vector<float>* outLuminance);

// 指標から、機械的に判る破綻だけを短い所見として並べる (空なら「明らかな破綻は無い」)。
// WHY: 数値を返すだけだと閾値の解釈がクライアント側でぶれる。判定の基準は
//      vfx_guide のオーサリング規約と同じくエンジン側に置き、AI へは結論を渡す。
[[nodiscard]] std::vector<std::string> DescribePreviewMetricIssues(const PreviewMetrics& metrics);

} // namespace fbzz::editor::ai
