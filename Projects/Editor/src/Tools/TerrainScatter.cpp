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
#include <map>
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

const char* TerrainPaintReasonLabel(TerrainPaintReason reason)
{
    constexpr const char* labels[]{"Accepted", "Density / edge fade", "Terrain hole", "Height", "Slope", "Ground layer",
        "Water clearance", "Path clearance", "Existing object", "Minimum spacing", "Occupied cell"};
    const auto i = static_cast<size_t>(reason);
    return i < std::size(labels) ? labels[i] : "Unknown";
}

float TerrainRegionCoverage(const scene::Transform& terrain, const TerrainScatterBrush& brush, const math::Vector3& world)
{
    const auto inverse = terrain.worldRotation.Inverse();
    const auto project = [&](const math::Vector3& value) { auto p = inverse * (value - terrain.worldPosition); p.y = 0; return p; };
    const auto point = project(world);
    float edge = -INFINITY;
    if (brush.polygon) {
        if (brush.centers.size() < 3) return 0;
        bool inside = false; float nearest = INFINITY;
        for (size_t i = 0, j = brush.centers.size() - 1; i < brush.centers.size(); j = i++) {
            const auto a = project(brush.centers[j]), b = project(brush.centers[i]), delta = b - a;
            const float t = delta.LengthSq() > 0 ? std::clamp(math::Vector3::Dot(point - a, delta) / delta.LengthSq(), 0.0f, 1.0f) : 0;
            nearest = std::min(nearest, (point - (a + delta * t)).Length());
            if ((a.z > point.z) != (b.z > point.z) && point.x < (b.x - a.x) * (point.z - a.z) / (b.z - a.z) + a.x) inside = !inside;
        }
        if (!inside && nearest > 0.0001f) return 0;
        edge = nearest;
    } else {
        for (const auto& center : brush.centers) edge = std::max(edge, brush.radius - (point - project(center)).Length());
    }
    if (edge < 0) return 0;
    return brush.feather > 0 ? std::clamp(edge / (brush.radius * brush.feather), 0.0f, 1.0f) : 1.0f;
}

bool ValidateTerrainRegion(const scene::Transform& terrain, const TerrainScatterBrush& brush)
{
    using namespace terrain_scatter_detail;
    if (brush.centers.empty() || brush.centers.size() > TERRAIN_SCATTER_STROKE_LIMIT || !std::isfinite(brush.radius)
        || brush.radius < 0.25f || brush.radius > 500 || !std::isfinite(brush.feather) || brush.feather < 0 || brush.feather > 1) return false;
    for (const auto& p : brush.centers) if (!Finite(p)) return false;
    if (!brush.polygon) return true;
    if (brush.centers.size() < 3) return false;
    const auto inverse = terrain.worldRotation.Inverse();
    const auto project = [&](const auto& p) { return inverse * (p - terrain.worldPosition); };
    const auto cross = [](const auto& a, const auto& b, const auto& c) { return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x); };
    double area = 0;
    const size_t n = brush.centers.size();
    for (size_t i = 0; i < n; ++i) {
        const auto a = project(brush.centers[i]), b = project(brush.centers[(i + 1) % n]);
        if ((a - b).LengthSq() < 0.000001f) return false;
        area += static_cast<double>(a.x) * b.z - static_cast<double>(a.z) * b.x;
        for (size_t j = i + 2; j < n; ++j) {
            if (i == 0 && j == n - 1) continue;
            const auto c = project(brush.centers[j]), d = project(brush.centers[(j + 1) % n]);
            if (std::max(std::min(a.x,b.x),std::min(c.x,d.x)) > std::min(std::max(a.x,b.x),std::max(c.x,d.x))
                || std::max(std::min(a.z,b.z),std::min(c.z,d.z)) > std::min(std::max(a.z,b.z),std::max(c.z,d.z))) continue;
            if (cross(a,b,c) * cross(a,b,d) <= 0 && cross(c,d,a) * cross(c,d,b) <= 0) return false;
        }
    }
    return std::isfinite(area) && std::abs(area) > 0.0001;
}

