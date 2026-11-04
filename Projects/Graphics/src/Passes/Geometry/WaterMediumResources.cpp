/// @file    WaterMediumResources.cpp
/// @brief   Builds bounded per-view water-medium table and atlas resources.
/// @author  Hasegawa Jin
/// @date    2026-10-09
#include <Graphics/Passes/Geometry/WaterRenderPass.hpp>
#include <Graphics/Effects/RenderWaterInput.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Renderer/Format.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Core/Logger.hpp>
#include <Math/Frustum.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::renderer {
namespace {

constexpr uint32_t ATLAS_RESOLUTION = 1024u;
constexpr uint32_t MAXIMUM_TILE_RESOLUTION = 512u;
constexpr uint32_t MINIMUM_TILE_RESOLUTION = 64u;
constexpr uint32_t GRID_RESOLUTION = 16u;
constexpr uint32_t GRID_CELL_COUNT = GRID_RESOLUTION * GRID_RESOLUTION;
constexpr uint32_t MAXIMUM_BODIES = 256u;
constexpr uint32_t MAXIMUM_BOXES_PER_BODY = 64u;
constexpr uint32_t MAXIMUM_PLUMES_PER_BODY = 64u;
constexpr uint32_t MAXIMUM_TABLE_ROWS = 65536u;
constexpr uint32_t HEADER_ROWS = 5u;
constexpr uint32_t BODY_ROWS = 12u;
constexpr uint32_t EXCLUSION_ROWS = 4u;
constexpr uint32_t PLUME_ROWS = 2u;
constexpr float MINIMUM_BOUNDS_SPAN = 1.0e-5f;

struct BodyCandidate {
    uint32_t inputIndex = 0;
    uint64_t ownerKey = 0;
    int32_t priority = 0;
    const WaterMediumInput* medium = nullptr;
    math::Vector4 bounds{};
    math::Vector3 queryMinimum{};
    math::Vector3 queryMaximum{};
    std::vector<uint32_t> exclusionIndices;
    std::vector<uint32_t> plumeIndices;
    uint32_t cellMinX = 0;
    uint32_t cellMinZ = 0;
    uint32_t cellMaxX = 0;
    uint32_t cellMaxZ = 0;
    bool variableDepth = false;
    float depthQuantization = 0.0f;
    bool horizon = false;
    float certifiedDisplacement = 0.0f;
    float physicalMinimumY = 0.0f;
    float physicalMaximumY = 0.0f;
};

struct GridCell {
    std::vector<uint32_t> bodies;
};

struct Bounds3 {
    math::Vector3 minimum{};
    math::Vector3 maximum{};
};

[[nodiscard]] bool IsFinite(float value)
{
    return std::isfinite(value);
}

[[nodiscard]] bool IsFinite(const math::Vector2& value)
{
    return IsFinite(value.x) && IsFinite(value.y);
}

[[nodiscard]] bool IsFinite(const math::Vector3& value)
{
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
}

[[nodiscard]] bool IsFinite(const math::Vector4& value)
{
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z) && IsFinite(value.w);
}

[[nodiscard]] bool IsFinite(const std::array<math::Vector4, 3>& rows)
{
    return IsFinite(rows[0]) && IsFinite(rows[1]) && IsFinite(rows[2]);
}

[[nodiscard]] bool IsFinite(const math::Matrix4& matrix)
{
    for (const auto& row : matrix.m)
        for (float value : row)
            if (!IsFinite(value)) return false;
    return true;
}

[[nodiscard]] bool HasValidSource(const RenderWaterInput& input)
{
    const WaterMediumInput& medium = input.medium;
    if (!IsFinite(medium.worldBoundsXZ) || !(medium.worldBoundsXZ.z > medium.worldBoundsXZ.x)
        || !(medium.worldBoundsXZ.w > medium.worldBoundsXZ.y)
        || !IsFinite(medium.worldToLocalRows)
        || !IsFinite(medium.absorptionPhaseG) || !IsFinite(medium.scatteringLight)
        || !IsFinite(medium.surfaceY) || !IsFinite(medium.bottomY) || !IsFinite(medium.depthLimit)
        || !IsFinite(medium.maxDistance) || !IsFinite(medium.transitionDepth)
        || !IsFinite(medium.causticsIntensity) || !IsFinite(medium.distortion)
        || !IsFinite(medium.localHalfExtent)
        || !IsFinite(input.constants.worldMatrix) || !IsFinite(input.aabbMin) || !IsFinite(input.aabbMax)
        || !IsFinite(input.margin.horizontal) || !IsFinite(input.margin.vertical)
        || input.margin.horizontal < 0.0f || input.margin.vertical < 0.0f
        || input.aabbMin.x > input.aabbMax.x || input.aabbMin.y > input.aabbMax.y
        || input.aabbMin.z > input.aabbMax.z
        || medium.depthLimit < 0.0f || medium.bottomY > medium.surfaceY + 1.0e-3f
        || medium.maxDistance < 0.0f || medium.localHalfExtent.x < 0.0f || medium.localHalfExtent.y < 0.0f)
        return false;
    if (!IsFinite(medium.worldBoundsXZ.z - medium.worldBoundsXZ.x)
        || !IsFinite(medium.worldBoundsXZ.w - medium.worldBoundsXZ.y)) return false;

    return true;
}

