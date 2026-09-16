/// @file    ProceduralVFXTextures.cpp
/// @brief   VFX向けFlipbookとImpact DecalテクスチャのCPUプロシージャル生成実装。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <Engine/Asset/ProceduralVFXTextures.hpp>

#include "FlipbookImageIO.hpp"

#include <Engine/Asset/TextureAsset.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fbzz::asset {
namespace {

constexpr float PI = 3.14159265358979323846f;

// CPU生成中の8bit RGBA画像と、安全な正規化値書き込みをまとめる。
struct RgbaImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;

    // 0～1の各チャンネルをクランプして8bit画素へ格納する。
    void Set(int x, int y, float r, float g, float b, float a)
    {
        const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4;
        pixels[index + 0] = static_cast<std::uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
        pixels[index + 1] = static_cast<std::uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
        pixels[index + 2] = static_cast<std::uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
        pixels[index + 3] = static_cast<std::uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
};

// Distortionの2次元ベクトル計算だけに使う軽量値型。
struct Float2 {
    float x = 0.0f;
    float y = 0.0f;
};

// ノイズやマスクの値を画像チャンネル範囲へ収める。
float Saturate(float value) { return std::clamp(value, 0.0f, 1.0f); }

// エッジを滑らかに補間し、プロシージャル形状のジャギーを抑える。
float SmoothStep(float edge0, float edge1, float value)
{
    if (edge0 == edge1) return value < edge0 ? 0.0f : 1.0f;
    const float t = Saturate((value - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

// Seedと格子座標から再現可能な疑似乱数を作る整数ハッシュ。
std::uint32_t Hash(std::uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

// 2次元格子座標を0～1の決定論的乱数へ変換する。
float Hash01(int x, int y, std::uint32_t seed)
{
    const std::uint32_t ux = static_cast<std::uint32_t>(x);
    const std::uint32_t uy = static_cast<std::uint32_t>(y);
    return static_cast<float>(Hash(ux * 0x9e3779b9u ^ uy * 0x85ebca6bu ^ seed) & 0x00ffffffu)
        / static_cast<float>(0x01000000u);
}

// 格子乱数を滑らかに補間した2次元Value Noiseを返す。
float ValueNoise(float x, float y, std::uint32_t seed)
{
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(ix);
    const float fy = y - static_cast<float>(iy);
    const float sx = fx * fx * (3.0f - 2.0f * fx);
    const float sy = fy * fy * (3.0f - 2.0f * fy);
    const float a = Hash01(ix, iy, seed);
    const float b = Hash01(ix + 1, iy, seed);
    const float c = Hash01(ix, iy + 1, seed);
    const float d = Hash01(ix + 1, iy + 1, seed);
    const float top = a + (b - a) * sx;
    const float bottom = c + (d - c) * sx;
    return top + (bottom - top) * sy;
}

// 複数オクターブを合成して煙・炎・クレーターの中周波ディテールを作る。
float Fbm(float x, float y, std::uint32_t seed, int octaves = 5)
{
    float value = 0.0f;
    float amplitude = 0.5f;
    float weight = 0.0f;
    for (int octave = 0; octave < octaves; ++octave) {
        value += ValueNoise(x, y, seed + static_cast<std::uint32_t>(octave) * 1013u) * amplitude;
        weight += amplitude;
        x = x * 2.03f + 17.1f;
        y = y * 2.03f - 11.7f;
        amplitude *= 0.5f;
    }
    return weight > 0.0f ? value / weight : 0.0f;
}

// 低周波ノイズで座標を歪めて、単純な格子感のない流体状ノイズを返す。
float WarpedNoise(float x, float y, float time, const ProceduralFlipbookSettings& settings,
                  std::uint32_t seed)
{
    const float scale = (std::max)(settings.noiseScale, 0.01f);
    const float warpX = Fbm(x * scale + time * 0.73f, y * scale - time * 0.31f, seed + 19u) - 0.5f;
    const float warpY = Fbm(x * scale - time * 0.27f, y * scale + time * 0.61f, seed + 43u) - 0.5f;
    return Fbm((x + warpX * settings.warpStrength) * scale,
               (y + warpY * settings.warpStrength) * scale - time, seed + 71u);
}

using detail::FindAvailableBase;
using detail::SaveTextureMeta;

bool SavePng(const RgbaImage& source, const std::filesystem::path& path, std::string& outError)
{
    return detail::SavePngRgba8(path, static_cast<std::uint32_t>(source.width),
                                static_cast<std::uint32_t>(source.height), source.pixels, outError);
}

// 膨張・上昇・散逸するグレースケール煙の1画素を生成する。
void GenerateSmokePixel(float x, float y, float time, const ProceduralFlipbookSettings& settings,
                        std::uint32_t seed, float& r, float& g, float& b, float& a)
{
    const float rise = time * 0.42f;
    const float radiusX = 0.28f + time * 0.34f;
    const float radiusY = 0.25f + time * 0.46f;
    const float px = x / radiusX;
    const float py = (y + rise - 0.18f) / radiusY;
    const float distance = std::sqrt(px * px + py * py);
    const float noise = WarpedNoise(x, y + rise, time * 1.7f, settings, seed);
    const float shape = 1.0f - distance + (noise - 0.5f) * 0.72f;
    a = SmoothStep(0.0f, 0.28f, shape) * (1.0f - time * 0.38f);
    // Smokeは通常Alpha Blendへ割り当てるためRGBを事前乗算しない。
    // アルファを二重に掛けると薄い縁が不自然に消える。
    const float shade = 0.62f + noise * 0.38f;
    r = shade;
    g = shade;
    b = shade;
}

// 下部が太く上部が揺らぐ加算向け炎の1画素を生成する。
void GenerateFirePixel(float x, float y, float time, const ProceduralFlipbookSettings& settings,
                       std::uint32_t seed, float& r, float& g, float& b, float& a)
{
    const float vertical = Saturate((1.0f - y) * 0.5f);
    const float width = 0.08f + (1.0f - vertical) * 0.48f;
    const float noise = WarpedNoise(x, y, time * 2.6f, settings, seed);
    const float sway = (Fbm(time * 2.0f, vertical * 3.0f, seed + 109u, 3) - 0.5f) * 0.28f * vertical;
    const float side = std::abs(x - sway) / width;
    const float top = vertical + (noise - 0.5f) * 0.35f;
    const float shape = (1.02f - side) * SmoothStep(0.03f, 0.22f, top)
        * SmoothStep(1.08f, 0.72f, top);
    a = SmoothStep(0.0f, 0.32f, shape + (noise - 0.5f) * 0.42f);
    const float core = Saturate((1.0f - side) * (1.2f - vertical) * 1.7f) * a;
    r = a;
    g = a * (0.18f + core * 0.72f);
    b = core * 0.08f;
}

// 発光コアから煙へ遷移する事前乗算Alpha爆発の1画素を生成する。
void GenerateExplosionPixel(float x, float y, float time,
                            const ProceduralFlipbookSettings& settings, std::uint32_t seed,
                            float& r, float& g, float& b, float& a)
{
    const float radius = 0.08f + (1.0f - std::pow(1.0f - time, 2.2f)) * 0.78f;
    const float distance = std::sqrt(x * x + y * y);
    const float noise = WarpedNoise(x, y, time * 1.4f, settings, seed);
    const float irregularDistance = distance + (noise - 0.5f) * 0.30f * radius;
    const float body = SmoothStep(radius + 0.10f, radius - 0.18f, irregularDistance);
    const float fade = 1.0f - SmoothStep(0.72f, 1.0f, time);
    a = body * (0.72f + noise * 0.28f) * fade;
    const float hot = Saturate((1.0f - time * 1.45f) * 2.0f)
        * SmoothStep(radius, radius * 0.18f, irregularDistance);
    const float smoke = a * (1.0f - hot * 0.55f);
    // Premultiplied Alphaで発光する芯と遮蔽する煙を同居させる。
    r = smoke * 0.18f + hot * 1.0f;
    g = smoke * 0.16f + hot * 0.34f;
    b = smoke * 0.14f + hot * 0.035f;
}

// Curl状のRG変位と円形Alphaマスクを持つ歪みの1画素を生成する。
void GenerateDistortionPixel(float x, float y, float time,
                             const ProceduralFlipbookSettings& settings, std::uint32_t seed,
                             float& r, float& g, float& b, float& a)
{
    const float phase = time * PI * 2.0f;
    const float sampleX = x + std::cos(phase) * 0.23f;
    const float sampleY = y + std::sin(phase) * 0.23f;
    constexpr float epsilon = 0.012f;
    const float nx0 = WarpedNoise(sampleX - epsilon, sampleY, 0.0f, settings, seed);
    const float nx1 = WarpedNoise(sampleX + epsilon, sampleY, 0.0f, settings, seed);
    const float ny0 = WarpedNoise(sampleX, sampleY - epsilon, 0.0f, settings, seed);
    const float ny1 = WarpedNoise(sampleX, sampleY + epsilon, 0.0f, settings, seed);
    Float2 flow{ (ny1 - ny0) / (epsilon * 2.0f), -(nx1 - nx0) / (epsilon * 2.0f) };
    const float length = std::sqrt(flow.x * flow.x + flow.y * flow.y);
    if (length > 0.001f) {
        flow.x /= length;
        flow.y /= length;
    }
    const float radialDistance = std::sqrt(x * x + y * y);
    const float mask = SmoothStep(1.0f, 0.62f, radialDistance);
    const float strength = mask * 0.42f;
    r = 0.5f + flow.x * strength;
    g = 0.5f + flow.y * strength;
    b = 0.5f;
    a = mask;
}

// 角度の周期境界を考慮した最短距離を返す。
float AngleDistance(float a, float b)
{
    float difference = std::fmod(std::abs(a - b), PI * 2.0f);
    if (difference > PI) difference = PI * 2.0f - difference;
    return difference;
}

// Seedから放射状の亀裂群を作り、指定座標でのマスク値を返す。
float DecalCracks(float x, float y, const ProceduralImpactDecalSettings& settings)
{
    const float angle = std::atan2(y, x);
    const float distance = std::sqrt(x * x + y * y);
    float cracks = 0.0f;
    for (int index = 0; index < settings.crackCount; ++index) {
        const float crackAngle = Hash01(index, 17, settings.seed) * PI * 2.0f;
        const float bend = (Fbm(distance * 6.0f, static_cast<float>(index), settings.seed + 307u, 3) - 0.5f)
            * 0.32f;
        const float angularDistance = AngleDistance(angle, crackAngle + bend);
        const float thickness = 0.012f + Hash01(index, 29, settings.seed) * 0.018f;
        const float line = 1.0f - SmoothStep(thickness, thickness * 2.8f, angularDistance * distance);
        const float start = 0.10f + Hash01(index, 43, settings.seed) * 0.18f;
        const float end = settings.radius * (0.78f + Hash01(index, 59, settings.seed) * 0.52f);
        const float radialMask = SmoothStep(start - 0.04f, start + 0.04f, distance)
            * SmoothStep(end + 0.06f, end - 0.03f, distance);
        cracks = (std::max)(cracks, line * radialMask);
    }
    return cracks;
}

// クレーターの凹み・盛り上がった縁・亀裂を合成した高さを返す。
float DecalHeight(float x, float y, const ProceduralImpactDecalSettings& settings)
{
    const float distance = std::sqrt(x * x + y * y);
    const float irregular = (Fbm(x * 4.0f, y * 4.0f, settings.seed + 401u) - 0.5f) * 0.12f;
    const float crater = SmoothStep(settings.radius, 0.0f, distance + irregular);
    const float rim = std::exp(-std::pow((distance - settings.radius * 0.72f) / 0.075f, 2.0f));
    return -crater * 0.32f + rim * 0.20f - DecalCracks(x, y, settings) * 0.16f;
}

// 回復可能エラーをUIへ返す共通失敗結果を作る。
ProceduralVFXTextureResult Fail(std::string message)
{
    ProceduralVFXTextureResult result;
    result.message = std::move(message);
    return result;
}

} // namespace

// UIとファイル名で共有する安定したプリセット表示名を返す。
const char* ProceduralFlipbookPresetName(ProceduralFlipbookPreset preset)
{
    switch (preset) {
    case ProceduralFlipbookPreset::Smoke: return "Smoke";
    case ProceduralFlipbookPreset::Fire: return "Fire";
    case ProceduralFlipbookPreset::Explosion: return "Explosion";
    case ProceduralFlipbookPreset::Distortion: return "Distortion";
    }
    return "VFX";
}

// 指定プリセットを行別バリエーション付きRGBAアトラスとして生成する。
ProceduralVFXTextureResult GenerateProceduralFlipbook(
    const std::string& outputDirectory, const ProceduralFlipbookSettings& inputSettings)
{
    ProceduralFlipbookSettings settings = inputSettings;
    settings.frameSize = std::clamp(settings.frameSize, 32, 512);
    settings.columns = std::clamp(settings.columns, 2, 32);
    settings.rows = std::clamp(settings.rows, 1, 16);
    settings.noiseScale = std::clamp(settings.noiseScale, 0.25f, 32.0f);
    settings.warpStrength = std::clamp(settings.warpStrength, 0.0f, 3.0f);

    const std::size_t atlasWidth = static_cast<std::size_t>(settings.frameSize) * settings.columns;
    const std::size_t atlasHeight = static_cast<std::size_t>(settings.frameSize) * settings.rows;
    constexpr std::size_t MAX_ATLAS_PIXELS = 16u * 1024u * 1024u;
    if (atlasWidth > 16384u || atlasHeight > 16384u
        || atlasWidth * atlasHeight > MAX_ATLAS_PIXELS) {
        return Fail("アトラスが大きすぎます (最大16384px/辺かつ合計16M pixels)");
    }

    std::filesystem::path directory(outputDirectory);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return Fail("出力ディレクトリを作成できません: " + directory.string());

    const std::string baseName = std::string("Procedural")
        + ProceduralFlipbookPresetName(settings.preset) + "_" + std::to_string(settings.seed);
    const std::filesystem::path base = FindAvailableBase(directory, baseName, { ".png" });
    if (base.empty()) return Fail("空いている出力ファイル名を確保できません");
    const std::filesystem::path destination = base.string() + ".png";

    RgbaImage image;
    image.width = static_cast<int>(atlasWidth);
    image.height = static_cast<int>(atlasHeight);
    image.pixels.resize(static_cast<std::size_t>(image.width) * image.height * 4);

    for (int row = 0; row < settings.rows; ++row) {
        const std::uint32_t rowSeed = settings.seed + static_cast<std::uint32_t>(row) * 7919u;
        for (int frame = 0; frame < settings.columns; ++frame) {
            const float oneShotTime = settings.columns > 1
                ? static_cast<float>(frame) / static_cast<float>(settings.columns - 1) : 0.0f;
            const float loopTime = static_cast<float>(frame) / static_cast<float>(settings.columns);
            for (int pixelY = 0; pixelY < settings.frameSize; ++pixelY) {
                for (int pixelX = 0; pixelX < settings.frameSize; ++pixelX) {
                    const float x = (static_cast<float>(pixelX) + 0.5f)
                        / static_cast<float>(settings.frameSize) * 2.0f - 1.0f;
                    const float y = (static_cast<float>(pixelY) + 0.5f)
                        / static_cast<float>(settings.frameSize) * 2.0f - 1.0f;
                    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
                    switch (settings.preset) {
                    case ProceduralFlipbookPreset::Smoke:
                        GenerateSmokePixel(x, y, oneShotTime, settings, rowSeed, r, g, b, a);
                        break;
                    case ProceduralFlipbookPreset::Fire:
                        GenerateFirePixel(x, y, oneShotTime, settings, rowSeed, r, g, b, a);
                        break;
                    case ProceduralFlipbookPreset::Explosion:
                        GenerateExplosionPixel(x, y, oneShotTime, settings, rowSeed, r, g, b, a);
                        break;
                    case ProceduralFlipbookPreset::Distortion:
                        GenerateDistortionPixel(x, y, loopTime, settings, rowSeed, r, g, b, a);
                        break;
                    }
                    image.Set(frame * settings.frameSize + pixelX,
                              row * settings.frameSize + pixelY, r, g, b, a);
                }
            }
        }
    }

    std::string saveError;
    if (!SavePng(image, destination, saveError)) return Fail(std::move(saveError));
    if (settings.preset == ProceduralFlipbookPreset::Distortion) {
        // RG変位とAlphaマスクを全て保持するためBC7。BC5ではAlphaが失われる。
        if (!SaveTextureMeta(destination, TextureType::Data, TextureCompression::BC7,
                             AlphaMode::Straight, false, saveError)) {
            return Fail(std::move(saveError));
        }
    } else {
        const AlphaMode alphaMode = settings.preset == ProceduralFlipbookPreset::Explosion
            ? AlphaMode::Premultiplied : AlphaMode::Straight;
        if (!SaveTextureMeta(destination, TextureType::Color, TextureCompression::Auto,
                             alphaMode, false, saveError)) {
            return Fail(std::move(saveError));
        }
    }

    ProceduralVFXTextureResult result;
    result.success = true;
    result.albedoPath = destination.string();
    result.message = std::string(ProceduralFlipbookPresetName(settings.preset)) + " Flipbookを生成しました: "
        + std::to_string(settings.columns) + " frames x " + std::to_string(settings.rows) + " variants";
    return result;
}

// Impact Decal用Albedo・Normal・Emissiveセットを同一Seedから生成する。
ProceduralVFXTextureResult GenerateProceduralImpactDecal(
    const std::string& outputDirectory, const ProceduralImpactDecalSettings& inputSettings)
{
    ProceduralImpactDecalSettings settings = inputSettings;
    settings.textureSize = std::clamp(settings.textureSize, 64, 2048);
    settings.radius = std::clamp(settings.radius, 0.15f, 0.95f);
    settings.crackCount = std::clamp(settings.crackCount, 0, 64);
    settings.emissiveStrength = std::clamp(settings.emissiveStrength, 0.0f, 4.0f);

    std::filesystem::path directory(outputDirectory);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return Fail("出力ディレクトリを作成できません: " + directory.string());

    const std::string baseName = "ProceduralImpact_" + std::to_string(settings.seed);
    const std::filesystem::path base = FindAvailableBase(
        directory, baseName, { "_albedo.png", "_normal.png", "_emissive.png" });
    if (base.empty()) return Fail("空いている出力ファイル名を確保できません");

    RgbaImage albedo{ settings.textureSize, settings.textureSize };
    RgbaImage normal{ settings.textureSize, settings.textureSize };
    RgbaImage emissive{ settings.textureSize, settings.textureSize };
    const std::size_t byteCount = static_cast<std::size_t>(settings.textureSize)
        * settings.textureSize * 4;
    albedo.pixels.resize(byteCount);
    normal.pixels.resize(byteCount);
    emissive.pixels.resize(byteCount);

    const float epsilon = 2.0f / static_cast<float>(settings.textureSize);
    for (int pixelY = 0; pixelY < settings.textureSize; ++pixelY) {
        for (int pixelX = 0; pixelX < settings.textureSize; ++pixelX) {
            const float x = (static_cast<float>(pixelX) + 0.5f)
                / static_cast<float>(settings.textureSize) * 2.0f - 1.0f;
            const float y = (static_cast<float>(pixelY) + 0.5f)
                / static_cast<float>(settings.textureSize) * 2.0f - 1.0f;
            const float distance = std::sqrt(x * x + y * y);
            const float noise = Fbm(x * 4.0f, y * 4.0f, settings.seed + 503u);
            const float distortedDistance = distance + (noise - 0.5f) * 0.13f;
            const float craterMask = SmoothStep(settings.radius + 0.08f,
                                                settings.radius - 0.08f, distortedDistance);
            const float cracks = DecalCracks(x, y, settings);
            const float alpha = Saturate((std::max)(craterMask * 0.88f, cracks));

            const float soot = 0.55f + noise * 0.45f;
            albedo.Set(pixelX, pixelY, 0.13f * soot, 0.075f * soot, 0.045f * soot, alpha);

            const float heightLeft = DecalHeight(x - epsilon, y, settings);
            const float heightRight = DecalHeight(x + epsilon, y, settings);
            const float heightDown = DecalHeight(x, y - epsilon, settings);
            const float heightUp = DecalHeight(x, y + epsilon, settings);
            float nx = -(heightRight - heightLeft) / (epsilon * 2.0f);
            float ny = -(heightUp - heightDown) / (epsilon * 2.0f);
            float nz = 1.0f;
            const float inverseLength = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
            nx *= inverseLength;
            ny *= inverseLength;
            nz *= inverseLength;
            normal.Set(pixelX, pixelY, nx * 0.5f + 0.5f, ny * 0.5f + 0.5f,
                       nz * 0.5f + 0.5f, alpha);

            const float glow = Saturate(cracks * settings.emissiveStrength);
            emissive.Set(pixelX, pixelY, glow, glow * 0.20f, glow * 0.025f, alpha);
        }
    }

    const std::filesystem::path albedoPath = base.string() + "_albedo.png";
    const std::filesystem::path normalPath = base.string() + "_normal.png";
    const std::filesystem::path emissivePath = base.string() + "_emissive.png";
    std::string saveError;
    if (!SavePng(albedo, albedoPath, saveError)
        || !SavePng(normal, normalPath, saveError)
        || !SavePng(emissive, emissivePath, saveError)) {
        return Fail(std::move(saveError));
    }
    if (!SaveTextureMeta(albedoPath, TextureType::Color, TextureCompression::Auto,
                         AlphaMode::Straight, true, saveError)
        // Decal.hlslはZを再構築せずRGBを直接読むため、Bを保持できるBC7を使う。
        || !SaveTextureMeta(normalPath, TextureType::Normal, TextureCompression::BC7,
                            AlphaMode::None, true, saveError)
        || !SaveTextureMeta(emissivePath, TextureType::Color, TextureCompression::Auto,
                            AlphaMode::None, true, saveError)) {
        return Fail(std::move(saveError));
    }

    ProceduralVFXTextureResult result;
    result.success = true;
    result.albedoPath = albedoPath.string();
    result.normalPath = normalPath.string();
    result.emissivePath = emissivePath.string();
    result.message = "Impact Decalを生成しました: " + std::to_string(settings.textureSize) + "x"
        + std::to_string(settings.textureSize);
    return result;
}

} // namespace fbzz::asset