bool BuildTerrainScatter(const scene::TerrainComponent& terrain,
    const scene::Transform& terrainTransform, const scene::Transform& targetTransform,
    const TerrainScatterSettings& settings, std::vector<renderer::InstancePlacement>& placements,
    std::string& reason, const TerrainScatterBrush* brush,
    const std::function<bool(const math::Vector3&)>& accepts, TerrainPaintDiagnostics* diagnostics,
    const std::function<TerrainPaintReason(const math::Vector3&)>& classify)
{
    using namespace terrain_scatter_detail;
    const auto fail = [&](const char* text) { reason = text; return false; };
    const float finiteValues[] = {settings.spacing, settings.density, settings.minHeight, settings.maxHeight,
        settings.minSlope, settings.maxSlope, settings.layerThreshold, settings.minScale, settings.maxScale,
        settings.minYaw, settings.maxYaw, settings.downslopeJitter, settings.embedDepth, settings.minimumDistance};
    for (float value : finiteValues) if (!std::isfinite(value)) return fail("配置条件には有限の数値を指定してください。");
    if (settings.spacing < 0.25f || settings.spacing > 10000.0f || settings.density < 0.0f || settings.density > 1.0f
        || settings.minHeight > settings.maxHeight || settings.minSlope < 0.0f || settings.maxSlope > 90.0f
        || settings.minSlope > settings.maxSlope || settings.layer < -1 || settings.layer >= terrain.LayerCount()
        || settings.layerThreshold < 0.0f || settings.layerThreshold > 1.0f || settings.minScale < 0.01f
        || settings.maxScale > 100.0f || settings.minScale > settings.maxScale || settings.minYaw < -360.0f
        || settings.maxYaw > 360.0f || settings.minYaw > settings.maxYaw
        || settings.downslopeJitter < 0.0f || settings.downslopeJitter > 180.0f
        || settings.embedDepth < 0.0f || settings.embedDepth > 100.0f
        || settings.minimumDistance < 0.0f || settings.minimumDistance > 10000.0f
        || settings.maxInstances == 0 || settings.maxInstances > MAX_CANDIDATES)
        return fail("配置条件の範囲または上下限が不正です。");
    if (!ValidTransform(terrainTransform) || !ValidTransform(targetTransform))
        return fail("地形または配置先の Transform が非有限・非可逆です。");
    const auto targetScale = targetTransform.worldScale;
    if ((!brush || !brush->erase) && (targetScale.x <= 0.0f || std::abs(targetScale.y - targetScale.x) > targetScale.x * 0.00001f
        || std::abs(targetScale.z - targetScale.x) > targetScale.x * 0.00001f
        || settings.minScale / targetScale.x < 0.00001f))
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
        if (!ValidateTerrainRegion(terrainTransform, *brush))
            return fail("ブラシは 1〜512 点、半径 0.25〜500 m で指定してください。");
        centers.reserve(brush->centers.size());
        for (const auto& point : brush->centers) {
            const auto center = plane.Project(point);
            if (!Finite(point) || !Finite(center)) return fail("ブラシの座標が不正です。");
            centers.push_back(center);
        }
        if (brush->erase) {
            const auto world = targetTransform.GetWorldMatrix();
            std::erase_if(placements, [&](const renderer::InstancePlacement& placement) {
                const auto point = world * math::Vector4{placement.position.x, placement.position.y, placement.position.z, 1.0f};
                return TerrainRegionCoverage(terrainTransform, *brush, {point.x, point.y, point.z}) > 0.0f;
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
    /// @note 地形平面上の近傍だけを調べ、大量の既存配置でも全件対比較を避ける。
    std::map<std::pair<int64_t, int64_t>, std::vector<math::Vector3>> neighbors;
    const auto neighborhood = [&](const math::Vector3& position) {
        const auto p = plane.Project(position);
        const double distance = std::max(settings.minimumDistance, 0.001f);
        return std::pair<int64_t, int64_t>{static_cast<int64_t>(std::clamp(std::floor(p.x / distance), -1.0e12, 1.0e12)),
            static_cast<int64_t>(std::clamp(std::floor(p.z / distance), -1.0e12, 1.0e12))};
    };
    const auto remember = [&](const math::Vector3& position) {
        if (settings.minimumDistance > 0.0f) neighbors[neighborhood(position)].push_back(plane.Project(position));
    };
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
            if (placement.active) remember({point.x, point.y, point.z});
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
            const float height = terrain.GetHeightAt(localX, localZ);
            const auto point = worldMatrix * math::Vector4{localX, height, localZ, 1.0f};
            const math::Vector3 worldPosition{point.x, point.y, point.z};
            const auto record = [&](TerrainPaintReason value) {
                if (diagnostics) {
                    ++diagnostics->counts[static_cast<size_t>(value)];
                    if (diagnostics->counts[static_cast<size_t>(value)] <= 64 && value != TerrainPaintReason::ACCEPTED)
                        diagnostics->points.push_back({worldPosition, value});
                }
                return true;
            };
            if (terrain.IsHoleAtLocal(localX, localZ)) return record(TerrainPaintReason::HOLE);
            const float weight = settings.layer < 0 ? 1.0f : LayerWeight(terrain, localX, localZ, settings.layer);
            if (weight <= 0.0f || weight < settings.layerThreshold) return record(TerrainPaintReason::LAYER);
            const float coverage = brush ? TerrainRegionCoverage(terrainTransform, *brush, worldPosition) : 1.0f;
            if (UnitRandom(key, 3) >= settings.density * weight * coverage) return record(TerrainPaintReason::DENSITY);
            math::Vector3 worldNormal;
            if (!Finite(worldPosition) || !scene::TryTerrainNormalToWorld(terrainTransform, terrain.GetNormalAt(localX, localZ), worldNormal))
                return fail("配置点の座標または法線を有限の値で表せません。");
            const float slope = math::ToDeg(std::acos(std::clamp(worldNormal.y, -1.0f, 1.0f)));
            if (worldPosition.y < settings.minHeight || worldPosition.y > settings.maxHeight) return record(TerrainPaintReason::HEIGHT);
            if (slope < settings.minSlope || slope > settings.maxSlope) return record(TerrainPaintReason::SLOPE);
            if (classify) { const auto rejected = classify(worldPosition); if (rejected != TerrainPaintReason::ACCEPTED) return record(rejected); }
            if (accepts && !accepts(worldPosition)) return record(TerrainPaintReason::OBSTACLE);
            if (settings.minimumDistance > 0.0f) {
                const auto cell = neighborhood(worldPosition);
                const auto projected = plane.Project(worldPosition);
                for (int64_t dz = -1; dz <= 1; ++dz) for (int64_t dx = -1; dx <= 1; ++dx) {
                    const auto found = neighbors.find({cell.first + dx, cell.second + dz});
                    if (found != neighbors.end()) for (const auto& other : found->second)
                        if ((projected - other).LengthSq() < settings.minimumDistance * settings.minimumDistance) return record(TerrainPaintReason::SPACING);
                }
            }
            const auto embeddedPosition = worldPosition - worldNormal * settings.embedDepth;
            if (brush) {
                const auto cell = cellAt(embeddedPosition);
                if (occupied.contains(id) || (cell && !occupied.insert(cell).second)) return record(TerrainPaintReason::OCCUPIED);
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
            record(TerrainPaintReason::ACCEPTED);
            staged.push_back(placement);
            remember(worldPosition);
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
        if (brush->polygon) {
            math::Vector3 lo{INFINITY,0,INFINITY}, hi{-INFINITY,0,-INFINITY};
            for (const auto& c : centers) { lo.x=std::min(lo.x,c.x);lo.z=std::min(lo.z,c.z);hi.x=std::max(hi.x,c.x);hi.z=std::max(hi.z,c.z); }
            const auto x0=bound(lo.x,columns),x1=bound(hi.x,columns),z0=bound(lo.z,rows),z1=bound(hi.z,rows);
            if ((x1-x0+1)*(z1-z0+1)>MAX_CANDIDATES) return fail("範囲の候補が100万件を超えます。範囲を分けてください。");
            for(uint64_t z=z0;z<=z1;++z) for(uint64_t x=x0;x<=x1;++x) {
                const auto p=sample(static_cast<uint32_t>(x),static_cast<uint32_t>(z));
                const auto w=worldMatrix*math::Vector4{p,1};
                if (TerrainRegionCoverage(terrainTransform,*brush,{w.x,w.y,w.z})>0) covered.insert(z*nx+x);
            }
        }
        for (const auto& center : centers) {
            if (brush->polygon) break;
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
