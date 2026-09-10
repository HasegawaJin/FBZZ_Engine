/// @file    VectorFieldAsset.cpp
/// @brief   .vfield の読み書き・手続きベイク・GPU アップロード。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Asset/VectorFieldAsset.hpp>

#include <Engine/Core/CurlNoise.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace fbzz::asset {
namespace {

constexpr char     kMagic[4]  = { 'F', 'Z', 'V', 'F' };
constexpr uint32_t kVersion   = 1;
// 1 辺の上限。32³ で 32768 ベクトル、64³ で 262144。これ以上は 3D テクスチャの
// 転送量が効いてくるうえ、粒子の乱流としては格子が細かすぎて絵に出ない。
constexpr uint32_t kMaxResolution = 128;

template<class T>
void WritePod(std::ofstream& out, const T& value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template<class T>
bool ReadPod(std::ifstream& in, T& outValue)
{
    in.read(reinterpret_cast<char*>(&outValue), sizeof(T));
    return static_cast<bool>(in);
}

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

    // [0,1] の正規化座標。AABB の外は場が無いものとして扱う。
    const float u = (localPosition.x - boundsMin.x) / span.x;
    const float v = (localPosition.y - boundsMin.y) / span.y;
    const float w = (localPosition.z - boundsMin.z) / span.z;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f || w < 0.0f || w > 1.0f)
        return math::Vector3::ZERO;

