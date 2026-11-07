/// @file    WorldHlod.cpp
/// @brief   Cell-local proxy instances aggregate compatible lowest-LOD definitions without modifying their sources.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <Engine/Scene/WorldHlod.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace fbzz::scene {
namespace {
constexpr uint32_t MAX_ASSET_INSTANCES = 1048576;
constexpr double AXIS_TOLERANCE = 0.00001;
using PartKey = std::pair<std::string, std::string>;

struct GroupKey {
    WorldCellId cell;
    uint32_t layer = 0;
    bool castShadows = true;
    std::vector<PartKey> parts;
    bool operator<(const GroupKey& other) const
    {
        return std::tie(cell, layer, castShadows, parts)
            < std::tie(other.cell, other.layer, other.castShadows, other.parts);
    }
};

struct ProxyPlacement {
    renderer::InstancePlacement placement;
    WorldHlodPlacementSource source;
};

struct StagedGroup {
    math::Vector3 origin;
    WorldCellBounds bounds;
    bool initializedBounds = false;
    bool completeBounds = true;
    std::vector<ProxyPlacement> placements;
};

bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool Finite(const math::Matrix4& value)
{
    for (const auto& row : value.m) for (float element : row)
        if (!std::isfinite(element)) return false;
    return true;
}

bool Fail(std::string& diagnostic, std::string message)
{
    diagnostic = std::move(message);
    return false;
}

bool PositiveScale(const math::Vector3& value)
{
    return Finite(value) && value.x >= 0.00001f && value.y >= 0.00001f && value.z >= 0.00001f;
}

bool Decompose(const math::Matrix4& matrix, renderer::InstancePlacement& out)
{
    if (!Finite(matrix) || matrix.m[3][0] != 0.0f || matrix.m[3][1] != 0.0f
        || matrix.m[3][2] != 0.0f || matrix.m[3][3] != 1.0f) return false;
    double axes[3][3]{};
    double scales[3]{};
    for (size_t column = 0; column < 3; ++column) {
        for (size_t row = 0; row < 3; ++row) {
            axes[column][row] = matrix.m[row][column];
            scales[column] += axes[column][row] * axes[column][row];
        }
        scales[column] = std::sqrt(scales[column]);
        if (!std::isfinite(scales[column]) || scales[column] < 0.00001
            || scales[column] > std::numeric_limits<float>::max()) return false;
        for (double& value : axes[column]) value /= scales[column];
    }
    for (size_t a = 0; a < 3; ++a) for (size_t b = a + 1; b < 3; ++b) {
        double dot = 0.0;
        for (size_t row = 0; row < 3; ++row) dot += axes[a][row] * axes[b][row];
        if (std::abs(dot) > AXIS_TOLERANCE) return false;
    }
    const double determinant = axes[0][0] * (axes[1][1] * axes[2][2] - axes[1][2] * axes[2][1])
        - axes[1][0] * (axes[0][1] * axes[2][2] - axes[0][2] * axes[2][1])
        + axes[2][0] * (axes[0][1] * axes[1][2] - axes[0][2] * axes[1][1]);
    if (determinant <= 0.0 || std::abs(determinant - 1.0) > AXIS_TOLERANCE * 3.0) return false;
    /// @note Quaternion::FromEuler uses YXZ; the singular branch fixes roll at zero while preserving the combined yaw.
    const double cosinePitch = std::hypot(axes[2][0], axes[2][2]);
    const double pitch = std::atan2(-axes[2][1], cosinePitch);
    const double yaw = cosinePitch > 0.000001
        ? std::atan2(axes[2][0], axes[2][2]) : std::atan2(-axes[0][2], axes[0][0]);
    const double roll = cosinePitch > 0.000001 ? std::atan2(axes[0][1], axes[1][1]) : 0.0;
    renderer::InstancePlacement result = out;
    result.position = {matrix.m[0][3], matrix.m[1][3], matrix.m[2][3]};
    result.eulerRadians = {static_cast<float>(pitch), static_cast<float>(yaw), static_cast<float>(roll)};
    result.scale = {static_cast<float>(scales[0]), static_cast<float>(scales[1]), static_cast<float>(scales[2])};
    const auto rebuilt = math::Matrix4::TRS(result.position,
        math::Quaternion::FromEuler(result.eulerRadians), result.scale);
    for (size_t column = 0; column < 3; ++column) for (size_t row = 0; row < 3; ++row)
        if (std::abs(static_cast<double>(rebuilt.m[row][column]) - matrix.m[row][column])
            > scales[column] * 0.00003) return false;
    out = result;
    return true;
}

bool LowestParts(const WorldHlodSource& source, std::vector<PartKey>& out)
{
    if (source.parts.empty() || source.parts.size() > 64 || source.lodHeights.empty()
        || source.lodHeights.size() > 4) return false;
    float previous = 2.0f;
    for (float height : source.lodHeights) {
        if (!std::isfinite(height) || height < 0.0f || height > 1.0f || height >= previous) return false;
        previous = height;
    }
    std::array<bool, 4> present{};
    uint32_t lowest = 0;
    for (const auto& part : source.parts) {
        if (part.meshPath.empty() || part.materialPath.empty() || part.lod >= source.lodHeights.size()) return false;
        present[part.lod] = true;
        lowest = (std::max)(lowest, part.lod);
    }
    for (size_t i = 0; i < source.lodHeights.size(); ++i) if (!present[i]) return false;
    /// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition---hierarchical-level-of-detail-in-unreal-engine HLOD Layers, Instancing retains the lowest mesh LOD in cell proxy instance components.
    for (const auto& part : source.parts) if (part.lod == lowest)
        out.emplace_back(part.meshPath, part.materialPath);
    std::sort(out.begin(), out.end());
    return !out.empty();
}

bool ValidBounds(const WorldCellBounds& bounds)
{
    return Finite(bounds.min) && Finite(bounds.max) && bounds.min.x <= bounds.max.x
        && bounds.min.y <= bounds.max.y && bounds.min.z <= bounds.max.z;
}

bool IncludeBounds(const WorldCellBounds& local, const math::Matrix4& world, StagedGroup& group)
{
    for (unsigned corner = 0; corner < 8; ++corner) {
        const double position[]{corner & 1 ? local.max.x : local.min.x,
            corner & 2 ? local.max.y : local.min.y, corner & 4 ? local.max.z : local.min.z};
        float low[3]{}, high[3]{};
        for (size_t row = 0; row < 3; ++row) {
            double value = world.m[row][3];
            double magnitude = std::abs(value);
            for (size_t column = 0; column < 3; ++column) {
                const double term = static_cast<double>(world.m[row][column]) * position[column];
                value += term;
                magnitude += std::abs(term);
            }
            /// @note The arithmetic margin and outward float rounding keep both authored and reconstituted proxy corners enclosed.
            const double margin = (magnitude + 1.0) * 8.0 * std::numeric_limits<float>::epsilon();
            const double minimum = value - margin, maximum = value + margin;
            low[row] = static_cast<float>(minimum);
            high[row] = static_cast<float>(maximum);
            if (!std::isfinite(low[row]) || !std::isfinite(high[row])) return false;
            if (static_cast<double>(low[row]) > minimum)
                low[row] = std::nextafter(low[row], -std::numeric_limits<float>::infinity());
            if (static_cast<double>(high[row]) < maximum)
                high[row] = std::nextafter(high[row], std::numeric_limits<float>::infinity());
            if (!std::isfinite(low[row]) || !std::isfinite(high[row])) return false;
        }
        const math::Vector3 minimum{low[0], low[1], low[2]}, maximum{high[0], high[1], high[2]};
        if (!group.initializedBounds) {
            group.bounds = {minimum, maximum};
            group.initializedBounds = true;
        } else {
            group.bounds.min = {(std::min)(group.bounds.min.x, minimum.x),
                (std::min)(group.bounds.min.y, minimum.y), (std::min)(group.bounds.min.z, minimum.z)};
            group.bounds.max = {(std::max)(group.bounds.max.x, maximum.x),
                (std::max)(group.bounds.max.y, maximum.y), (std::max)(group.bounds.max.z, maximum.z)};
        }
    }
    return true;
}
} /// @note namespace