[[nodiscard]] bool ComputeWorldSurfaceYBounds(const RenderWaterInput& input,
                                              float& outMinimumY,
                                              float& outMaximumY)
{
    outMinimumY = std::numeric_limits<float>::infinity();
    outMaximumY = -std::numeric_limits<float>::infinity();
    for (uint32_t corner = 0u; corner < 8u; ++corner) {
        const math::Vector3 local{
            (corner & 1u) != 0u ? input.aabbMax.x : input.aabbMin.x,
            (corner & 2u) != 0u ? input.aabbMax.y : input.aabbMin.y,
            (corner & 4u) != 0u ? input.aabbMax.z : input.aabbMin.z
        };
        const math::Vector4 world = input.constants.worldMatrix * math::Vector4{ local.x, local.y, local.z, 1.0f };
        if (!IsFinite(world) || std::abs(world.w) <= 1.0e-7f) return false;
        const float worldY = world.y / world.w;
        if (!IsFinite(worldY)) return false;
        outMinimumY = (std::min)(outMinimumY, worldY);
        outMaximumY = (std::max)(outMaximumY, worldY);
    }
    outMinimumY = (std::min)(outMinimumY, input.medium.surfaceY);
    outMaximumY = (std::max)(outMaximumY, input.medium.surfaceY);
    return IsFinite(outMinimumY) && IsFinite(outMaximumY) && outMaximumY >= outMinimumY;
}

[[nodiscard]] bool ComputeWorldAabb(const WaterMediumExclusionInput& exclusion, Bounds3& outBounds)
{
    if (!IsFinite(exclusion.worldToLocalRows) || !IsFinite(exclusion.halfExtent)
        || exclusion.halfExtent.x < 0.0f || exclusion.halfExtent.y < 0.0f
        || exclusion.halfExtent.z < 0.0f)
        return false;

    const double matrix[3][3] = {
        { exclusion.worldToLocalRows[0].x, exclusion.worldToLocalRows[0].y, exclusion.worldToLocalRows[0].z },
        { exclusion.worldToLocalRows[1].x, exclusion.worldToLocalRows[1].y, exclusion.worldToLocalRows[1].z },
        { exclusion.worldToLocalRows[2].x, exclusion.worldToLocalRows[2].y, exclusion.worldToLocalRows[2].z }
    };
    const double translation[3] = { exclusion.worldToLocalRows[0].w,
                                    exclusion.worldToLocalRows[1].w,
                                    exclusion.worldToLocalRows[2].w };
    const double determinant = matrix[0][0] * (matrix[1][1] * matrix[2][2] - matrix[1][2] * matrix[2][1])
        - matrix[0][1] * (matrix[1][0] * matrix[2][2] - matrix[1][2] * matrix[2][0])
        + matrix[0][2] * (matrix[1][0] * matrix[2][1] - matrix[1][1] * matrix[2][0]);
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-24) return false;

    const double inverseDeterminant = 1.0 / determinant;
    const double inverse[3][3] = {
        { (matrix[1][1] * matrix[2][2] - matrix[1][2] * matrix[2][1]) * inverseDeterminant,
          (matrix[0][2] * matrix[2][1] - matrix[0][1] * matrix[2][2]) * inverseDeterminant,
          (matrix[0][1] * matrix[1][2] - matrix[0][2] * matrix[1][1]) * inverseDeterminant },
        { (matrix[1][2] * matrix[2][0] - matrix[1][0] * matrix[2][2]) * inverseDeterminant,
          (matrix[0][0] * matrix[2][2] - matrix[0][2] * matrix[2][0]) * inverseDeterminant,
          (matrix[0][2] * matrix[1][0] - matrix[0][0] * matrix[1][2]) * inverseDeterminant },
        { (matrix[1][0] * matrix[2][1] - matrix[1][1] * matrix[2][0]) * inverseDeterminant,
          (matrix[0][1] * matrix[2][0] - matrix[0][0] * matrix[2][1]) * inverseDeterminant,
          (matrix[0][0] * matrix[1][1] - matrix[0][1] * matrix[1][0]) * inverseDeterminant }
    };
    const double halfExtent[3] = { exclusion.halfExtent.x, exclusion.halfExtent.y, exclusion.halfExtent.z };
    double minimum[3] = {};
    double maximum[3] = {};
    for (uint32_t axis = 0; axis < 3u; ++axis) {
        double center = 0.0;
        double extent = 0.0;
        for (uint32_t localAxis = 0; localAxis < 3u; ++localAxis) {
            center -= inverse[axis][localAxis] * translation[localAxis];
            extent += std::abs(inverse[axis][localAxis]) * halfExtent[localAxis];
        }
        minimum[axis] = center - extent;
        maximum[axis] = center + extent;
        if (!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis])) return false;
    }
    outBounds.minimum = { static_cast<float>(minimum[0]), static_cast<float>(minimum[1]), static_cast<float>(minimum[2]) };
    outBounds.maximum = { static_cast<float>(maximum[0]), static_cast<float>(maximum[1]), static_cast<float>(maximum[2]) };
    return IsFinite(outBounds.minimum) && IsFinite(outBounds.maximum);
}

[[nodiscard]] bool Intersects(const Bounds3& left, const math::Vector3& rightMinimum,
                              const math::Vector3& rightMaximum)
{
    return left.maximum.x >= rightMinimum.x && left.minimum.x <= rightMaximum.x
        && left.maximum.y >= rightMinimum.y && left.minimum.y <= rightMaximum.y
        && left.maximum.z >= rightMinimum.z && left.minimum.z <= rightMaximum.z;
}

