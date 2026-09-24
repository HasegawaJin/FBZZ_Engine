/// @file    FluidBaker.cpp
/// @brief   流体レシピ → フリップブック / Motion Vector / 速度場 PNG
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// @note DirectXTex の WIC PNG エンコーダーに必要。
#pragma comment(lib, "ole32.lib")

#include <Engine/Asset/FluidBaker.hpp>

#include "FlipbookImageIO.hpp"

#include <Engine/Asset/BakeFingerprint.hpp>
#include <Engine/Asset/FluidFireRendering.hpp>
#include <Engine/Asset/FluidRenderMath.hpp>
#include <Engine/Asset/FlipbookMotionVectorEncoding.hpp>
#include <Fluid/FluidSolver.hpp>
#include <Fluid/FluidStepping.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Asset/VectorFieldFile.hpp>
#include <Engine/Scene/Components/ParticleColorSpace.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <DirectXTex.h>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <system_error>
#include <utility>

namespace fbzz::asset {
namespace {

constexpr int kMaxAtlasDimension = 16384;
/// @brief 超解像で内部に描く 1 コマの上限 [px]。これを超えると 1 コマで数百 MB になる。
constexpr int kMaxInternalFrameSize = 1024;

[[nodiscard]] float Saturate(float value) { return std::clamp(value, 0.0f, 1.0f); }

[[nodiscard]] float SmoothStep(float edge0, float edge1, float value)
{
    const float t = Saturate((value - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

[[nodiscard]] float ToLinear(float srgb) { return scene::ParticleSrgbToLinear(Saturate(srgb)); }
[[nodiscard]] float ToSrgb(float linear) { return scene::ParticleLinearToSrgb(Saturate(linear)); }

struct Rgb {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

[[nodiscard]] Rgb LinearColor(const math::Vector4& srgb)
{
    return { ToLinear(srgb.x), ToLinear(srgb.y), ToLinear(srgb.z) };
}

/// @brief 気体の格子は «最長軸 = [-1,1]» で、コマの正方形がちょうどそれに重なる。
[[nodiscard]] float ToGridCoordinate(float position, int count, float cellSize)
{
    return position / cellSize + static_cast<float>(count) * 0.5f - 0.5f;
}

[[nodiscard]] float SampleCells(const std::vector<float>& cells, int nx, int ny, float gx, float gy)
{
    gx = std::clamp(gx, 0.0f, static_cast<float>(nx - 1));
    gy = std::clamp(gy, 0.0f, static_cast<float>(ny - 1));
    const int x0 = static_cast<int>(gx);
    const int y0 = static_cast<int>(gy);
    const int x1 = (std::min)(x0 + 1, nx - 1);
    const int y1 = (std::min)(y0 + 1, ny - 1);
    const float tx = gx - static_cast<float>(x0);
    const float ty = gy - static_cast<float>(y0);
    const auto at = [&](int x, int y) { return cells[static_cast<std::size_t>(y) * nx + x]; };
    const float top    = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
    const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
    return top + (bottom - top) * ty;
}

[[nodiscard]] float CatmullRom(float p0, float p1, float p2, float p3, float t)
{
    return 0.5f * (2.0f * p1 + (p2 - p0) * t
                   + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t
                   + (3.0f * p1 - p0 - 3.0f * p2 + p3) * t * t * t);
}

/// @brief 双三次 (Catmull-Rom)。格子より大きく描くと、バイリニアでは格子の菱形が輪郭に残る。
[[nodiscard]] float SampleCubicCells(const std::vector<float>& cells, int nx, int ny, float gx, float gy)
{
    gx = std::clamp(gx, 0.0f, static_cast<float>(nx - 1));
    gy = std::clamp(gy, 0.0f, static_cast<float>(ny - 1));
    const int ix = static_cast<int>(gx);
    const int iy = static_cast<int>(gy);
    const float tx = gx - static_cast<float>(ix);
    const float ty = gy - static_cast<float>(iy);
    const auto at = [&](int x, int y) {
        x = std::clamp(x, 0, nx - 1);
        y = std::clamp(y, 0, ny - 1);
        return cells[static_cast<std::size_t>(y) * nx + x];
    };
    float column[4];
    for (int j = 0; j < 4; ++j) {
        const int y = iy + j - 1;
        column[j] = CatmullRom(at(ix - 1, y), at(ix, y), at(ix + 1, y), at(ix + 2, y), tx);
    }
    return CatmullRom(column[0], column[1], column[2], column[3], ty);
}

/// @brief 各セルへ届く光の透過率。光源に近い順に処理し、1.5 セル上流の «もう計算した» 値へ
/// @note 途中の吸収を掛けて伝える。
/// @note 1 セルずつ光源まで辿ると格子 N で N^3 になり (256 格子で 1 コマ数秒)、最近傍だと影が
/// @note 階段状になる。上流を双線形で引けば O(N^2) で影も滑らかになる。1.5 セルは上流点の
/// @note 双線形 4 タップが全て自分より光源側 (射影が大きい) になる最小距離。
std::vector<float> ComputeLightTransmittance(const fluid::FluidGasSolver& solver, const fluid::FluidRenderSettings& look)
{
    const int nx = solver.SizeX();
    const int ny = solver.SizeY();
    const std::size_t count = static_cast<std::size_t>(nx) * ny;
    std::vector<float> transmittance(count, 1.0f);
    if (look.selfShadow <= 0.0f) return transmittance;

    float lx = look.lightDirection.x;
    float ly = look.lightDirection.y;
    const float length = std::sqrt(lx * lx + ly * ly);
    if (length < 1.0e-5f) { lx = 0.0f; ly = 1.0f; }
    else                  { lx /= length; ly /= length; }

    std::vector<float> projection(count);
    std::vector<int> order(count);
    std::iota(order.begin(), order.end(), 0);
    for (std::size_t i = 0; i < count; ++i)
        projection[i] = static_cast<float>(i % static_cast<std::size_t>(nx)) * lx
                      + static_cast<float>(i / static_cast<std::size_t>(nx)) * ly;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return projection[static_cast<std::size_t>(a)] > projection[static_cast<std::size_t>(b)];
    });

    constexpr float kStep = 1.5f;
    const std::vector<float>& density = solver.Density();
    const float absorption = look.selfShadow * solver.CellSize() * kStep;
    const float maxX = static_cast<float>(nx - 1);
    const float maxY = static_cast<float>(ny - 1);
    for (const int index : order) {
        const float x = static_cast<float>(index % nx);
        const float y = static_cast<float>(index / nx);
        const float upstreamX = x + lx * kStep;
        const float upstreamY = y + ly * kStep;
        /// @note 格子の外から来る光は減衰していない。
        const bool inside = upstreamX >= 0.0f && upstreamX <= maxX && upstreamY >= 0.0f && upstreamY <= maxY;
        const float upstream = inside ? SampleCells(transmittance, nx, ny, upstreamX, upstreamY) : 1.0f;
        const float midDensity = SampleCells(density, nx, ny, x + lx * kStep * 0.5f, y + ly * kStep * 0.5f);
        transmittance[static_cast<std::size_t>(index)] = upstream * std::exp(-absorption * midDensity);
    }
    return transmittance;
}

/// @brief .fluid の emission_ramp / albedo_ramp (リニア HDR)。規則は EvaluateVolumeRamp と同じ: 端の外は端の色、
/// @brief 前の点より手前にある点は前の点の位置に寄せる (焼き分けで 2D と 3D の色がずれないように)。
[[nodiscard]] math::Vector3 EvaluateFluidRamp(const fluid::FluidColorRamp& ramp, float t)
{
    t = Saturate(t);
    const auto& stops = ramp.stops;
    float previous = stops[0].position;
    if (t <= previous) return stops[0].color;
    for (std::size_t i = 1; i < stops.size(); ++i) {
        const float position = (std::max)(stops[i].position, previous);
        if (t <= position) {
            const float f = Saturate((t - previous) / (std::max)(position - previous, 1.0e-5f));
            const math::Vector3& a = stops[i - 1].color;
            const math::Vector3& b = stops[i].color;
            return { a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f };
        }
        previous = position;
    }
    return stops[stops.size() - 1].color;
}

/// @brief リニアの色の明るさ (Rec.709)。
[[nodiscard]] float Luminance(const Rgb& color)
{
    return 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
}

[[nodiscard]] fluid::FluidShading EffectiveGasShading(fluid::FluidShading shading)
{
    return shading == fluid::FluidShading::Liquid ? fluid::FluidShading::Smoke : shading;
}

/// @brief [1 2 1] の分離可能なぼかし。メタボールの場を微分する前に 1 回掛けると、粒子の継ぎ目で法線が暴れない。
void BlurField(std::vector<float>& field, int size)
{
    std::vector<float> temp(field.size());
    const auto at = [size](const std::vector<float>& f, int x, int y) {
        x = std::clamp(x, 0, size - 1);
        y = std::clamp(y, 0, size - 1);
        return f[static_cast<std::size_t>(y) * size + x];
    };
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            temp[static_cast<std::size_t>(y) * size + x] =
                (at(field, x - 1, y) + 2.0f * at(field, x, y) + at(field, x + 1, y)) * 0.25f;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            field[static_cast<std::size_t>(y) * size + x] =
                (at(temp, x, y - 1) + 2.0f * at(temp, x, y) + at(temp, x, y + 1)) * 0.25f;
}

/// @brief factor 倍で描いたコマを縮める。
/// @note ストレートの色は α で重み付けする。透明画素の色 (液体では 0) を素直に平均すると
/// @note 縁が黒く縁取られるため。事前乗算はそのまま平均してよい。
void Downsample(const FluidFrameImage& source, int factor, bool premultiplied, FluidFrameImage& out)
{
    const int size = source.size / factor;
    out.size = size;
    out.rgba.assign(static_cast<std::size_t>(size) * size * 4, 0.0f);
    out.motion.assign(static_cast<std::size_t>(size) * size * 2, 0.0f);
    const float inverse = 1.0f / static_cast<float>(factor * factor);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float weighted[3] = { 0.0f, 0.0f, 0.0f };
            float plain[3] = { 0.0f, 0.0f, 0.0f };
            float alpha = 0.0f;
            float motionX = 0.0f;
            float motionY = 0.0f;
            for (int sy = 0; sy < factor; ++sy) {
                for (int sx = 0; sx < factor; ++sx) {
                    const std::size_t s = static_cast<std::size_t>(y * factor + sy) * source.size
                                        + static_cast<std::size_t>(x * factor + sx);
                    const float* p = &source.rgba[s * 4];
                    for (int c = 0; c < 3; ++c) {
                        weighted[c] += p[c] * p[3];
                        plain[c] += p[c];
                    }
                    alpha += p[3];
                    motionX += source.motion[s * 2 + 0];
                    motionY += source.motion[s * 2 + 1];
                }
            }
            const std::size_t d = static_cast<std::size_t>(y) * size + x;
            for (int c = 0; c < 3; ++c) {
                out.rgba[d * 4 + c] = premultiplied || alpha <= 1.0e-6f
                    ? plain[c] * inverse : weighted[c] / alpha;
            }
            out.rgba[d * 4 + 3] = alpha * inverse;
            out.motion[d * 2 + 0] = motionX * inverse;
            out.motion[d * 2 + 1] = motionY * inverse;
        }
    }
}

/// @brief WIC は呼び出しスレッドで COM が初期化されている必要がある。エディターは焼きを別スレッドで回すので
/// @brief ここで初期化し、自分が初期化した分だけ戻す (メインスレッドの STA では RPC_E_CHANGED_MODE で素通り)。
class ComScope {
public:
    ComScope() : m_result(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope()
    {
        if (SUCCEEDED(m_result)) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    HRESULT m_result;
};

FluidBakeResult Fail(std::string message)
{
    FluidBakeResult result;
    result.message = std::move(message);
    return result;
}

bool SaveRgbaPng(const std::vector<std::uint8_t>& pixels, int width, int height,
                 const std::filesystem::path& path, std::string& outError)
{
    DirectX::ScratchImage image;
    HRESULT hr = image.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, static_cast<std::size_t>(width),
                                    static_cast<std::size_t>(height), 1, 1);
    if (FAILED(hr)) {
        outError = "PNG の出力バッファを確保できません";
        return false;
    }
    const DirectX::Image* destination = image.GetImage(0, 0, 0);
    const std::size_t sourcePitch = static_cast<std::size_t>(width) * 4;
    for (int y = 0; y < height; ++y)
        std::memcpy(destination->pixels + static_cast<std::size_t>(y) * destination->rowPitch,
                    pixels.data() + static_cast<std::size_t>(y) * sourcePitch, sourcePitch);
    hr = DirectX::SaveToWICFile(*destination, DirectX::WIC_FLAGS_NONE,
                                DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), path.wstring().c_str());
    if (FAILED(hr)) {
        outError = "PNG を書き出せません: " + util::FileSystem::PathToUtf8(path);
        return false;
    }
    return true;
}

/// @brief アトラスの .meta。Mip はコマの境界を混ぜて縁を汚すので切り、ラップも Clamp にする。
bool SaveAtlasMeta(const std::filesystem::path& path, TextureType type, TextureCompression compression,
                   AlphaMode alphaMode, std::string& outError)
{
    TextureAsset texture;
    texture.sourcePath = util::FileSystem::PathToUtf8(path);
    texture.settings = DefaultSettingsForType(type);
    texture.settings.compression = compression;
    texture.settings.alphaMode = alphaMode;
    texture.settings.mipmaps = false;
    texture.settings.maxSize = kMaxAtlasDimension;
    texture.settings.wrapU = TextureWrap::Clamp;
    texture.settings.wrapV = TextureWrap::Clamp;
    texture.settings.filter = TextureFilter::Bilinear;
    /// @note 速度・変位は色ではない。sRGB として読むと 0.5 (= 動かない) がずれる。
    if (type == TextureType::Data) texture.settings.srgb = false;
    TexDescSerializer serializer;
    if (!serializer.Save(texture, texture.sourcePath + ".meta")) {
        outError = ".meta を書き出せません: " + texture.sourcePath + ".meta";
        return false;
    }
    return true;
}

/// @brief 実際に使う超解像の倍率 (内部で描く 1 コマの上限で頭打ち)。焼きとプレビューが同じ値を使う。
int ResolveSupersampling(const fluid::FluidOutputSettings& output, int frameSize)
{
    return std::clamp((std::min)(output.supersampling, kMaxInternalFrameSize / (std::max)(frameSize, 1)), 1, 4);
}

/// @brief 1 コマを «出力の大きさ» で描く。超解像は内部で大きく描いてから縮める。
/// @note プレビューと共有する。焼きだけ超解像を掛けていた頃は、AI が見た絵より焼いた絵の方が
/// @note 滑らかで細部ノイズの効き具合を見誤っていた。
template <class Render>
void CaptureFrame(Render&& render, int size, int supersampling, bool premultiplied,
                  FluidFrameImage& scratch, FluidFrameImage& out)
{
    if (supersampling == 1) {
        render(out, size);
        return;
    }
    render(scratch, size * supersampling);
    Downsample(scratch, supersampling, premultiplied, out);
}

} // namespace

bool FluidShadingIsPremultiplied(fluid::FluidShading shading)
{
    return shading == fluid::FluidShading::Fire;
}

void RenderFluidGasFrame(const fluid::FluidGasSolver& solver, const fluid::FluidRecipe& recipe,
                         int size, float frameDt, FluidFrameImage& out)
{
    size = (std::max)(size, 1);
    out.size = size;
    out.rgba.assign(static_cast<std::size_t>(size) * size * 4, 0.0f);
    out.motion.assign(static_cast<std::size_t>(size) * size * 2, 0.0f);

    const fluid::FluidRenderSettings& look = recipe.render;
    const fluid::FluidShading shading = EffectiveGasShading(look.shading);
    const bool shadowed = shading == fluid::FluidShading::Smoke || shading == fluid::FluidShading::Fire;
    const std::vector<float> transmittance =
        shadowed ? ComputeLightTransmittance(solver, look) : std::vector<float>{};
    const Rgb litColor    = LinearColor(look.smokeColor);
    const Rgb shadowColor = LinearColor(look.shadowColor);
    const float opacity   = (std::max)(look.opacity, 0.0f);
    const float reference = (std::max)(look.fireKelvin, 1.0f);
    const float intensity = (std::max)(look.fireIntensity, 0.0f);
    const FluidFireColorLut blackbody((shading == fluid::FluidShading::Fire ? reference : 1.0f) * 4.0f);
    const bool emissionRamp = look.useEmissionRamp;
    const bool albedoRamp = look.useAlbedoRamp && shading != fluid::FluidShading::Distortion;
    /// @note albedo_ramp の色で影を描くときは、smoke_color に対する shadow_color の明るさの比だけを掛ける。
    /// @note 成分ごとの比にすると smoke_color 用の色味が Ramp のどの色にも乗ってしまう
    /// @note (灰色の煙用の青い影が赤い煙にも乗る)。明るさの比なら Ramp の色味のまま «光と影の差» だけを引き継げる。
    const float shadowRatio = Saturate(Luminance(shadowColor) / (std::max)(Luminance(litColor), 1.0e-4f));
    /// @note Glow の Ramp を何で引くか。温度が生まれるレシピ (温度か燃料を注ぐ) では温度で引き、冷めながら
    /// @note 色が移るようにする。温度が生まれないレシピで温度を引くと、常に左端の 1 色になって Ramp が効かない。
    /// @note そのときは濃さ (1 − e^−密度×opacity、[0,1] に収まる) で引く。
    const bool glowByTemperature =
        std::any_of(recipe.sources.begin(), recipe.sources.end(), [](const fluid::FluidSource& source) {
            return source.enabled && (source.temperature > 0.0f || source.fuel > 0.0f);
        });

    const int nx = solver.SizeX();
    const int ny = solver.SizeY();
    const float h = solver.CellSize();
    const std::vector<float>& densityField = solver.Density();
    const std::vector<float>& temperatureField = solver.Temperature();

    const bool detail = look.detailStrength > 0.0f && solver.HasDetail() && shading != fluid::FluidShading::Distortion;
    float weight0 = 1.0f;
    float weight1 = 0.0f;
    if (detail) solver.DetailWeights(weight0, weight1);
    const float detailScale = (std::max)(look.detailScale, 0.1f);

    for (int py = 0; py < size; ++py) {
        for (int px = 0; px < size; ++px) {
            /// @note 画像の行は下向き、領域の y は上向き。
            const float x = (static_cast<float>(px) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
            const float y = 1.0f - (static_cast<float>(py) + 0.5f) / static_cast<float>(size) * 2.0f;
            const float gx = ToGridCoordinate(x, nx, h);
            const float gy = ToGridCoordinate(y, ny, h);
            float density = (std::max)(SampleCubicCells(densityField, nx, ny, gx, gy), 0.0f);
            float temperature = (std::max)(SampleCubicCells(temperatureField, nx, ny, gx, gy), 0.0f);
            if (detail && density + temperature > 1.0e-4f) {
                float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
                solver.SampleDetailCoordinate(0, x, y, u0, v0);
                solver.SampleDetailCoordinate(1, x, y, u1, v1);
                const float noise0 = FluidDetailNoise({ u0 * detailScale, v0 * detailScale, 0.0f });
                const float noise1 = FluidDetailNoise({ u1 * detailScale + kFluidDetailLayerOffset,
                                                        v1 * detailScale + kFluidDetailLayerOffset,
                                                        kFluidDetailLayerOffset });
                const float factor = FluidDetailFactor(look.detailStrength, weight0, weight1, noise0, noise1);
                density *= factor;
                temperature *= factor;
            }
            const float alpha = 1.0f - std::exp(-density * opacity);
            float vx = 0.0f;
            float vy = 0.0f;
            float vz = 0.0f;
            solver.SampleVelocity(x, y, 0.0f, vx, vy, vz);

            /// @note 地の色 (Glow では発光色) と影の色。albedo_ramp なら、ここに流れてきた煙の色の鍵で引く。
            Rgb lit = litColor;
            Rgb shade = shadowColor;
            if (albedoRamp && density > 0.0f) {
                const math::Vector3 keyed = EvaluateFluidRamp(look.albedoRamp, solver.SampleColorKey(x, y, 0.0f));
                lit = { keyed.x, keyed.y, keyed.z };
                shade = { keyed.x * shadowRatio, keyed.y * shadowRatio, keyed.z * shadowRatio };
            }

            float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
            switch (shading) {
            case fluid::FluidShading::Glow:
                /// @note 加算は rgb × a で足される。色は一定にして、明るさを a で運ぶ。
                if (emissionRamp) {
                    /// @note Ramp は HDR。1 で切ると芯の色が白へ潰れるので、最大の成分で割って色味だけを残す
                    /// @note (明るさは a と .mat の emissiveScale が運ぶ)。
                    const math::Vector3 emission =
                        EvaluateFluidRamp(look.emissionRamp, glowByTemperature ? temperature : alpha);
                    const float peak = (std::max)({ emission.x, emission.y, emission.z, 1.0f });
                    r = ToSrgb(emission.x / peak);
                    g = ToSrgb(emission.y / peak);
                    b = ToSrgb(emission.z / peak);
                } else {
                    r = ToSrgb(lit.r);
                    g = ToSrgb(lit.g);
                    b = ToSrgb(lit.b);
                }
                a = Saturate(alpha * (0.6f + 0.4f * Saturate(temperature)));
                break;
            case fluid::FluidShading::Distortion: {
                /// @note 既存の歪み素材 (ProceduralVFXTextures) と同じ符号化: RG = 0.5 ± 変位、A = 効く範囲。
                const math::Vector4 encoded = EncodeFluidDistortion({ vx, vy }, alpha, kFluidDistortionScale);
                r = encoded.x;
                g = encoded.y;
                b = encoded.z;
                a = encoded.w;
                break;
            }
            case fluid::FluidShading::Smoke:
            case fluid::FluidShading::Fire:
            default: {
                const float light = transmittance.empty() ? 1.0f : SampleCells(transmittance, nx, ny, gx, gy);
                const Rgb smoke = { shade.r + (lit.r - shade.r) * light,
                                    shade.g + (lit.g - shade.g) * light,
                                    shade.b + (lit.b - shade.b) * light };
                if (shading == fluid::FluidShading::Smoke) {
                    r = ToSrgb(smoke.r);
                    g = ToSrgb(smoke.g);
                    b = ToSrgb(smoke.b);
                    a = alpha;
                    break;
                }
                /// @note 炎: 煤は背景を隠し、光は足すだけ (事前乗算)。8bit へ収めるため発光は 1 − e^−x で丸め、
                /// @note 明るさは .mat の emissiveScale で戻す。輝度は T^4 (Stefan-Boltzmann)。
                math::Vector3 emission;
                if (emissionRamp) {
                    /// @note Ramp が温度 → 色と明るさの両方を決める。T^4 は掛けない (明るさの伸びも Ramp の点で描く)。
                    const math::Vector3 color = EvaluateFluidRamp(look.emissionRamp, temperature);
                    emission = FluidFireRampRadiance(color, intensity);
                } else {
                    const math::Vector3 chroma = blackbody.Chroma(temperature * reference);
                    emission = FluidFireBlackbodyRadiance(temperature, intensity, chroma);
                }
                const math::Vector3 fireColor = FluidFireSoftKnee(emission);
                r = ToSrgb(smoke.r * alpha + fireColor.x);
                g = ToSrgb(smoke.g * alpha + fireColor.y);
                b = ToSrgb(smoke.b * alpha + fireColor.z);
                a = alpha;
                break;
            }
            }

            const std::size_t pixel = static_cast<std::size_t>(py) * size + px;
            out.rgba[pixel * 4 + 0] = r;
            out.rgba[pixel * 4 + 1] = g;
            out.rgba[pixel * 4 + 2] = b;
            out.rgba[pixel * 4 + 3] = a;
            /// @note 領域は幅 2、コマは幅 1。y は画像の下向きへ反転する。
            out.motion[pixel * 2 + 0] =  vx * frameDt * 0.5f;
            out.motion[pixel * 2 + 1] = -vy * frameDt * 0.5f;
        }
    }
}

void RenderFluidLiquidFrame(const fluid::FluidLiquidSolver& solver, const fluid::FluidRecipe& recipe,
                            int size, float frameDt, FluidFrameImage& out)
{
    size = (std::max)(size, 1);
    out.size = size;
    const std::size_t pixelCount = static_cast<std::size_t>(size) * size;
    out.rgba.assign(pixelCount * 4, 0.0f);
    out.motion.assign(pixelCount * 2, 0.0f);

    const fluid::FluidRenderSettings& look = recipe.render;
    const float lifetime = recipe.liquid.particleLifetime;
    const float pixelsPerUnit = static_cast<float>(size) * 0.5f;
    const float radiusPixels = (std::max)(solver.ParticleRadius() * (std::max)(look.liquidRadiusScale, 0.5f)
                                          * pixelsPerUnit, 1.0f);

    /// @note メタボール: 粒子ごとに滑らかな山を足し、その等値線を液面とみなす。
    std::vector<float> field(pixelCount, 0.0f);
    std::vector<float> velocityX(pixelCount, 0.0f);
    std::vector<float> velocityY(pixelCount, 0.0f);
    const bool albedoRamp = look.useAlbedoRamp;
    /// @note 色の鍵 × 山の重み。場と同じぼかしを掛けてから場で割ると、液面のどの画素でも近くの粒子の鍵の重み付き平均になる。
    std::vector<float> keyField(albedoRamp ? pixelCount : 0, 0.0f);
    for (const fluid::FluidLiquidSolver::Particle& particle : solver.Particles()) {
        /// @note 寿命がある飛沫は細りながら消える。いきなり消すとコマ間でポツポツ抜けて見える。
        const float life = lifetime > 0.0f ? Saturate(1.0f - particle.age / lifetime) : 1.0f;
        const float radius = radiusPixels * std::sqrt(life);
        if (radius < 0.35f) continue;
        const float cx = (particle.x + 1.0f) * pixelsPerUnit - 0.5f;
        const float cy = (1.0f - particle.y) * pixelsPerUnit - 0.5f;
        const int x0 = (std::max)(0, static_cast<int>(std::floor(cx - radius)));
        const int x1 = (std::min)(size - 1, static_cast<int>(std::ceil(cx + radius)));
        const int y0 = (std::max)(0, static_cast<int>(std::floor(cy - radius)));
        const int y1 = (std::min)(size - 1, static_cast<int>(std::ceil(cy + radius)));
        const float inverseRadius = 1.0f / radius;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float dx = (static_cast<float>(x) - cx) * inverseRadius;
                const float dy = (static_cast<float>(y) - cy) * inverseRadius;
                const float q2 = dx * dx + dy * dy;
                if (q2 >= 1.0f) continue;
                const float falloff = (1.0f - q2) * (1.0f - q2) * (1.0f - q2);
                const std::size_t pixel = static_cast<std::size_t>(y) * size + x;
                field[pixel]     += falloff;
                velocityX[pixel] += falloff * particle.vx;
                velocityY[pixel] += falloff * particle.vy;
                if (albedoRamp) keyField[pixel] += falloff * particle.colorKey;
            }
        }
    }
    std::vector<float> smoothField = field;
    BlurField(smoothField, size);
    if (albedoRamp) BlurField(keyField, size);

