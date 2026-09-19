/// @file    PreviewMetrics.cpp
/// @brief   プレビュー RT の線形画素から評価指標を算出する実装。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <Editor/Ai/PreviewMetrics.hpp>

#include <algorithm>
#include <cmath>

namespace fbzz::editor::ai {
namespace {

/// Rec.709 相対輝度。パーティクルは加算合成で 1.0 を大きく超えるため、飽和はさせない。
float Luminance(float r, float g, float b)
{
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

/// 背景から有意に離れているか。加算ブレンドの薄い裾まで拾えるよう、閾値は低めに取る。
/// @note 厳しくすると「煙の外周が消えている」と誤判定し、coverage が実感と合わなくなる。
constexpr float kCoverageEpsilon = 0.004f;
/// 動きの有無を判定する輝度差。読み戻しの量子化 (R16F) より十分大きく取る。
constexpr float kMotionEpsilon = 0.01f;

/// log2 輝度 [-8, +8] を kPreviewHistogramBuckets 等分した階級 index。
int HistogramBucket(float luminance)
{
    if (!(luminance > 0.0f)) return 0;
    const float log2Luminance = std::log2(luminance);
    const float normalized = (log2Luminance + 8.0f) / 16.0f;
    return std::clamp(static_cast<int>(normalized * kPreviewHistogramBuckets),
                      0, kPreviewHistogramBuckets - 1);
}

} // namespace

PreviewMetrics ComputePreviewMetrics(const std::vector<float>& rgba,
                                     std::uint32_t width, std::uint32_t height,
                                     const float background[3],
                                     const std::vector<float>* previousLuminance,
                                     std::vector<float>* outLuminance)
{
    PreviewMetrics metrics;
    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
    if (pixelCount == 0 || rgba.size() < pixelCount * 4u) return metrics;
    metrics.width = width;
    metrics.height = height;

    std::vector<float> luminance(pixelCount, 0.0f);
    double luminanceSum = 0.0;
    double coveredLuminanceSum = 0.0;
    std::size_t coveredCount = 0;
    std::size_t clippedCount = 0;
    std::size_t blownOutCount = 0;
    double centroidXSum = 0.0;
    double centroidYSum = 0.0;
    std::uint32_t minX = width, minY = height, maxX = 0, maxY = 0;
    std::array<std::size_t, kPreviewHistogramBuckets> histogramCounts{};

    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const float* texel = rgba.data() + index * 4u;
            const float value = Luminance(texel[0], texel[1], texel[2]);
            luminance[index] = value;
            luminanceSum += value;
            if (value >= 1.0f) ++clippedCount;
            if (value >= 4.0f) ++blownOutCount;
            ++histogramCounts[static_cast<std::size_t>(HistogramBucket(value))];
            metrics.luminanceMax = (std::max)(metrics.luminanceMax, value);

            /// @note 背景との差で「何かが描かれた画素」を判定する。アルファではなく色差を見るのは、
            ///       加算合成のパーティクルが背景のアルファを 1 のまま残すため。
            const float difference = (std::max)({ std::fabs(texel[0] - background[0]),
                                                  std::fabs(texel[1] - background[1]),
                                                  std::fabs(texel[2] - background[2]) });
            if (difference <= kCoverageEpsilon) continue;
            ++coveredCount;
            coveredLuminanceSum += value;
            centroidXSum += x;
            centroidYSum += y;
            minX = (std::min)(minX, x);
            minY = (std::min)(minY, y);
            maxX = (std::max)(maxX, x);
            maxY = (std::max)(maxY, y);
        }
    }