[[nodiscard]] bool Intersects(const math::Vector4& sphere, const math::Vector3& minimum,
                              const math::Vector3& maximum)
{
    if (!IsFinite(sphere) || sphere.w < 0.0f) return false;
    return sphere.x + sphere.w >= minimum.x && sphere.x - sphere.w <= maximum.x
        && sphere.y + sphere.w >= minimum.y && sphere.y - sphere.w <= maximum.y
        && sphere.z + sphere.w >= minimum.z && sphere.z - sphere.w <= maximum.z;
}

[[nodiscard]] bool BuildQueryBounds(BodyCandidate& body, const RenderWaterInput& input)
{
    float surfaceMinimumY = 0.0f;
    float surfaceMaximumY = 0.0f;
    if (!ComputeWorldSurfaceYBounds(input, surfaceMinimumY, surfaceMaximumY)) return false;
    const WaterMediumInput& medium = input.medium;
    const auto& worldY = input.constants.worldMatrix.m[1];
    const float verticalMargin = input.margin.vertical * (std::max)(1.0f, std::abs(worldY[1]))
        + input.margin.horizontal * (std::abs(worldY[0]) + std::abs(worldY[2]));
    const float displacement = (std::max)(std::abs(surfaceMinimumY - medium.surfaceY),
        std::abs(surfaceMaximumY - medium.surfaceY)) + verticalMargin;
    if (IsFinite(displacement)) body.certifiedDisplacement = displacement + 1.0f;
    const float verticalReach = medium.maxDistance;
    body.physicalMinimumY = (std::min)(surfaceMinimumY - verticalMargin - medium.depthLimit, medium.bottomY);
    body.physicalMaximumY = surfaceMaximumY + verticalMargin;
    body.queryMinimum = { body.bounds.x,
        (std::min)(surfaceMinimumY - medium.depthLimit, medium.bottomY) - verticalReach,
        body.bounds.y };
    body.queryMaximum = { body.bounds.z, surfaceMaximumY + verticalReach, body.bounds.w };
    return IsFinite(body.queryMinimum) && IsFinite(body.queryMaximum)
        && body.queryMaximum.x >= body.queryMinimum.x
        && body.queryMaximum.y >= body.queryMinimum.y
        && body.queryMaximum.z >= body.queryMinimum.z;
}

[[nodiscard]] bool GatherRelevantFeatures(BodyCandidate& body)
{
    const WaterMediumInput& medium = *body.medium;
    body.exclusionIndices.clear();
    body.plumeIndices.clear();
    for (size_t index = 0; index < medium.exclusions.size(); ++index) {
        Bounds3 bounds;
        if (!ComputeWorldAabb(medium.exclusions[index], bounds)) return false;
        if (!Intersects(bounds, body.queryMinimum, body.queryMaximum)) continue;
        body.exclusionIndices.push_back(static_cast<uint32_t>(index));
        if (body.exclusionIndices.size() > MAXIMUM_BOXES_PER_BODY) return false;
    }
    for (size_t index = 0; index < medium.plumes.size(); ++index) {
        const WaterMediumPlumeInput& plume = medium.plumes[index];
        if (!IsFinite(plume.worldPositionRadius) || !IsFinite(plume.scatteringDensity)
            || plume.worldPositionRadius.w < 0.0f)
            return false;
        if (!Intersects(plume.worldPositionRadius, body.queryMinimum, body.queryMaximum)) continue;
        body.plumeIndices.push_back(static_cast<uint32_t>(index));
        if (body.plumeIndices.size() > MAXIMUM_PLUMES_PER_BODY) return false;
    }
    return true;
}

[[nodiscard]] bool ComputeCameraXZBounds(const Camera& camera, math::Vector4& outBounds)
{
    const math::Matrix4 inverseViewProjection = math::Matrix4::Inverse(camera.GetViewProjection());
    float minimumX = std::numeric_limits<float>::infinity();
    float minimumZ = std::numeric_limits<float>::infinity();
    float maximumX = -std::numeric_limits<float>::infinity();
    float maximumZ = -std::numeric_limits<float>::infinity();

    for (float clipZ : { 0.0f, 1.0f }) {
        for (float clipY : { -1.0f, 1.0f }) {
            for (float clipX : { -1.0f, 1.0f }) {
                const math::Vector4 world = inverseViewProjection * math::Vector4{ clipX, clipY, clipZ, 1.0f };
                if (!IsFinite(world) || std::abs(world.w) <= 1.0e-7f) return false;
                const float x = world.x / world.w;
                const float z = world.z / world.w;
                if (!IsFinite(x) || !IsFinite(z)) return false;
                minimumX = (std::min)(minimumX, x);
                minimumZ = (std::min)(minimumZ, z);
                maximumX = (std::max)(maximumX, x);
                maximumZ = (std::max)(maximumZ, z);
            }
        }
    }
    if (!(maximumX > minimumX) || !(maximumZ > minimumZ)) return false;
    outBounds = { minimumX, minimumZ, maximumX, maximumZ };
    return true;
}