    const Rgb baseColor = LinearColor(look.liquidColor);
    float lx = look.lightDirection.x;
    float ly = look.lightDirection.y;
    float lz = 0.7f;
    const float lightLength = std::sqrt(lx * lx + ly * ly + lz * lz);
    lx /= lightLength; ly /= lightLength; lz /= lightLength;
    /// @note ハーフベクトル (視線は画面の手前 (0,0,1))。
    float hx = lx, hy = ly, hz = lz + 1.0f;
    const float halfLength = std::sqrt(hx * hx + hy * hy + hz * hz);
    hx /= halfLength; hy /= halfLength; hz /= halfLength;
    const float threshold = (std::max)(look.liquidThreshold, 0.01f);
    const auto fieldAt = [&](int x, int y) {
        x = std::clamp(x, 0, size - 1);
        y = std::clamp(y, 0, size - 1);
        return smoothField[static_cast<std::size_t>(y) * size + x];
    };

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) * size + x;
            const float value = smoothField[pixel];
            if (value < 1.0e-4f) continue;
            const float coverage = SmoothStep(threshold * 0.75f, threshold * 1.25f, value);
            if (coverage <= 0.0f) continue;
            /// @note 場の勾配から疑似法線を作る (y は領域の上向きへ直す)。縁ほど外へ傾いて丸く見える。
            const float gradientX = (fieldAt(x + 1, y) - fieldAt(x - 1, y)) * 0.5f;
            const float gradientY = (fieldAt(x, y - 1) - fieldAt(x, y + 1)) * 0.5f;
            float nx = -gradientX * radiusPixels;
            float ny = -gradientY * radiusPixels;
            float nz = 1.0f;
            const float normalLength = std::sqrt(nx * nx + ny * ny + nz * nz);
            nx /= normalLength; ny /= normalLength; nz /= normalLength;

            const float diffuse  = (std::max)(nx * lx + ny * ly + nz * lz, 0.0f);
            const float specular = std::pow((std::max)(nx * hx + ny * hy + nz * hz, 0.0f), 48.0f)
                * (std::max)(look.specular, 0.0f);
            const float rim = (1.0f - nz) * (1.0f - nz);
            const float lighting = 0.35f + 0.65f * diffuse;
            Rgb base = baseColor;
            if (albedoRamp) {
                const math::Vector3 keyed = EvaluateFluidRamp(look.albedoRamp, keyField[pixel] / value);
                base = { keyed.x, keyed.y, keyed.z };
            }
            out.rgba[pixel * 4 + 0] = ToSrgb(base.r * lighting + specular + rim * 0.25f * base.r);
            out.rgba[pixel * 4 + 1] = ToSrgb(base.g * lighting + specular + rim * 0.25f * base.g);
            out.rgba[pixel * 4 + 2] = ToSrgb(base.b * lighting + specular + rim * 0.25f * base.b);
            /// @note ハイライトは液の色の透け具合に関係なく見える。
            out.rgba[pixel * 4 + 3] = (std::max)(coverage * Saturate(look.liquidColor.w),
                                                 Saturate(specular) * coverage);

            if (field[pixel] > 1.0e-4f) {
                const float vx = velocityX[pixel] / field[pixel];
                const float vy = velocityY[pixel] / field[pixel];
                out.motion[pixel * 2 + 0] =  vx * frameDt * 0.5f;
                out.motion[pixel * 2 + 1] = -vy * frameDt * 0.5f;
            }
        }
    }
}