    const auto pixels = static_cast<double>(pixelCount);
    metrics.luminanceMean  = static_cast<float>(luminanceSum / pixels);
    metrics.clippedRatio   = static_cast<float>(static_cast<double>(clippedCount) / pixels);
    metrics.blownOutRatio  = static_cast<float>(static_cast<double>(blownOutCount) / pixels);
    metrics.coverage       = static_cast<float>(static_cast<double>(coveredCount) / pixels);
    for (int bucket = 0; bucket < kPreviewHistogramBuckets; ++bucket) {
        metrics.histogram[static_cast<std::size_t>(bucket)] =
            static_cast<float>(static_cast<double>(histogramCounts[static_cast<std::size_t>(bucket)]) / pixels);
    }
    if (coveredCount > 0) {
        const auto covered = static_cast<double>(coveredCount);
        metrics.coveredLuminanceMean = static_cast<float>(coveredLuminanceSum / covered);
        metrics.centroidX = static_cast<float>(centroidXSum / covered / (std::max)(1u, width - 1u));
        metrics.centroidY = static_cast<float>(centroidYSum / covered / (std::max)(1u, height - 1u));
        metrics.boundsMinX = static_cast<float>(minX) / static_cast<float>((std::max)(1u, width - 1u));
        metrics.boundsMinY = static_cast<float>(minY) / static_cast<float>((std::max)(1u, height - 1u));
        metrics.boundsMaxX = static_cast<float>(maxX) / static_cast<float>((std::max)(1u, width - 1u));
        metrics.boundsMaxY = static_cast<float>(maxY) / static_cast<float>((std::max)(1u, height - 1u));
    }

    /// @note 上位 1% 点。全体ソートは不要なので nth_element で境界だけ求める。
    {
        std::vector<float> sorted = luminance;
        const std::size_t rank = pixelCount - (std::max)(std::size_t{1}, pixelCount / std::size_t{100});
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(rank), sorted.end());
        metrics.luminanceP99 = sorted[rank];
    }

    if (previousLuminance != nullptr && previousLuminance->size() == pixelCount) {
        double differenceSum = 0.0;
        std::size_t changedCount = 0;
        for (std::size_t index = 0; index < pixelCount; ++index) {
            const float difference = std::fabs(luminance[index] - (*previousLuminance)[index]);
            differenceSum += difference;
            if (difference > kMotionEpsilon) ++changedCount;
        }
        metrics.hasPrevious   = true;
        metrics.motion        = static_cast<float>(differenceSum / pixels);
        metrics.changedRatio  = static_cast<float>(static_cast<double>(changedCount) / pixels);
    }

    if (outLuminance != nullptr) *outLuminance = std::move(luminance);
    return metrics;
}

std::vector<std::string> DescribePreviewMetricIssues(const PreviewMetrics& metrics)
{
    std::vector<std::string> issues;
    if (metrics.width == 0 || metrics.height == 0) return issues;

    /// @note 何も描かれていない。他の指標は全て意味を失うので、これだけを返して打ち切る。
    if (metrics.coverage < 0.0005f) {
        issues.emplace_back("EMPTY_FRAME: 画にエフェクトがほとんど出ていません "
                            "(coverage < 0.05%)。vfx_runtime_state で起動状態を確認してください");
        return issues;
    }
    if (metrics.blownOutRatio > 0.05f) {
        issues.emplace_back("BLOWN_OUT: 画面の 5% 超が輝度 4 以上で、形が白へ潰れています。"
                            "emissiveScale / colorOverLifetime の明度、または重なり枚数を下げてください");
    } else if (metrics.clippedRatio > 0.35f) {
        issues.emplace_back("OVEREXPOSED: 画面の 35% 超が輝度 1 以上です。"
                            "全体が明るすぎて階調が残っていません");
    }
    if (metrics.coverage > 0.75f) {
        issues.emplace_back("SCREEN_FLOODED: 画面の 75% 超をエフェクトが覆っています。"
                            "ゲーム内では視界を潰します。サイズか粒子数を下げてください");
    }
    if (metrics.coveredLuminanceMean < 0.02f) {
        issues.emplace_back("TOO_DIM: 描画された画素の平均輝度が 0.02 未満で、"
                            "背景に埋もれています。emissive か色の明度を上げてください");
    }
    if (metrics.hasPrevious && metrics.changedRatio < 0.002f) {
        issues.emplace_back("STATIC_FRAME: 前のサンプルからほぼ変化していません。"
                            "その区間はエフェクトが止まって見えます");
    }
    /// @note 画角ずれ。重心が中央から大きく外れているのは、カメラかエミッター位置のどちらかがずれている。
    if (std::fabs(metrics.centroidX - 0.5f) > 0.25f || std::fabs(metrics.centroidY - 0.5f) > 0.25f) {
        issues.emplace_back("OFF_CENTER: 描画の重心が画面中央から大きく外れています。"
                            "camera 引数か emitPosition を見直してください");
    }
    return issues;
}

} // namespace fbzz::editor::ai
