/// @file    TextureAnalysis.cpp
/// @brief   VFX 素材テクスチャの特徴量抽出と、そこから決まるオーサリング推奨値の導出。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// 処理フロー:
/// 1. テクスチャを R32G32B32A32_FLOAT で読む
/// 2. 原寸のまま flipbook のコマ割りを推定 (縮小すると境界が潰れるため)
/// 3. 統計用に縮小し、アルファ・輝度・形状の特徴量を取る
/// 4. 特徴量から blendMode / alphaSource / sprite 設定などの推奨値を組み立てる
#pragma comment(lib, "ole32.lib")  // DirectXTex の WIC コーデックに必要

#include <Engine/Asset/TextureAnalysis.hpp>

#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <DirectXTex.h>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace fbzz::asset {
namespace {

TextureAnalysis Fail(std::string message)
{
    TextureAnalysis result;
    result.success = false;
    result.message = std::move(message);
    return result;
}

// RGBA float の平面。解析は全てこの形に落としてから行う。
struct Pixels {
    int width = 0;
    int height = 0;
    std::vector<float> rgba; // width * height * 4

    [[nodiscard]] const float* At(int x, int y) const
    {
        const int cx = std::clamp(x, 0, width - 1);
        const int cy = std::clamp(y, 0, height - 1);
        return &rgba[(static_cast<std::size_t>(cy) * width + cx) * 4];
    }
};

float Luminance(const float* pixel)
{
    return 0.2126f * pixel[0] + 0.7152f * pixel[1] + 0.0722f * pixel[2];
}

Pixels ToPixels(const DirectX::Image& image)
{
    Pixels result;
    result.width = static_cast<int>(image.width);
    result.height = static_cast<int>(image.height);
    result.rgba.resize(static_cast<std::size_t>(result.width) * result.height * 4);
    const auto* source = reinterpret_cast<const float*>(image.pixels);
    const std::size_t rowFloats = image.rowPitch / sizeof(float);
    for (int y = 0; y < result.height; ++y) {
        for (int x = 0; x < result.width; ++x) {
            const std::size_t from = static_cast<std::size_t>(y) * rowFloats + static_cast<std::size_t>(x) * 4;
            const std::size_t to = (static_cast<std::size_t>(y) * result.width + x) * 4;
            for (int c = 0; c < 4; ++c) result.rgba[to + c] = source[from + c];
        }
    }
    return result;
}

// ボックスフィルタで縮小する。統計量を取るだけなので品質より速度と単純さを優先する。
Pixels Downsample(const Pixels& source, int maxDimension)
{
    const int longest = (std::max)(source.width, source.height);
    if (longest <= maxDimension || maxDimension <= 0) return source;
    const float scale = static_cast<float>(maxDimension) / static_cast<float>(longest);
    Pixels result;
    result.width = (std::max)(1, static_cast<int>(std::lround(source.width * scale)));
    result.height = (std::max)(1, static_cast<int>(std::lround(source.height * scale)));
    result.rgba.assign(static_cast<std::size_t>(result.width) * result.height * 4, 0.0f);
    for (int y = 0; y < result.height; ++y) {
        const int y0 = y * source.height / result.height;
        const int y1 = (std::max)(y0 + 1, (y + 1) * source.height / result.height);
        for (int x = 0; x < result.width; ++x) {
            const int x0 = x * source.width / result.width;
            const int x1 = (std::max)(x0 + 1, (x + 1) * source.width / result.width);
            float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            int count = 0;
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx) {
                    const float* pixel = source.At(sx, sy);
                    for (int c = 0; c < 4; ++c) sum[c] += pixel[c];
                    ++count;
                }
            const std::size_t to = (static_cast<std::size_t>(y) * result.width + x) * 4;
            for (int c = 0; c < 4; ++c) result.rgba[to + c] = sum[c] / static_cast<float>((std::max)(count, 1));
        }
    }
    return result;
}

