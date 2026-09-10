/// @file    VectorFieldAsset.hpp
/// @brief   .vfield — 空間へ焼いた速度場。解析的な力場を増やす代わりに 1 枚で任意の流れを表す。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// WHY 場をアセットにするか:
///   ParticleForceField は Wind / Vortex / Turbulence … と «式» を 1 つずつ enum へ足してきた。
///   竜巻・磁気パルス・魔法陣のように «その形にしか無い流れ» は式では書けず、足すたびに
///   フィールドと HLSL の分岐が増える。焼いたグリッドを 1 枚サンプルする形にすれば、
///   どんな流れでも型の変更なしに表現でき、粒子以外 (草・煙・布) も同じ場を共有できる。
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <Math/Vector3.hpp>

namespace fbzz::asset {

/// 取り込み後の格子の 1 辺。**全ての .vfield はこの解像度へ揃えられる**。
///
/// WHY 揃えるか: GPU では常駐中の場を 1 枚の Texture3D へタイルとして積む
///   (cs_5_0 にテクスチャ配列が無いため)。タイルは同じ寸法でなければ添字が計算できない。
///   取り込みの時点で揃えてしまえば、CPU が引く格子と GPU が引く格子が同じものになる。
///   後段でリサンプルすると «CPU では回るのに GPU では違う軌跡» が生まれる。
inline constexpr uint32_t kVelocityFieldTileResolution = 32;

/// 場の 1 成分を RGBA8 の 1 バイトへ詰める。**GPU が読むバイトそのもの**。
/// HLSL 側の復元は (rgb * 2 - 1) * maxMagnitude で、この式と対になっている。
[[nodiscard]] inline uint8_t EncodeVectorFieldByte(float value, float maxMagnitude)
{
    const float inverse = maxMagnitude > 1.0e-6f ? 1.0f / maxMagnitude : 1.0f;
    const float normalized = std::clamp(value * inverse, -1.0f, 1.0f);
    return static_cast<uint8_t>(
        std::clamp(std::round((normalized * 0.5f + 0.5f) * 255.0f), 0.0f, 255.0f));
}

/// 上の丸めを通した後に復元される値。CPU 側の data はこれを保持する。
///
/// WHY CPU も量子化後を持つか: 力場の式は CPU と GPU で一致させる規約になっている。
///   片方だけ float 精度で読むと、同じ場なのに «CPU では回るのに GPU では違う軌跡»
///   になり、しかも見比べないと気付けない。丸めごと合わせてしまうのが唯一確実。
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

/// 手続きベイクのレシピ。焼いた後は等しく .vfield なので、読む側はレシピを知らない。
enum class VectorFieldRecipe : uint8_t {
    Curl = 0,   ///< カールノイズ (発散ゼロ)。煙・炎の揺らぎ
    Vortex,     ///< 軸まわりの回転 + 軸方向の上昇
    Tornado,    ///< 高さが上がるほど半径が絞られる渦
    Sphere,     ///< 中心から外向き (strength < 0 で吸い込み)
    Turbulence, ///< 多オクターブのカールノイズ
};

[[nodiscard]] const char* VectorFieldRecipeName(VectorFieldRecipe recipe);

/// 焼かれた速度場 1 枚。
///
/// @note 取り込みを終えた時点で data は **kVelocityFieldTileResolution³ かつ量子化済み**。
///       GPU が RGBA8 から復元するのと同じ値が入っているので、CPU と GPU は必ず同じ場を見る。
struct VectorFieldAsset {
    uint32_t sizeX = 0;
    uint32_t sizeY = 0;
    uint32_t sizeZ = 0;
    /// 場のローカル AABB。GameObject の Transform でワールドへ運ぶ。
    math::Vector3 boundsMin = { -1.0f, -1.0f, -1.0f };
    math::Vector3 boundsMax = {  1.0f,  1.0f,  1.0f };
    /// x が最も速く変わる順 (index = (z * sizeY + y) * sizeX + x)。
    std::vector<math::Vector3> data;
    /// RGBA8 へ詰めるときの正規化係数。復元は (rgb * 2 - 1) * maxMagnitude。
    float maxMagnitude = 1.0f;

    [[nodiscard]] bool Empty() const { return data.empty() || sizeX == 0 || sizeY == 0 || sizeZ == 0; }
    [[nodiscard]] size_t Index(uint32_t x, uint32_t y, uint32_t z) const
    {
        return (static_cast<size_t>(z) * sizeY + y) * sizeX + x;
    }

    /// ローカル座標をトリリニア補間でサンプルする。
    /// @note AABB の外はゼロを返す。場の外へ出た粒子が縁の値を引きずり続けないため。
    [[nodiscard]] math::Vector3 SampleLocal(const math::Vector3& localPosition) const;
};

/// バイナリ .vfield の読み書き。TOML にしないのは 32³ でも 32768 ベクトルあるため。
[[nodiscard]] bool SaveVectorField(const std::string& absPath, const VectorFieldAsset& field);
[[nodiscard]] bool LoadVectorFieldFile(const std::string& absPath, VectorFieldAsset& outField);

/// Unreal の .fga (ASCII) を取り込む。既存の資産をそのまま持ち込めるようにするためだけの経路。
[[nodiscard]] bool ImportFgaFile(const std::string& absPath, VectorFieldAsset& outField);

/// レシピからグリッドを焼く。解像度も量子化もここでは揃えない
/// (呼び出し側が保存し、取り込みで NormalizeVectorField が通る)。
void BakeVectorField(VectorFieldRecipe recipe, uint32_t resolution,
                     const math::Vector3& extents, uint32_t seed, float strength,
                     VectorFieldAsset& outField);

/// 取り込み直後の場を «エンジンが扱う形» へ揃える。
///   1. kVelocityFieldTileResolution³ へトリリニアでリサンプルする
///   2. 最大長で正規化して RGBA8 相当へ量子化し、復元される値を data へ書き戻す
///
/// WHY 読み込み時に必ず通すか: GPU に載せる瞬間ではなく取り込みで揃えることで、
///   «GPU で使い始めた途端に CPU の軌跡も変わった» が起きなくなる。GPU を持たない経路
///   (テスト・静的検査) も同じ値を見る。
void NormalizeVectorField(VectorFieldAsset& field);

} // namespace fbzz::asset
