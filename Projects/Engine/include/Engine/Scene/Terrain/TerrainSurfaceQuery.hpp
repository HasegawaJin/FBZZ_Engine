/// @file    TerrainSurfaceQuery.hpp
/// @brief   Authored Terrain 行を world 鉛直線で照会し、物理 collider の初期化へ依存しない。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <Engine/Api.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <cstddef>
#include <span>
namespace fbzz::scene {
struct TerrainComponent;
struct Transform;
struct TerrainSurfaceSample {
    math::Vector3 position;
    math::Vector3 normal;
};
struct TerrainHeightOverride {
    std::size_t index;
    float normalizedHeight;
};
/// @return TRS の逆転置による単位法線。非可逆 scale・非有限法線では false、out 未変更。
/// @see https://pbr-book.org/4ed/Geometry_and_Transformations/Applying_Transformations#Normals PBRT 3.10.3 Normals。
FBZZ_ENGINE_API bool TryTerrainNormalToWorld(const Transform& transform, const math::Vector3& localNormal, math::Vector3& out);
/// @return 有限 worldXZ の鉛直線と地形三角形の最上交点。欠損・穴・範囲外・非可逆/無効形状では false、out 未変更。
/// @note 地形が所有する追加面も含める。高さ場では滑らかな法線、追加三角形では面法線を使う。
/// @note 法線はワールド TRS の逆転置で変換する。物理の初期化は不要。
/// @note 高さ場の最上面探索は交差する格子を訪れ、追加面の探索は制作時の三角形を走査する。
/// @see https://pbr-book.org/4ed/Geometry_and_Transformations/Applying_Transformations#Normals PBRT 3.10.3 Normals。
FBZZ_ENGINE_API bool TrySampleTerrainWorld(const TerrainComponent& terrain, const Transform& transform,
    float worldX, float worldZ, TerrainSurfaceSample& out);
/// @note 上書きは一意の頂点添字順。穴と追加面には制作時の形状を使う。
/// @note 高さのプレビューが使うのは位置だけ。戻り値の法線は制作時の地形から求める。
FBZZ_ENGINE_API bool TrySampleTerrainWorld(const TerrainComponent& terrain, const Transform& transform,
    float worldX, float worldZ, TerrainSurfaceSample& out, std::span<const TerrainHeightOverride> overrides);
/// @return 基準より下かつ maxDistance [ワールド m] 以内で最も近い上向きの床。失敗時は false で out は未変更。
/// @note 天井と基準より上の外側地面は除外する。基準が床面そのものなら maxDistance=0 でも成功する。
FBZZ_ENGINE_API bool TrySampleTerrainFloorWorld(const TerrainComponent& terrain, const Transform& transform,
    const math::Vector3& reference, float maxDistance, TerrainSurfaceSample& out);
/// @return 基準より上かつ maxDistance 以内で最も近い下向きの天井。失敗時は false で out は未変更。
FBZZ_ENGINE_API bool TrySampleTerrainCeilingWorld(const TerrainComponent& terrain, const Transform& transform,
    const math::Vector3& reference, float maxDistance, TerrainSurfaceSample& out);
}
