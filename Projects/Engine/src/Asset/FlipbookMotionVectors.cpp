// FBZZ Engine
// FlipbookMotionVectors.cpp | fbzz::asset
// フリップブックアトラス → モーションベクターアトラス生成の実装
//
// 処理フロー:
//   1. アトラスを読み込み R32G32B32A32_FLOAT へ変換
//   2. コマを切り出し、隣接コマ間でブロックマッチング → 画素ごとの移動量 [px]
//   3. フロー場を平滑化してブロック境界の段差を消す
//   4. 探索半径で正規化し [0,1] へエンコードして PNG (RG) へ書き出す
#pragma comment(lib, "ole32.lib")  // DirectXTex の WIC コーデックに必要

#include <Engine/Asset/FlipbookMotionVectors.hpp>

#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <DirectXTex.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

namespace fbzz::asset {
namespace {

// 1 コマ分のグレースケール輝度。マッチングは輝度だけで足りる
// (色差までは見なくても炎・煙の動きは追える)。
struct FramePixels {
    int width = 0;
    int height = 0;
    std::vector<float> luminance;

    [[nodiscard]] float At(int x, int y) const
    {
        // 範囲外は端の値を延長する。0 で埋めるとコマの縁に偽の動きが出る。
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

// アトラスから 1 コマを切り出して輝度化する。
FramePixels ExtractFrame(const DirectX::Image& atlas, int frameIndex,
                         int columns, int rows)
{
    const int frameWidth = static_cast<int>(atlas.width) / columns;
    const int frameHeight = static_cast<int>(atlas.height) / rows;
    const int originX = (frameIndex % columns) * frameWidth;
    const int originY = (frameIndex / columns) * frameHeight;

    FramePixels frame;
    frame.width = frameWidth;
    frame.height = frameHeight;
    frame.luminance.resize(static_cast<std::size_t>(frameWidth) * frameHeight);

    const auto* pixels = reinterpret_cast<const float*>(atlas.pixels);
    const std::size_t rowFloats = atlas.rowPitch / sizeof(float);
    for (int y = 0; y < frameHeight; ++y) {
        for (int x = 0; x < frameWidth; ++x) {
            const std::size_t index =
                static_cast<std::size_t>(originY + y) * rowFloats
                + static_cast<std::size_t>(originX + x) * 4;
            const float r = pixels[index + 0];
            const float g = pixels[index + 1];
            const float b = pixels[index + 2];
            const float a = pixels[index + 3];
            // アルファを掛けておく。透明部分に残ったゴミ色へ引っ張られないようにする。
            frame.luminance[static_cast<std::size_t>(y) * frameWidth + x] =
                (0.299f * r + 0.587f * g + 0.114f * b) * a;
        }
    }
    return frame;
}

// ブロックマッチング: current の各画素まわりの窓が next のどこへ移ったかを探す。
// 返すのは「current → next」の移動量 [px]。
FlowField ComputeBlockMatchFlow(const FramePixels& current, const FramePixels& next,
                                int blockRadius, int searchRadius)
{
    FlowField flow;
    flow.width = current.width;
    flow.height = current.height;
    flow.dx.assign(static_cast<std::size_t>(flow.width) * flow.height, 0.0f);
    flow.dy.assign(static_cast<std::size_t>(flow.width) * flow.height, 0.0f);

    // 全画素で全探索すると O(W*H*S^2*B^2) になる。ブロック単位で 1 回だけ探索し、
    // 結果をブロック内へ配る (フリップブックの解像度なら十分な粒度)。
    const int step = (std::max)(blockRadius, 1);
    for (int blockY = 0; blockY < current.height; blockY += step) {
        for (int blockX = 0; blockX < current.width; blockX += step) {
            float bestScore = -1.0f;
            int bestDx = 0;
            int bestDy = 0;
            for (int offsetY = -searchRadius; offsetY <= searchRadius; ++offsetY) {
                for (int offsetX = -searchRadius; offsetX <= searchRadius; ++offsetX) {
                    // SAD (絶対差の総和) を最小化する。符号を反転してスコア最大化に揃える。
                    float difference = 0.0f;
                    for (int y = -blockRadius; y <= blockRadius; ++y) {
                        for (int x = -blockRadius; x <= blockRadius; ++x) {
                            difference += std::fabs(
                                current.At(blockX + x, blockY + y)
                                - next.At(blockX + x + offsetX, blockY + y + offsetY));
                        }
                    }
                    const float score = -difference;
                    // 同スコアなら移動量の小さい方を採る (静止部分が暴れないように)。
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

// 3x3 平均でフロー場を均す。ブロック単位の探索結果はそのままだと
// ブロック境界で段差になり、warp したときにタイル状の継ぎ目が見える。
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

FlipbookMotionVectorResult Fail(std::string message)
{
    FlipbookMotionVectorResult result;
    result.success = false;
    result.message = std::move(message);
    return result;
}

} // namespace

FlipbookMotionVectorResult GenerateFlipbookMotionVectors(
    const std::string& sourcePath, const FlipbookMotionVectorSettings& settings)
{
    const int columns = (std::max)(settings.columns, 1);
    const int rows = (std::max)(settings.rows, 1);
    const int frameCount = columns * rows;
    if (frameCount < 2)
        return Fail("コマが 1 枚しかないため動きを解析できません (Columns × Rows を 2 以上に)");

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

    // 以降の解析は float で行う。8bit のまま差分を取ると量子化で動きが潰れる。
    DirectX::ScratchImage atlas;
    hr = DirectX::Convert(*loaded.GetImage(0, 0, 0), DXGI_FORMAT_R32G32B32A32_FLOAT,
                          DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, atlas);
    if (FAILED(hr)) return Fail("テクスチャを float へ変換できません");

    const DirectX::Image& atlasImage = *atlas.GetImage(0, 0, 0);
    if (atlasImage.width % static_cast<std::size_t>(columns) != 0
        || atlasImage.height % static_cast<std::size_t>(rows) != 0) {
        return Fail("アトラスの寸法が Columns / Rows で割り切れません ("
                    + std::to_string(atlasImage.width) + "x" + std::to_string(atlasImage.height) + ")");
    }

    const int frameWidth = static_cast<int>(atlasImage.width) / columns;
    const int frameHeight = static_cast<int>(atlasImage.height) / rows;
    const int searchRadius = std::clamp(settings.searchRadius, 1,
                                        (std::min)(frameWidth, frameHeight) / 2);
    const int blockRadius = std::clamp(settings.blockRadius, 1, searchRadius);

    // 出力は入力と同じ寸法。各コマの位置に、そのコマから次のコマへの速度を書く。
    DirectX::ScratchImage output;
    hr = output.Initialize2D(DXGI_FORMAT_R32G32B32A32_FLOAT, atlasImage.width, atlasImage.height, 1, 1);
    if (FAILED(hr)) return Fail("出力バッファを確保できません");
    const DirectX::Image& outputImage = *output.GetImage(0, 0, 0);
    auto* outputPixels = reinterpret_cast<float*>(outputImage.pixels);
    const std::size_t outputRowFloats = outputImage.rowPitch / sizeof(float);

    float maxObservedFlow = 0.0f;
    for (int frame = 0; frame < frameCount; ++frame) {
        const int column = frame % columns;
        const int row = frame / columns;
        const int nextFrame = settings.rowSequences
            ? row * columns + (column + 1) % columns
            : (frame + 1) % frameCount;
        // 行を独立列として扱う場合、非ループの終端判定も各行末尾で行う。
        const bool hasNext = settings.rowSequences
            ? (settings.loop || column + 1 < columns)
            : (settings.loop || frame + 1 < frameCount);

        FlowField flow;
        if (hasNext) {
            const FramePixels current = ExtractFrame(atlasImage, frame, columns, rows);
            const FramePixels next = ExtractFrame(atlasImage, nextFrame, columns, rows);
            flow = ComputeBlockMatchFlow(current, next, blockRadius, searchRadius);
            SmoothFlow(flow, settings.smoothIterations);
        } else {
            flow.width = frameWidth;
            flow.height = frameHeight;
            flow.dx.assign(static_cast<std::size_t>(frameWidth) * frameHeight, 0.0f);
            flow.dy.assign(static_cast<std::size_t>(frameWidth) * frameHeight, 0.0f);
        }

        const int originX = (frame % columns) * frameWidth;
        const int originY = (frame / columns) * frameHeight;
        for (int y = 0; y < frameHeight; ++y) {
            for (int x = 0; x < frameWidth; ++x) {
                const std::size_t flowIndex = static_cast<std::size_t>(y) * flow.width + x;
                const float rawX = flow.dx[flowIndex];
                const float rawY = flow.dy[flowIndex];
                maxObservedFlow = (std::max)(maxObservedFlow,
                                             std::sqrt(rawX * rawX + rawY * rawY));
                // 探索半径で正規化して [-1,1] に収め、シェーダー側の期待に合わせて
                // [0,1] へエンコードする (Particle.hlsl が *2-1 で戻す)。
                const float normalizedX =
                    std::clamp(rawX / static_cast<float>(searchRadius) * settings.strength, -1.0f, 1.0f);
                const float normalizedY =
                    std::clamp(rawY / static_cast<float>(searchRadius) * settings.strength, -1.0f, 1.0f);
                const std::size_t outIndex =
                    static_cast<std::size_t>(originY + y) * outputRowFloats
                    + static_cast<std::size_t>(originX + x) * 4;
                outputPixels[outIndex + 0] = normalizedX * 0.5f + 0.5f;
                outputPixels[outIndex + 1] = normalizedY * 0.5f + 0.5f;
                outputPixels[outIndex + 2] = 0.0f;
                outputPixels[outIndex + 3] = 1.0f;
            }
        }
    }

    // PNG は 8bit。書き出し前に 8bit へ落とす (MV の精度は 1/255 で十分)。
    DirectX::ScratchImage encoded;
    hr = DirectX::Convert(outputImage, DXGI_FORMAT_R8G8B8A8_UNORM,
                          DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, encoded);
    if (FAILED(hr)) return Fail("出力を 8bit へ変換できません");

    std::filesystem::path destination(resolvedPath);
    destination.replace_extension();
    destination += "_mv.png";
    const std::wstring wideDestination = destination.wstring();
    hr = DirectX::SaveToWICFile(*encoded.GetImage(0, 0, 0), DirectX::WIC_FLAGS_NONE,
                                DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG),
                                wideDestination.c_str());
    if (FAILED(hr)) return Fail("PNG を書き出せません: " + destination.string());

    // RGは色ではなく速度なのでsRGB変換を禁止し、2チャンネルを保持できるBC5で扱う。
    // Mip生成はフレーム境界の速度を混ぜるため無効にする。
    TextureAsset motionVectorAsset;
    motionVectorAsset.sourcePath = destination.string();
    motionVectorAsset.settings = DefaultSettingsForType(TextureType::Data);
    motionVectorAsset.settings.compression = TextureCompression::BC5;
    motionVectorAsset.settings.mipmaps = false;
    motionVectorAsset.settings.maxSize = 16384;
    motionVectorAsset.settings.alphaMode = AlphaMode::None;
    motionVectorAsset.settings.wrapU = TextureWrap::Clamp;
    motionVectorAsset.settings.wrapV = TextureWrap::Clamp;
    motionVectorAsset.settings.filter = TextureFilter::Bilinear;
    TexDescSerializer serializer;
    if (!serializer.Save(motionVectorAsset, destination.string() + ".meta"))
        return Fail("Motion Vector .meta を書き出せません: " + destination.string() + ".meta");

    FlipbookMotionVectorResult result;
    result.success = true;
    result.outputPath = destination.string();
    result.frameCount = frameCount;
    result.maxObservedFlow = maxObservedFlow;
    // 探索半径に張り付いている = 実際の動きが探索範囲を超えている可能性が高い。
    // 使う側が半径を上げる判断をできるよう、要約に必ず出す。
    result.message = "生成しました: " + std::to_string(frameCount) + " コマ / 最大移動量 "
        + std::to_string(static_cast<int>(maxObservedFlow)) + "px (探索半径 "
        + std::to_string(searchRadius) + "px)";
    if (maxObservedFlow >= static_cast<float>(searchRadius))
        result.message += " — 探索半径を上げると精度が上がる可能性があります";
    return result;
}

} // namespace fbzz::asset
