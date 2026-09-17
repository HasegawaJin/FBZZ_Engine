/// @file    TerrainSplat.hpp
/// @brief   頂点ごとの «上位 4 層の番号と重み» を正準形に保つ純粋関数。
/// @author  Hasegawa Jin
/// @date    2026-09-17
///
/// @note 1 頂点 = 番号 4 バイト + 重み 4 バイト。正準形は «合計 255・重み降順 (同重みは番号昇順)・
///       番号の重複なし・重み 0 の番号は 0»。Undo 比較・保存差分・Fiber の内容署名が
///       «見た目が同じならバイト列も同じ» を前提にするため、書き込みは必ずここを通す。
/// @see Docs/design/terrain-layers.md
#pragma once

#include <cstdint>
#include <vector>

namespace fbzz::scene {

/// @brief 1 頂点が持つ層の枠数。
inline constexpr int TERRAIN_SPLAT_SLOTS = 4;
/// @brief 層番号は uint8 で持つので 255 層まで (番号 0〜254)。
inline constexpr int TERRAIN_MAX_LAYERS = 255;

namespace terrain_splat {

/// @brief 任意の (番号, 重み) 列を上位 4 層の正準形へ畳む。
/// @param layers 層番号。重複してよい (重みを合算する)。
/// @param weights 非負の重み。合計が 0 なら層 0 = 255 にする。
/// @param keepLayer 0 以上なら、重みが 5 番目以下でもこの層を 4 枠に残す (塗りが進まなくなるのを防ぐ)。
void Canonicalize(const int* layers, const float* weights, int count,
                  std::uint8_t* outIndices, std::uint8_t* outWeights, int keepLayer = -1);

/// @brief 既に 4 枠の整数データを正準形へ直す。正準形の入力はそのまま返る。
void Canonicalize(std::uint8_t* indices, std::uint8_t* weights);

/// @return layer の重み [0, 1]。4 枠に無ければ 0。
[[nodiscard]] float WeightOf(const std::uint8_t* indices, const std::uint8_t* weights, int layer);

/// @brief 分布を «layer が 100%» へ t だけ線形補間する。
/// @note t > 0 で量子化が 1 段も進まないときは 1/255 を強制的に移す。長押しで塗りが止まるのを防ぐ。
void BlendToward(std::uint8_t* indices, std::uint8_t* weights, int layer, float t);

/// @brief layer の重みを weight [0, 1] に置き、他の層は比率を保って残りを分け合う。
/// @note 他の層が無い頂点で layer を減らすことはできない (残りの行き先が無い)。そのときは変えない。
void SetWeight(std::uint8_t* indices, std::uint8_t* weights, int layer, float weight);

/// @brief 番号を付け替える。oldToNew[old] が負ならその層の重みを捨てる。範囲外の番号も捨てる。
void Remap(std::uint8_t* indices, std::uint8_t* weights, const std::vector<int>& oldToNew);

/// @brief 旧形式の RGBA 4 層 (R=層 0 … A=層 3) を正準形へ変換する。見た目は変わらない。
void FromLegacyRgba(const std::uint8_t* rgba, std::uint8_t* outIndices, std::uint8_t* outWeights);

} // namespace terrain_splat
} // namespace fbzz::scene
