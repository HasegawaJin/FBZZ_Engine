/// @file    TerrainScatter.cpp
/// @brief   標高・傾斜・層で絞った配置を決定的な格子から一括生成する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include "TerrainScatter.hpp"
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace fbzz::editor {
namespace terrain_scatter_detail {
constexpr uint32_t MAX_CANDIDATES = 1000000;

bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool ValidTransform(const scene::Transform& transform)
{
    const auto scale = transform.worldScale;
    const float length = transform.worldRotation.Length();
    return Finite(transform.worldPosition) && Finite(scale)
        && std::abs(scale.x) >= 0.00001f && std::abs(scale.y) >= 0.00001f && std::abs(scale.z) >= 0.00001f
        && std::isfinite(length) && std::abs(length - 1.0f) <= 0.001f;
}

uint32_t Mix(uint32_t value)
{
    value ^= value >> 16; value *= 0x7feb352du;
    value ^= value >> 15; value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float UnitRandom(uint32_t key, uint32_t channel)
{
    return static_cast<float>(Mix(key ^ (channel * 0x9e3779b9u)) >> 8) / 16777216.0f;
}

math::Quaternion PlacementRotation(const TerrainScatterSettings& settings, const math::Vector3& normal, float sample)
{
    if (settings.alignDownslope) {
        const auto downhill = normal * normal.y - math::Vector3::UP;
        /// @note ほぼ平坦な場所では勾配の方向が安定しないため、通常の yaw に戻す。
        if (downhill.LengthSq() > 0.000001f) {
            const float yaw = math::ToRad(std::lerp(-settings.downslopeJitter, settings.downslopeJitter, sample));
            return math::Quaternion::LookRotation(downhill.Normalized(), normal)
                * math::Quaternion::FromAxisAngle(math::Vector3::UP, yaw);
        }
    }
    const float yaw = math::ToRad(std::lerp(settings.minYaw, settings.maxYaw, sample));
    const auto rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, yaw);
    if (!settings.alignToNormal && !settings.alignDownslope) return rotation;
    const auto axis = math::Vector3::Cross(math::Vector3::UP, normal);
    return math::Quaternion{axis.x, axis.y, axis.z, 1.0f + normal.y}.Normalized() * rotation;
}

/// @note Quaternion::FromEuler と同じ YXZ 順。特異点では Z=0 として同じ姿勢を表す。
math::Vector3 EulerRadians(const math::Quaternion& rotation)
{
    const auto matrix = math::Matrix4::Rotate(rotation);
    const float pitch = std::asin(std::clamp(-matrix.m[1][2], -1.0f, 1.0f));
    if (std::abs(std::cos(pitch)) <= 0.000001f)
        return {pitch, std::atan2(-matrix.m[2][0], matrix.m[0][0]), 0.0f};
    return {pitch, std::atan2(matrix.m[0][2], matrix.m[2][2]), std::atan2(matrix.m[1][0], matrix.m[1][1])};
}

float LayerWeight(const scene::TerrainComponent& terrain, float x, float z, int layer)
{
    const float gx = x / terrain.cellSize, gz = z / terrain.cellSize;
    const int ix = std::clamp(static_cast<int>(gx), 0, terrain.columns - 2);
    const int iz = std::clamp(static_cast<int>(gz), 0, terrain.rows - 2);
    const float fx = std::clamp(gx - static_cast<float>(ix), 0.0f, 1.0f);
    const float fz = std::clamp(gz - static_cast<float>(iz), 0.0f, 1.0f);
    return std::lerp(std::lerp(terrain.GetLayerWeightAtGrid(ix, iz, layer), terrain.GetLayerWeightAtGrid(ix + 1, iz, layer), fx),
        std::lerp(terrain.GetLayerWeightAtGrid(ix, iz + 1, layer), terrain.GetLayerWeightAtGrid(ix + 1, iz + 1, layer), fx), fz);
}

/// @note ブラシの距離は高さ差を含めず、地形軸に沿った world m で測る。
struct ScatterPlane {
    const scene::Transform& transform;
    math::Quaternion inverseRotation;

    math::Vector3 Project(const math::Vector3& world) const
    {
        const auto point = inverseRotation * (world - transform.worldPosition);
        return {point.x * std::copysign(1.0f, transform.worldScale.x), 0.0f,
            point.z * std::copysign(1.0f, transform.worldScale.z)};
    }
};
} /// @note namespace terrain_scatter_detail

bool BuildTerrainScatter(const scene::TerrainComponent& terrain,
    const scene::Transform& terrainTransform, const scene::Transform& targetTransform,
    const TerrainScatterSettings& settings, std::vector<renderer::InstancePlacement>& placements,
    std::string& reason, const TerrainScatterBrush* brush)
{
    using namespace terrain_scatter_detail;
    const auto fail = [&](const char* text) { reason = text; return false; };
    const float finiteValues[] = {settings.spacing, settings.density, settings.minHeight, settings.maxHeight,
        settings.minSlope, settings.maxSlope, settings.layerThreshold, settings.minScale, settings.maxScale,
        settings.minYaw, settings.maxYaw, settings.downslopeJitter, settings.embedDepth};
    for (float value : finiteValues) if (!std::isfinite(value)) return fail("配置条件には有限の数値を指定してください。");
    if (settings.spacing < 0.25f || settings.spacing > 10000.0f || settings.density < 0.0f || settings.density > 1.0f
        || settings.minHeight > settings.maxHeight || settings.minSlope < 0.0f || settings.maxSlope > 90.0f
        || settings.minSlope > settings.maxSlope || settings.layer < -1 || settings.layer >= terrain.LayerCount()
        || settings.layerThreshold < 0.0f || settings.layerThreshold > 1.0f || settings.minScale < 0.01f
        || settings.maxScale > 100.0f || settings.minScale > settings.maxScale || settings.minYaw < -360.0f
        || settings.maxYaw > 360.0f || settings.minYaw > settings.maxYaw
        || settings.downslopeJitter < 0.0f || settings.downslopeJitter > 180.0f
        || settings.embedDepth < 0.0f || settings.embedDepth > 100.0f
        || settings.maxInstances == 0 || settings.maxInstances > MAX_CANDIDATES)
        return fail("配置条件の範囲または上下限が不正です。");
    if (!ValidTransform(terrainTransform) || !ValidTransform(targetTransform))
        return fail("地形または配置先の Transform が非有限・非可逆です。");
    const auto targetScale = targetTransform.worldScale;
    if (targetScale.x <= 0.0f || std::abs(targetScale.y - targetScale.x) > targetScale.x * 0.00001f
        || std::abs(targetScale.z - targetScale.x) > targetScale.x * 0.00001f
        || settings.minScale / targetScale.x < 0.00001f)
        return fail("配置先の world scale は正の一様 scale にしてください。");
    if (terrain.columns < 2 || terrain.rows < 2 || terrain.columns > 4097 || terrain.rows > 4097
        || terrain.heightData.size() != terrain.VertexCount() || !std::isfinite(terrain.cellSize) || terrain.cellSize <= 0.0f
        || !std::isfinite(terrain.maxHeight) || (!terrain.holeData.empty() && terrain.holeData.size() != terrain.CellCount())
        || ((!brush || !brush->erase) && settings.layer >= 0 && !terrain.HasValidSplat()))
        return fail("地形の高さ・穴・層データが不正です。");
    if (!brush) {
        for (float value : terrain.heightData)
            if (!std::isfinite(value) || !std::isfinite(value * terrain.maxHeight))
                return fail("地形の高さに非有限値があります。");
    }

    const ScatterPlane plane{terrainTransform, terrainTransform.worldRotation.Inverse()};
    std::vector<math::Vector3> centers;
    if (brush) {
        if (brush->centers.empty() || brush->centers.size() > TERRAIN_SCATTER_STROKE_LIMIT
            || !std::isfinite(brush->radius) || brush->radius < 0.25f || brush->radius > 500.0f)
            return fail("ブラシは 1〜512 点、半径 0.25〜500 m で指定してください。");
        centers.reserve(brush->centers.size());
        for (const auto& point : brush->centers) {
            const auto center = plane.Project(point);
            if (!Finite(point) || !Finite(center)) return fail("ブラシの座標が不正です。");
            centers.push_back(center);
        }
        if (brush->erase) {
            const auto world = targetTransform.GetWorldMatrix();
            const float radiusSq = brush->radius * brush->radius;
            std::erase_if(placements, [&](const renderer::InstancePlacement& placement) {
                const auto point = world * math::Vector4{placement.position.x, placement.position.y, placement.position.z, 1.0f};
                const auto projected = plane.Project({point.x, point.y, point.z});
                return std::any_of(centers.begin(), centers.end(), [&](const math::Vector3& center) {
                    return (projected - center).LengthSq() <= radiusSq;
                });
            });
            reason.clear();
            return true;
        }
    }
    const double width = static_cast<double>(terrain.columns - 1) * terrain.cellSize;
    const double depth = static_cast<double>(terrain.rows - 1) * terrain.cellSize;
    const double stepX = settings.spacing / std::abs(static_cast<double>(terrainTransform.worldScale.x));
    const double stepZ = settings.spacing / std::abs(static_cast<double>(terrainTransform.worldScale.z));
    const double columns = std::ceil(width / stepX), rows = std::ceil(depth / stepZ);
    if (!std::isfinite(columns) || !std::isfinite(rows) || columns < 1.0 || rows < 1.0
        || columns > std::numeric_limits<uint32_t>::max() || rows > std::numeric_limits<uint32_t>::max()
        || (!brush && columns * rows > MAX_CANDIDATES)
        || width > std::numeric_limits<float>::max() || depth > std::numeric_limits<float>::max())
        return fail("配置候補が 100 万件を超えます。間隔を広げてください。");
    const uint32_t nx = static_cast<uint32_t>(columns), nz = static_cast<uint32_t>(rows);
    const auto worldMatrix = terrainTransform.GetWorldMatrix();
    const auto targetInverseRotation = targetTransform.worldRotation.Inverse();
    std::vector<renderer::InstancePlacement> staged;
    std::unordered_set<uint64_t> occupied;
    std::unordered_set<uint32_t> usedIds;
    uint32_t nextId = 1;
    const auto cellAt = [&](const math::Vector3& world) -> uint64_t {
        const auto p = plane.Project(world);
        const double x = std::floor(static_cast<double>(p.x) / settings.spacing);
        const double z = std::floor(static_cast<double>(p.z) / settings.spacing);
        return x >= 0.0 && x < columns && z >= 0.0 && z < rows
            ? static_cast<uint64_t>(z) * nx + static_cast<uint64_t>(x) + 1 : 0;
    };
    if (brush) {
        const auto targetWorld = targetTransform.GetWorldMatrix();
        staged = placements;
        for (const auto& placement : staged) {
            const auto point = targetWorld * math::Vector4{placement.position.x, placement.position.y, placement.position.z, 1.0f};
            const auto cell = cellAt({point.x, point.y, point.z});
            if (cell != 0) occupied.insert(cell);
            usedIds.insert(placement.id);
        }
    } else {
        staged.reserve(std::min(nx * nz, settings.maxInstances));
    }
    const auto sample = [&](uint32_t x, uint32_t z) {
        const uint64_t id = static_cast<uint64_t>(z) * nx + x + 1;
        const uint32_t key = Mix(static_cast<uint32_t>(id) ^ static_cast<uint32_t>(id >> 32)) ^ Mix(settings.seed);
        const double startX = x * stepX, startZ = z * stepZ;
        const float localX = static_cast<float>(startX + std::min(stepX, width - startX) * (0.15 + 0.7 * UnitRandom(key, 1)));
        const float localZ = static_cast<float>(startZ + std::min(stepZ, depth - startZ) * (0.15 + 0.7 * UnitRandom(key, 2)));
        return math::Vector3{localX, 0.0f, localZ};
    };
    const auto place = [&](uint32_t x, uint32_t z) {
            const uint64_t id = static_cast<uint64_t>(z) * nx + x + 1;
            const uint32_t key = Mix(static_cast<uint32_t>(id) ^ static_cast<uint32_t>(id >> 32)) ^ Mix(settings.seed);
            const auto local = sample(x, z);
            const float localX = local.x, localZ = local.z;
            if (terrain.IsHoleAtLocal(localX, localZ)) return true;
            const float weight = settings.layer < 0 ? 1.0f : LayerWeight(terrain, localX, localZ, settings.layer);
            if (weight <= 0.0f || weight < settings.layerThreshold || UnitRandom(key, 3) >= settings.density * weight) return true;
            const float height = terrain.GetHeightAt(localX, localZ);
            const auto point = worldMatrix * math::Vector4{localX, height, localZ, 1.0f};
            const math::Vector3 worldPosition{point.x, point.y, point.z};
            math::Vector3 worldNormal;
            if (!Finite(worldPosition) || !scene::TryTerrainNormalToWorld(terrainTransform, terrain.GetNormalAt(localX, localZ), worldNormal))
                return fail("配置点の座標または法線を有限の値で表せません。");
            const float slope = math::ToDeg(std::acos(std::clamp(worldNormal.y, -1.0f, 1.0f)));
            if (worldPosition.y < settings.minHeight || worldPosition.y > settings.maxHeight
                || slope < settings.minSlope || slope > settings.maxSlope) return true;
            const auto embeddedPosition = worldPosition - worldNormal * settings.embedDepth;
            if (brush) {
                const auto cell = cellAt(embeddedPosition);
                if (occupied.contains(id) || (cell && !occupied.insert(cell).second)) return true;
            }
            if (staged.size() >= settings.maxInstances) return fail("配置数が上限を超えます。密度を下げるか上限を増やしてください。");
            const auto rotation = PlacementRotation(settings, worldNormal, UnitRandom(key, 4));
            const float scale = std::lerp(settings.minScale, settings.maxScale, UnitRandom(key, 5)) / targetScale.x;
            uint32_t instanceId = static_cast<uint32_t>(id);
            if (brush && (id > static_cast<uint64_t>(std::numeric_limits<int>::max()) || !usedIds.insert(instanceId).second)) {
                while (!usedIds.insert(nextId).second) ++nextId;
                instanceId = nextId++;
            }
            renderer::InstancePlacement placement{instanceId,
                (targetInverseRotation * (embeddedPosition - targetTransform.worldPosition)) / targetScale.x,
                EulerRadians((targetInverseRotation * rotation).Normalized()), {scale, scale, scale}, true};
            if (!Finite(placement.position) || !Finite(placement.eulerRadians) || !Finite(placement.scale))
                return fail("配置先のローカル変換を有限の値で表せません。");
            staged.push_back(placement);
            return true;
    };
    if (brush && settings.density > 0.0f) {
        std::unordered_set<uint64_t> covered;
        uint64_t visits = 0;
        const auto bound = [&](double value, double limit) {
            return static_cast<uint64_t>(std::clamp(std::floor(value / settings.spacing), 0.0, limit - 1.0));
        };
        const float radiusSq = brush->radius * brush->radius;
        const float scaleX = std::abs(terrainTransform.worldScale.x), scaleZ = std::abs(terrainTransform.worldScale.z);
        for (const auto& center : centers) {
            if (center.x + brush->radius < 0.0f || center.z + brush->radius < 0.0f
                || center.x - brush->radius > width * scaleX || center.z - brush->radius > depth * scaleZ) continue;
            const auto x0 = bound(static_cast<double>(center.x) - brush->radius, columns);
            const auto x1 = bound(static_cast<double>(center.x) + brush->radius, columns);
            const auto z0 = bound(static_cast<double>(center.z) - brush->radius, rows);
            const auto z1 = bound(static_cast<double>(center.z) + brush->radius, rows);
            const auto columnCount = x1 - x0 + 1, rowCount = z1 - z0 + 1;
            if (columnCount > (MAX_CANDIDATES - visits) / rowCount)
                return fail("ブラシ候補が 100 万件を超えます。半径またはストロークを小さくしてください。");
            visits += columnCount * rowCount;
            for (uint64_t z = z0; z <= z1; ++z) {
                for (uint64_t x = x0; x <= x1; ++x) {
                    const auto local = sample(static_cast<uint32_t>(x), static_cast<uint32_t>(z));
                    const math::Vector3 point{local.x * scaleX, 0.0f, local.z * scaleZ};
                    if ((point - center).LengthSq() <= radiusSq) covered.insert(z * nx + x);
                }
            }
        }
        std::vector<uint64_t> order(covered.begin(), covered.end());
        std::sort(order.begin(), order.end());
        for (uint64_t cell : order)
            if (!place(static_cast<uint32_t>(cell % nx), static_cast<uint32_t>(cell / nx))) return false;
    } else if (!brush) {
        for (uint32_t z = 0; z < nz && settings.density > 0.0f; ++z) {
            for (uint32_t x = 0; x < nx; ++x)
                if (!place(x, z)) return false;
        }
    }
    placements = std::move(staged);
    reason.clear();
    return true;
}
} /// @note namespace fbzz::editor