// 指定グリッドで切ったときの「タイル境界のまたぎ差」と「コマ間の内容量の揃い方」を測る。
// WHY: フリップブックは各コマが独立した絵なので、正しい境界では隣り合う画素が
//      不連続になる。逆に 1 枚絵を誤って切ると境界は連続したままになる。
//      つまり seamScore は「大きいほどそのグリッドらしい」。
//      ただし境界が偶然目立つだけの 1 枚絵も拾ってしまうため、
//      各コマの内容量 (アルファ総和) が揃っていることを uniformity で併せて要求する。
FlipbookGridCandidate ScoreGrid(const Pixels& image, int columns, int rows)
{
    FlipbookGridCandidate candidate;
    candidate.columns = columns;
    candidate.rows = rows;

    const int tileWidth = image.width / columns;
    const int tileHeight = image.height / rows;
    if (tileWidth < 4 || tileHeight < 4) return candidate;

    // 縦の境界線をまたぐ差 / 同じ位置の 1 画素内側の差、の比を取る。
    // 生の差分だと絵の細かさで値が変わるため、周辺の変化量で正規化する。
    float seamSum = 0.0f;
    float interiorSum = 0.0f;
    int seamCount = 0;
    for (int c = 1; c < columns; ++c) {
        const int x = c * tileWidth;
        for (int y = 0; y < image.height; ++y) {
            const float across = std::abs(Luminance(image.At(x, y)) - Luminance(image.At(x - 1, y)));
            const float inside = std::abs(Luminance(image.At(x + 1, y)) - Luminance(image.At(x, y)));
            seamSum += across;
            interiorSum += inside;
            ++seamCount;
        }
    }
    for (int r = 1; r < rows; ++r) {
        const int y = r * tileHeight;
        for (int x = 0; x < image.width; ++x) {
            const float across = std::abs(Luminance(image.At(x, y)) - Luminance(image.At(x, y - 1)));
            const float inside = std::abs(Luminance(image.At(x, y + 1)) - Luminance(image.At(x, y)));
            seamSum += across;
            interiorSum += inside;
            ++seamCount;
        }
    }
    if (seamCount == 0) return candidate;
    const float seamMean = seamSum / static_cast<float>(seamCount);
    const float interiorMean = interiorSum / static_cast<float>(seamCount);
    candidate.seamScore = seamMean / (std::max)(interiorMean, 1.0e-5f);

    // 各コマの内容量。フリップブックなら全コマに絵があり、総和が極端にばらつかない。
    std::vector<float> tileMass(static_cast<std::size_t>(columns) * rows, 0.0f);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c) {
            float mass = 0.0f;
            for (int y = 0; y < tileHeight; ++y)
                for (int x = 0; x < tileWidth; ++x) {
                    const float* pixel = image.At(c * tileWidth + x, r * tileHeight + y);
                    mass += pixel[3] > 0.0f ? pixel[3] : Luminance(pixel);
                }
            tileMass[static_cast<std::size_t>(r) * columns + c] =
                mass / static_cast<float>(tileWidth * tileHeight);
        }
    float massMean = 0.0f;
    for (const float mass : tileMass) massMean += mass;
    massMean /= static_cast<float>(tileMass.size());
    if (massMean <= 1.0e-5f) return candidate;
    float variance = 0.0f;
    for (const float mass : tileMass) variance += (mass - massMean) * (mass - massMean);
    variance /= static_cast<float>(tileMass.size());
    // 変動係数が小さいほど揃っている。1 - CV を [0,1] へ clamp して使う。
    const float cv = std::sqrt(variance) / massMean;
    candidate.uniformity = std::clamp(1.0f - cv, 0.0f, 1.0f);
    // 空コマがあるアトラスは「割り方が違う」と判断する。
    for (const float mass : tileMass)
        if (mass < massMean * 0.05f) candidate.uniformity = 0.0f;
    return candidate;
}