bool RenderFluidBakeFrames(const fluid::FluidRecipe& source, std::span<const int> frames, int size,
                           std::vector<FluidFrameImage>& out, std::atomic<float>* progress,
                           const std::atomic<bool>* cancel, FluidBakeTiming* timing)
{
    out.clear();
    if (frames.empty()) return false;
    const auto cancelled = [cancel]() { return cancel != nullptr && cancel->load(std::memory_order_relaxed); };
    const auto report = [progress](float value) {
        if (progress != nullptr) progress->store(value, std::memory_order_relaxed);
    };
    fluid::FluidRecipe recipe = source;
    fluid::FluidOutputSettings& output = recipe.output;
    fluid::NormalizeFluidOutput(output);
    const fluid::FluidStepPlan plan = fluid::MakeFluidStepPlan(recipe);
    const int frameSize = size > 0 ? std::clamp(size, 16, kMaxInternalFrameSize) : output.frameSize;
    const bool gas = recipe.kind == fluid::FluidKind::Gas;
    const fluid::FluidShading shading = gas ? EffectiveGasShading(recipe.render.shading) : fluid::FluidShading::Liquid;
    recipe.render.shading = shading;
    const bool premultiplied = FluidShadingIsPremultiplied(shading);
    const int supersampling = ResolveSupersampling(output, frameSize);
    const int total = plan.frameCount + plan.loopOverlap;

    /// @note 要求されたコマと、ループの «先頭へ混ぜる末尾の続き» を撮る番号として集める。
    std::vector<int> wanted;
    wanted.reserve(frames.size() * 2);
    std::vector<int> requested;
    requested.reserve(frames.size());
    for (const int frame : frames) {
        const int clamped = std::clamp(frame, 0, total - 1);
        requested.push_back(clamped);
        wanted.push_back(clamped);
        if (plan.loopOverlap > 0 && clamped < plan.loopOverlap) wanted.push_back(plan.frameCount + clamped);
    }
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    const int lastFrame = wanted.back();
    const int totalSteps = (std::max)(plan.warmupFrames + lastFrame + 1, 1);

    fluid::FluidGasSolver gasSolver;
    fluid::FluidLiquidSolver liquidSolver;
    if (gas) {
        const int resolution = fluid::ResolveGasResolution(recipe);
        gasSolver.Reset(recipe, resolution, resolution, 1);
    } else {
        liquidSolver.Reset(recipe);
    }
    const auto advance = [&]() {
        if (timing != nullptr) timing->stage.store(0, std::memory_order_relaxed);
        const auto started = std::chrono::steady_clock::now();
        if (gas) fluid::AdvanceFluidFrame(gasSolver, plan);
        else     fluid::AdvanceFluidFrame(liquidSolver, plan);
        if (timing != nullptr) timing->simulationSeconds.fetch_add(
            std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count(),
            std::memory_order_relaxed);
    };
    FluidFrameImage scratch;
    const auto capture = [&](FluidFrameImage& image) {
        if (timing != nullptr) timing->stage.store(1, std::memory_order_relaxed);
        const auto started = std::chrono::steady_clock::now();
        CaptureFrame([&](FluidFrameImage& destination, int renderPixels) {
            if (gas) RenderFluidGasFrame(gasSolver, recipe, renderPixels, plan.frameDt, destination);
            else     RenderFluidLiquidFrame(liquidSolver, recipe, renderPixels, plan.frameDt, destination);
        }, frameSize, supersampling, premultiplied, scratch, image);
        if (timing != nullptr) timing->renderSeconds.fetch_add(
            std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count(),
            std::memory_order_relaxed);
    };
    const auto step = [&](int index) {
        report(0.95f * static_cast<float>(index + 1) / static_cast<float>(totalSteps));
    };

    for (int i = 0; i < plan.warmupFrames; ++i) {
        if (cancelled()) return false;
        advance();
        step(i);
    }
    std::vector<FluidFrameImage> captured(wanted.size());
    std::size_t next = 0;
    for (int i = 0; i <= lastFrame; ++i) {
        if (cancelled()) return false;
        if (next < wanted.size() && wanted[next] == i) capture(captured[next++]);
        if (i < lastFrame) advance();
        step(plan.warmupFrames + i);
    }
    const auto indexOf = [&wanted](int frame) {
        return static_cast<std::size_t>(std::lower_bound(wanted.begin(), wanted.end(), frame) - wanted.begin());
    };
    /// @note 先頭 overlap コマは «最終コマの続き» から元のコマへ滑らかに移す (焼きと同じ規則)。
    for (const int frame : requested) {
        if (plan.loopOverlap <= 0 || frame >= plan.loopOverlap) continue;
        FluidFrameImage& head = captured[indexOf(frame)];
        const FluidFrameImage& tail = captured[indexOf(plan.frameCount + frame)];
        if (head.size != tail.size) continue;
        const float keep = static_cast<float>(frame + 1) / static_cast<float>(plan.loopOverlap + 1);
        for (std::size_t k = 0; k < head.rgba.size() && k < tail.rgba.size(); ++k)
            head.rgba[k] = tail.rgba[k] + (head.rgba[k] - tail.rgba[k]) * keep;
        for (std::size_t k = 0; k < head.motion.size() && k < tail.motion.size(); ++k)
            head.motion[k] = tail.motion[k] + (head.motion[k] - tail.motion[k]) * keep;
    }
    out.reserve(requested.size());
    for (const int frame : requested) out.push_back(std::move(captured[indexOf(frame)]));
    report(timing != nullptr ? 0.95f : 1.0f);
    return !out.empty() && out.front().size > 0;
}