bool BuildWorldHlod(std::span<const WorldHlodSource> sources, const WorldHlodBuildSettings& settings,
    std::vector<WorldHlodGroup>& out, std::string& diagnostic)
{
    if (!ValidateWorldCellGrid(settings.grid, diagnostic)) return false;
    if (settings.maxGroups == 0 || settings.maxInstances == 0)
        return Fail(diagnostic, "HLOD group and instance budgets must be positive");
    std::map<GroupKey, StagedGroup> groups;
    std::set<std::string> sourceKeys;
    uint64_t total = 0;
    for (const auto& source : sources) {
        if (!source.includeHlod) continue;
        if (source.sourceKey.empty() || !sourceKeys.insert(source.sourceKey).second)
            return Fail(diagnostic, "HLOD source keys must be nonempty and unique");
        std::vector<PartKey> parts;
        if (!LowestParts(source, parts)) return Fail(diagnostic, "HLOD source has invalid mesh parts or LOD thresholds");
        renderer::InstancePlacement owner;
        if (!Decompose(source.worldTransform, owner))
            return Fail(diagnostic, "HLOD owner transform must be a finite positive-scale affine TRS without shear");
        if (source.hasLocalBounds && !ValidBounds(source.localBounds))
            return Fail(diagnostic, "HLOD source bounds are invalid");
        std::set<uint32_t> ids;
        for (const auto& placement : source.placements) {
            if (!placement.active) continue;
            if (placement.id == 0 || placement.id > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())
                || !ids.insert(placement.id).second || !Finite(placement.position)
                || !Finite(placement.eulerRadians) || !PositiveScale(placement.scale))
                return Fail(diagnostic, "HLOD source has invalid placement IDs or transforms");
            if (++total > settings.maxInstances) return Fail(diagnostic, "HLOD instance budget exceeded");
            const auto world = source.worldTransform * math::Matrix4::TRS(placement.position,
                math::Quaternion::FromEuler(placement.eulerRadians), placement.scale);
            if (!Finite(world)) return Fail(diagnostic, "HLOD composed transform is not finite");
            const math::Vector3 position{world.m[0][3], world.m[1][3], world.m[2][3]};
            WorldCellId cell;
            if (!TryLocateWorldCell(settings.grid, position, cell))
                return Fail(diagnostic, "HLOD placement lies outside the representable world grid");
            const auto origin = WorldCellOrigin(settings.grid, cell);
            if (!Finite(origin)) return Fail(diagnostic, "HLOD cell origin is not finite");
            auto relative = world;
            relative.m[0][3] = static_cast<float>(static_cast<double>(position.x) - origin.x);
            relative.m[1][3] = static_cast<float>(static_cast<double>(position.y) - origin.y);
            relative.m[2][3] = static_cast<float>(static_cast<double>(position.z) - origin.z);
            renderer::InstancePlacement proxyPlacement;
            if (!Decompose(relative, proxyPlacement))
                return Fail(diagnostic, "HLOD composed transform cannot be represented by a positive-scale TRS");
            GroupKey key{cell, source.layer, source.castShadows, parts};
            auto found = groups.find(key);
            if (found == groups.end()) {
                if (groups.size() >= settings.maxGroups) return Fail(diagnostic, "HLOD group budget exceeded");
                found = groups.emplace(std::move(key), StagedGroup{}).first;
                found->second.origin = origin;
            }
            auto& group = found->second;
            if (group.placements.size() >= MAX_ASSET_INSTANCES)
                return Fail(diagnostic, "HLOD proxy asset instance capacity exceeded");
            if (source.hasLocalBounds) {
                const auto proxyWorld = math::Matrix4::Translate(origin) * math::Matrix4::TRS(proxyPlacement.position,
                    math::Quaternion::FromEuler(proxyPlacement.eulerRadians), proxyPlacement.scale);
                if (!Finite(proxyWorld) || !IncludeBounds(source.localBounds, world, group)
                    || !IncludeBounds(source.localBounds, proxyWorld, group))
                    return Fail(diagnostic, "HLOD world bounds exceed the supported finite range");
            } else group.completeBounds = false;
            group.placements.push_back({proxyPlacement, {source.sourceKey, placement.id}});
        }
    }
    std::vector<WorldHlodGroup> result;
    result.reserve(groups.size());
    const std::array<float, 1> heights{0.0f};
    for (auto& [key, staged] : groups) {
        std::sort(staged.placements.begin(), staged.placements.end(), [](const auto& first, const auto& second) {
            return std::tie(first.source.sourceKey, first.source.sourceId)
                < std::tie(second.source.sourceKey, second.source.sourceId);
        });
        WorldHlodGroup group;
        group.cell = key.cell;
        group.origin = staged.origin;
        group.layer = key.layer;
        group.castShadows = key.castShadows;
        group.worldBounds = staged.bounds;
        group.hasWorldBounds = staged.completeBounds && staged.initializedBounds;
        std::vector<asset::InstanceMeshPart> definition;
        definition.reserve(key.parts.size());
        for (const auto& part : key.parts) definition.push_back({part.first, part.second, 0});
        std::vector<renderer::InstancePlacement> placements;
        placements.reserve(staged.placements.size());
        group.sources.reserve(staged.placements.size());
        for (auto& value : staged.placements) {
            value.placement.id = static_cast<uint32_t>(placements.size() + 1);
            placements.push_back(value.placement);
            group.sources.push_back(std::move(value.source));
        }
        if (!group.proxy.instances.Replace(definition, heights, placements))
            return Fail(diagnostic, "HLOD proxy definition failed instance asset validation");
        result.push_back(std::move(group));
    }
    out = std::move(result);
    diagnostic.clear();
    return true;
}
} /// @note namespace fbzz::scene