// アトラスらしいコマ割りを探す。2^n 分割と一般的な段組だけを候補にする。
// WHY: 総当たりすると 1x2 や 2x1 のような「切っても切らなくても同じ」候補が
//      上位に紛れ、判定がぶれる。実際に使われるのは正方に近い等分割なので、
//      候補を絞った方が精度も速度も上がる。
std::vector<FlipbookGridCandidate> DetectFlipbook(const Pixels& image)
{
    static constexpr std::array<int, 6> kDivisors = { 2, 3, 4, 5, 6, 8 };
    std::vector<FlipbookGridCandidate> candidates;
    for (const int columns : kDivisors) {
        for (const int rows : kDivisors) {
            if (image.width % columns != 0 || image.height % rows != 0) continue;
            // 極端に細長いコマは実素材ではまず使わない。
            const float tileAspect = static_cast<float>(image.width / columns)
                                   / static_cast<float>(image.height / rows);
            if (tileAspect < 0.5f || tileAspect > 2.0f) continue;
            const FlipbookGridCandidate candidate = ScoreGrid(image, columns, rows);
            // 境界が周囲より明確に不連続で、かつ全コマが揃っているものだけ残す。
            if (candidate.seamScore < 1.6f || candidate.uniformity < 0.5f) continue;
            candidates.push_back(candidate);
        }
    }
    // コマ数が少ない方を優先しつつ、境界の明確さで並べる。
    // WHY: 4x4 が正しいアトラスは 2x2 でも境界が立つ (4x4 の境界を含むため)。
    //      分割数が大きいほど seamScore は上がりやすいので、素点だけで並べると
    //      常に最大分割が勝ってしまう。コマ数で割って正規化する。
    std::sort(candidates.begin(), candidates.end(),
        [](const FlipbookGridCandidate& a, const FlipbookGridCandidate& b) {
            const float scoreA = a.seamScore * a.uniformity / std::sqrt(static_cast<float>(a.columns * a.rows));
            const float scoreB = b.seamScore * b.uniformity / std::sqrt(static_cast<float>(b.columns * b.rows));
            return scoreA > scoreB;
        });
    if (candidates.size() > 4) candidates.resize(4);
    return candidates;
}

