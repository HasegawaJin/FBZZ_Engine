/// @file    RayReflectionMediumCache.cpp
/// @brief   Exact CPU geometry snapshots and outward-rounded affine bounds for camera air proofs.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <Graphics/RayTracing/RayReflectionMediumCache.hpp>
#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace fbzz::renderer {
namespace {
using Components = std::array<double, 3>;
constexpr double FLOAT_EPSILON = 0x1p-23;
constexpr double FLOAT_MINIMUM = std::numeric_limits<float>::min();
constexpr double FLOAT_MAXIMUM = std::numeric_limits<float>::max();
static_assert(sizeof(Vertex) == 60 && offsetof(Vertex, position) == 0 && offsetof(Vertex, normal) == 12);

constexpr double Gamma(unsigned operations)
{
    return operations * FLOAT_EPSILON / (1.0 - operations * FLOAT_EPSILON);
}

bool ValidComponent(float value)
{
    return std::isfinite(value) && (value == 0 || std::abs(value) >= FLOAT_MINIMUM);
}

double Length(const Components& value)
{
    return std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
}

Components Cross(const Components& left, const Components& right)
{
    return {left[1] * right[2] - left[2] * right[1], left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0]};
}

bool SameGeometryRange(const RayGeometryKey& left, const RayGeometryKey& right)
{
    auto a = left, b = right;
    a.vertexContentVersion = b.vertexContentVersion = 0;
    a.indexContentVersion = b.indexContentVersion = 0;
    return a == b;
}

bool MatchesInput(const RaySceneGeometry& geometry)
{
    const auto& key = geometry.key;
    const auto& triangles = geometry.triangles;
    return triangles.vertices == key.vertices && triangles.indices == key.indices
        && triangles.firstVertex == key.firstVertex && triangles.vertexCount == key.vertexCount
        && triangles.positionOffset == key.positionOffset && triangles.firstIndex == key.firstIndex
        && triangles.indexCount == key.indexCount && triangles.opaque == key.opaque;
}

bool SameMedium(const SurfaceMaterialData& left, const SurfaceMaterialData& right)
{
    return IsHybridRayDielectricSupported(left) && IsHybridRayDielectricSupported(right)
        && !left.dielectric.thinWalled && !right.dielectric.thinWalled && left.dielectric == right.dielectric;
}

} /// @note namespace

bool RayReflectionMediumCache::IsOriginProvenAir(const RayScene& scene, ResourceManager& resources,
    const math::Vector3& origin)
{
    if (m_resources != &resources || m_resetVersion != resources.GetResetVersion()) {
        Reset();
        m_resources = &resources;
        m_resetVersion = resources.GetResetVersion();
    }
    return IsOriginProvenAirImpl(scene, [&](ResourceHandle<BufferTag> handle) { return resources.Get(handle); }, origin);
}

bool RayReflectionMediumCache::IsOriginProvenAir(const RayScene& scene,
    const RayReflectionMediumBufferLookup& lookup, const math::Vector3& origin)
{
    if (m_resources) Reset();
    return IsOriginProvenAirImpl(scene, lookup, origin);
}

void RayReflectionMediumCache::Reset()
{
    m_geometry.clear();
    m_resources = nullptr;
    m_resetVersion = m_sceneGeneration = 0;
}

