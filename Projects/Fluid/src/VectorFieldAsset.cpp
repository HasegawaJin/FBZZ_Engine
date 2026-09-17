/// @file    VectorFieldAsset.cpp
/// @brief   速度場の標本化と手続きベイク。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Fluid/VectorFieldAsset.hpp>

#include <Math/CurlNoise.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::fluid {
namespace {

float LerpF(float a, float b, float t) { return a + (b - a) * t; }

math::Vector3 LerpV(const math::Vector3& a, const math::Vector3& b, float t)
{
    return { LerpF(a.x, b.x, t), LerpF(a.y, b.y, t), LerpF(a.z, b.z, t) };
}

} // namespace

const char* VectorFieldRecipeName(VectorFieldRecipe recipe)
{
    switch (recipe) {
    case VectorFieldRecipe::Curl:       return "Curl";
    case VectorFieldRecipe::Vortex:     return "Vortex";
    case VectorFieldRecipe::Tornado:    return "Tornado";
    case VectorFieldRecipe::Sphere:     return "Sphere";
    case VectorFieldRecipe::Turbulence: return "Turbulence";
    }
    return "Curl";
}

math::Vector3 VectorFieldAsset::SampleLocal(const math::Vector3& localPosition) const
{
    if (Empty()) return math::Vector3::ZERO;

    const math::Vector3 span = {
        boundsMax.x - boundsMin.x, boundsMax.y - boundsMin.y, boundsMax.z - boundsMin.z };
    if (span.x <= 1.0e-6f || span.y <= 1.0e-6f || span.z <= 1.0e-6f) return math::Vector3::ZERO;

    /// @note [0,1] の正規化座標。AABB の外は場が無いものとして扱う。
    const float u = (localPosition.x - boundsMin.x) / span.x;
    const float v = (localPosition.y - boundsMin.y) / span.y;
    const float w = (localPosition.z - boundsMin.z) / span.z;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f || w < 0.0f || w > 1.0f)
        return math::Vector3::ZERO;

    /// @note テクセル中心へ寄せる。GPU の SampleLevel と同じ位置を引くための半テクセルずらし。
    const float fx = u * static_cast<float>(sizeX) - 0.5f;
    const float fy = v * static_cast<float>(sizeY) - 0.5f;
    const float fz = w * static_cast<float>(sizeZ) - 0.5f;

    const auto clampIndex = [](float f, uint32_t size) -> uint32_t {
        const int i = static_cast<int>(std::floor(f));
        return static_cast<uint32_t>(std::clamp(i, 0, static_cast<int>(size) - 1));
    };
    const auto nextIndex = [](uint32_t i, uint32_t size) -> uint32_t {
        return (std::min)(i + 1u, size - 1u);
    };

    const uint32_t x0 = clampIndex(fx, sizeX);
    const uint32_t y0 = clampIndex(fy, sizeY);
    const uint32_t z0 = clampIndex(fz, sizeZ);
    const uint32_t x1 = nextIndex(x0, sizeX);
    const uint32_t y1 = nextIndex(y0, sizeY);
    const uint32_t z1 = nextIndex(z0, sizeZ);

    const float tx = std::clamp(fx - std::floor(fx), 0.0f, 1.0f);
    const float ty = std::clamp(fy - std::floor(fy), 0.0f, 1.0f);
    const float tz = std::clamp(fz - std::floor(fz), 0.0f, 1.0f);

    const math::Vector3 c00 = LerpV(data[Index(x0, y0, z0)], data[Index(x1, y0, z0)], tx);
    const math::Vector3 c10 = LerpV(data[Index(x0, y1, z0)], data[Index(x1, y1, z0)], tx);
    const math::Vector3 c01 = LerpV(data[Index(x0, y0, z1)], data[Index(x1, y0, z1)], tx);
    const math::Vector3 c11 = LerpV(data[Index(x0, y1, z1)], data[Index(x1, y1, z1)], tx);
    return LerpV(LerpV(c00, c10, ty), LerpV(c01, c11, ty), tz);
}