void AnalyzeStatistics(const Pixels& image, TextureAnalysis& out)
{
    const std::size_t total = static_cast<std::size_t>(image.width) * image.height;
    if (total == 0) return;

    out.alphaMin = 1.0f;
    out.alphaMax = 0.0f;
    double alphaSum = 0.0;
    double luminanceSum = 0.0;
    double saturationSum = 0.0;
    double colorSum[3] = { 0.0, 0.0, 0.0 };
    double colorWeight = 0.0;
    std::size_t transparent = 0;
    std::size_t opaque = 0;
    std::size_t covered = 0;
    // 事前乗算判定は「RGB が A を超える画素が 1 つも無い」ことで見る。
    // 単純な閾値だと 8bit 由来の丸めで誤検出するため、わずかな超過は許す。
    bool anyRgbAboveAlpha = false;
    bool anyPartialAlpha = false;

    for (std::size_t index = 0; index < total; ++index) {
        const float* pixel = &image.rgba[index * 4];
        const float alpha = pixel[3];
        out.alphaMin = (std::min)(out.alphaMin, alpha);
        out.alphaMax = (std::max)(out.alphaMax, alpha);
        alphaSum += alpha;
        if (alpha < 0.02f) ++transparent;
        else if (alpha > 0.98f) ++opaque;
        else anyPartialAlpha = true;
        if (alpha > 0.1f) ++covered;

        const float luminance = Luminance(pixel);
        luminanceSum += luminance;
        out.luminanceMax = (std::max)(out.luminanceMax, luminance);

        const float maxChannel = (std::max)({ pixel[0], pixel[1], pixel[2] });
        const float minChannel = (std::min)({ pixel[0], pixel[1], pixel[2] });
        if (maxChannel > 1.0e-4f) saturationSum += (maxChannel - minChannel) / maxChannel;

        // 色は「見える部分」の平均。透明部分の色は最終的な絵に出ない。
        const double weight = static_cast<double>(alpha);
        for (int c = 0; c < 3; ++c) colorSum[c] += pixel[c] * weight;
        colorWeight += weight;

        if (maxChannel > alpha + 0.02f) anyRgbAboveAlpha = true;
    }

    out.alphaMean = static_cast<float>(alphaSum / static_cast<double>(total));
    out.luminanceMean = static_cast<float>(luminanceSum / static_cast<double>(total));
    out.saturationMean = static_cast<float>(saturationSum / static_cast<double>(total));
    out.transparentRatio = static_cast<float>(transparent) / static_cast<float>(total);
    out.opaqueRatio = static_cast<float>(opaque) / static_cast<float>(total);
    out.coverage = static_cast<float>(covered) / static_cast<float>(total);
    if (colorWeight > 1.0e-6) {
        for (int c = 0; c < 3; ++c)
            out.averageColor[c] = static_cast<float>(colorSum[c] / colorWeight);
    }
    // アルファが全画素同じなら実質「アルファ無し」。マスクとして機能していない。
    out.alphaIsMeaningful = (out.alphaMax - out.alphaMin) > 0.05f;
    // 事前乗算はストレート素材との判別が目的なので、中間アルファが存在する素材でのみ意味を持つ。
    out.likelyPremultiplied = out.alphaIsMeaningful && anyPartialAlpha && !anyRgbAboveAlpha;

    // 中心 1/3 と外周の輝度比。発光する芯を持つ素材で大きくなる。
    const int cx0 = image.width / 3, cx1 = image.width * 2 / 3;
    const int cy0 = image.height / 3, cy1 = image.height * 2 / 3;
    double centerSum = 0.0, edgeSum = 0.0;
    std::size_t centerCount = 0, edgeCount = 0;
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x) {
            const float* pixel = image.At(x, y);
            // アルファを掛けた「実際に見える明るさ」で比べる。
            const float visible = Luminance(pixel) * (out.alphaIsMeaningful ? pixel[3] : 1.0f);
            if (x >= cx0 && x < cx1 && y >= cy0 && y < cy1) { centerSum += visible; ++centerCount; }
            else { edgeSum += visible; ++edgeCount; }
        }
    const float centerMean = centerCount > 0 ? static_cast<float>(centerSum / centerCount) : 0.0f;
    const float edgeMean = edgeCount > 0 ? static_cast<float>(edgeSum / edgeCount) : 0.0f;
    out.coreHotspot = centerMean / (std::max)(edgeMean, 1.0e-4f);

    // 中心対称性: 180 度回転したものとの一致度。放射状の puff / glow で高くなる。
    double symmetryDiff = 0.0;
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x) {
            const float* a = image.At(x, y);
            const float* b = image.At(image.width - 1 - x, image.height - 1 - y);
            symmetryDiff += std::abs(Luminance(a) * a[3] - Luminance(b) * b[3]);
        }
    out.radialSymmetry = std::clamp(
        1.0f - static_cast<float>(symmetryDiff / static_cast<double>(total)) * 4.0f, 0.0f, 1.0f);

    // 縁の硬さ: 半透明画素におけるアルファ勾配の平均。
    // 切り抜き素材は 0 と 1 の間が数画素しかないため勾配が大きい。
    double gradientSum = 0.0;
    std::size_t gradientCount = 0;
    for (int y = 1; y < image.height - 1; ++y)
        for (int x = 1; x < image.width - 1; ++x) {
            const float alpha = image.At(x, y)[3];
            if (alpha < 0.05f || alpha > 0.95f) continue;
            const float dx = image.At(x + 1, y)[3] - image.At(x - 1, y)[3];
            const float dy = image.At(x, y + 1)[3] - image.At(x, y - 1)[3];
            gradientSum += std::sqrt(dx * dx + dy * dy);
            ++gradientCount;
        }
    out.edgeHardness = gradientCount > 0
        ? static_cast<float>(gradientSum / static_cast<double>(gradientCount)) : 0.0f;

    // タイリング可否: 対向する端どうしの一致度。
    double seamDiff = 0.0;
    for (int y = 0; y < image.height; ++y)
        seamDiff += std::abs(Luminance(image.At(0, y)) - Luminance(image.At(image.width - 1, y)));
    for (int x = 0; x < image.width; ++x)
        seamDiff += std::abs(Luminance(image.At(x, 0)) - Luminance(image.At(x, image.height - 1)));
    const double seamSamples = static_cast<double>(image.width + image.height);
    out.seamlessScore = std::clamp(1.0f - static_cast<float>(seamDiff / seamSamples) * 4.0f, 0.0f, 1.0f);
}

