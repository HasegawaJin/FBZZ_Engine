/// @file    VectorFieldAsset.hpp
/// @brief   空間へ焼いた速度場。解析的な力場を増やす代わりに 1 枚で任意の流れを表す。
/// @author  Hasegawa Jin
/// @date    2026-09-11

///
/// @note 流れを格子に焼いておくと、力場の種類やシェーダーの分岐を追加せずに粒子・草・煙・布で同じ場を標本化できる。
/// @note 速度場 PNG と FGA の読み書きは Engine の VectorFieldFile が担当する。
/// @see Docs/design/fluid-library.md
#pragma once
#include <Fluid/Api.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <Math/LinearAlgebra/Vector3.hpp>

namespace fbzz::fluid {

/// @brief 格子 1 辺の上限。
/// @note 32³ で 32768 ベクトル、64³ で 262144。これ以上は 3D テクスチャの転送量が効いてくるうえ、
/// @note 粒子の乱流としては格子が細かすぎて絵に出ない。
inline constexpr uint32_t kMaxVectorFieldResolution = 128;

/// @brief 取り込み後の格子の 1 辺。全ての速度場はこの解像度へ揃えられる。
/// @note GPU では常駐中の場を 1 枚の Texture3D へタイルとして積む (cs_5_0 にテクスチャ配列が
/// @note 無いため)。タイルは同じ寸法でなければ添字が計算できない。取り込みの時点で揃えて
/// @note しまえば、CPU が引く格子と GPU が引く格子が同じものになる。後段でリサンプルすると
/// @note «CPU では回るのに GPU では違う軌跡» が生まれる。
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
/// @note 同じ場なのに «CPU では回るのに GPU では違う軌跡» になり、しかも見比べないと気付けない。
/// @note 丸めごと合わせてしまうのが唯一確実。
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

[[nodiscard]] FBZZ_FLUID_API const char* VectorFieldRecipeName(VectorFieldRecipe recipe);

/// @brief 焼かれた速度場 1 枚。
/// @note 取り込みを終えた時点で data は **kVelocityFieldTileResolution³ かつ量子化済み**。
/// @note GPU が RGBA8 から復元するのと同じ値が入っているので、CPU と GPU は必ず同じ場を見る。
struct VectorFieldAsset {
    uint32_t sizeX = 0; ///< ベクトル場の X 軸方向の格子点数。
    uint32_t sizeY = 0; ///< ベクトル場の Y 軸方向の格子点数。
    uint32_t sizeZ = 0; ///< ベクトル場の Z 軸方向の格子点数。
    math::Vector3 boundsMin = { -1.0f, -1.0f, -1.0f }; ///< 場のローカル AABB。GameObject の Transform でワールドへ運ぶ。
    math::Vector3 boundsMax = {  1.0f,  1.0f,  1.0f }; ///< ベクトル場の局所座標範囲の最大点。
    std::vector<math::Vector3> data; ///< x が最も速く変わる順 (index = (z * sizeY + y) * sizeX + x)。
    float maxMagnitude = 1.0f; ///< RGBA8 へ詰めるときの正規化係数。復元は (rgb * 2 - 1) * maxMagnitude。

    [[nodiscard]] bool Empty() const { return data.empty() || sizeX == 0 || sizeY == 0 || sizeZ == 0; }
    [[nodiscard]] size_t Index(uint32_t x, uint32_t y, uint32_t z) const
    {
        return (static_cast<size_t>(z) * sizeY + y) * sizeX + x;
    }

    /// @brief ローカル座標をトリリニア補間で標本化する。
    /// @param[in] localPosition boundsMin および boundsMax と同じ座標系での標本位置。有限値を渡す。
    /// @return 局所座標系での速度。格子が空、AABB のいずれかの辺が 1e-6 以下、または標本位置が範囲外ならゼロ。
    /// @note AABB の境界は標本化に含める。場の外で縁の速度が残らないよう、外側はゼロを返す。
    [[nodiscard]] FBZZ_FLUID_API math::Vector3 SampleLocal(const math::Vector3& localPosition) const;
};

/// @brief レシピからグリッドを焼く。
/// @param[in] recipe 局所座標から速度を生成する手続き。
/// @param[in] resolution 格子の 1 辺の点数。2 から kMaxVectorFieldResolution に収める。
/// @param[in] extents 原点から AABB の端までの局所距離。各成分の絶対値を使う。
/// @param[in] seed 手続きノイズの標本位置を決める種。
/// @param[in] strength 焼き込む速度の倍率。
/// @param[out] outField 焼いた速度格子で置き換える。
/// @note 解像度も量子化もここでは揃えない (呼び出し側が保存し、取り込みで NormalizeVectorField が通る)。
FBZZ_FLUID_API void BakeVectorField(VectorFieldRecipe recipe, uint32_t resolution,
                     const math::Vector3& extents, uint32_t seed, float strength,
                     VectorFieldAsset& outField);

/// @brief 取り込み直後の場を «エンジンが扱う形» へ揃える。
/// @param[in,out] field 取り込んだ格子。現在の速度を標本化し、格子解像度と量子化後の値を更新する。
/// @note 1. kVelocityFieldTileResolution³ へトリリニアでリサンプルし、
/// @note 2. 最大長で正規化して RGBA8 相当へ量子化し、復元される値を data へ書き戻す。
/// @note GPU に載せる瞬間ではなく取り込みで揃えることで «GPU で使い始めた途端に CPU の軌跡も
/// @note 変わった» が起きなくなる。GPU を持たない経路 (テスト・静的検査) も同じ値を見る。
FBZZ_FLUID_API void NormalizeVectorField(VectorFieldAsset& field);

} /// @note namespace fbzz::fluid