[[nodiscard]] bool IsVisibleInFrustum(const RenderWaterInput& input, const math::Frustum& frustum)
{
    const WaterMediumInput& medium = input.medium;
    const float verticalMargin = input.margin.vertical;
    const float horizontalMargin = input.margin.horizontal;
    const float minX = medium.worldBoundsXZ.x - horizontalMargin;
    const float maxX = medium.worldBoundsXZ.z + horizontalMargin;
    const float minZ = medium.worldBoundsXZ.y - horizontalMargin;
    const float maxZ = medium.worldBoundsXZ.w + horizontalMargin;
    float worldSurfaceMinY = 0.0f;
    float worldSurfaceMaxY = 0.0f;
    if (!ComputeWorldSurfaceYBounds(input, worldSurfaceMinY, worldSurfaceMaxY)) return false;
    const float minY = (std::min)(worldSurfaceMinY - medium.depthLimit, medium.bottomY) - verticalMargin;
    const float maxY = worldSurfaceMaxY + verticalMargin;
    const math::Vector3 center{
        static_cast<float>((static_cast<double>(minX) + maxX) * 0.5),
        static_cast<float>((static_cast<double>(minY) + maxY) * 0.5),
        static_cast<float>((static_cast<double>(minZ) + maxZ) * 0.5)
    };
    const math::Vector3 halfExtents{
        (maxX - minX) * 0.5f,
        (maxY - minY) * 0.5f,
        (maxZ - minZ) * 0.5f
    };
    return IsFinite(center) && IsFinite(halfExtents) && frustum.IntersectsAABB(center, halfExtents);
}

[[nodiscard]] bool ClipToViewAndLightRange(const WaterMediumInput& medium,
                                           const math::Vector4& viewBounds,
                                           float horizontalMargin,
                                           math::Vector4& outBounds)
{
    const double expansion = static_cast<double>((std::max)(medium.maxDistance, 0.0f))
        + static_cast<double>((std::max)(horizontalMargin, 0.0f));
    const double minX = (std::max)(static_cast<double>(medium.worldBoundsXZ.x) - horizontalMargin,
        static_cast<double>(viewBounds.x) - expansion);
    const double minZ = (std::max)(static_cast<double>(medium.worldBoundsXZ.y) - horizontalMargin,
        static_cast<double>(viewBounds.y) - expansion);
    const double maxX = (std::min)(static_cast<double>(medium.worldBoundsXZ.z) + horizontalMargin,
        static_cast<double>(viewBounds.z) + expansion);
    const double maxZ = (std::min)(static_cast<double>(medium.worldBoundsXZ.w) + horizontalMargin,
        static_cast<double>(viewBounds.w) + expansion);
    if (!(maxX - minX > MINIMUM_BOUNDS_SPAN) || !(maxZ - minZ > MINIMUM_BOUNDS_SPAN)) return false;
    outBounds = { static_cast<float>(minX), static_cast<float>(minZ),
                  static_cast<float>(maxX), static_cast<float>(maxZ) };
    return IsFinite(outBounds) && outBounds.z > outBounds.x && outBounds.w > outBounds.y;
}

[[nodiscard]] uint32_t CellCoordinate(float value, float minimum, float span)
{
    const double normalized = (static_cast<double>(value) - minimum) / span;
    const double scaled = std::floor(normalized * GRID_RESOLUTION);
    return static_cast<uint32_t>(std::clamp(scaled, 0.0, static_cast<double>(GRID_RESOLUTION - 1u)));
}

[[nodiscard]] uint32_t ChooseTileResolution(uint32_t bodyCount)
{
    if (bodyCount <= 4u) return 512u;
    if (bodyCount <= 16u) return 256u;
    if (bodyCount <= 64u) return 128u;
    return MINIMUM_TILE_RESOLUTION;
}

[[nodiscard]] uint64_t RequiredTableRows(std::vector<BodyCandidate>& bodies,
                                         math::Vector4& globalBounds,
                                         uint64_t& candidateCount,
                                         uint64_t& exclusionCount,
                                         uint64_t& plumeCount)
{
    globalBounds = {
        std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()
    };
    exclusionCount = 0;
    plumeCount = 0;
    for (const BodyCandidate& body : bodies) {
        globalBounds.x = (std::min)(globalBounds.x, body.bounds.x);
        globalBounds.y = (std::min)(globalBounds.y, body.bounds.y);
        globalBounds.z = (std::max)(globalBounds.z, body.bounds.z);
        globalBounds.w = (std::max)(globalBounds.w, body.bounds.w);
        exclusionCount += body.exclusionIndices.size();
        plumeCount += body.plumeIndices.size();
    }

    const float spanX = globalBounds.z - globalBounds.x;
    const float spanZ = globalBounds.w - globalBounds.y;
    if (!IsFinite(spanX) || !IsFinite(spanZ)
        || !(spanX > MINIMUM_BOUNDS_SPAN) || !(spanZ > MINIMUM_BOUNDS_SPAN)) return UINT64_MAX;

    candidateCount = 0;
    for (BodyCandidate& body : bodies) {
        body.cellMinX = CellCoordinate(body.bounds.x, globalBounds.x, spanX);
        body.cellMinZ = CellCoordinate(body.bounds.y, globalBounds.y, spanZ);
        body.cellMaxX = CellCoordinate(body.bounds.z, globalBounds.x, spanX);
        body.cellMaxZ = CellCoordinate(body.bounds.w, globalBounds.y, spanZ);
        const uint64_t cellsX = body.cellMaxX - body.cellMinX + 1u;
        const uint64_t cellsZ = body.cellMaxZ - body.cellMinZ + 1u;
        candidateCount += cellsX * cellsZ;
    }

    return HEADER_ROWS + static_cast<uint64_t>(bodies.size()) * BODY_ROWS
        + exclusionCount * EXCLUSION_ROWS + plumeCount * PLUME_ROWS
        + GRID_CELL_COUNT + candidateCount;
}