bool RenderFluidBakeFrame(const fluid::FluidRecipe& recipe, int frame, int size, FluidFrameImage& out,
                          std::atomic<float>* progress, const std::atomic<bool>* cancel)
{
    const int frames[] = { frame };
    std::vector<FluidFrameImage> images;
    if (!RenderFluidBakeFrames(recipe, frames, size, images, progress, cancel)) return false;
    out = std::move(images.front());
    return true;
}

FluidBakeResult BakeFluid(const fluid::FluidRecipe& source, const std::string& basePath,
                          std::atomic<float>* progress, FluidBakeTiming* timing)
{
    const auto report = [progress](float value) {
        if (progress != nullptr) progress->store(value, std::memory_order_relaxed);
    };
    report(0.0f);
    if (basePath.empty()) return Fail("出力先がありません");
    fluid::FluidRecipe recipe = source;
    fluid::FluidOutputSettings& output = recipe.output;
    fluid::NormalizeFluidOutput(output);
    const fluid::FluidStepPlan plan = fluid::MakeFluidStepPlan(recipe);

    const int size   = output.frameSize;
    const int frames = plan.frameCount;
    if (size * output.columns > kMaxAtlasDimension || size * output.rows > kMaxAtlasDimension)
        return Fail("アトラスが最大寸法 16384px を超えます (Frame Size × Columns / Rows を下げてください)");
    const bool gas = recipe.kind == fluid::FluidKind::Gas;
    /// @note 無効な発生源しか無いと、何も湧かないまま空のコマを焼き切ってしまう。
    if (std::none_of(recipe.sources.begin(), recipe.sources.end(),
                     [](const fluid::FluidSource& source) { return source.enabled; }))
        return Fail("有効な発生源 (Source) がありません");

    const fluid::FluidShading shading = gas ? EffectiveGasShading(recipe.render.shading) : fluid::FluidShading::Liquid;
    recipe.render.shading = shading;
    const bool premultiplied = FluidShadingIsPremultiplied(shading);
    const int supersampling = ResolveSupersampling(output, size);
    const int gridResolution = gas ? fluid::ResolveGasResolution(recipe) : 0;

    ComScope com;
    const auto started = std::chrono::steady_clock::now();
    /// @note コマを解くのはプレビューと同じ入口 (ループの重ねもここが面倒を見る)。
    std::vector<int> wanted(static_cast<std::size_t>(frames));
    std::iota(wanted.begin(), wanted.end(), 0);
    std::vector<FluidFrameImage> images;
    if (!RenderFluidBakeFrames(recipe, wanted, size, images, progress, nullptr, timing)
        || images.size() != wanted.size())
        return Fail("コマを解けませんでした");
    if (timing != nullptr) timing->stage.store(2, std::memory_order_relaxed);
    const auto outputStarted = std::chrono::steady_clock::now();

    /// @note Motion Vector の保存値は FlipbookMotionVectorEncoding.hpp の規約 (m = −d / S) に従う。
    /// @note 規約を自前で持つと生成器ごとに符号や単位がずれても誰も気付けない (実際に一度ずれていた)。
    /// @note S は «見えている画素» の最大変位 [アトラス UV]。空っぽの場所の速い流れで上限を決めると、
    /// @note 肝心の煙の動きが 8bit の数段にしか割り当たらず warp がガタつくため見えている画素だけで計る。
    bool writeMotion = output.motionVectors && shading != fluid::FluidShading::Distortion;
    const int atlasWidth  = size * output.columns;
    const int atlasHeight = size * output.rows;
    const std::size_t atlasPixels = static_cast<std::size_t>(atlasWidth) * atlasHeight;
    const auto tileSize = static_cast<std::uint32_t>(size);
    FlipbookGrid grid;
    grid.columns = output.columns;
    grid.rows = output.rows;
    grid.frameCount = frames;
    std::vector<math::Vector2> atlasMotion;
    float motionStrength = 0.0f;
    if (writeMotion) {
        atlasMotion.assign(atlasPixels, math::Vector2::ZERO);
        std::vector<float> coverage(atlasPixels, 0.0f);
        std::vector<math::Vector2> visible;
        for (int frame = 0; frame < frames; ++frame) {
            const FluidFrameImage& image = images[static_cast<std::size_t>(frame)];
            const FlipbookTileOrigin origin = TileOriginPx(frame, grid, tileSize, tileSize);
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    const std::size_t sourcePixel = static_cast<std::size_t>(y) * size + x;
                    const float* rgba = &image.rgba[sourcePixel * 4];
                    /// @note 炎の芯は α が 0 でも光って見える (事前乗算の加算成分)。
                    const float visibility = shading == fluid::FluidShading::Fire
                        ? (std::max)(rgba[3], (std::max)(rgba[0], (std::max)(rgba[1], rgba[2])))
                        : rgba[3];
                    if (visibility < 0.02f) continue;
                    const math::Vector2 displacement = TileUvToAtlasUv(
                        { image.motion[sourcePixel * 2 + 0], image.motion[sourcePixel * 2 + 1] }, grid);
                    const std::size_t destination =
                        static_cast<std::size_t>(origin.y + static_cast<std::uint32_t>(y)) * atlasWidth
                        + origin.x + static_cast<std::uint32_t>(x);
                    atlasMotion[destination] = displacement;
                    coverage[destination] = visibility;
                    visible.push_back(displacement);
                }
            }
        }
        /// @note 縁は Bilinear で «速度 0 の空白» と混ざって動きが鈍る。覆われた近傍の速度を外へ広げる。
        DilateMotion(atlasMotion, coverage, static_cast<std::uint32_t>(atlasWidth),
                     static_cast<std::uint32_t>(atlasHeight), grid, tileSize, tileSize, /*iterations=*/4);
        motionStrength = visible.empty() ? 0.0f : ComputeRecommendedStrength(visible, 0.0f);
        if (motionStrength < 1.0e-5f) writeMotion = false;
    }

    std::vector<std::uint8_t> albedo(atlasPixels * 4, 0);
    std::vector<std::uint8_t> motion(writeMotion ? atlasPixels * 4 : 0, 0);
    const auto toByte = [](float value) {
        return static_cast<std::uint8_t>(Saturate(value) * 255.0f + 0.5f);
    };
    for (int frame = 0; frame < frames; ++frame) {
        const FluidFrameImage& image = images[static_cast<std::size_t>(frame)];
        const FlipbookTileOrigin origin = TileOriginPx(frame, grid, tileSize, tileSize);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const std::size_t sourcePixel = static_cast<std::size_t>(y) * size + x;
                const std::size_t pixel =
                    static_cast<std::size_t>(origin.y + static_cast<std::uint32_t>(y)) * atlasWidth
                    + origin.x + static_cast<std::uint32_t>(x);
                for (int channel = 0; channel < 4; ++channel)
                    albedo[pixel * 4 + channel] = toByte(image.rgba[sourcePixel * 4 + channel]);
                if (writeMotion) {
                    const EncodedMotionVector encoded = EncodeMotionVector(atlasMotion[pixel], motionStrength);
                    motion[pixel * 4 + 0] = encoded.r;
                    motion[pixel * 4 + 1] = encoded.g;
                    motion[pixel * 4 + 2] = 0;
                    motion[pixel * 4 + 3] = 255;
                }
            }
        }
    }

    const std::filesystem::path albedoPath = util::FileSystem::PathFromUtf8(basePath + "_Flipbook.png");
    const std::filesystem::path motionPath = util::FileSystem::PathFromUtf8(basePath + "_MV.png");
    std::error_code directoryError;
    if (const std::filesystem::path parent = albedoPath.parent_path(); !parent.empty())
        std::filesystem::create_directories(parent, directoryError);
    if (directoryError) return Fail("出力フォルダーを作れません: " + directoryError.message());

    std::string error;
    if (!SaveRgbaPng(albedo, atlasWidth, atlasHeight, albedoPath, error)) return Fail(error);
    const TextureType albedoType = shading == fluid::FluidShading::Distortion ? TextureType::Data : TextureType::Color;
    const AlphaMode alphaMode = premultiplied ? AlphaMode::Premultiplied : AlphaMode::Straight;
    if (!SaveAtlasMeta(albedoPath, albedoType, TextureCompression::Auto, alphaMode, error)) return Fail(error);
    if (writeMotion) {
        if (!SaveRgbaPng(motion, atlasWidth, atlasHeight, motionPath, error)) return Fail(error);
        /// @note RG の 2 チャンネルを保てる BC5 で扱う (MV 生成ツールと同じ扱い)。
        if (!SaveAtlasMeta(motionPath, TextureType::Data, TextureCompression::BC5, AlphaMode::None, error))
            return Fail(error);
    }
    /// @note 実行時に使うのはコマを跨がないミップ付きの DDS。PNG は 1 段しか読まれず、遠くの粒子がちらつく。
    const std::filesystem::path albedoDds = util::FileSystem::PathFromUtf8(basePath + "_Flipbook.dds");
    const std::filesystem::path motionDds = util::FileSystem::PathFromUtf8(basePath + "_MV.dds");
    const FlipbookMipContent albedoContent = albedoType == TextureType::Data ? FlipbookMipContent::Plain
        : premultiplied                                                    ? FlipbookMipContent::PremultipliedSrgb
                                                                           : FlipbookMipContent::StraightSrgb;
    const auto tile = static_cast<std::uint32_t>(size);
    if (!detail::SaveFlipbookDds(albedoDds, static_cast<std::uint32_t>(atlasWidth), static_cast<std::uint32_t>(atlasHeight),
                                 albedo, tile, tile, albedoContent, detail::FlipbookDdsCompression::BC7, error)
        || !detail::SaveFlipbookMeta(albedoDds, albedoType, TextureCompression::BC7, alphaMode, error))
        return Fail(error);
    if (writeMotion
        && (!detail::SaveFlipbookDds(motionDds, static_cast<std::uint32_t>(atlasWidth),
                                     static_cast<std::uint32_t>(atlasHeight), motion, tile, tile,
                                     FlipbookMipContent::Plain, detail::FlipbookDdsCompression::BC5, error)
            || !detail::SaveFlipbookMeta(motionDds, TextureType::Data, TextureCompression::BC5, AlphaMode::None, error)))
        return Fail(error);

    FluidBakeResult result;
    if (gas && output.vectorField) {
        /// @note 粒子を乗せる場は «時間平均した流れ»。1 瞬の場を焼くと、その瞬間の渦だけが永久に回り続ける。
        fluid::FluidGasSolver volume;
        const int resolution = std::clamp(output.vectorFieldResolution, 8, 64);
        volume.Reset(recipe, resolution, resolution, resolution);
        fluid::AdvanceFluidWarmup(volume, plan);
        const std::size_t cells = static_cast<std::size_t>(resolution) * resolution * resolution;
        std::vector<float> sumX(cells, 0.0f), sumY(cells, 0.0f), sumZ(cells, 0.0f);
        for (int frame = 0; frame < frames; ++frame) {
            fluid::AdvanceFluidFrame(volume, plan);
            for (std::size_t i = 0; i < cells; ++i) {
                sumX[i] += volume.VelocityX()[i];
                sumY[i] += volume.VelocityY()[i];
                sumZ[i] += volume.VelocityZ()[i];
            }
            report(0.95f + 0.04f * static_cast<float>(frame + 1) / static_cast<float>(frames));
        }
        fluid::VectorFieldAsset field;
        field.sizeX = field.sizeY = field.sizeZ = static_cast<uint32_t>(resolution);
        const math::Vector3 extents = { std::fabs(output.vectorFieldExtents.x), std::fabs(output.vectorFieldExtents.y),
                                        std::fabs(output.vectorFieldExtents.z) };
        field.boundsMin = { -extents.x, -extents.y, -extents.z };
        field.boundsMax = extents;
        field.data.resize(cells);
        float maxMagnitude = 0.0f;
        const float inverseFrames = 1.0f / static_cast<float>(frames);
        for (std::size_t i = 0; i < cells; ++i) {
            /// @note 領域の半幅 1 が extents [m] に当たるので、速度も軸ごとに extents 倍して m/s にする。
            const math::Vector3 value = { sumX[i] * inverseFrames * extents.x,
                                          sumY[i] * inverseFrames * extents.y,
                                          sumZ[i] * inverseFrames * extents.z };
            field.data[i] = value;
            maxMagnitude = (std::max)(maxMagnitude, value.Length());
        }
        field.maxMagnitude = (std::max)(maxMagnitude, 1.0e-4f);
        const std::string fieldPath = basePath + "_Velocity.png";
        if (!SaveVectorField(fieldPath, field)) return Fail("速度場 PNG を書き出せません: " + fieldPath);
        result.vectorFieldPath = fieldPath;
    }

    /// @note 指紋は «人が見る絵» から取る (速度場 PNG は絵に出ないので混ぜない)。
    BakeFingerprint fingerprint;
    fingerprint.Add(albedo);
    if (writeMotion) fingerprint.Add(motion);
    result.fingerprint = fingerprint.Finish();
    result.success = true;
    result.albedoPath = util::FileSystem::PathToUtf8(albedoDds);
    if (writeMotion) result.motionVectorPath = util::FileSystem::PathToUtf8(motionDds);
    result.columns = output.columns;
    result.rows = output.rows;
    result.frameSize = size;
    result.gridResolution = gridResolution;
    result.supersampling = supersampling;
    result.motionVectorStrength = writeMotion ? motionStrength : 0.0f;
    result.flipbookMode = output.loop ? scene::ParticleFlipbookMode::FramesPerSecond
                                      : scene::ParticleFlipbookMode::Lifetime;
    result.framesPerSecond = static_cast<float>(frames) / output.duration;
    result.distortion = shading == fluid::FluidShading::Distortion;
    switch (shading) {
    case fluid::FluidShading::Fire:
        result.blendMode = renderer::BlendMode::PREMULTIPLIED;
        result.emissiveScale = 2.5f;
        break;
    case fluid::FluidShading::Glow:
        result.blendMode = renderer::BlendMode::ADDITIVE;
        result.emissiveScale = 1.5f;
        break;
    default:
        result.blendMode = renderer::BlendMode::ALPHA_BLEND;
        result.emissiveScale = 1.0f;
        break;
    }

    const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
    char summary[320];
    if (gas)
        std::snprintf(summary, sizeof(summary),
                      "焼きました: %d コマ (%dx%d) / %dx%d px / 格子 %d / 超解像 x%d / %.2f 秒ぶん / 所要 %.1f 秒",
                      frames, output.columns, output.rows, atlasWidth, atlasHeight, gridResolution,
                      supersampling, output.duration, seconds);
    else
        std::snprintf(summary, sizeof(summary),
                      "焼きました: %d コマ (%dx%d) / %dx%d px / 超解像 x%d / %.2f 秒ぶん / 所要 %.1f 秒",
                      frames, output.columns, output.rows, atlasWidth, atlasHeight, supersampling,
                      output.duration, seconds);
    result.message = summary;
    if (writeMotion) {
        std::snprintf(summary, sizeof(summary), "\nMotion Vector: 強さ %.4f (.mat の Motion Strength にこの値を使います)",
                      motionStrength);
        result.message += summary;
    }
    if (!result.vectorFieldPath.empty()) result.message += "\nVelocity PNG: " + result.vectorFieldPath;
    if (timing != nullptr) timing->outputSeconds.store(
        std::chrono::duration<float>(std::chrono::steady_clock::now() - outputStarted).count(),
        std::memory_order_relaxed);
    report(1.0f);
    return result;
}

} // namespace fbzz::asset
