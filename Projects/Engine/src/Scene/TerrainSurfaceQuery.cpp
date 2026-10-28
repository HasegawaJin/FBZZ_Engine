/// @file    TerrainSurfaceQuery.cpp
/// @brief   World 鉛直線を Terrain local grid へ写し、通過 cell のみで三角形を照会する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Math/Ray.hpp>
#include <Engine/Scene/TerrainSurfaceGeometry.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <type_traits>
namespace fbzz::scene {
namespace {
bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
math::Vector3 Divide(const math::Vector3& value, const math::Vector3& divisor)
{
    return {value.x / divisor.x, value.y / divisor.y, value.z / divisor.z};
}
bool Unit(const math::Vector3& value, math::Vector3& out)
{
    const double magnitude = (std::max)({std::abs(static_cast<double>(value.x)),
        std::abs(static_cast<double>(value.y)), std::abs(static_cast<double>(value.z))});
    if (!std::isfinite(magnitude) || magnitude == 0) return false;
    const double x = value.x / magnitude, y = value.y / magnitude, z = value.z / magnitude;
    const double length = std::sqrt(x * x + y * y + z * z);
    out = {static_cast<float>(x / length), static_cast<float>(y / length), static_cast<float>(z / length)};
    return Finite(out);
}
bool WorldNormal(const Transform& transform, const math::Vector3& normal, math::Vector3& out)
{
    const double x = static_cast<double>(normal.x) / transform.worldScale.x;
    const double y = static_cast<double>(normal.y) / transform.worldScale.y;
    const double z = static_cast<double>(normal.z) / transform.worldScale.z;
    const double magnitude = (std::max)({std::abs(x), std::abs(y), std::abs(z)});
    if (!std::isfinite(magnitude) || magnitude == 0) return false;
    /// @note 法線は R*S^-1 で変換する。正負を含む非一様 scale でも接線と直交させる。
    /// @see https://pbr-book.org/4ed/Geometry_and_Transformations/Applying_Transformations#Normals PBRT 3.10.3 Normals。
    return Unit(transform.worldRotation * math::Vector3{static_cast<float>(x / magnitude),
        static_cast<float>(y / magnitude), static_cast<float>(z / magnitude)}, out);
}
bool ClipAxis(double origin, double direction, double upper, double& entry, double& exit)
{
    if (direction == 0) return origin >= 0 && origin <= upper;
    double first = -origin / direction, last = (upper - origin) / direction;
    if (first > last) std::swap(first, last);
    entry = (std::max)(entry, first); exit = (std::min)(exit, last);
    return entry <= exit;
}
bool Finish(const TerrainComponent& terrain, const Transform& transform, const math::Vector3& local,
    float worldX, float worldZ, float worldY, TerrainSurfaceSample& out)
{
    TerrainSurfaceSample sample{{worldX, worldY, worldZ}, {}};
    if (!std::isfinite(worldY) || !Finite(local)
        || !TryTerrainNormalToWorld(transform, terrain.GetNormalAt(local.x, local.z), sample.normal)) return false;
    out = sample;
    return true;
}
} /// @note namespace

bool TryTerrainNormalToWorld(const Transform& transform, const math::Vector3& localNormal, math::Vector3& out)
{
    const auto scale = transform.worldScale;
    const double rotationLength = transform.worldRotation.Length();
    if (!Finite(scale) || scale.x == 0 || scale.y == 0 || scale.z == 0 || !Finite(localNormal)
        || !std::isfinite(rotationLength) || std::abs(rotationLength - 1) > 0.001) return false;
    math::Vector3 normal;
    if (!WorldNormal(transform, localNormal, normal)) return false;
    out = normal;
    return true;
}

static bool TrySampleHeightfieldWorld(const TerrainComponent& terrain, const Transform& transform,
    float worldX, float worldZ, TerrainSurfaceSample& out)
{
    const auto scale = transform.worldScale;
    const double rotationLength = transform.worldRotation.Length();
    if (!std::isfinite(worldX) || !std::isfinite(worldZ) || !Finite(transform.worldPosition) || !Finite(scale)
        || scale.x == 0 || scale.y == 0 || scale.z == 0 || !std::isfinite(rotationLength)
        || std::abs(rotationLength - 1) > 0.001 || terrain.columns < 2 || terrain.rows < 2
        || !std::isfinite(terrain.cellSize) || terrain.cellSize <= 0 || !std::isfinite(terrain.maxHeight)) return false;
    const std::size_t columns = static_cast<std::size_t>(terrain.columns), rows = static_cast<std::size_t>(terrain.rows);
    if (rows > std::numeric_limits<std::size_t>::max() / columns || terrain.heightData.size() != columns * rows
        || (!terrain.holeData.empty() && terrain.holeData.size() != (columns - 1) * (rows - 1))) return false;
    const double width = (columns - 1) * static_cast<double>(terrain.cellSize);
    const double depth = (rows - 1) * static_cast<double>(terrain.cellSize);
    if (!std::isfinite(width) || !std::isfinite(depth)) return false;
    const auto inverseRotation = transform.worldRotation.Inverse();
    const auto origin = Divide(inverseRotation * (math::Vector3{worldX, 0, worldZ} - transform.worldPosition), scale);
    const auto direction = Divide(inverseRotation * math::Vector3{0, -1, 0}, scale);
    if (!Finite(origin) || !Finite(direction)) return false;
    const auto cellAt = [&](double value, int cells) { return (std::clamp)(static_cast<int>(std::floor(value / terrain.cellSize)), 0, cells - 1); };
    const auto edge = [&](double value) { return std::abs(value / terrain.cellSize - std::round(value / terrain.cellSize)) <= 0.000001; };
    const auto availableCell = [&](int x, int z) {
        return x >= 0 && z >= 0 && x < terrain.columns - 1 && z < terrain.rows - 1 && !terrain.IsHoleCell(x, z);
    };
    if (direction.x == 0 && direction.z == 0) {
        if (origin.x < 0 || origin.z < 0 || origin.x > width || origin.z > depth) return false;
        const int x = cellAt(origin.x, terrain.columns - 1), z = cellAt(origin.z, terrain.rows - 1);
        bool present = availableCell(x, z);
        if (edge(origin.x)) present |= availableCell(x - 1, z);
        if (edge(origin.z)) present |= availableCell(x, z - 1);
        if (edge(origin.x) && edge(origin.z)) present |= availableCell(x - 1, z - 1);
        if (!present) return false;
        const math::Vector3 local{origin.x, terrain.GetHeightAt(origin.x, origin.z), origin.z};
        const auto world = transform.worldPosition + transform.worldRotation * math::Vector3{
            local.x * scale.x, local.y * scale.y, local.z * scale.z};
        return Finish(terrain, transform, local, worldX, worldZ, world.y, out);
    }
    double entry = -std::numeric_limits<double>::infinity(), exit = std::numeric_limits<double>::infinity();
    if (!ClipAxis(origin.x, direction.x, width, entry, exit)
        || !ClipAxis(origin.z, direction.z, depth, entry, exit) || !std::isfinite(entry) || !std::isfinite(exit)) return false;
    math::Vector3 unitDirection;
    if (!Unit(direction, unitDirection)) return false;
    const double directionLength = std::sqrt(static_cast<double>(direction.x) * direction.x
        + static_cast<double>(direction.y) * direction.y + static_cast<double>(direction.z) * direction.z);
    /// @note 正逆2本の重なる ray で無限鉛直線を表す。surface が基準 worldY=0 と一致しても Ray の t>epsilon 条件を満たす。
    const math::Ray down{origin - unitDirection, unitDirection}, up{origin + unitDirection, -unitDirection};
    int x = cellAt((std::clamp)(origin.x + direction.x * entry, 0.0, width), terrain.columns - 1);
    int z = cellAt((std::clamp)(origin.z + direction.z * entry, 0.0, depth), terrain.rows - 1);
    const int stepX = direction.x > 0 ? 1 : -1, stepZ = direction.z > 0 ? 1 : -1;
    const auto nextX = [&] { return direction.x == 0 ? std::numeric_limits<double>::infinity()
        : ((x + (stepX > 0 ? 1 : 0)) * static_cast<double>(terrain.cellSize) - origin.x) / direction.x; };
    const auto nextZ = [&] { return direction.z == 0 ? std::numeric_limits<double>::infinity()
        : ((z + (stepZ > 0 ? 1 : 0)) * static_cast<double>(terrain.cellSize) - origin.z) / direction.z; };
    double current = entry;
    /// @note 2D格子の次境界だけを更新するDDA。全頂点ではなく通過cellを順に検査し、最初の交点を最上面にする。
    /// @see https://www.eecs.yorku.ca/~amana/research/ Amanatides/Woo, A Fast Voxel Traversal Algorithm for Ray Tracing (1987)。
    while (x >= 0 && z >= 0 && x < terrain.columns - 1 && z < terrain.rows - 1 && current <= exit) {
        const double crossX = nextX(), crossZ = nextZ(), next = (std::min)({crossX, crossZ, exit});
        const double tolerance = 0.00001 * (1 + (std::max)(std::abs(current), std::abs(next)));
        double nearest = std::numeric_limits<double>::infinity();
        const auto visit = [&](int cellX, int cellZ) {
            if (!availableCell(cellX, cellZ)) return;
            const auto vertex = [&](int vx, int vz) {
                return math::Vector3{vx * terrain.cellSize,
                    terrain.heightData[static_cast<std::size_t>(vz) * columns + vx] * terrain.maxHeight, vz * terrain.cellSize};
            };
            const auto v00 = vertex(cellX, cellZ), v10 = vertex(cellX + 1, cellZ);
            const auto v01 = vertex(cellX, cellZ + 1), v11 = vertex(cellX + 1, cellZ + 1);
            if (!Finite(v00) || !Finite(v10) || !Finite(v01) || !Finite(v11)) return;
            const auto triangle = [&](const math::Vector3& a, const math::Vector3& b, const math::Vector3& c) {
                float distance = 0;
                if (down.IntersectTriangle(a, b, c, distance)) {
                    const double parameter = (distance - 1.0) / directionLength;
                    if (parameter >= current - tolerance && parameter <= next + tolerance) nearest = (std::min)(nearest, parameter);
                }
                if (up.IntersectTriangle(a, b, c, distance)) {
                    const double parameter = (1.0 - distance) / directionLength;
                    if (parameter >= current - tolerance && parameter <= next + tolerance) nearest = (std::min)(nearest, parameter);
                }
            };
            triangle(v00, v01, v10); triangle(v10, v01, v11);
        };
        visit(x, z);
        if (direction.x == 0 && edge(origin.x)) visit(x - 1, z);
        if (direction.z == 0 && edge(origin.z)) visit(x, z - 1);
        if (crossX == crossZ && crossX <= exit) { visit(x + stepX, z); visit(x, z + stepZ); }
        if (std::isfinite(nearest)) {
            const math::Vector3 local{static_cast<float>((std::clamp)(origin.x + direction.x * nearest, 0.0, width)),
                static_cast<float>(origin.y + direction.y * nearest), static_cast<float>((std::clamp)(origin.z + direction.z * nearest, 0.0, depth))};
            return Finish(terrain, transform, local, worldX, worldZ, static_cast<float>(-nearest), out);
        }
        if (next >= exit) break;
        const bool advanceX = crossX <= crossZ, advanceZ = crossZ <= crossX;
        if (advanceX) x += stepX;
        if (advanceZ) z += stepZ;
        current = next;
    }
    return false;
}

namespace {
enum class SurfaceSelection { TOP, FLOOR, CEILING };
math::Vector3 SurfaceWorldPoint(const Transform& transform, const math::Vector3& local)
{
    const auto scale = transform.worldScale;
    return transform.worldPosition + transform.worldRotation * math::Vector3{local.x * scale.x, local.y * scale.y, local.z * scale.z};
}
constexpr size_t SURFACE_VERTEX_SCRATCH_CAPACITY = 512;
struct SurfaceVertexScratch {
    std::array<math::Vector3, SURFACE_VERTEX_SCRATCH_CAPACITY> worldPositions;
    std::array<bool, SURFACE_VERTEX_SCRATCH_CAPACITY> transformed{};
    bool busy = false;
};
/// @note Only values are retained per thread; each query clears its flags and nested queries use the scalar path.
class SurfaceVertexScratchLease {
public:
    explicit SurfaceVertexScratchLease(size_t vertexCount)
    {
        if (vertexCount == 0 || vertexCount > SURFACE_VERTEX_SCRATCH_CAPACITY) return;
        thread_local SurfaceVertexScratch scratch;
        if (scratch.busy) return;
        scratch.busy = true;
        m_scratch = &scratch;
    }
    ~SurfaceVertexScratchLease()
    {
        if (!m_scratch) return;
        m_scratch->transformed.fill(false);
        m_scratch->busy = false;
    }
    SurfaceVertexScratchLease(const SurfaceVertexScratchLease&) = delete;
    SurfaceVertexScratchLease& operator=(const SurfaceVertexScratchLease&) = delete;
    SurfaceVertexScratch* Get() const { return m_scratch; }
private:
    SurfaceVertexScratch* m_scratch = nullptr;
};
struct SurfaceTriangleProjection {
    math::Vector3 origin;
    float heightDifferenceB = 0, heightDifferenceC = 0;
    double ax = 0, az = 0, bx = 0, bz = 0, determinant = 0;
    bool usable = false;
};
SurfaceTriangleProjection PrepareProjection(const math::Vector3& wa, const math::Vector3& wb, const math::Vector3& wc)
{
    SurfaceTriangleProjection projection;
    if (!Finite(wa) || !Finite(wb) || !Finite(wc)) return projection;
    projection.ax = static_cast<double>(wb.x) - wa.x;
    projection.az = static_cast<double>(wb.z) - wa.z;
    projection.bx = static_cast<double>(wc.x) - wa.x;
    projection.bz = static_cast<double>(wc.z) - wa.z;
    projection.determinant = projection.ax * projection.bz - projection.az * projection.bx;
    if (std::abs(projection.determinant) <= 1e-12) return projection;
    projection.origin = wa;
    /// @note Preserve the original float subtraction before barycentric double multiplication.
    projection.heightDifferenceB = wb.y - wa.y;
    projection.heightDifferenceC = wc.y - wa.y;
    projection.usable = true;
    return projection;
}
/// @note Intersect the infinite world vertical line by projected barycentrics; t=0 is a valid contact.
bool VerticalProjectedTriangle(const Transform& transform, const math::Vector3& a, const math::Vector3& b,
    const math::Vector3& c, const SurfaceTriangleProjection& projection,
    float worldX, float worldZ, TerrainSurfaceSample& out)
{
    if (!projection.usable) return false;
    const double px = static_cast<double>(worldX) - projection.origin.x;
    const double pz = static_cast<double>(worldZ) - projection.origin.z;
    const double u = (px * projection.bz - pz * projection.bx) / projection.determinant;
    const double v = (projection.ax * pz - projection.az * px) / projection.determinant;
    if (u < -1e-6 || v < -1e-6 || u + v > 1.0 + 1e-6) return false;
    TerrainSurfaceSample sample;
    sample.position = {worldX, static_cast<float>(projection.origin.y + u * projection.heightDifferenceB
        + v * projection.heightDifferenceC), worldZ};
    if (!std::isfinite(sample.position.y)
        || !TryTerrainNormalToWorld(transform, math::Vector3::Cross(b - a, c - a), sample.normal)) return false;
    out = sample;
    return true;
}
bool VerticalWorldTriangle(const Transform& transform, const math::Vector3& a, const math::Vector3& b,
    const math::Vector3& c, const math::Vector3& wa, const math::Vector3& wb, const math::Vector3& wc,
    float worldX, float worldZ, TerrainSurfaceSample& out)
{
    if (!Finite(wa) || !Finite(wb) || !Finite(wc)) return false;
    const double ax = static_cast<double>(wb.x) - wa.x, az = static_cast<double>(wb.z) - wa.z;
    const double bx = static_cast<double>(wc.x) - wa.x, bz = static_cast<double>(wc.z) - wa.z;
    const double det = ax * bz - az * bx;
    if (std::abs(det) <= 1e-12) return false;
    const double px = static_cast<double>(worldX) - wa.x, pz = static_cast<double>(worldZ) - wa.z;
    const double u = (px * bz - pz * bx) / det, v = (ax * pz - az * px) / det;
    if (u < -1e-6 || v < -1e-6 || u + v > 1.0 + 1e-6) return false;
    TerrainSurfaceSample sample;
    sample.position = {worldX, static_cast<float>(wa.y + u * (wb.y - wa.y) + v * (wc.y - wa.y)), worldZ};
    if (!std::isfinite(sample.position.y)
        || !TryTerrainNormalToWorld(transform, math::Vector3::Cross(b - a, c - a), sample.normal)) return false;
    out = sample;
    return true;
}
bool VerticalTriangle(const Transform& transform, const math::Vector3& a, const math::Vector3& b,
    const math::Vector3& c, float worldX, float worldZ, TerrainSurfaceSample& out)
{
    const auto wa = SurfaceWorldPoint(transform, a), wb = SurfaceWorldPoint(transform, b), wc = SurfaceWorldPoint(transform, c);
    return VerticalWorldTriangle(transform, a, b, c, wa, wb, wc, worldX, worldZ, out);
}
constexpr size_t SURFACE_PREPARED_INDEX_CAPACITY = 2048;
constexpr size_t SURFACE_PREPARED_TRIANGLE_CAPACITY = SURFACE_PREPARED_INDEX_CAPACITY / 3;
enum class SurfacePreparation { PREPARED, FALLBACK, INVALID };
/// @note The byte key contains exactly x/y/z float bits; layout changes must not silently add padding to the key.
static_assert(std::is_standard_layout_v<math::Vector3> && std::is_trivially_copyable_v<math::Vector3>
    && sizeof(math::Vector3) == 3 * sizeof(float)
    && offsetof(math::Vector3, x) == 0 && offsetof(math::Vector3, y) == sizeof(float)
    && offsetof(math::Vector3, z) == 2 * sizeof(float));
constexpr size_t SURFACE_PROJECTION_BLOCK_SIZE = 16;
constexpr size_t SURFACE_PROJECTION_BLOCK_COUNT =
    (SURFACE_PREPARED_TRIANGLE_CAPACITY + SURFACE_PROJECTION_BLOCK_SIZE - 1) / SURFACE_PROJECTION_BLOCK_SIZE;
/// @note Bounds are deliberately loose; an uncertain projection keeps the entire original-order block searchable.
struct SurfaceProjectionBlock {
    double minimumX = std::numeric_limits<double>::infinity(), maximumX = -std::numeric_limits<double>::infinity();
    double minimumZ = std::numeric_limits<double>::infinity(), maximumZ = -std::numeric_limits<double>::infinity();
    bool bounded = true;
    void Include(const SurfaceTriangleProjection& projection)
    {
        if (!projection.usable || !bounded) return;
        const double edge = (std::max)({std::abs(projection.ax), std::abs(projection.az),
            std::abs(projection.bx), std::abs(projection.bz)});
        /// @note With |det|>=1e-6*edge^2, double evaluation uncertainty is far below the spare factor in radius=8*edge.
        /// @see Docs/design/script-dispatch-workspaces.md Projected-surface conservative rejection.
        if (std::abs(projection.determinant) < 1e-6 * edge * edge) { bounded = false; return; }
        const double radius = 8 * edge;
        minimumX = (std::min)(minimumX, std::nextafter(projection.origin.x - radius, -std::numeric_limits<double>::infinity()));
        maximumX = (std::max)(maximumX, std::nextafter(projection.origin.x + radius, std::numeric_limits<double>::infinity()));
        minimumZ = (std::min)(minimumZ, std::nextafter(projection.origin.z - radius, -std::numeric_limits<double>::infinity()));
        maximumZ = (std::max)(maximumZ, std::nextafter(projection.origin.z + radius, std::numeric_limits<double>::infinity()));
    }
    bool Rejects(float worldX, float worldZ) const
    {
        return bounded && (worldX < minimumX || worldX > maximumX || worldZ < minimumZ || worldZ > maximumZ);
    }
};
struct PreparedSurfaceEntry {
    std::array<math::Vector3, SURFACE_VERTEX_SCRATCH_CAPACITY> localPositions;
    std::array<uint32_t, SURFACE_PREPARED_INDEX_CAPACITY> indices;
    std::array<SurfaceTriangleProjection, SURFACE_PREPARED_TRIANGLE_CAPACITY> projections;
    std::array<SurfaceProjectionBlock, SURFACE_PROJECTION_BLOCK_COUNT> projectionBlocks;
    std::array<uint32_t, 10> transformBits{};
    size_t vertexCount = 0, indexCount = 0;
    bool validated = false, prepared = false, busy = false;
};
/// @note One value-only entry per thread; no source pointers, revisions or scene identity survive the call.
class PreparedSurfaceLease {
public:
    explicit PreparedSurfaceLease(bool eligible)
    {
        if (!eligible) return;
        thread_local PreparedSurfaceEntry entry;
        if (entry.busy) return;
        entry.busy = true;
        m_entry = &entry;
    }
    ~PreparedSurfaceLease() { if (m_entry) m_entry->busy = false; }
    PreparedSurfaceLease(const PreparedSurfaceLease&) = delete;
    PreparedSurfaceLease& operator=(const PreparedSurfaceLease&) = delete;
    PreparedSurfaceEntry* Get() const { return m_entry; }
private:
    PreparedSurfaceEntry* m_entry = nullptr;
};
bool CanPrepareSurface(const TerrainComponent& terrain)
{
    return !terrain.surfacePositions.empty() && !terrain.surfaceIndices.empty()
        && terrain.surfacePositions.size() <= SURFACE_VERTEX_SCRATCH_CAPACITY
        && terrain.surfaceIndices.size() <= SURFACE_PREPARED_INDEX_CAPACITY;
}
bool GeometryMatches(const PreparedSurfaceEntry& entry, const TerrainComponent& terrain)
{
    return entry.validated && entry.vertexCount == terrain.surfacePositions.size()
        && entry.indexCount == terrain.surfaceIndices.size()
        && std::memcmp(entry.localPositions.data(), terrain.surfacePositions.data(),
            entry.vertexCount * sizeof(math::Vector3)) == 0
        && std::memcmp(entry.indices.data(), terrain.surfaceIndices.data(), entry.indexCount * sizeof(uint32_t)) == 0;
}
void StoreValidatedGeometry(PreparedSurfaceEntry& entry, const TerrainComponent& terrain)
{
    entry.validated = false;
    entry.prepared = false;
    entry.vertexCount = terrain.surfacePositions.size();
    entry.indexCount = terrain.surfaceIndices.size();
    std::memcpy(entry.localPositions.data(), terrain.surfacePositions.data(), entry.vertexCount * sizeof(math::Vector3));
    std::memcpy(entry.indices.data(), terrain.surfaceIndices.data(), entry.indexCount * sizeof(uint32_t));
    entry.validated = true;
}
bool ValidateSurfaceGeometryCached(const TerrainComponent& terrain)
{
    PreparedSurfaceLease lease(CanPrepareSurface(terrain));
    auto* entry = lease.Get();
    if (entry && GeometryMatches(*entry, terrain)) return true;
    if (!IsValidTerrainSurfaceGeometry(terrain.surfacePositions, terrain.surfaceIndices)) return false;
    if (entry) StoreValidatedGeometry(*entry, terrain);
    return true;
}
std::array<uint32_t, 10> TransformBits(const Transform& transform)
{
    return {std::bit_cast<uint32_t>(transform.worldPosition.x), std::bit_cast<uint32_t>(transform.worldPosition.y),
        std::bit_cast<uint32_t>(transform.worldPosition.z), std::bit_cast<uint32_t>(transform.worldRotation.x),
        std::bit_cast<uint32_t>(transform.worldRotation.y), std::bit_cast<uint32_t>(transform.worldRotation.z),
        std::bit_cast<uint32_t>(transform.worldRotation.w), std::bit_cast<uint32_t>(transform.worldScale.x),
        std::bit_cast<uint32_t>(transform.worldScale.y), std::bit_cast<uint32_t>(transform.worldScale.z)};
}
SurfacePreparation PrepareSurface(PreparedSurfaceEntry& entry, const TerrainComponent& terrain, const Transform& transform)
{
    if (!GeometryMatches(entry, terrain)) {
        /// @note Heightfield reporting can mutate inputs or replace this entry through a nested query; invalid indices must not reach fallback.
        if (!IsValidTerrainSurfaceGeometry(terrain.surfacePositions, terrain.surfaceIndices)) return SurfacePreparation::INVALID;
        StoreValidatedGeometry(entry, terrain);
    }
    const auto transformBits = TransformBits(transform);
    if (entry.prepared && entry.transformBits == transformBits) {
        FBZZ_PROFILE_SCOPE("TerrainSurfaceQuery::CachedProjection");
        return SurfacePreparation::PREPARED;
    }
    FBZZ_PROFILE_SCOPE("TerrainSurfaceQuery::RebuildProjection");
    entry.prepared = false;
    SurfaceVertexScratchLease lease(terrain.surfacePositions.size());
    auto* scratch = lease.Get();
    if (!scratch) return SurfacePreparation::FALLBACK;
    const auto worldPoint = [&](uint32_t index) -> const math::Vector3& {
        if (!scratch->transformed[index]) {
            scratch->worldPositions[index] = SurfaceWorldPoint(transform, terrain.surfacePositions[index]);
            scratch->transformed[index] = true;
        }
        return scratch->worldPositions[index];
    };
    entry.projectionBlocks.fill({});
    for (size_t i = 0; i < terrain.surfaceIndices.size(); i += 3) {
        const auto& wa = worldPoint(terrain.surfaceIndices[i]);
        const auto& wb = worldPoint(terrain.surfaceIndices[i + 1]);
        const auto& wc = worldPoint(terrain.surfaceIndices[i + 2]);
        entry.projections[i / 3] = PrepareProjection(wa, wb, wc);
        entry.projectionBlocks[i / (3 * SURFACE_PROJECTION_BLOCK_SIZE)].Include(entry.projections[i / 3]);
    }
    entry.transformBits = transformBits;
    entry.prepared = true;
    return SurfacePreparation::PREPARED;
}
bool SampleSurface(const TerrainComponent& terrain, const Transform& transform, const math::Vector3& reference,
    float maxDistance, SurfaceSelection selection, TerrainSurfaceSample& out)
{
    FBZZ_PROFILE_SCOPE("TerrainSurfaceQuery::SampleSurface");
    math::Vector3 normal;
    if (!terrain.enabled || !Finite(reference) || !std::isfinite(maxDistance) || maxDistance < 0
        || !Finite(transform.worldPosition) || !TryTerrainNormalToWorld(transform, math::Vector3::UP, normal)) return false;
    {
        FBZZ_PROFILE_SCOPE("TerrainSurfaceQuery::GeometryValidation");
        if (!ValidateSurfaceGeometryCached(terrain)) return false;
    }
    bool found = false;
    TerrainSurfaceSample best;
    const auto accept = [&](const TerrainSurfaceSample& sample) {
        const double delta = static_cast<double>(sample.position.y) - reference.y;
        if (selection == SurfaceSelection::FLOOR && (sample.normal.y <= 0.0001f || delta > 1e-5 || -delta > maxDistance)) return;
        if (selection == SurfaceSelection::CEILING && (sample.normal.y >= -0.0001f || delta < -1e-5 || delta > maxDistance)) return;
        const bool nearer = selection == SurfaceSelection::CEILING ? sample.position.y < best.position.y : sample.position.y > best.position.y;
        if (!found || nearer) { best = sample; found = true; }
    };
    {
        FBZZ_PROFILE_SCOPE("TerrainSurfaceQuery::Heightfield");
        /// @note Upright heightfields have one intersection per vertical line. Preserve the existing constant-time grid path.
        const auto localUp = transform.worldRotation.Inverse() * math::Vector3::UP;
        if (selection == SurfaceSelection::TOP || (localUp.x == 0 && localUp.z == 0)) {
            TerrainSurfaceSample heightfield;
            if (TrySampleHeightfieldWorld(terrain, transform, reference.x, reference.z, heightfield)) accept(heightfield);
        } else if (terrain.columns >= 2 && terrain.rows >= 2 && std::isfinite(terrain.cellSize) && terrain.cellSize > 0
            && std::isfinite(terrain.maxHeight) && terrain.heightData.size() == terrain.VertexCount()
            && (terrain.holeData.empty() || terrain.holeData.size() == terrain.CellCount())) {
            /// @note A rotated heightfield can fold across world vertical. Search each candidate to preserve floor/ceiling selection.
            const auto vertex = [&](int x, int z) { return math::Vector3{x * terrain.cellSize,
                terrain.heightData[static_cast<size_t>(z) * terrain.columns + x] * terrain.maxHeight, z * terrain.cellSize}; };
            for (int z = 0; z < terrain.rows - 1; ++z) for (int x = 0; x < terrain.columns - 1; ++x) {
                if (terrain.IsHoleCell(x, z)) continue;
                const auto a = vertex(x, z), b = vertex(x, z + 1), c = vertex(x + 1, z), d = vertex(x + 1, z + 1);
                TerrainSurfaceSample sample;
                if (VerticalTriangle(transform, a, b, c, reference.x, reference.z, sample)) accept(sample);
                if (VerticalTriangle(transform, c, b, d, reference.x, reference.z, sample)) accept(sample);
            }
        }
    }
    {
        FBZZ_PROFILE_SCOPE("TerrainSurfaceQuery::CustomSurface");
        /// @note Recompare all geometry and pose bits after heightfield callbacks; a lease never spans those callbacks.
        PreparedSurfaceLease preparedLease(CanPrepareSurface(terrain));
        auto* prepared = preparedLease.Get();
        const auto preparation = prepared ? PrepareSurface(*prepared, terrain, transform)
            : (IsValidTerrainSurfaceGeometry(terrain.surfacePositions, terrain.surfaceIndices)
                ? SurfacePreparation::FALLBACK : SurfacePreparation::INVALID);
        if (preparation == SurfacePreparation::INVALID) return false;
        if (preparation == SurfacePreparation::PREPARED) {
            const size_t triangleCount = terrain.surfaceIndices.size() / 3;
            for (size_t first = 0; first < triangleCount; first += SURFACE_PROJECTION_BLOCK_SIZE) {
                if (prepared->projectionBlocks[first / SURFACE_PROJECTION_BLOCK_SIZE].Rejects(reference.x, reference.z)) continue;
                const size_t last = (std::min)(first + SURFACE_PROJECTION_BLOCK_SIZE, triangleCount);
                for (size_t triangle = first; triangle < last; ++triangle) {
                    const size_t i = triangle * 3;
                    TerrainSurfaceSample sample;
                    if (VerticalProjectedTriangle(transform, terrain.surfacePositions[terrain.surfaceIndices[i]],
                        terrain.surfacePositions[terrain.surfaceIndices[i + 1]], terrain.surfacePositions[terrain.surfaceIndices[i + 2]],
                        prepared->projections[triangle], reference.x, reference.z, sample)) accept(sample);
                }
            }
        } else if (!terrain.surfaceIndices.empty()) {
            SurfaceVertexScratchLease lease(terrain.surfacePositions.size());
            auto* scratch = lease.Get();
            const auto worldPoint = [&](uint32_t index) -> const math::Vector3& {
                if (!scratch->transformed[index]) {
                    scratch->worldPositions[index] = SurfaceWorldPoint(transform, terrain.surfacePositions[index]);
                    scratch->transformed[index] = true;
                }
                return scratch->worldPositions[index];
            };
            for (size_t i = 0; i < terrain.surfaceIndices.size(); i += 3) {
                const auto ia = terrain.surfaceIndices[i], ib = terrain.surfaceIndices[i + 1], ic = terrain.surfaceIndices[i + 2];
                const auto& a = terrain.surfacePositions[ia];
                const auto& b = terrain.surfacePositions[ib];
                const auto& c = terrain.surfacePositions[ic];
                TerrainSurfaceSample sample;
                bool hit;
                if (scratch) {
                    const auto& wa = worldPoint(ia);
                    const auto& wb = worldPoint(ib);
                    const auto& wc = worldPoint(ic);
                    hit = VerticalWorldTriangle(transform, a, b, c, wa, wb, wc, reference.x, reference.z, sample);
                } else {
                    hit = VerticalTriangle(transform, a, b, c, reference.x, reference.z, sample);
                }
                if (hit) accept(sample);
            }
        }
    }
    if (!found) return false;
    out = best;
    return true;
}
} /// @note namespace

bool TrySampleTerrainWorld(const TerrainComponent& terrain, const Transform& transform,
    float worldX, float worldZ, TerrainSurfaceSample& out)
{
    return SampleSurface(terrain, transform, {worldX, 0, worldZ}, 0, SurfaceSelection::TOP, out);
}
bool TrySampleTerrainFloorWorld(const TerrainComponent& terrain, const Transform& transform,
    const math::Vector3& reference, float maxDistance, TerrainSurfaceSample& out)
{
    return SampleSurface(terrain, transform, reference, maxDistance, SurfaceSelection::FLOOR, out);
}
bool TrySampleTerrainCeilingWorld(const TerrainComponent& terrain, const Transform& transform,
    const math::Vector3& reference, float maxDistance, TerrainSurfaceSample& out)
{
    return SampleSurface(terrain, transform, reference, maxDistance, SurfaceSelection::CEILING, out);
}
} /// @note namespace fbzz::scene