void AppendDisabled(std::vector<uint64_t>& disabled, std::unordered_set<uint64_t>& seen, uint64_t key)
{
    if (seen.insert(key).second) disabled.push_back(key);
}

[[nodiscard]] bool PackRows(const std::vector<BodyCandidate>& bodies,
                            const math::Vector4& globalBounds,
                            uint32_t tileResolution,
                            uint64_t candidateCount,
                            uint64_t exclusionCount,
                            uint64_t plumeCount,
                            std::vector<math::Vector4>& outRows,
                            std::vector<RenderPassHandles::WaterMediumViewBody>& outViewBodies)
{
    const uint64_t gridHeaderStart = HEADER_ROWS + static_cast<uint64_t>(bodies.size()) * BODY_ROWS
        + exclusionCount * EXCLUSION_ROWS + plumeCount * PLUME_ROWS;
    const uint64_t candidateStart = gridHeaderStart + GRID_CELL_COUNT;
    const uint64_t rowCount = candidateStart + candidateCount;
    if (rowCount > MAXIMUM_TABLE_ROWS || rowCount > std::numeric_limits<uint32_t>::max()) return false;

    std::array<GridCell, GRID_CELL_COUNT> cells;
    for (uint32_t bodyIndex = 0; bodyIndex < bodies.size(); ++bodyIndex) {
        const BodyCandidate& body = bodies[bodyIndex];
        for (uint32_t z = body.cellMinZ; z <= body.cellMaxZ; ++z)
            for (uint32_t x = body.cellMinX; x <= body.cellMaxX; ++x)
                cells[z * GRID_RESOLUTION + x].bodies.push_back(bodyIndex);
    }

    outRows.clear();
    outRows.reserve(static_cast<size_t>(rowCount));
    outViewBodies.clear();
    outViewBodies.reserve(bodies.size());
    const uint32_t exclusionStart = HEADER_ROWS + static_cast<uint32_t>(bodies.size()) * BODY_ROWS;
    const uint32_t plumeStart = exclusionStart + static_cast<uint32_t>(exclusionCount) * EXCLUSION_ROWS;
    /// @note Candidate indices follow all grid-cell headers; the shader adds each cell's relative candidate offset.
    const uint32_t candidateStartRow = static_cast<uint32_t>(candidateStart);
    float physicalMinimumY = std::numeric_limits<float>::infinity();
    float physicalMaximumY = -std::numeric_limits<float>::infinity();
    float opticalMaximumY = -std::numeric_limits<float>::infinity();
    for (const auto& body : bodies) {
        physicalMinimumY = (std::min)(physicalMinimumY, body.physicalMinimumY);
        physicalMaximumY = (std::max)(physicalMaximumY, body.physicalMaximumY);
        if (body.medium->opticalEnabled)
            opticalMaximumY = (std::max)(opticalMaximumY, body.physicalMaximumY);
    }
    const bool hasPhysicalBounds = IsFinite(physicalMinimumY) && IsFinite(physicalMaximumY)
        && physicalMinimumY < physicalMaximumY;
    /// @note A caustics-only owner remains visible to interface and caustic passes, but cannot extend the optical march.
    const bool hasTighterOpticalMaximum = hasPhysicalBounds && IsFinite(opticalMaximumY)
        && opticalMaximumY >= physicalMinimumY && opticalMaximumY < physicalMaximumY;
    const float verticalBoundsMode = hasTighterOpticalMaximum ? 2.0f : (hasPhysicalBounds ? 1.0f : 0.0f);
    outRows.emplace_back(static_cast<float>(0x574Du), 1.0f,
                         static_cast<float>(bodies.size()), static_cast<float>(rowCount));
    outRows.emplace_back(globalBounds);
    outRows.emplace_back(static_cast<float>(GRID_RESOLUTION), static_cast<float>(GRID_RESOLUTION),
                          static_cast<float>(gridHeaderStart), verticalBoundsMode);
    outRows.emplace_back(hasTighterOpticalMaximum ? opticalMaximumY : 0.0f,
                         static_cast<float>(exclusionStart), static_cast<float>(plumeStart),
                         static_cast<float>(candidateStartRow));
    outRows.emplace_back(static_cast<float>(ATLAS_RESOLUTION), static_cast<float>(ATLAS_RESOLUTION),
                         hasPhysicalBounds ? physicalMinimumY : 0.0f, hasPhysicalBounds ? physicalMaximumY : 0.0f);

    int32_t lastPriority = 0;
    bool hasLastPriority = false;
    float priorityRank = 0.0f;
    uint32_t exclusionIndex = 0;
    uint32_t plumeIndex = 0;
    for (uint32_t bodyIndex = 0; bodyIndex < bodies.size(); ++bodyIndex) {
        const BodyCandidate& candidate = bodies[bodyIndex];
        const WaterMediumInput& medium = *candidate.medium;
        if (!hasLastPriority || candidate.priority != lastPriority) {
            if (hasLastPriority) priorityRank -= 1.0f;
            lastPriority = candidate.priority;
            hasLastPriority = true;
        }
        const uint32_t tileCount = ATLAS_RESOLUTION / tileResolution;
        const uint32_t tileX = bodyIndex % tileCount;
        const uint32_t tileY = bodyIndex / tileCount;
        const float inverseAtlas = 1.0f / static_cast<float>(ATLAS_RESOLUTION);
        const float uMin = static_cast<float>(tileX * tileResolution) * inverseAtlas;
        const float vMin = static_cast<float>(tileY * tileResolution) * inverseAtlas;
        const float uMax = static_cast<float>((tileX + 1u) * tileResolution) * inverseAtlas;
        const float vMax = static_cast<float>((tileY + 1u) * tileResolution) * inverseAtlas;

        const float certifiedDisplacement = medium.analyticRectangle && !candidate.variableDepth
            ? candidate.certifiedDisplacement : 0.0f;
        /// @note Positive row w certifies a wet interior; negative w is the Terrain packed-depth half-step for receiver lighting only.
        outRows.emplace_back(medium.localHalfExtent.x, medium.localHalfExtent.y,
                              candidate.variableDepth ? 1.0f : 0.0f,
                              candidate.variableDepth ? -candidate.depthQuantization : certifiedDisplacement);
        outRows.insert(outRows.end(), medium.worldToLocalRows.begin(), medium.worldToLocalRows.end());
        outRows.push_back(medium.absorptionPhaseG);
        outRows.push_back(medium.scatteringLight);
        outRows.emplace_back(medium.surfaceY, medium.bottomY, medium.depthLimit, medium.maxDistance);
        outRows.emplace_back(medium.transitionDepth, medium.causticsIntensity, medium.distortion,
                             static_cast<float>(std::clamp(medium.lightSteps, 16, 64)));
        outRows.emplace_back(uMin, vMin, uMax, vMax);
        outRows.push_back(candidate.bounds);
        outRows.emplace_back(static_cast<float>(exclusionIndex),
                             static_cast<float>(candidate.exclusionIndices.size()),
                             static_cast<float>(plumeIndex),
                             static_cast<float>(candidate.plumeIndices.size()));
        outRows.emplace_back(priorityRank, medium.opticalEnabled ? 1.0f : 0.0f,
                             medium.causticsEnabled ? 1.0f : 0.0f,
                             candidate.horizon ? 2.0f : (medium.analyticRectangle ? 1.0f : 0.0f));
        exclusionIndex += static_cast<uint32_t>(candidate.exclusionIndices.size());
        plumeIndex += static_cast<uint32_t>(candidate.plumeIndices.size());
        outViewBodies.push_back({ candidate.inputIndex, tileX, tileY, candidate.bounds });
    }

    for (const BodyCandidate& body : bodies) {
        for (uint32_t index : body.exclusionIndices) {
            const WaterMediumExclusionInput& exclusion = body.medium->exclusions[index];
            outRows.insert(outRows.end(), exclusion.worldToLocalRows.begin(), exclusion.worldToLocalRows.end());
            outRows.emplace_back(exclusion.halfExtent.x, exclusion.halfExtent.y, exclusion.halfExtent.z, 0.0f);
        }
    }
    for (const BodyCandidate& body : bodies) {
        for (uint32_t index : body.plumeIndices) {
            const WaterMediumPlumeInput& plume = body.medium->plumes[index];
            outRows.push_back(plume.worldPositionRadius);
            outRows.push_back(plume.scatteringDensity);
        }
    }

    uint32_t firstCandidate = 0;
    for (const GridCell& cell : cells) {
        outRows.emplace_back(static_cast<float>(firstCandidate), static_cast<float>(cell.bodies.size()), 0.0f, 0.0f);
        firstCandidate += static_cast<uint32_t>(cell.bodies.size());
    }
    for (const GridCell& cell : cells)
        for (uint32_t bodyIndex : cell.bodies)
            outRows.emplace_back(static_cast<float>(bodyIndex), 0.0f, 0.0f, 0.0f);
    return outRows.size() == rowCount && outViewBodies.size() == bodies.size();
}