// 観測から設定を決める。ここが「解析」を「オーサリング支援」に変えている部分で、
// 判断基準は vfx.guide の規約と一致させてある (別基準にすると助言が食い違う)。
void BuildRecommendations(TextureAnalysis& out)
{
    const auto add = [&out](std::string schemaPath, std::string value, std::string reason) {
        out.recommendations.push_back({ std::move(schemaPath), std::move(value), std::move(reason) });
    };

    const bool isFlipbook = !out.flipbookCandidates.empty();
    if (isFlipbook) {
        const FlipbookGridCandidate& best = out.flipbookCandidates.front();
        add("material.particle.spriteColumns", std::to_string(best.columns),
            "タイル境界の不連続からコマ割りを検出しました (境界比 "
                + std::to_string(best.seamScore).substr(0, 4) + ")");
        add("material.particle.spriteRows", std::to_string(best.rows),
            "同上。誤検出が疑われる場合は flipbookCandidates の他候補を試してください");
        add("material.particle.spriteEndFrame", std::to_string(best.columns * best.rows - 1),
            "全コマを再生する場合の終端フレーム");
        add("material.particle.spriteRandomStartFrame", "true",
            "同時に湧いた粒子が全部同じコマで回るのを防ぎます");
    }

    // alphaSource — アルファが機能していない素材は輝度から抜くしかない。
    if (!out.alphaIsMeaningful) {
        add("material.particle.alphaSource", "1",
            "アルファチャンネルが実データを持たない (min=" + std::to_string(out.alphaMin).substr(0, 4)
                + " max=" + std::to_string(out.alphaMax).substr(0, 4)
                + ") ため、Luminance からアルファを取る必要があります。"
                  "TextureAlpha のままだと矩形の板として描かれます");
    }

    // blend_mode — 事前乗算 > 発光する芯 > それ以外の順で決まる。
    if (out.likelyPremultiplied) {
        add("material.blend_mode", "Premultiplied",
            "全画素で RGB <= A が成り立つ事前乗算済み素材です。"
            "Alpha ブレンドで使うと縁が黒く縁取られます");
    } else if (out.coreHotspot > 2.2f && out.saturationMean < 0.55f) {
        add("material.blend_mode", "Additive",
            "中心の輝度が周辺の " + std::to_string(out.coreHotspot).substr(0, 4)
                + " 倍で、発光する芯を持つ素材です。加算が向きます");
    } else if (out.coverage > 0.45f) {
        add("material.blend_mode", "AlphaBlend",
            "面積の " + std::to_string(static_cast<int>(out.coverage * 100.0f))
                + "% を覆う body 素材です。背景を隠す層は Alpha にしないと"
                  "重なりが白飽和します");
        add("particle.sortMode", "1",
            "Alpha ブレンドは描画順で結果が変わるため BackToFront が必須です");
    }

    // softParticles — 硬い縁の素材は交差面が線として見える。
    if (out.edgeHardness > 0.35f && out.alphaIsMeaningful) {
        add("material.particle.softParticles", "true",
            "縁のアルファ勾配が大きい (硬い) 素材です。地面や壁と交差したとき"
            "切り口が直線として出るため、深度フェードで隠します");
    }

    // 分類。層構成のどこへ置く素材かを一言で示す。
    if (isFlipbook) out.classification = "flipbook";
    else if (out.coreHotspot > 2.2f && out.radialSymmetry > 0.6f) out.classification = "glow";
    else if (out.coverage > 0.45f && out.edgeHardness < 0.3f) out.classification = "smoke";
    else if (out.coverage < 0.12f) out.classification = "spark";
    else if (!out.alphaIsMeaningful) out.classification = "mask";
    else out.classification = "unknown";
}

} // namespace