const RayReflectionMediumCache::GeometryEntry* RayReflectionMediumCache::ReadGeometry(
    const RaySceneGeometry& geometry, const RayReflectionMediumBufferLookup& lookup)
{
    const auto& key = geometry.key;
    if (!lookup || !MatchesInput(geometry) || !key.vertices.IsValid()
        || !key.vertexContentVersion || key.vertexStride != sizeof(Vertex)
        || key.positionOffset != offsetof(Vertex, position) || !key.vertexCount
        || (key.indices.IsValid() ? !key.indexContentVersion || !key.indexCount || key.indexCount % 3 != 0
                                 : key.indexCount != 0 || key.vertexCount % 3 != 0)) return nullptr;
    const auto* vertices = lookup(key.vertices);
    const auto* indices = key.indices.IsValid() ? lookup(key.indices) : nullptr;
    if (!vertices || vertices->GetContentVersion() != key.vertexContentVersion
        || vertices->GetStride() != key.vertexStride
        || vertices->GetSize() > UINT32_MAX
        || (key.indices.IsValid() && (!indices || indices->GetContentVersion() != key.indexContentVersion
            || indices->GetStride() != 0 || indices->GetSize() > UINT32_MAX))) return nullptr;
    const uint64_t vertexBegin = static_cast<uint64_t>(key.firstVertex) * key.vertexStride;
    const uint64_t vertexEnd = (static_cast<uint64_t>(key.firstVertex) + key.vertexCount) * key.vertexStride;
    const uint64_t indexBegin = static_cast<uint64_t>(key.firstIndex) * sizeof(uint32_t);
    const uint64_t indexEnd = indexBegin + static_cast<uint64_t>(key.indexCount) * sizeof(uint32_t);
    if (vertexEnd > vertices->GetSize() || vertexEnd > std::numeric_limits<size_t>::max()
        || (indices && (indexEnd > indices->GetSize() || indexEnd > std::numeric_limits<size_t>::max()))) return nullptr;
    uint8_t byte = 0;
    /// @note Recheck actual readable ranges even on a cache hit; capacity and unchanged versions alone do not prove CPU accessibility.
    if (!vertices->CopyData(static_cast<size_t>(vertexBegin), 1, &byte)
        || !vertices->CopyData(static_cast<size_t>(vertexEnd - 1), 1, &byte)
        || (indices && (!indices->CopyData(static_cast<size_t>(indexBegin), 1, &byte)
            || !indices->CopyData(static_cast<size_t>(indexEnd - 1), 1, &byte)))
        || vertices->GetBindlessUavIndex() != INVALID_BINDLESS_INDEX
        || (indices && indices->GetBindlessUavIndex() != INVALID_BINDLESS_INDEX)) return nullptr;
    for (const auto& cached : m_geometry)
        if (cached.key == key && cached.vertices == vertices && cached.indices == indices) return &cached;

    GeometryEntry entry;
    entry.key = key; entry.vertices = vertices; entry.indices = indices;
    entry.local.lower.fill(std::numeric_limits<double>::infinity());
    entry.local.upper.fill(-std::numeric_limits<double>::infinity());
    entry.minimumNormalLength = entry.minimumCrossLength = std::numeric_limits<double>::infinity();
    entry.minimumNormalizedProjection = 1;
    struct VertexInput { Components position, normal; double normalLength = 0; };
    std::vector<VertexInput> input(key.vertexCount);
    for (uint32_t i = 0; i < key.vertexCount; ++i) {
        std::array<float, 6> attributes{};
        const uint64_t offset = vertexBegin + static_cast<uint64_t>(i) * key.vertexStride;
        if (!vertices->CopyData(static_cast<size_t>(offset), sizeof(attributes), attributes.data())) return nullptr;
        for (uint32_t axis = 0; axis < 3; ++axis) {
            if (!ValidComponent(attributes[axis]) || !ValidComponent(attributes[axis + 3])) return nullptr;
            input[i].position[axis] = attributes[axis]; input[i].normal[axis] = attributes[axis + 3];
            entry.local.lower[axis] = std::min(entry.local.lower[axis], input[i].position[axis]);
            entry.local.upper[axis] = std::max(entry.local.upper[axis], input[i].position[axis]);
        }
        const double length = Length(input[i].normal);
        if (!std::isfinite(length) || length * length < FLOAT_MINIMUM || length * length > FLOAT_MAXIMUM) return nullptr;
        input[i].normalLength = length;
        entry.minimumNormalLength = std::min(entry.minimumNormalLength, length);
        entry.maximumNormalLength = std::max(entry.maximumNormalLength, length);
    }
    std::vector<uint32_t> indexValues(key.indexCount);
    if (indices && !indices->CopyData(static_cast<size_t>(indexBegin), indexValues.size() * sizeof(uint32_t), indexValues.data()))
        return nullptr;
    for (uint32_t index : indexValues) if (index >= key.vertexCount) return nullptr;
    const uint32_t count = (indices ? key.indexCount : key.vertexCount) / 3;
    uint32_t nondegenerate = 0;
    for (uint32_t triangle = 0; triangle < count; ++triangle) {
        std::array<uint32_t, 3> index{};
        for (uint32_t vertex = 0; vertex < 3; ++vertex)
            index[vertex] = indices ? indexValues[triangle * 3 + vertex] : triangle * 3 + vertex;
        const auto& v0 = input[index[0]], &v1 = input[index[1]], &v2 = input[index[2]];
        /// @note Identical endpoint positions certify a degenerate triangle; a merely rounded zero cross product does not.
        /// @see https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#degenerate-primitives-and-instances Degenerate triangles never intersect.
        if (v0.position == v1.position || v0.position == v2.position || v1.position == v2.position) continue;
        /// @note Match the shader's subtraction/cross uncertainty before certifying that a triangle and its interpolated normals are conditioned.
        /// @see https://developer.nvidia.com/blog/solving-self-intersection-artifacts-in-directx-raytracing/ Listings 1-3, object reconstruction and normal transforms.
        Components edge1{}, edge2{}, error1{}, error2{}, crossError{};
        for (uint32_t axis = 0; axis < 3; ++axis) {
            edge1[axis] = v1.position[axis] - v0.position[axis]; edge2[axis] = v2.position[axis] - v0.position[axis];
            error1[axis] = FLOAT_EPSILON * (std::abs(v1.position[axis]) + std::abs(v0.position[axis]));
            error2[axis] = FLOAT_EPSILON * (std::abs(v2.position[axis]) + std::abs(v0.position[axis]));
        }
        const auto cross = Cross(edge1, edge2);
        for (uint32_t axis = 0; axis < 3; ++axis) {
            const auto a = (axis + 1) % 3, b = (axis + 2) % 3;
            crossError[axis] = Gamma(2) * (std::abs(edge1[a] * edge2[b]) + std::abs(edge1[b] * edge2[a]))
                + std::abs(edge1[a]) * error2[b] + std::abs(edge1[b]) * error2[a]
                + std::abs(edge2[b]) * error1[a] + std::abs(edge2[a]) * error1[b]
                + error1[a] * error2[b] + error1[b] * error2[a];
        }
        const double crossLength = Length(cross), errorLength = Length(crossError);
        if (!std::isfinite(crossLength) || crossLength == 0 || errorLength >= crossLength * 0.25) return nullptr;
        /// @note A positive raw alignment is insufficient: normalized vertex normals can nearly cancel and underflow after interpolation.
        for (const auto vertex : index) {
            const auto& normal = input[vertex].normal;
            double alignment = 0, magnitude = 0;
            for (uint32_t axis = 0; axis < 3; ++axis) {
                alignment += normal[axis] * cross[axis]; magnitude += std::abs(normal[axis] * cross[axis]);
            }
            const double normalLength = input[vertex].normalLength;
            const double projection = (alignment - Gamma(8) * magnitude - normalLength * errorLength)
                / (normalLength * crossLength);
            if (!std::isfinite(projection) || projection <= 0) return nullptr;
            entry.minimumNormalizedProjection = std::min(entry.minimumNormalizedProjection, projection);
        }
        entry.minimumCrossLength = std::min(entry.minimumCrossLength, crossLength);
        entry.maximumCrossLength = std::max(entry.maximumCrossLength, crossLength);
        entry.relativeCrossError = std::max(entry.relativeCrossError, errorLength / crossLength);
        ++nondegenerate;
    }
    if (!nondegenerate || vertices->GetContentVersion() != key.vertexContentVersion
        || (indices && indices->GetContentVersion() != key.indexContentVersion)) return nullptr;
    for (auto& cached : m_geometry)
        if (SameGeometryRange(cached.key, key)) { cached = std::move(entry); return &cached; }
    m_geometry.push_back(std::move(entry));
    return &m_geometry.back();
}

