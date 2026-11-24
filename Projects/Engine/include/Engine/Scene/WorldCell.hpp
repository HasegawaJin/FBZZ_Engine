/// @file    WorldCell.hpp
/// @brief   ワールド空間で共有するセル座標と決定的な生成キー。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Engine/Api.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <string>

namespace fbzz::scene {

/// @note 制作時の配置セルが使うワールド XZ の寸法。ストリーミングセルとは独立する。
inline constexpr float VEGETATION_BUDGET_CELL_SIZE = 128.0f;

struct WorldCellId {
    int32_t x = 0;
    int32_t z = 0;
    bool operator==(const WorldCellId&) const = default;
    bool operator<(const WorldCellId& other) const { return z == other.z ? x < other.x : z < other.z; }
};

struct WorldCellGrid {
    math::Vector3 origin{};
    float cellSize = 256.0f;
};

struct WorldCellBounds {
    math::Vector3 min{};
    math::Vector3 max{};
};

/// @note セルの所有範囲は XZ の半開区間。Y は所属判定に使わない。
[[nodiscard]] FBZZ_ENGINE_API bool ValidateWorldCellGrid(const WorldCellGrid& grid, std::string& diagnostic);
[[nodiscard]] FBZZ_ENGINE_API bool TryLocateWorldCell(const WorldCellGrid& grid, const math::Vector3& worldPosition, WorldCellId& out);
[[nodiscard]] FBZZ_ENGINE_API math::Vector3 WorldCellOrigin(const WorldCellGrid& grid, WorldCellId id);
[[nodiscard]] FBZZ_ENGINE_API WorldCellBounds WorldCellFootprint(const WorldCellGrid& grid, WorldCellId id);
[[nodiscard]] FBZZ_ENGINE_API float WorldCellDistanceXZ(const WorldCellGrid& grid, WorldCellId id, const math::Vector3& source);
/// @note 固定の整数ハッシュを用い、標準ライブラリの分布の実装差を避ける。
[[nodiscard]] FBZZ_ENGINE_API uint64_t WorldCellSeed(uint64_t seed, WorldCellId id, uint64_t stream = 0);
[[nodiscard]] FBZZ_ENGINE_API std::string WorldCellName(WorldCellId id);

}
