/// @file    TerrainSurfaceQuery.hpp
/// @brief   Authored Terrain 行を world 鉛直線で照会し、物理 collider の初期化へ依存しない。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <Engine/Api.hpp>
#include <Math/Vector3.hpp>
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
/// @note Includes terrain-owned surfaces. Heightfield normals are smooth; additional triangles use their face normals.
/// @note Normal uses the world TRS inverse transpose. Physics initialization is not required.
/// @note Topmost heightfield lookup visits intersected grid cells; added surface lookup scans the authored triangles.
/// @see https://pbr-book.org/4ed/Geometry_and_Transformations/Applying_Transformations#Normals PBRT 3.10.3 Normals。
FBZZ_ENGINE_API bool TrySampleTerrainWorld(const TerrainComponent& terrain, const Transform& transform,
    float worldX, float worldZ, TerrainSurfaceSample& out);
/// @note Overrides are sorted by unique vertex index. Holes and added surfaces use the authored geometry.
/// @note Only position is used by height previews. The returned normal still comes from the authored terrain.
FBZZ_ENGINE_API bool TrySampleTerrainWorld(const TerrainComponent& terrain, const Transform& transform,
    float worldX, float worldZ, TerrainSurfaceSample& out, std::span<const TerrainHeightOverride> overrides);
/// @return Nearest upward-facing floor below reference, within maxDistance world metres; false leaves out unchanged.
/// @note Ceiling and exterior ground above reference are excluded. At the floor itself maxDistance=0 succeeds.
FBZZ_ENGINE_API bool TrySampleTerrainFloorWorld(const TerrainComponent& terrain, const Transform& transform,
    const math::Vector3& reference, float maxDistance, TerrainSurfaceSample& out);
/// @return Nearest downward-facing ceiling above reference, within maxDistance; false leaves out unchanged.
FBZZ_ENGINE_API bool TrySampleTerrainCeilingWorld(const TerrainComponent& terrain, const Transform& transform,
    const math::Vector3& reference, float maxDistance, TerrainSurfaceSample& out);
} /// @note namespace fbzz::scene