bool RayReflectionMediumCache::WorldBounds(const GeometryEntry& geometry, const math::Matrix4& world, Bounds& bounds) const
{
    for (const auto& row : world.m) for (float value : row) if (!ValidComponent(value)) return false;
    if (world.m[3][0] != 0 || world.m[3][1] != 0 || world.m[3][2] != 0 || world.m[3][3] != 1) return false;
    double linear[3][3]{}, inverse[3][3]{};
    for (uint32_t row = 0; row < 3; ++row) for (uint32_t col = 0; col < 3; ++col) linear[row][col] = world.m[row][col];
    for (uint32_t row = 0; row < 3; ++row) for (uint32_t col = 0; col < 3; ++col) {
        const auto a = (row + 1) % 3, b = (row + 2) % 3, c = (col + 1) % 3, d = (col + 2) % 3;
        inverse[col][row] = linear[a][c] * linear[b][d] - linear[a][d] * linear[b][c];
    }
    const double determinant = linear[0][0] * inverse[0][0] + linear[0][1] * inverse[1][0] + linear[0][2] * inverse[2][0];
    if (!std::isfinite(determinant) || determinant == 0) return false;
    double linearSquared = 0, inverseSquared = 0;
    for (uint32_t row = 0; row < 3; ++row) for (uint32_t col = 0; col < 3; ++col) {
        inverse[row][col] /= determinant;
        linearSquared += linear[row][col] * linear[row][col]; inverseSquared += inverse[row][col] * inverse[row][col];
    }
    const double linearNorm = std::sqrt(linearSquared), inverseNorm = std::sqrt(inverseSquared);
    const double condition = linearNorm * inverseNorm;
    /// @note An ill-conditioned inverse or normal reconstruction is unknown, not permission to skip the GPU safety checks.
    if (!std::isfinite(condition) || condition * (Gamma(64) + geometry.relativeCrossError) >= 0.125) return false;
    /// @note For A=M^-T and local separating axis g, h=normalize(M*g) gives each normalize(A*n) a projection >= localProjection/(||M||F*||A||F).
    /// @note Subtract normalized-vector and interpolation uncertainty, including slightly negative rounded edge barycentrics; uncertain inputs retain GPU initialization.
    /// @see https://pbr-book.org/4ed/Shapes/Managing_Rounding_Error Forward error and interval bounds, not an arbitrary normal-angle clamp.
    const double transformError = Gamma(64) * condition;
    const double normalizedError = 2 * transformError / (1 - transformError) + Gamma(16);
    const double interpolationLower = geometry.minimumNormalizedProjection / condition - normalizedError - Gamma(32);
    if (!std::isfinite(interpolationLower) || interpolationLower <= 0
        || interpolationLower * interpolationLower < 16 * FLOAT_MINIMUM) return false;
    const auto safeNormal = [&](double minimum, double maximum) {
        const double lower = minimum / linearNorm, upper = maximum * inverseNorm;
        return lower * lower >= 16 * FLOAT_MINIMUM && upper * upper <= FLOAT_MAXIMUM / 16;
    };
    if (!safeNormal(geometry.minimumNormalLength, geometry.maximumNormalLength)
        || !safeNormal(geometry.minimumCrossLength, geometry.maximumCrossLength)) return false;
    /// @note Float affine evaluation has three products and three sums; gamma(7) also covers the final float conversion. Absolute sums retain cancellation error.
    /// @see https://pbr-book.org/4ed/Shapes/Managing_Rounding_Error Error propagation and outward rounding.
    /// @see https://developer.nvidia.com/blog/solving-self-intersection-artifacts-in-directx-raytracing/ Listings 2 and 5, affine absolute error.
    for (uint32_t row = 0; row < 3; ++row) {
        double lower = world.m[row][3], upper = lower, magnitude = std::abs(lower);
        for (uint32_t col = 0; col < 3; ++col) {
            const double coefficient = linear[row][col];
            lower += coefficient * (coefficient >= 0 ? geometry.local.lower[col] : geometry.local.upper[col]);
            upper += coefficient * (coefficient >= 0 ? geometry.local.upper[col] : geometry.local.lower[col]);
            magnitude += std::abs(coefficient) * std::max(std::abs(geometry.local.lower[col]), std::abs(geometry.local.upper[col]));
        }
        const double padding = Gamma(7) * magnitude + 7 * FLOAT_MINIMUM;
        lower -= padding; upper += padding;
        if (!std::isfinite(lower) || !std::isfinite(upper) || lower < -FLOAT_MAXIMUM || upper > FLOAT_MAXIMUM) return false;
        const float outwardLower = std::nextafter(static_cast<float>(lower), -std::numeric_limits<float>::infinity());
        const float outwardUpper = std::nextafter(static_cast<float>(upper), std::numeric_limits<float>::infinity());
        if (!std::isfinite(outwardLower) || !std::isfinite(outwardUpper)) return false;
        bounds.lower[row] = outwardLower; bounds.upper[row] = outwardUpper;
    }
    return true;
}

