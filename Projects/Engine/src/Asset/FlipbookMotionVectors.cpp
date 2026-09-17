/// @file    FlipbookMotionVectors.cpp
/// @brief   フリップブックアトラス → モーションベクターアトラス生成の実装。
/// @author  Hasegawa Jin
/// @date    2026-08-12
/// @note DirectXTex の WIC コーデックに必要。
#pragma comment(lib, "ole32.lib")

#include <Engine/Asset/FlipbookMotionVectors.hpp>

#include "FlipbookImageIO.hpp"

#include <Engine/Asset/FlipbookMotionVectorEncoding.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <DirectXTex.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

namespace fbzz::asset {
namespace {

/// 1 コマ分のグレースケール輝度。マッチングは輝度だけで足りる
/// (色差までは見なくても炎・煙の動きは追える)。
struct FramePixels {
    int width = 0;
    int height = 0;
    std::vector<float> luminance;

    [[nodiscard]] float At(int x, int y) const
    {
        /// @note 範囲外は端の値を延長する。0 で埋めるとコマの縁に偽の動きが出る。
        const int cx = std::clamp(x, 0, width - 1);
        const int cy = std::clamp(y, 0, height - 1);
        return luminance[static_cast<std::size_t>(cy) * width + cx];
    }
};

struct FlowField {
    int width = 0;
    int height = 0;
    std::vector<float> dx;
    std::vector<float> dy;
};

FramePixels ExtractFrame(std::span<const float> rgba, std::uint32_t atlasWidth,
                         int frameIndex, int columns, int frameWidth, int frameHeight)
{
    const int originX = (frameIndex % columns) * frameWidth;
    const int originY = (frameIndex / columns) * frameHeight;

    FramePixels frame;
    frame.width = frameWidth;
    frame.height = frameHeight;
    frame.luminance.resize(static_cast<std::size_t>(frameWidth) * frameHeight);
    for (int y = 0; y < frameHeight; ++y) {
        for (int x = 0; x < frameWidth; ++x) {
            const std::size_t index =
                (static_cast<std::size_t>(originY + y) * atlasWidth + static_cast<std::size_t>(originX + x)) * 4;
            const float r = rgba[index + 0];
            const float g = rgba[index + 1];
            const float b = rgba[index + 2];
            const float a = rgba[index + 3];
            /// @note アルファを掛けておく。透明部分に残ったゴミ色へ引っ張られないようにする。
            frame.luminance[static_cast<std::size_t>(y) * frameWidth + x] =
                (0.299f * r + 0.587f * g + 0.114f * b) * a;
        }
    }
    return frame;
}

/// ブロックマッチング: current の各画素まわりの窓が next のどこへ移ったかを探す。
/// 返すのは「current → next」の移動量 [px]。
FlowField ComputeBlockMatchFlow(const FramePixels& current, const FramePixels& next,
                                int blockRadius, int searchRadius)
{
    FlowField flow;
    flow.width = current.width;
    flow.height = current.height;
    flow.dx.assign(static_cast<std::size_t>(flow.width) * flow.height, 0.0f);
    flow.dy.assign(static_cast<std::size_t>(flow.width) * flow.height, 0.0f);

    /// @note 全画素で全探索すると O(W*H*S^2*B^2) になる。ブロック単位で 1 回だけ探索し、
    ///       結果をブロック内へ配る (フリップブックの解像度なら十分な粒度)。
    const int step = (std::max)(blockRadius, 1);
    for (int blockY = 0; blockY < current.height; blockY += step) {
        for (int blockX = 0; blockX < current.width; blockX += step) {
            float bestScore = -1.0f;
            int bestDx = 0;
            int bestDy = 0;
            for (int offsetY = -searchRadius; offsetY <= searchRadius; ++offsetY) {
                for (int offsetX = -searchRadius; offsetX <= searchRadius; ++offsetX) {
                    /// @note SAD (絶対差の総和) を最小化する。符号を反転してスコア最大化に揃える。
                    float difference = 0.0f;
                    for (int y = -blockRadius; y <= blockRadius; ++y) {
                        for (int x = -blockRadius; x <= blockRadius; ++x) {
                            difference += std::fabs(
                                current.At(blockX + x, blockY + y)
                                - next.At(blockX + x + offsetX, blockY + y + offsetY));
                        }
                    }
                    const float score = -difference;
                    /// @note 同スコアなら移動量の小さい方を採る (静止部分が暴れないように)。
                    if (score > bestScore
                        || (score == bestScore
                            && offsetX * offsetX + offsetY * offsetY < bestDx * bestDx + bestDy * bestDy)) {
                        bestScore = score;
                        bestDx = offsetX;
                        bestDy = offsetY;
                    }
                }
            }
            for (int y = blockY; y < (std::min)(blockY + step, current.height); ++y) {
                for (int x = blockX; x < (std::min)(blockX + step, current.width); ++x) {
                    const std::size_t index = static_cast<std::size_t>(y) * flow.width + x;
                    flow.dx[index] = static_cast<float>(bestDx);
                    flow.dy[index] = static_cast<float>(bestDy);
                }
            }
        }
    }
    return flow;
}

/// 3x3 平均でフロー場を均す。ブロック単位の探索結果はそのままだと
/// ブロック境界で段差になり、warp したときにタイル状の継ぎ目が見える。
void SmoothFlow(FlowField& flow, int iterations)
{
    if (iterations <= 0) return;
    std::vector<float> tempX(flow.dx.size());
    std::vector<float> tempY(flow.dy.size());
    for (int pass = 0; pass < iterations; ++pass) {
        for (int y = 0; y < flow.height; ++y) {
            for (int x = 0; x < flow.width; ++x) {
                float sumX = 0.0f;
                float sumY = 0.0f;
                int count = 0;
                for (int oy = -1; oy <= 1; ++oy) {
                    for (int ox = -1; ox <= 1; ++ox) {
                        const int sx = std::clamp(x + ox, 0, flow.width - 1);
                        const int sy = std::clamp(y + oy, 0, flow.height - 1);
                        const std::size_t index = static_cast<std::size_t>(sy) * flow.width + sx;
                        sumX += flow.dx[index];
                        sumY += flow.dy[index];
                        ++count;
                    }
                }
                const std::size_t index = static_cast<std::size_t>(y) * flow.width + x;
                tempX[index] = sumX / static_cast<float>(count);
                tempY[index] = sumY / static_cast<float>(count);
            }
        }
        flow.dx.swap(tempX);
        flow.dy.swap(tempY);
    }
}

FlipbookMotionAnalysis FailAnalysis(std::string message)
{
    FlipbookMotionAnalysis analysis;
    analysis.message = std::move(message);
    return analysis;
}

FlipbookMotionVectorResult Fail(std::string message)
{
    FlipbookMotionVectorResult result;
    result.success = false;
    result.message = std::move(message);
    return result;
}

} // namespace

FlipbookMotionAnalysis AnalyzeFlipbookMotion(std::span<const float> rgba, std::uint32_t width,
                                             std::uint32_t height,
                                             const FlipbookMotionVectorSettings& settings)
{
    const int columns = (std::max)(settings.columns, 1);
    const int rows = (std::max)(settings.rows, 1);
    const int frameCount = columns * rows;
    if (frameCount < 2)
        return FailAnalysis("コマが 1 枚しかないため動きを解析できません (Columns × Rows を 2 以上に)");
    if (rgba.size() < static_cast<std::size_t>(width) * height * 4)
        return FailAnalysis("画素数が寸法と一致しません");
    if (width % static_cast<std::uint32_t>(columns) != 0 || height % static_cast<std::uint32_t>(rows) != 0) {
        return FailAnalysis("アトラスの寸法が Columns / Rows で割り切れません ("
                            + std::to_string(width) + "x" + std::to_string(height) + ")");
    }

    const int frameWidth = static_cast<int>(width) / columns;
    const int frameHeight = static_cast<int>(height) / rows;
    const int searchRadius = std::clamp(settings.searchRadius, 1,
                                        (std::max)((std::min)(frameWidth, frameHeight) / 2, 1));
    const int blockRadius = std::clamp(settings.blockRadius, 1, searchRadius);

    FlipbookMotionAnalysis analysis;
    analysis.displacementUv.assign(static_cast<std::size_t>(width) * height, math::Vector2::ZERO);
    analysis.frameCount = frameCount;
    analysis.searchRadius = searchRadius;

    for (int frame = 0; frame < frameCount; ++frame) {
        const int column = frame % columns;
        const int row = frame / columns;
        const int nextFrame = settings.rowSequences
            ? row * columns + (column + 1) % columns
            : (frame + 1) % frameCount;
        /// @note 行を独立列として扱う場合、非ループの終端判定も各行末尾で行う。
        const bool hasNext = settings.rowSequences
            ? (settings.loop || column + 1 < columns)
            : (settings.loop || frame + 1 < frameCount);
        if (!hasNext) continue;

        const FramePixels current = ExtractFrame(rgba, width, frame, columns, frameWidth, frameHeight);
        const FramePixels next = ExtractFrame(rgba, width, nextFrame, columns, frameWidth, frameHeight);
        FlowField flow = ComputeBlockMatchFlow(current, next, blockRadius, searchRadius);
        SmoothFlow(flow, settings.smoothIterations);

        const int originX = column * frameWidth;
        const int originY = row * frameHeight;
        for (int y = 0; y < frameHeight; ++y) {
            for (int x = 0; x < frameWidth; ++x) {
                const std::size_t flowIndex = static_cast<std::size_t>(y) * flow.width + x;
                const float rawX = flow.dx[flowIndex];
                const float rawY = flow.dy[flowIndex];
                analysis.maxObservedFlow = (std::max)(analysis.maxObservedFlow,
                                                      std::sqrt(rawX * rawX + rawY * rawY));
                analysis.displacementUv[static_cast<std::size_t>(originY + y) * width + (originX + x)] =
                    PixelToAtlasUv(rawX, rawY, width, height);
            }
        }
    }

    /// @note 1 画素も動かないアトラスでも S が 0 にならないよう、下限を 1 画素ぶりにする。
    analysis.recommendedStrength = ComputeRecommendedStrength(
        analysis.displacementUv, 1.0f / static_cast<float>((std::max)(width, height)));
    analysis.success = true;
    return analysis;
}

FlipbookMotionVectorResult GenerateFlipbookMotionVectors(
    const std::string& sourcePath, const FlipbookMotionVectorSettings& settings)
{
    std::string resolvedPath;
    if (!TexDescSerializer::ResolveSourcePath(sourcePath, resolvedPath))
        return Fail("テクスチャを解決できません: " + sourcePath);

    const std::wstring widePath = util::StringUtils::ToWide(resolvedPath);
    DirectX::ScratchImage loaded;
    HRESULT hr;
    if (resolvedPath.ends_with(".dds") || resolvedPath.ends_with(".DDS"))
        hr = DirectX::LoadFromDDSFile(widePath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, loaded);
    else if (resolvedPath.ends_with(".tga") || resolvedPath.ends_with(".TGA"))
        hr = DirectX::LoadFromTGAFile(widePath.c_str(), nullptr, loaded);
    else
        hr = DirectX::LoadFromWICFile(widePath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, loaded);
    if (FAILED(hr)) return Fail("テクスチャを読み込めません: " + resolvedPath);

    /// @note 以降の解析は float で行う。8bit のまま差分を取ると量子化で動きが潰れる。
    DirectX::ScratchImage converted;
    hr = DirectX::Convert(*loaded.GetImage(0, 0, 0), DXGI_FORMAT_R32G32B32A32_FLOAT,
                          DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
    if (FAILED(hr)) return Fail("テクスチャを float へ変換できません");

    const DirectX::Image& image = *converted.GetImage(0, 0, 0);
    const auto width = static_cast<std::uint32_t>(image.width);
    const auto height = static_cast<std::uint32_t>(image.height);
    std::vector<float> rgba(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        std::memcpy(rgba.data() + static_cast<std::size_t>(y) * width * 4,
                    image.pixels + static_cast<std::size_t>(y) * image.rowPitch,
                    static_cast<std::size_t>(width) * 4 * sizeof(float));
    }

    const FlipbookMotionAnalysis analysis = AnalyzeFlipbookMotion(rgba, width, height, settings);
    if (!analysis.success) return Fail(analysis.message);

    std::vector<std::uint8_t> encoded(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < analysis.displacementUv.size(); ++i) {
        const EncodedMotionVector motion =
            EncodeMotionVector(analysis.displacementUv[i], analysis.recommendedStrength);
        encoded[i * 4 + 0] = motion.r;
        encoded[i * 4 + 1] = motion.g;
        encoded[i * 4 + 2] = 0;
        encoded[i * 4 + 3] = 255;
    }

    std::filesystem::path destination(resolvedPath);
    destination.replace_extension();
    destination += "_mv.png";
    std::string saveError;
    if (!detail::SavePngRgba8(destination, width, height, encoded, saveError))
        return Fail(saveError);
    /// @note RG は色ではなく速度なので sRGB 変換を禁止し、2 チャンネルを保持できる BC5 で扱う。
    ///       Mip 生成はフレーム境界の速度を混ぜるため無効にする。
    if (!detail::SaveTextureMeta(destination, TextureType::Data, TextureCompression::BC5,
                                 AlphaMode::None, false, saveError))
        return Fail(saveError);

    FlipbookMotionVectorResult result;
    result.success = true;
    result.outputPath = destination.string();
    result.frameCount = analysis.frameCount;
    result.maxObservedFlow = analysis.maxObservedFlow;
    result.recommendedStrength = analysis.recommendedStrength;
    char strengthText[32]{};
    std::snprintf(strengthText, sizeof(strengthText), "%.4f", analysis.recommendedStrength);
    /// @note 探索半径に張り付いている = 実際の動きが探索範囲を超えている可能性が高い。
    ///       使う側が半径を上げる判断をできるよう、要約に必ず出す。
    result.message = "生成しました: " + std::to_string(analysis.frameCount) + " コマ / 最大移動量 "
        + std::to_string(static_cast<int>(analysis.maxObservedFlow)) + "px (探索半径 "
        + std::to_string(analysis.searchRadius) + "px) / Motion Strength " + strengthText;
    if (analysis.maxObservedFlow >= static_cast<float>(analysis.searchRadius))
        result.message += " — 探索半径を上げると精度が上がる可能性があります";
    return result;
}

} // namespace fbzz::asset
