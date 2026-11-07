/// @file    WorldCell.cpp
/// @brief   Validated grid ownership, cell distance and stable integer seeds.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <Engine/Scene/WorldCell.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace fbzz::scene {
namespace {
/// @see https://prng.di.unimi.it/splitmix.c Public-domain SplitMix64 finalizer by Sebastiano Vigna.
uint64_t Mix(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
bool Finite(const math::Vector3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
}

bool ValidateWorldCellGrid(const WorldCellGrid& grid, std::string& diagnostic)
{
    if (!Finite(grid.origin) || !std::isfinite(grid.cellSize) || grid.cellSize < 1.0f || grid.cellSize > 65536.0f) {
        diagnostic = "Cell origin must be finite and cellSize must be within [1,65536] metres.";
        return false;
    }
    diagnostic.clear();
    return true;
}

bool TryLocateWorldCell(const WorldCellGrid& grid, const math::Vector3& p, WorldCellId& out)
{
    std::string diagnostic;
    if (!ValidateWorldCellGrid(grid, diagnostic) || !Finite(p)) return false;
    const double x = std::floor((static_cast<double>(p.x) - grid.origin.x) / grid.cellSize);
    const double z = std::floor((static_cast<double>(p.z) - grid.origin.z) / grid.cellSize);
    if (x < (std::numeric_limits<int32_t>::min)() || x > (std::numeric_limits<int32_t>::max)()
        || z < (std::numeric_limits<int32_t>::min)() || z > (std::numeric_limits<int32_t>::max)()) return false;
    out = {static_cast<int32_t>(x), static_cast<int32_t>(z)};
    return true;
}

math::Vector3 WorldCellOrigin(const WorldCellGrid& grid, WorldCellId id)
{
    return {static_cast<float>(static_cast<double>(grid.origin.x) + static_cast<double>(id.x) * grid.cellSize),
        grid.origin.y, static_cast<float>(static_cast<double>(grid.origin.z) + static_cast<double>(id.z) * grid.cellSize)};
}

WorldCellBounds WorldCellFootprint(const WorldCellGrid& grid, WorldCellId id)
{
    const auto p = WorldCellOrigin(grid, id);
    return {p, {p.x + grid.cellSize, p.y, p.z + grid.cellSize}};
}

float WorldCellDistanceXZ(const WorldCellGrid& grid, WorldCellId id, const math::Vector3& source)
{
    const auto box = WorldCellFootprint(grid, id);
    const float dx = (std::max)({box.min.x - source.x, 0.0f, source.x - box.max.x});
    const float dz = (std::max)({box.min.z - source.z, 0.0f, source.z - box.max.z});
    return std::hypot(dx, dz);
}

uint64_t WorldCellSeed(uint64_t seed, WorldCellId id, uint64_t stream)
{
    const uint64_t coordinate = (static_cast<uint64_t>(static_cast<uint32_t>(id.x)) << 32)
        | static_cast<uint32_t>(id.z);
    const uint64_t seeded = Mix(seed ^ 0xa0761d6478bd642full);
    const uint64_t cell = Mix(seeded ^ coordinate);
    return Mix(cell ^ stream ^ 0xe7037ed1a0b428dbull);
}

std::string WorldCellName(WorldCellId id) { return std::to_string(id.x) + "_" + std::to_string(id.z); }

} /// @note namespace fbzz::scene
