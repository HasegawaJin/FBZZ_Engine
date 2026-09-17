/// @file    VectorFieldAsset.hpp
/// @brief   空間へ焼いた速度場。解析的な力場を増やす代わりに 1 枚で任意の流れを表す。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// @note 場をアセットにするのは、ForceField が Wind / Vortex / Turbulence … と «式» を 1 つずつ
///       enum へ足してきたため。竜巻・磁気パルス・魔法陣のように «その形にしか無い流れ» は式では
///       書けず、足すたびにフィールドと HLSL の分岐が増える。焼いたグリッドを 1 枚標本化する形に
///       すれば、どんな流れでも型の変更なしに表現でき、粒子以外 (草・煙・布) も同じ場を共有できる。
/// @note ファイル (速度場 PNG / .fga) の読み書きは Engine の VectorFieldFile。ここは «格子と式» だけ。
/// @see Docs/design/fluid-library.md
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <Math/Vector3.hpp>

namespace fbzz::fluid {

/// @brief 格子 1 辺の上限。
/// @note 32³ で 32768 ベクトル、64³ で 262144。これ以上は 3D テクスチャの転送量が効いてくるうえ、
///       粒子の乱流としては格子が細かすぎて絵に出ない。
inline constexpr uint32_t kMaxVectorFieldResolution = 128;

/// @brief 取り込み後の格子の 1 辺。全ての速度場はこの解像度へ揃えられる。
/// @note GPU では常駐中の場を 1 枚の Texture3D へタイルとして積む (cs_5_0 にテクスチャ配列が
///       無いため)。タイルは同じ寸法でなければ添字が計算できない。取り込みの時点で揃えて
///       しまえば、CPU が引く格子と GPU が引く格子が同じものになる。後段でリサンプルすると
///       «CPU では回るのに GPU では違う軌跡» が生まれる。
inline constexpr uint32_t kVelocityFieldTileResolution = 32;

/// @brief 場の 1 成分を RGBA8 の 1 バイトへ詰める。GPU が読むバイトそのもの。
/// @note HLSL 側の復元は (rgb * 2 - 1) * maxMagnitude で、この式と対になっている。
[[nodiscard]] inline uint8_t EncodeVectorFieldByte(float value, float maxMagnitude)
{
    const float inverse = maxMagnitude > 1.0e-6f ? 1.0f / maxMagnitude : 1.0f;
    const float normalized = std::clamp(value * inverse, -1.0f, 1.0f);
    return static_cast<uint8_t>(
        std::clamp(std::round((normalized * 0.5f + 0.5f) * 255.0f), 0.0f, 255.0f));
}

/// @brief EncodeVectorFieldByte の丸めを通した後に復元される値。CPU 側の data はこれを保持する。
/// @note 力場の式は CPU と GPU で一致させる規約になっている。片方だけ float 精度で読むと、
///       同じ場なのに «CPU では回るのに GPU では違う軌跡» になり、しかも見比べないと気付けない。
///       丸めごと合わせてしまうのが唯一確実。
[[nodiscard]] inline math::Vector3 QuantizeVectorFieldValue(const math::Vector3& value,
                                                            float maxMagnitude)
{
    const auto decode = [maxMagnitude](uint8_t byteValue) {
        return (static_cast<float>(byteValue) / 255.0f * 2.0f - 1.0f) * maxMagnitude;
    };
    return { decode(EncodeVectorFieldByte(value.x, maxMagnitude)),
             decode(EncodeVectorFieldByte(value.y, maxMagnitude)),
             decode(EncodeVectorFieldByte(value.z, maxMagnitude)) };
}

/// @brief 手続きベイクのレシピ。焼いた後は等しく速度格子なので、読む側はレシピを知らない。
enum class VectorFieldRecipe : uint8_t {
    Curl = 0,   ///< @brief カールノイズ (発散ゼロ)。煙・炎の揺らぎ
    Vortex,     ///< @brief 軸まわりの回転 + 軸方向の上昇
    Tornado,    ///< @brief 高さが上がるほど半径が絞られる渦
    Sphere,     ///< @brief 中心から外向き (strength < 0 で吸い込み)
    Turbulence, ///< @brief 多オクターブのカールノイズ
};

[[nodiscard]] const char* VectorFieldRecipeName(VectorFieldRecipe recipe);

/// @brief 焼かれた速度場 1 枚。
/// @note 取り込みを終えた時点で data は **kVelocityFieldTileResolution³ かつ量子化済み**。
///       GPU が RGBA8 から復元するのと同じ値が入っているので、CPU と GPU は必ず同じ場を見る。
struct VectorFieldAsset {
    uint32_t sizeX = 0;
    uint32_t sizeY = 0;
    uint32_t sizeZ = 0;
    /// @brief 場のローカル AABB。GameObject の Transform でワールドへ運ぶ。
    math::Vector3 boundsMin = { -1.0f, -1.0f, -1.0f };
    math::Vector3 boundsMax = {  1.0f,  1.0f,  1.0f };
    /// @brief x が最も速く変わる順 (index = (z * sizeY + y) * sizeX + x)。
    std::vector<math::Vector3> data;
    /// @brief RGBA8 へ詰めるときの正規化係数。復元は (rgb * 2 - 1) * maxMagnitude。
    float maxMagnitude = 1.0f;

    [[nodiscard]] bool Empty() const { return data.empty() || sizeX == 0 || sizeY == 0 || sizeZ == 0; }
    [[nodiscard]] size_t Index(uint32_t x, uint32_t y, uint32_t z) const
    {
        return (static_cast<size_t>(z) * sizeY + y) * sizeX + x;
    }

    /// @brief ローカル座標をトリリニア補間で標本化する。
    /// @return AABB の外はゼロ。場の外へ出た粒子が縁の値を引きずり続けないため。
    [[nodiscard]] math::Vector3 SampleLocal(const math::Vector3& localPosition) const;
};

/// @brief レシピからグリッドを焼く。
/// @note 解像度も量子化もここでは揃えない (呼び出し側が保存し、取り込みで NormalizeVectorField が通る)。
void BakeVectorField(VectorFieldRecipe recipe, uint32_t resolution,
                     const math::Vector3& extents, uint32_t seed, float strength,
                     VectorFieldAsset& outField);

/// @brief 取り込み直後の場を «エンジンが扱う形» へ揃える。
/// @note 1. kVelocityFieldTileResolution³ へトリリニアでリサンプルし、
///       2. 最大長で正規化して RGBA8 相当へ量子化し、復元される値を data へ書き戻す。
/// @note GPU に載せる瞬間ではなく取り込みで揃えることで «GPU で使い始めた途端に CPU の軌跡も
///       変わった» が起きなくなる。GPU を持たない経路 (テスト・静的検査) も同じ値を見る。
void NormalizeVectorField(VectorFieldAsset& field);

} // namespace fbzz::fluid