void DisableBodies(const std::vector<BodyCandidate>& bodies,
                   std::vector<uint64_t>& disabled,
                   std::unordered_set<uint64_t>& seen)
{
    for (const BodyCandidate& body : bodies) AppendDisabled(disabled, seen, body.ownerKey);
}

void ClearPreparedState(RenderPassContext& ctx, RenderViewResources& viewTargets)
{
    ctx.waterMediumPassActive = false;
    ctx.waterInterfacePassActive = false;
    ctx.waterMediumQualityLimited = false;
    ctx.handles.waterMediumAtlasRT = &viewTargets.waterMediumAtlasRT;
    ctx.handles.waterInterfaceRT = &viewTargets.waterInterfaceRT;
    ctx.handles.waterMediumTable = {};
    ctx.handles.waterMediumBodyCount = 0;
    ctx.handles.waterMediumTileResolution = 0;
    ctx.handles.waterMediumBodies.clear();
    ctx.handles.waterMediumDisabledOwnerKeys.clear();
    ctx.handles.waterMediumQualityLimitedOwnerKeys.clear();
}

void ReportDisabledOwners(const RenderViewResources& viewTargets,
                          const ResourceManager& resources,
                          uint64_t sceneGeneration,
                          std::vector<uint64_t> ownerKeys)
{
    std::sort(ownerKeys.begin(), ownerKeys.end());
    ownerKeys.erase(std::unique(ownerKeys.begin(), ownerKeys.end()), ownerKeys.end());

    struct DiagnosticState {
        const ResourceManager* resources = nullptr;
        uint64_t sceneGeneration = 0;
        std::vector<uint64_t> ownerKeys;
    };
    static std::mutex diagnosticMutex;
    static std::unordered_map<const RenderViewResources*, DiagnosticState> diagnosticStates;

    bool changed = false;
    {
        const std::lock_guard<std::mutex> lock(diagnosticMutex);
        DiagnosticState& state = diagnosticStates[&viewTargets];
        changed = state.resources != &resources || state.sceneGeneration != sceneGeneration
            || state.ownerKeys != ownerKeys;
        state.resources = &resources;
        state.sceneGeneration = sceneGeneration;
        state.ownerKeys = ownerKeys;
        if (diagnosticStates.size() > 32u) {
            for (auto it = diagnosticStates.begin(); it != diagnosticStates.end();) {
                if (it->first != &viewTargets) it = diagnosticStates.erase(it);
                else ++it;
            }
        }
    }
    if (!changed || ownerKeys.empty()) return;

    std::string visibleKeys;
    constexpr size_t MAXIMUM_LOGGED_KEYS = 16u;
    const size_t visibleCount = (std::min)(ownerKeys.size(), MAXIMUM_LOGGED_KEYS);
    for (size_t index = 0; index < visibleCount; ++index) {
        if (index > 0u) visibleKeys += ",";
        visibleKeys += std::to_string(ownerKeys[index]);
    }
    const size_t omittedCount = ownerKeys.size() - visibleCount;
    FBZZ_LOG_WARN("Water medium disabled %zu owner(s) in scene generation %llu; owner keys: %s%s",
                  ownerKeys.size(), static_cast<unsigned long long>(sceneGeneration),
                  visibleKeys.c_str(), omittedCount > 0u ? " (additional keys omitted)" : "");
}

} /// @note namespace