void BakeVectorField(VectorFieldRecipe recipe, uint32_t resolution,
                     const math::Vector3& extents, uint32_t seed, float strength,
                     VectorFieldAsset& outField)
{
    const uint32_t size = std::clamp(resolution, 2u, kMaxVectorFieldResolution);
    VectorFieldAsset field;
    field.sizeX = field.sizeY = field.sizeZ = size;
    field.boundsMin = { -std::abs(extents.x), -std::abs(extents.y), -std::abs(extents.z) };
    field.boundsMax = {  std::abs(extents.x),  std::abs(extents.y),  std::abs(extents.z) };
    field.data.resize(static_cast<size_t>(size) * size * size);

    /// @note seed は「ノイズ格子のどこを切り出すか」を決める。値そのものを変えるのではなく
    ///       座標をずらすことで、同じレシピから何枚でも別の場を焼ける。
    const float offset = static_cast<float>(math::PcgHash(seed) % 4096u) * 0.25f;

    const math::Vector3 span = {
        field.boundsMax.x - field.boundsMin.x,
        field.boundsMax.y - field.boundsMin.y,
        field.boundsMax.z - field.boundsMin.z };

    for (uint32_t z = 0; z < size; ++z) {
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                /// @note テクセル中心のローカル座標
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
                const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
                const float w = (static_cast<float>(z) + 0.5f) / static_cast<float>(size);
                const math::Vector3 local = {
                    field.boundsMin.x + span.x * u,
                    field.boundsMin.y + span.y * v,
                    field.boundsMin.z + span.z * w };

                math::Vector3 value = math::Vector3::ZERO;
                switch (recipe) {
                case VectorFieldRecipe::Curl:
                    value = math::CurlNoise({ local.x + offset, local.y + offset, local.z + offset });
                    break;
                case VectorFieldRecipe::Turbulence: {
                    /// @note オクターブごとに周波数を倍、振幅を半分。細かい渦が粗い渦に乗る。
                    float amplitude = 1.0f;
                    float frequency = 1.0f;
                    for (int octave = 0; octave < 3; ++octave) {
                        const math::Vector3 p = {
                            (local.x + offset) * frequency,
                            (local.y + offset) * frequency,
                            (local.z + offset) * frequency };
                        value = value + math::CurlNoise(p) * amplitude;
                        amplitude *= 0.5f;
                        frequency *= 2.0f;
                    }
                    break;
                }
                case VectorFieldRecipe::Vortex: {
                    /// @note Y 軸まわりの接線 + 一定の上昇。中心では接線が定義できないのでゼロ。
                    const math::Vector3 radial = { local.x, 0.0f, local.z };
                    const float distance = radial.Length();
                    if (distance > 1.0e-4f) {
                        const math::Vector3 dir = radial * (1.0f / distance);
                        value = { -dir.z, 0.35f, dir.x };
                    } else {
                        value = { 0.0f, 0.35f, 0.0f };
                    }
                    break;
                }
                case VectorFieldRecipe::Tornado: {
                    /// @note 上へ行くほど半径が絞られる。吸い込み (内向き) と接線を混ぜて
                    ///       «巻きながら中心へ吸われて上がる» を作る。
                    const math::Vector3 radial = { local.x, 0.0f, local.z };
                    const float distance = radial.Length();
                    const float height = std::clamp(
                        (local.y - field.boundsMin.y) / (std::max)(span.y, 1.0e-4f), 0.0f, 1.0f);
                    /// @note 上ほど強く吸う。下端では素通しにして «足元から吸い上げる» 形にする。
                    const float pull = 0.25f + height * 0.75f;
                    if (distance > 1.0e-4f) {
                        const math::Vector3 dir = radial * (1.0f / distance);
                        const math::Vector3 tangent = { -dir.z, 0.0f, dir.x };
                        value = tangent - dir * pull;
                        value.y = 0.3f + height * 0.9f;
                    } else {
                        value = { 0.0f, 0.3f + height * 0.9f, 0.0f };
                    }
                    break;
                }
                case VectorFieldRecipe::Sphere: {
                    const float distance = local.Length();
                    value = distance > 1.0e-4f ? local * (1.0f / distance) : math::Vector3::ZERO;
                    break;
                }
                }
                field.data[field.Index(x, y, z)] = value * strength;
            }
        }
    }
    outField = std::move(field);
}

void NormalizeVectorField(VectorFieldAsset& field)
{
    if (field.Empty()) return;

    constexpr uint32_t kTile = kVelocityFieldTileResolution;

    /// @name 1. 固定解像度へリサンプル
    /// @note 既にその解像度なら触らない (焼き直しのたびに補間を重ねて鈍らせない)。
    if (field.sizeX != kTile || field.sizeY != kTile || field.sizeZ != kTile) {
        const math::Vector3 span = {
            field.boundsMax.x - field.boundsMin.x,
            field.boundsMax.y - field.boundsMin.y,
            field.boundsMax.z - field.boundsMin.z };

        std::vector<math::Vector3> resampled(static_cast<size_t>(kTile) * kTile * kTile);
        for (uint32_t z = 0; z < kTile; ++z) {
            for (uint32_t y = 0; y < kTile; ++y) {
                for (uint32_t x = 0; x < kTile; ++x) {
                    /// @note テクセル中心を元の場の座標へ写して引く。
                    const math::Vector3 local = {
                        field.boundsMin.x + span.x * (static_cast<float>(x) + 0.5f) / kTile,
                        field.boundsMin.y + span.y * (static_cast<float>(y) + 0.5f) / kTile,
                        field.boundsMin.z + span.z * (static_cast<float>(z) + 0.5f) / kTile };
                    resampled[(static_cast<size_t>(z) * kTile + y) * kTile + x] =
                        field.SampleLocal(local);
                }
            }
        }
        field.data = std::move(resampled);
        field.sizeX = field.sizeY = field.sizeZ = kTile;
    }

    /// @name 2. 正規化係数を決めて量子化
    /// @note 係数は «成分の絶対値の最大» を使う (長さだと各成分がバイト範囲を使い切らず、最悪 sqrt(3) 倍
    ///       精度を捨てる)。この選び方は冪等: 係数と同じ大きさの成分はバイト 0/255 に当たり復元すると
    ///       ちょうど ±maxMagnitude に戻るため、量子化後のデータから測り直しても同じ係数が出る (長さだと
    ///       丸めで最長ベクトルが縮み、次回係数がずれて格子ごと全成分がずれる)。長さ 0 の場は 0 除算しない。
    float maxComponent = 0.0f;
    for (const math::Vector3& v : field.data) {
        maxComponent = (std::max)(maxComponent,
            (std::max)(std::abs(v.x), (std::max)(std::abs(v.y), std::abs(v.z))));
    }
    field.maxMagnitude = maxComponent > 1.0e-6f ? maxComponent : 1.0f;

    for (math::Vector3& value : field.data) value = QuantizeVectorFieldValue(value, field.maxMagnitude);
}

} // namespace fbzz::fluid
