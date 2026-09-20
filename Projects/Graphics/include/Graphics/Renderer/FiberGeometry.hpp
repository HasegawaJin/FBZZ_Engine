/// @file    FiberGeometry.hpp
/// @brief   GPU 非依存の Fin トポロジー生成。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Matrix4.hpp>
#include <span>
#include <cstdint>
#include <vector>

namespace fbzz::renderer {
struct Mesh;

/// @note FiberFin.hlsl の入力順と一致。position は変形前の根元、height は [0,1]。
struct FiberFinVertex {
    math::Vector3 m_position;
    math::Vector3 m_normal;
    math::Vector2 m_uv;
    math::Vector3 m_faceNormal0;
    math::Vector3 m_faceNormal1;
    float m_height = 0.0f;
    float m_edgeCoordinate = 0.0f;
    math::Vector3 m_edgeVector;
    uint32_t m_boneIndices[4] = {};
    float m_boneWeights[4] = {1, 0, 0, 0};
};
static_assert(sizeof(FiberFinVertex) == 108);

/// @brief Blade 1 葉ぶんの根元。GPU は VS で SV_VertexID から 2 リボン × 高さ 6 分割の頂点を組み立てる。
/// @note FiberSurface.hlsli の FiberBladeRoot と一致。座標・法線・幅ベクトルは元メッシュのローカル空間。
/// @note side0 / side1 はリボン 0 / 1 の半幅ベクトル。height は葉の相対高さ [0.65,1]、rank は密度判定用の乱数 [0,1)。
struct FiberBladeRoot {
    math::Vector3 m_position;
    float m_height = 1.0f;
    math::Vector3 m_normal;
    float m_rank = 0.0f;
    math::Vector3 m_side0;
    float m_u = 0.0f;
    math::Vector3 m_side1;
    float m_v = 0.0f;
};
static_assert(sizeof(FiberBladeRoot) == 64);

/// @note 1 葉の頂点数。リボン 2 枚 × 高さ 6 区間 × 三角形 2 枚 × 3 頂点。FiberSurface.hlsli と一致。
inline constexpr uint32_t FIBER_BLADE_VERTICES = 72;
/// @note 1 メッシュ / 地形パッチの葉の上限。
inline constexpr uint32_t FIBER_MAX_BLADES = 32768;

struct FiberFinMesh {
    std::vector<FiberFinVertex> m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<uint32_t> m_sourceVertices;
};

/// @return 不正なインデックス / 非有限値 / 非多様体辺は false。失敗時 out は変更しない。
/// @note 完全に同じ位置の頂点を接続して UV 継ぎ目の二重 Fin を防ぐ。離れた頂点を許容誤差で結ばない。
/// @note UV は最初の隣接面から取る。縮退面は無視する。分離した重なり面は別 Mesh にする。
/// @see https://hhoppe.com/fur.pdf
[[nodiscard]] bool BuildFiberFins(const Mesh& mesh, FiberFinMesh& out);

/// @note 三角形面積に比例した決定的な根元分布。幅はローカル座標、密度はローカル面積あたり。
/// @note 頂点は作らず根元だけを返す (1 葉 64 バイト)。形状は FiberSurface.hlsli が SV_VertexID から組み立てる。
/// @param densityFromVertexAlpha true なら補間した頂点色 A を採択確率として根元を棄却法で間引く。false では A を読まず乱数の消費順も従来のまま。
/// @return 不正な入力または生成上限超過は false。out は失敗時に変更しない。上限は間引く前の本数で判定する。
/// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-7-rendering-countless-blades-waving-grass
/// @see https://en.wikipedia.org/wiki/Rejection_sampling (Rejection sampling)
/// @see Docs/design/terrain-layers.md §5 Fiber を層で制御する
[[nodiscard]] bool BuildFiberBlades(const Mesh& mesh, float density, float width, uint32_t seed,
                                    std::vector<FiberBladeRoot>& out, bool densityFromVertexAlpha = false);

/// @note 近距離では最大品質、遠距離では段階的に層数を減らす。距離・境界はワールド m。
[[nodiscard]] int FiberLodShellCount(int maximum, int minimum, float distance, float nearDistance, float farDistance);

} /// @note namespace fbzz::renderer