bool RayReflectionMediumCache::IsOriginProvenAirImpl(const RayScene& scene,
    const RayReflectionMediumBufferLookup& lookup, const math::Vector3& origin)
{
    if (!scene.sceneGeneration || !scene.diagnostics.empty() || !scene.surfaceDiagnostics.empty()
        || !ValidComponent(origin.x) || !ValidComponent(origin.y) || !ValidComponent(origin.z)) return false;
    if (m_sceneGeneration != scene.sceneGeneration) { m_geometry.clear(); m_sceneGeneration = scene.sceneGeneration; }
    std::erase_if(m_geometry, [&](const GeometryEntry& entry) {
        return std::none_of(scene.geometries.begin(), scene.geometries.end(),
            [&](const RaySceneGeometry& geometry) { return SameGeometryRange(entry.key, geometry.key); });
    });
    const Components point{origin.x, origin.y, origin.z};
    for (size_t i = 0; i < scene.instances.size(); ++i) {
        const auto& instance = scene.instances[i];
        const auto record = MakeRaySurfaceRecord(instance.surface);
        if (instance.surface.dielectric.transmission == 0 && !instance.surface.solidDielectricSupported) continue;
        if (record.supported != 2) return false;
        if (instance.surface.dielectric.thinWalled) continue;
        if (instance.objectId.sceneGeneration != scene.sceneGeneration || instance.denseInstanceId != i
            || instance.geometryIndex >= scene.geometries.size() || (instance.mask & RAY_PRIMARY_MASK) == 0) return false;
        bool previous = false;
        for (size_t sibling = 0; sibling < scene.instances.size(); ++sibling) {
            if (scene.instances[sibling].objectId != instance.objectId) continue;
            if (!SameMedium(instance.surface, scene.instances[sibling].surface)) return false;
            previous |= sibling < i;
        }
        if (previous) continue;
        Bounds owner;
        owner.lower.fill(std::numeric_limits<double>::infinity()); owner.upper.fill(-std::numeric_limits<double>::infinity());
        for (size_t sibling = i; sibling < scene.instances.size(); ++sibling) {
            const auto& part = scene.instances[sibling];
            if (part.objectId != instance.objectId) continue;
            if (part.denseInstanceId != sibling || part.geometryIndex >= scene.geometries.size()
                || (part.mask & RAY_PRIMARY_MASK) == 0) return false;
            const auto* geometry = ReadGeometry(scene.geometries[part.geometryIndex], lookup);
            Bounds world;
            if (!geometry || !WorldBounds(*geometry, part.world, world)) return false;
            for (uint32_t axis = 0; axis < 3; ++axis) {
                owner.lower[axis] = std::min(owner.lower[axis], world.lower[axis]);
                owner.upper[axis] = std::max(owner.upper[axis], world.upper[axis]);
            }
        }
        bool outside = false;
        for (uint32_t axis = 0; axis < 3; ++axis) outside |= point[axis] < owner.lower[axis] || point[axis] > owner.upper[axis];
        if (!outside) return false;
    }
    return true;
}

} /// @note namespace fbzz::renderer