bool PrepareWaterMediumResources(RenderPassContext& ctx, RenderViewResources& viewTargets)
{
    ClearPreparedState(ctx, viewTargets);
    const auto finish = [&](bool prepared) {
        ReportDisabledOwners(viewTargets,
                             ctx.resources,
                             ctx.renderScene ? ctx.renderScene->sceneGeneration : 0u,
                             ctx.handles.waterMediumDisabledOwnerKeys);
        return prepared;
    };
    if (!ctx.renderScene || ctx.width == 0u || ctx.height == 0u) return finish(false);

    std::vector<BodyCandidate> bodies;
    bodies.reserve((std::min)(ctx.renderScene->water.size(), static_cast<size_t>(MAXIMUM_BODIES)));
    std::unordered_set<uint64_t> disabledKeys;
    std::unordered_set<uint64_t> duplicateKeys;
    std::unordered_set<uint64_t> seenKeys;
    for (size_t inputIndex = 0; inputIndex < ctx.renderScene->water.size(); ++inputIndex) {
        const RenderWaterInput& input = ctx.renderScene->water[inputIndex];
        const WaterMediumInput& medium = input.medium;
        /// @note A hidden alias must not invalidate a visible body with the same stable owner key.
        if (input.layer >= 32u || (ctx.cullingMask & (1u << input.layer)) == 0u) continue;
        if (!medium.opticalEnabled && !medium.causticsEnabled && medium.exclusions.empty()) continue;
        if (seenKeys.contains(medium.stableOwnerKey)) duplicateKeys.insert(medium.stableOwnerKey);
        else seenKeys.insert(medium.stableOwnerKey);
        if (inputIndex > std::numeric_limits<uint32_t>::max() || !HasValidSource(input)) {
            AppendDisabled(ctx.handles.waterMediumDisabledOwnerKeys, disabledKeys, medium.stableOwnerKey);
            continue;
        }
        bodies.push_back({ static_cast<uint32_t>(inputIndex), medium.stableOwnerKey, medium.priority, &medium, {} });
        bodies.back().variableDepth = input.constants.terrainInfoParams.x > 0.5f && !medium.depthIsUniform;
        /// @note Signed UNORM16 spans twice the depth range; half a quantization step is range / 65535.
        bodies.back().depthQuantization = (std::max)(input.constants.terrainInfoParams.y, 0.0f) / 65535.0f;
    }

    if (!duplicateKeys.empty()) {
        for (auto it = bodies.begin(); it != bodies.end();) {
            if (duplicateKeys.contains(it->ownerKey)) {
                AppendDisabled(ctx.handles.waterMediumDisabledOwnerKeys, disabledKeys, it->ownerKey);
                it = bodies.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (bodies.empty()) return finish(false);

    math::Frustum generatedFrustum;
    const math::Frustum* frustum = ctx.cameraFrustum;
    if (!frustum) {
        generatedFrustum = math::Frustum::FromViewProjection(ctx.camera.GetViewProjection());
        frustum = &generatedFrustum;
    }
    math::Vector4 viewBounds;
    if (!ComputeCameraXZBounds(ctx.camera, viewBounds)) {
        DisableBodies(bodies, ctx.handles.waterMediumDisabledOwnerKeys, disabledKeys);
        return finish(false);
    }

    std::vector<BodyCandidate> visible;
    visible.reserve(bodies.size());
    for (BodyCandidate& body : bodies) {
        const RenderWaterInput& input = ctx.renderScene->water[body.inputIndex];
        body.horizon = input.farOceanEnabled && input.medium.analyticRectangle;
        if (!body.horizon && ctx.frustumCullingEnabled && !IsVisibleInFrustum(input, *frustum)) continue;
        const float horizontalMargin = IsFinite(input.margin.horizontal)
            ? (std::max)(input.margin.horizontal, 0.0f) : 0.0f;
        if (body.horizon) {
            math::Vector2 localMinimum, localMaximum;
            if (!ResolveWaterHorizonRectangle(input, ctx.camera, localMinimum, localMaximum)) {
                AppendDisabled(ctx.handles.waterMediumDisabledOwnerKeys, disabledKeys, body.ownerKey);
                continue;
            }
            body.bounds = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
            for (float x : {localMinimum.x, localMaximum.x}) {
                for (float z : {localMinimum.y, localMaximum.y}) {
                    const auto world = input.constants.worldMatrix * math::Vector4{x, 0.0f, z, 1.0f};
                    body.bounds.x = (std::min)(body.bounds.x, world.x - horizontalMargin);
                    body.bounds.y = (std::min)(body.bounds.y, world.z - horizontalMargin);
                    body.bounds.z = (std::max)(body.bounds.z, world.x + horizontalMargin);
                    body.bounds.w = (std::max)(body.bounds.w, world.z + horizontalMargin);
                }
            }
        } else if (!ClipToViewAndLightRange(*body.medium, viewBounds, horizontalMargin, body.bounds)) continue;
        if (!BuildQueryBounds(body, input) || !GatherRelevantFeatures(body)) {
            AppendDisabled(ctx.handles.waterMediumDisabledOwnerKeys, disabledKeys, body.ownerKey);
            continue;
        }
        visible.push_back(body);
    }
    bodies = std::move(visible);
    if (bodies.empty()) return finish(false);

    std::sort(bodies.begin(), bodies.end(), [](const BodyCandidate& left, const BodyCandidate& right) {
        if (left.priority != right.priority) return left.priority > right.priority;
        return left.ownerKey < right.ownerKey;
    });
    if (bodies.size() > MAXIMUM_BODIES) {
        for (size_t i = MAXIMUM_BODIES; i < bodies.size(); ++i)
            AppendDisabled(ctx.handles.waterMediumDisabledOwnerKeys, disabledKeys, bodies[i].ownerKey);
        bodies.resize(MAXIMUM_BODIES);
    }

    if (!WaterMediumPipelineResourcesReady(ctx.resources)) {
        DisableBodies(bodies, ctx.handles.waterMediumDisabledOwnerKeys, disabledKeys);
        return finish(false);
    }
    const RenderTargetDesc targetDesc{ 1u, Format::RGBA32F, true, true };
    viewTargets.waterMediumAtlasRT.Ensure(ctx.resources, ATLAS_RESOLUTION, ATLAS_RESOLUTION, targetDesc);
    viewTargets.waterInterfaceRT.Ensure(ctx.resources, ctx.width, ctx.height, targetDesc);
    if (!viewTargets.waterMediumAtlasRT.IsValid() || !viewTargets.waterInterfaceRT.IsValid()) {
        DisableBodies(bodies, ctx.handles.waterMediumDisabledOwnerKeys, disabledKeys);
        return finish(false);
    }

    math::Vector4 globalBounds{};
    uint64_t candidateCount = 0;
    uint64_t exclusionCount = 0;
    uint64_t plumeCount = 0;
    uint64_t rowCount = RequiredTableRows(bodies, globalBounds, candidateCount, exclusionCount, plumeCount);
    while (!bodies.empty() && rowCount > MAXIMUM_TABLE_ROWS) {
        AppendDisabled(ctx.handles.waterMediumDisabledOwnerKeys, disabledKeys, bodies.back().ownerKey);
        bodies.pop_back();
        rowCount = bodies.empty() ? 0u
            : RequiredTableRows(bodies, globalBounds, candidateCount, exclusionCount, plumeCount);
    }
    if (bodies.empty() || rowCount == 0u || rowCount > MAXIMUM_TABLE_ROWS) return finish(false);

    const uint32_t tileResolution = ChooseTileResolution(static_cast<uint32_t>(bodies.size()));
    std::vector<math::Vector4> rows;
    if (!PackRows(bodies, globalBounds, tileResolution, candidateCount, exclusionCount, plumeCount,
                  rows, ctx.handles.waterMediumBodies)) {
        ctx.handles.waterMediumBodies.clear();
        DisableBodies(bodies, ctx.handles.waterMediumDisabledOwnerKeys, disabledKeys);
        return finish(false);
    }
    const ResourceHandle<StructuredBufferTag> table = viewTargets.waterMediumTablePool.Acquire(
        ctx.resources, rows.size(), static_cast<uint32_t>(sizeof(math::Vector4)));
    if (!table.IsValid()) {
        ctx.handles.waterMediumBodies.clear();
        DisableBodies(bodies, ctx.handles.waterMediumDisabledOwnerKeys, disabledKeys);
        return finish(false);
    }
    ctx.resources.Update(table, rows.data(), rows.size() * sizeof(math::Vector4));
    if (!ctx.resources.Get(table)) {
        ctx.handles.waterMediumBodies.clear();
        DisableBodies(bodies, ctx.handles.waterMediumDisabledOwnerKeys, disabledKeys);
        return finish(false);
    }

    ctx.handles.waterMediumTable = table;
    ctx.handles.waterMediumBodyCount = static_cast<uint32_t>(bodies.size());
    ctx.handles.waterMediumTileResolution = tileResolution;
    ctx.waterMediumQualityLimited = tileResolution < MAXIMUM_TILE_RESOLUTION;
    if (ctx.waterMediumQualityLimited) {
        ctx.handles.waterMediumQualityLimitedOwnerKeys.reserve(bodies.size());
        for (const BodyCandidate& body : bodies)
            ctx.handles.waterMediumQualityLimitedOwnerKeys.push_back(body.ownerKey);
    }
    ctx.waterMediumPassActive = true;
    ctx.waterInterfacePassActive = true;
    return finish(true);
}

} /// @note namespace fbzz::renderer