TextureAnalysis AnalyzeTexture(const std::string& sourcePath, int maxSampleDimension)
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

    const DirectX::TexMetadata& metadata = loaded.GetMetadata();
    DirectX::ScratchImage converted;
    hr = DirectX::Convert(*loaded.GetImage(0, 0, 0), DXGI_FORMAT_R32G32B32A32_FLOAT,
                          DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
    if (FAILED(hr)) return Fail("テクスチャを float へ変換できません");

    TextureAnalysis result;
    result.success = true;
    result.resolvedPath = resolvedPath;
    result.width = static_cast<int>(metadata.width);
    result.height = static_cast<int>(metadata.height);
    result.powerOfTwo = result.width > 0 && result.height > 0
        && (result.width & (result.width - 1)) == 0
        && (result.height & (result.height - 1)) == 0;
    result.hasAlphaChannel = DirectX::HasAlpha(metadata.format);

    // DDS は事前乗算をメタデータで宣言できる。宣言があるなら画素の推測より確実なので、
    // 統計を取り終えた後に上書きする (declaredAlphaMode で分岐する)。
    const DirectX::TEX_ALPHA_MODE declaredAlphaMode = metadata.GetAlphaMode();

    const Pixels full = ToPixels(*converted.GetImage(0, 0, 0));
    // コマ割りの判定は原寸で行う。縮小すると境界の不連続が平均化されて消える。
    result.flipbookCandidates = DetectFlipbook(full);
    // 統計は縮小して取る。4K を原寸で何周も走査するとエディター操作としては遅すぎる。
    const Pixels sampled = Downsample(full, maxSampleDimension);
    AnalyzeStatistics(sampled, result);
    // 宣言があるならそれが正。画素からの推測 (RGB <= A) は、たまたま暗い素材を
    // 事前乗算と誤判定しうるため、明示情報がある場合は必ず優先する。
    if (declaredAlphaMode == DirectX::TEX_ALPHA_MODE_PREMULTIPLIED) result.likelyPremultiplied = true;
    else if (declaredAlphaMode == DirectX::TEX_ALPHA_MODE_STRAIGHT) result.likelyPremultiplied = false;
    BuildRecommendations(result);

    result.message = result.classification + " / " + std::to_string(result.width) + "x"
        + std::to_string(result.height)
        + (result.flipbookCandidates.empty()
               ? std::string(" / single frame")
               : " / flipbook " + std::to_string(result.flipbookCandidates.front().columns) + "x"
                     + std::to_string(result.flipbookCandidates.front().rows));
    return result;
}

namespace {

const char* BlendModeName(renderer::BlendMode mode)
{
    switch (mode) {
    case renderer::BlendMode::ALPHA_BLEND:   return "Alpha";
    case renderer::BlendMode::ADDITIVE:      return "Additive";
    case renderer::BlendMode::PREMULTIPLIED: return "Premultiplied";
    case renderer::BlendMode::OPAQUE_BLEND:
    default:                                 return "Opaque";
    }
}

const char* RenderPathName(RenderPath path)
{
    switch (path) {
    case RenderPath::Particle: return "particle";
    case RenderPath::Trail:    return "trail";
    case RenderPath::UI:       return "ui";
    case RenderPath::Decal:    return "decal";
    case RenderPath::PostProcess: return "post_process";
    case RenderPath::Auto:
    default:                   return "auto";
    }
}

} // namespace