    // テクセル中心へ寄せる。GPU の SampleLevel と同じ位置を引くための半テクセルずらし。
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

bool SaveVectorField(const std::string& absPath, const VectorFieldAsset& field)
{
    if (field.Empty()) return false;
    std::ofstream out(absPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        FBZZ_LOG_ERROR("VectorField: 書き出せません: %s", absPath.c_str());
        return false;
    }
    out.write(kMagic, sizeof(kMagic));
    WritePod(out, kVersion);
    WritePod(out, field.sizeX);
    WritePod(out, field.sizeY);
    WritePod(out, field.sizeZ);
    WritePod(out, field.boundsMin);
    WritePod(out, field.boundsMax);
    WritePod(out, field.maxMagnitude);
    out.write(reinterpret_cast<const char*>(field.data.data()),
              static_cast<std::streamsize>(field.data.size() * sizeof(math::Vector3)));
    return static_cast<bool>(out);
}

bool LoadVectorFieldFile(const std::string& absPath, VectorFieldAsset& outField)
{
    std::ifstream in(absPath, std::ios::binary);
    if (!in) return false;

    char magic[4] = {};
    in.read(magic, sizeof(magic));
    if (!in || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
        FBZZ_LOG_ERROR("VectorField: マジックが違います: %s", absPath.c_str());
        return false;
    }
    uint32_t version = 0;
    if (!ReadPod(in, version) || version > kVersion) {
        FBZZ_LOG_ERROR("VectorField: 未知のバージョン %u: %s", version, absPath.c_str());
        return false;
    }
    VectorFieldAsset field;
    if (!ReadPod(in, field.sizeX) || !ReadPod(in, field.sizeY) || !ReadPod(in, field.sizeZ))
        return false;
    if (field.sizeX == 0 || field.sizeY == 0 || field.sizeZ == 0
        || field.sizeX > kMaxResolution || field.sizeY > kMaxResolution
        || field.sizeZ > kMaxResolution) {
        FBZZ_LOG_ERROR("VectorField: 解像度が範囲外 (%ux%ux%u): %s",
                       field.sizeX, field.sizeY, field.sizeZ, absPath.c_str());
        return false;
    }
    if (!ReadPod(in, field.boundsMin) || !ReadPod(in, field.boundsMax)
        || !ReadPod(in, field.maxMagnitude))
        return false;

    const size_t count = static_cast<size_t>(field.sizeX) * field.sizeY * field.sizeZ;
    field.data.resize(count);
    in.read(reinterpret_cast<char*>(field.data.data()),
            static_cast<std::streamsize>(count * sizeof(math::Vector3)));
    if (!in) {
        FBZZ_LOG_ERROR("VectorField: データが足りません: %s", absPath.c_str());
        return false;
    }
    outField = std::move(field);
    return true;
}

// .fga は 1 行目にヘッダー、以降カンマ区切りで xyz が並ぶ ASCII。
//   sizeX,sizeY,sizeZ, minX,minY,minZ, maxX,maxY,maxZ, x,y,z, x,y,z, ...
// 改行の位置は書き出したツールによって違うので、区切りだけを見て数値を順に取る。
bool ImportFgaFile(const std::string& absPath, VectorFieldAsset& outField)
{
    std::ifstream in(absPath);
    if (!in) return false;

    std::vector<float> values;
    std::string token;
    while (std::getline(in, token, ',')) {
        // 行末の改行が混ざるので、数値として読めた分だけを拾う。
        std::istringstream parse(token);
        float value = 0.0f;
        if (parse >> value) values.push_back(value);
    }
    if (values.size() < 9) {
        FBZZ_LOG_ERROR("VectorField: .fga のヘッダーが読めません: %s", absPath.c_str());
        return false;
    }

    VectorFieldAsset field;
    field.sizeX = static_cast<uint32_t>((std::max)(values[0], 0.0f));
    field.sizeY = static_cast<uint32_t>((std::max)(values[1], 0.0f));
    field.sizeZ = static_cast<uint32_t>((std::max)(values[2], 0.0f));
    if (field.sizeX == 0 || field.sizeY == 0 || field.sizeZ == 0
        || field.sizeX > kMaxResolution || field.sizeY > kMaxResolution
        || field.sizeZ > kMaxResolution) {
        FBZZ_LOG_ERROR("VectorField: .fga の解像度が範囲外 (%ux%ux%u): %s",
                       field.sizeX, field.sizeY, field.sizeZ, absPath.c_str());
        return false;
    }
    // .fga は cm 基準で書かれる。エンジンは m なので 1/100 する。
    constexpr float kCmToM = 0.01f;
    field.boundsMin = { values[3] * kCmToM, values[4] * kCmToM, values[5] * kCmToM };
    field.boundsMax = { values[6] * kCmToM, values[7] * kCmToM, values[8] * kCmToM };

    const size_t count = static_cast<size_t>(field.sizeX) * field.sizeY * field.sizeZ;
    if (values.size() < 9 + count * 3) {
        FBZZ_LOG_ERROR("VectorField: .fga のベクトル数が足りません (%zu / %zu): %s",
                       (values.size() - 9) / 3, count, absPath.c_str());
        return false;
    }
    field.data.resize(count);
    for (size_t i = 0; i < count; ++i) {
        field.data[i] = { values[9 + i * 3 + 0], values[9 + i * 3 + 1], values[9 + i * 3 + 2] };
    }
    outField = std::move(field);
    return true;
}

void BakeVectorField(VectorFieldRecipe recipe, uint32_t resolution,
                     const math::Vector3& extents, uint32_t seed, float strength,
                     VectorFieldAsset& outField)
{
    const uint32_t size = std::clamp(resolution, 2u, kMaxResolution);
    VectorFieldAsset field;
    field.sizeX = field.sizeY = field.sizeZ = size;
    field.boundsMin = { -std::abs(extents.x), -std::abs(extents.y), -std::abs(extents.z) };
    field.boundsMax = {  std::abs(extents.x),  std::abs(extents.y),  std::abs(extents.z) };
    field.data.resize(static_cast<size_t>(size) * size * size);

    // seed は「ノイズ格子のどこを切り出すか」を決める。値そのものを変えるのではなく
    // 座標をずらすことで、同じレシピから何枚でも別の場を焼ける。
    const float offset = static_cast<float>(core::PcgHash(seed) % 4096u) * 0.25f;

    const math::Vector3 span = {
        field.boundsMax.x - field.boundsMin.x,
        field.boundsMax.y - field.boundsMin.y,
        field.boundsMax.z - field.boundsMin.z };

    for (uint32_t z = 0; z < size; ++z) {
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                // テクセル中心のローカル座標
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
                    value = core::CurlNoise({ local.x + offset, local.y + offset, local.z + offset });
                    break;
                case VectorFieldRecipe::Turbulence: {
                    // オクターブごとに周波数を倍、振幅を半分。細かい渦が粗い渦に乗る。
                    float amplitude = 1.0f;
                    float frequency = 1.0f;
                    for (int octave = 0; octave < 3; ++octave) {
                        const math::Vector3 p = {
                            (local.x + offset) * frequency,
                            (local.y + offset) * frequency,
                            (local.z + offset) * frequency };
                        value = value + core::CurlNoise(p) * amplitude;
                        amplitude *= 0.5f;
                        frequency *= 2.0f;
                    }
                    break;
                }
                case VectorFieldRecipe::Vortex: {
                    // Y 軸まわりの接線 + 一定の上昇。中心では接線が定義できないのでゼロ。
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
                    // 上へ行くほど半径が絞られる。吸い込み (内向き) と接線を混ぜて
                    // «巻きながら中心へ吸われて上がる» を作る。
                    const math::Vector3 radial = { local.x, 0.0f, local.z };
                    const float distance = radial.Length();
                    const float height = std::clamp(
                        (local.y - field.boundsMin.y) / (std::max)(span.y, 1.0e-4f), 0.0f, 1.0f);
                    // 上ほど強く吸う。下端では素通しにして «足元から吸い上げる» 形にする。
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

    // ── 1. 固定解像度へリサンプル ──
    // 既にその解像度なら触らない (焼き直しのたびに補間を重ねて鈍らせない)。
    if (field.sizeX != kTile || field.sizeY != kTile || field.sizeZ != kTile) {
        const math::Vector3 span = {
            field.boundsMax.x - field.boundsMin.x,
            field.boundsMax.y - field.boundsMin.y,
            field.boundsMax.z - field.boundsMin.z };

        std::vector<math::Vector3> resampled(static_cast<size_t>(kTile) * kTile * kTile);
        for (uint32_t z = 0; z < kTile; ++z) {
            for (uint32_t y = 0; y < kTile; ++y) {
                for (uint32_t x = 0; x < kTile; ++x) {
                    // テクセル中心を元の場の座標へ写して引く。
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

    // ── 2. 正規化係数を決めて量子化 ──
    // 係数は «成分の絶対値の最大» を使う。EncodeVectorFieldByte が成分ごとに
    // [-maxMagnitude, maxMagnitude] へ写す以上、長さで測ると各成分がバイト範囲を
    // 使い切らず精度を捨てることになる (最悪で sqrt(3) 倍粗い)。
    //
    // WHY 冪等になるか: 係数と同じ大きさの成分はバイト 0 / 255 に当たり、復元しても
    //   ちょうど ±maxMagnitude へ戻る。他の成分はその範囲に収まるので、量子化後の
    //   データから測り直しても同じ係数が出る ── 2 度通しても値が動かない。
    //   長さで測るとここが崩れる。丸めで最長ベクトルが少し縮み、次の呼び出しで
    //   係数が変わり、格子ごと全成分がずれる。
    // 長さ 0 の場を焼いたときに 0 除算しない。
    float maxComponent = 0.0f;
    for (const math::Vector3& v : field.data) {
        maxComponent = (std::max)(maxComponent,
            (std::max)(std::abs(v.x), (std::max)(std::abs(v.y), std::abs(v.z))));
    }
    field.maxMagnitude = maxComponent > 1.0e-6f ? maxComponent : 1.0f;

    for (math::Vector3& value : field.data) value = QuantizeVectorFieldValue(value, field.maxMagnitude);
}

} // namespace fbzz::asset