MaterialAnalysis AnalyzeMaterial(const std::string& materialPath)
{
    MaterialAnalysis result;
    MaterialAsset material;
    if (!LoadMaterialAssetFromFile(materialPath, material)) {
        result.success = false;
        result.message = "マテリアルを読み込めません: " + materialPath;
        return result;
    }

    result.success = true;
    result.resolvedPath = materialPath;
    result.shaderPath = material.shaderPath;
    result.blendMode = BlendModeName(material.blendMode);
    result.renderPath = RenderPathName(material.renderPath);
    result.doubleSided = material.doubleSided;
    result.depthWrite = material.depthWrite;
    result.renderQueue = static_cast<int>(material.renderQueue);

    if (const auto albedo = material.textures.find("albedo");
        albedo != material.textures.end() && !albedo->second.empty()) {
        result.albedoTexturePath = albedo->second;
        result.hasAlbedoTexture = true;
        result.albedoAnalysis = AnalyzeTexture(albedo->second);
    }

    const auto addFinding = [&result](std::string text) {
        result.findings.push_back(std::move(text));
    };
    const auto addRecommendation = [&result](std::string schemaPath, std::string value,
                                             std::string reason) {
        result.recommendations.push_back({ std::move(schemaPath), std::move(value), std::move(reason) });
    };

    // ── ParticleEmitter へ割り当てる前提での診断 ──

    // render_path が particle でないと ParticlePass のシェーダー変数と噛み合わない。
    if (material.renderPath != RenderPath::Particle)
        addFinding("render_path が \"" + result.renderPath
                   + "\" です。ParticleEmitter へ割り当てるなら \"particle\" にしてください "
                     "(ParticlePass が albedo と blendMode を読む経路が変わります)");

    // 不透明のままの .mat をパーティクルへ割り当てると、粒子が板として重なる。
    if (material.blendMode == renderer::BlendMode::OPAQUE_BLEND)
        addFinding("blend_mode が Opaque です。パーティクルは半透明前提なので、"
                   "Alpha / Additive / Premultiplied のいずれかにしてください");

    if (!result.hasAlbedoTexture) {
        addFinding("albedo テクスチャが未設定です。色だけのマテリアルになり、"
                   "粒子は単色の矩形として描かれます");
    } else if (!result.albedoAnalysis.success) {
        addFinding("albedo テクスチャを解析できません: " + result.albedoAnalysis.message);
    } else {
        const TextureAnalysis& texture = result.albedoAnalysis;
        // テクスチャの中身が要求する blend_mode と .mat の宣言を突き合わせる。
        // WHY: ここが「テクスチャ単体の推測」と「実際の描画設定」の差が出る唯一の場所。
        //      ブレンドは .mat が唯一の正本なので、直すのは常に .mat 側になる。
        std::string wanted;
        std::string why;
        if (texture.likelyPremultiplied) {
            wanted = "Premultiplied";
            why = "テクスチャが事前乗算済み (全画素で RGB <= A)";
        } else if (texture.coreHotspot > 2.2f && texture.saturationMean < 0.55f) {
            wanted = "Additive";
            why = "テクスチャが発光する芯を持つ (中心輝度が周辺の "
                + std::to_string(texture.coreHotspot).substr(0, 4) + " 倍)";
        } else if (texture.coverage > 0.45f) {
            wanted = "Alpha";
            why = "テクスチャが面積の " + std::to_string(static_cast<int>(texture.coverage * 100.0f))
                + "% を覆う body 素材";
        }
        if (!wanted.empty() && wanted != result.blendMode) {
            result.blendModeConflictsWithTexture = true;
            addFinding("blend_mode が " + result.blendMode + " ですが、" + why
                       + "なので " + wanted + " が適切です。"
                         "ブレンドは .mat が唯一の正本なので、直すのはこの .mat です");
        }

        // アルファの取り出し方も .mat の [particle]。
        if (!texture.alphaIsMeaningful)
            addRecommendation("material.particle.alphaSource", "1",
                              "albedo テクスチャのアルファが実データを持たない (min="
                                  + std::to_string(texture.alphaMin).substr(0, 4) + " max="
                                  + std::to_string(texture.alphaMax).substr(0, 4)
                                  + ")。この .mat の [particle] へ設定します");

        // flipbook のコマ割りも .mat の [particle]。
        if (!texture.flipbookCandidates.empty()) {
            const auto& best = texture.flipbookCandidates.front();
            addRecommendation("material.particle.spriteColumns", std::to_string(best.columns),
                              "albedo テクスチャがアトラスです (コマ割りは .mat の [particle])");
            addRecommendation("material.particle.spriteRows", std::to_string(best.rows),
                              "同上");
            addRecommendation("material.particle.spriteEndFrame",
                              std::to_string(best.columns * best.rows - 1), "全コマを再生する終端");
        }

        // 描画順だけは «いつどこに出すか» の側なので Emitter が持つ。
        if (material.blendMode == renderer::BlendMode::ALPHA_BLEND
            || material.blendMode == renderer::BlendMode::PREMULTIPLIED)
            addRecommendation("particle.sortMode", "1",
                              ".mat が " + result.blendMode
                                  + " なので、描画順が結果を変えます。BackToFront が必須です");
    }

    result.message = result.blendMode + " / " + result.renderPath
        + (result.hasAlbedoTexture ? " / albedo: " + result.albedoAnalysis.classification
                                   : std::string(" / no albedo texture"))
        + " / " + std::to_string(result.findings.size()) + " finding(s)";
    return result;
}

} // namespace fbzz::asset
