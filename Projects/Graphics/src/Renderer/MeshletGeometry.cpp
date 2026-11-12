/// @file    MeshletGeometry.cpp
/// @brief   Static geometry meshlet partitioning and cache lifetime.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Graphics/Renderer/MeshletGeometry.hpp>
#include "StaticClusterLod.hpp"

#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Core/Logger.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>

namespace fbzz::renderer {

namespace {

constexpr uint32_t kMaximumMeshletVertices = 64;
constexpr uint32_t kMaximumMeshletPrimitives = 126;
constexpr uint32_t kMaximumMeshletCount = 65535;
constexpr size_t kMaximumBlobBytes = 256u * 1024u * 1024u;
constexpr size_t kMaximumCacheBytes = 512u * 1024u * 1024u;
constexpr size_t kMaximumCacheEntries = 2048;
constexpr uint8_t kUnassignedLocalIndex = (std::numeric_limits<uint8_t>::max)();
constexpr uint32_t kTerrainVertexStride = 44;
static_assert(sizeof(std::array<uint32_t, 3>) == 12);

struct WorkingMeshlet {
    std::vector<uint32_t> vertices;
    std::vector<std::array<uint32_t, 3>> primitives;
    std::array<float, 3> boundsMin{
        (std::numeric_limits<float>::max)(),
        (std::numeric_limits<float>::max)(),
        (std::numeric_limits<float>::max)() };
    std::array<float, 3> boundsMax{
        (std::numeric_limits<float>::lowest)(),
        (std::numeric_limits<float>::lowest)(),
        (std::numeric_limits<float>::lowest)() };
};

struct StagedDescriptor {
    MeshletDescriptor descriptor{};
    uint32_t vertexLookupElementOffset = 0;
    uint32_t primitiveElementOffset = 0;
};

struct TerrainNormalCone {
    uint32_t packedAxisCutoff;
    float apexDistance;
};

[[nodiscard]] bool CheckedMultiply(size_t left, size_t right, size_t& out)
{
    if (left != 0 && right > (std::numeric_limits<size_t>::max)() / left)
        return false;
    out = left * right;
    return true;
}

[[nodiscard]] bool CheckedAdd(size_t left, size_t right, size_t& out)
{
    if (right > (std::numeric_limits<size_t>::max)() - left)
        return false;
    out = left + right;
    return true;
}

void IncludePosition(WorkingMeshlet& meshlet, const float position[3])
{
    for (size_t axis = 0; axis < 3; ++axis) {
        meshlet.boundsMin[axis] = (std::min)(meshlet.boundsMin[axis], position[axis]);
        meshlet.boundsMax[axis] = (std::max)(meshlet.boundsMax[axis], position[axis]);
    }
}

void ResetWorkingMeshlet(WorkingMeshlet& meshlet)
{
    meshlet.vertices.clear();
    meshlet.primitives.clear();
    meshlet.boundsMin = {
        (std::numeric_limits<float>::max)(),
        (std::numeric_limits<float>::max)(),
        (std::numeric_limits<float>::max)() };
    meshlet.boundsMax = {
        (std::numeric_limits<float>::lowest)(),
        (std::numeric_limits<float>::lowest)(),
        (std::numeric_limits<float>::lowest)() };
}

[[nodiscard]] bool ReadPosition(const uint8_t* vertexData, uint32_t vertexStride,
                                uint32_t vertexIndex, float position[3])
{
    const size_t offset = static_cast<size_t>(vertexIndex) * vertexStride;
    std::memcpy(position, vertexData + offset, sizeof(float) * 3);
    return std::isfinite(position[0]) && std::isfinite(position[1]) &&
           std::isfinite(position[2]);
}

/// @note The apex lies behind every contributing triangle plane, allowing a single perspective rejection.
/// @see https://github.com/zeux/meshoptimizer/blob/master/src/meshoptimizer.h meshopt_Bounds cone culling contract.
[[nodiscard]] bool BuildTerrainNormalCone(const WorkingMeshlet& meshlet, const uint8_t* vertexData,
                                         uint32_t vertexStride, TerrainNormalCone& out)
{
    using math::Vector3;
    std::array<Vector3, kMaximumMeshletVertices> positions;
    float coordinateMagnitude = 1.0f;
    for (size_t vertex = 0; vertex < meshlet.vertices.size(); ++vertex) {
        float xyz[3];
        if (!ReadPosition(vertexData, vertexStride, meshlet.vertices[vertex], xyz)) return false;
        positions[vertex] = {xyz[0], xyz[1], xyz[2]};
        for (float value : xyz) coordinateMagnitude = std::max(coordinateMagnitude, std::abs(value));
    }

    constexpr float guard = 128.0f * std::numeric_limits<float>::epsilon();
    std::array<Vector3, kMaximumMeshletPrimitives> normals;
    Vector3 sum{};
    for (size_t primitive = 0; primitive < meshlet.primitives.size(); ++primitive) {
        const auto& indices = meshlet.primitives[primitive];
        const auto p0 = positions[indices[0]];
        const auto p1 = positions[indices[1]];
        const auto p2 = positions[indices[2]];
        /// @note Compute oriented normals in double so nearly parallel edges cannot narrow the cone through float cancellation.
        const double ax = static_cast<double>(p1.x) - p0.x;
        const double ay = static_cast<double>(p1.y) - p0.y;
        const double az = static_cast<double>(p1.z) - p0.z;
        const double bx = static_cast<double>(p2.x) - p0.x;
        const double by = static_cast<double>(p2.y) - p0.y;
        const double bz = static_cast<double>(p2.z) - p0.z;
        const double nx = ay * bz - az * by;
        const double ny = az * bx - ax * bz;
        const double nz = ax * by - ay * bx;
        const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
        const double edgeScale = std::max(ax * ax + ay * ay + az * az, bx * bx + by * by + bz * bz);
        /// @note Fail open for tiny or degenerate triangles whose winding may be unstable after transform.
        if (!std::isfinite(length) || length <= guard * edgeScale) return false;
        const Vector3 normal{static_cast<float>(nx / length), static_cast<float>(ny / length),
            static_cast<float>(nz / length)};
        normals[primitive] = normal;
        sum += normal;
    }

    const float sumLength = sum.Length();
    if (!std::isfinite(sumLength) || sumLength <= guard) return false;
    const auto unitAxis = sum / sumLength;
    const auto quantizeAxis = [](float component) {
        return static_cast<int32_t>(std::lround(std::clamp(component, -1.0f, 1.0f) * 127.0f));
    };
    const int32_t axisX = quantizeAxis(unitAxis.x);
    const int32_t axisY = quantizeAxis(unitAxis.y);
    const int32_t axisZ = quantizeAxis(unitAxis.z);
    constexpr float axisScale = 1.0f / 127.0f;
    const Vector3 axis{static_cast<float>(axisX) * axisScale, static_cast<float>(axisY) * axisScale,
        static_cast<float>(axisZ) * axisScale};
    const Vector3 low{meshlet.boundsMin[0], meshlet.boundsMin[1], meshlet.boundsMin[2]};
    const Vector3 high{meshlet.boundsMax[0], meshlet.boundsMax[1], meshlet.boundsMax[2]};
    const auto center = low * 0.5f + high * 0.5f;
    const float planeGuard = guard * coordinateMagnitude;
    float maxCross = 0.0f;
    float apexDistance = 0.0f;
    for (size_t primitive = 0; primitive < meshlet.primitives.size(); ++primitive) {
        const auto normal = normals[primitive];
        const auto point = positions[meshlet.primitives[primitive][0]];
        const float facing = Vector3::Dot(axis, normal);
        if (!std::isfinite(facing) || facing <= 0.1f) return false;
        /// @note The decoded axis need not be unit length. Its cross magnitude already scales the cutoff to match its dot product.
        maxCross = std::max(maxCross, Vector3::Cross(axis, normal).Length());
        const float distance = (Vector3::Dot(center - point, normal) + planeGuard) / facing;
        if (!std::isfinite(distance)) return false;
        apexDistance = std::max(apexDistance, distance);
    }

    const auto apex = center - axis * apexDistance;
    /// @note Preserve the half-space proof whether the shader contracts the multiply and subtract or keeps them separate.
    const Vector3 fusedApex{std::fma(-axis.x, apexDistance, center.x), std::fma(-axis.y, apexDistance, center.y),
        std::fma(-axis.z, apexDistance, center.z)};
    for (size_t primitive = 0; primitive < meshlet.primitives.size(); ++primitive) {
        const auto point = positions[meshlet.primitives[primitive][0]];
        const float side = Vector3::Dot(apex - point, normals[primitive]);
        const float fusedSide = Vector3::Dot(fusedApex - point, normals[primitive]);
        if (!std::isfinite(side) || side > -0.5f * planeGuard
            || !std::isfinite(fusedSide) || fusedSide > -0.5f * planeGuard) return false;
    }
    const uint32_t cutoff = static_cast<uint32_t>(std::ceil((maxCross + guard) * 255.0f));
    if (cutoff >= 255u) return false;
    const auto axisByte = [](int32_t component) { return static_cast<uint32_t>(component) & 0xffu; };
    out = {axisByte(axisX) | (axisByte(axisY) << 8u) | (axisByte(axisZ) << 16u) | (cutoff << 24u), apexDistance};
    return true;
}

void ReleaseBlobs(ResourceManager& resources,
                  const std::vector<ResourceHandle<BufferTag>>& handles)
{
    for (ResourceHandle<BufferTag> handle : handles) {
        if (handle.IsValid()) resources.Release(handle);
    }
}

[[nodiscard]] bool SameInputVersions(const BufferResourceMetadata& vertexMetadata,
                                     const BufferResourceMetadata& indexMetadata,
                                     uint64_t vertexVersion, uint64_t indexVersion)
{
    return vertexMetadata.contentVersion == vertexVersion &&
           indexMetadata.contentVersion == indexVersion;
}

[[nodiscard]] bool CalculateAggregateBounds(const std::vector<uint8_t>& blob,
                                            uint32_t meshletCount,
                                            std::array<float, 3>& outMin,
                                            std::array<float, 3>& outMax)
{
    size_t descriptorBytes = 0;
    if (meshletCount == 0 ||
        !CheckedMultiply(meshletCount, sizeof(MeshletDescriptor), descriptorBytes) ||
        descriptorBytes > blob.size())
        return false;

    std::array<float, 3> boundsMin{
        (std::numeric_limits<float>::max)(),
        (std::numeric_limits<float>::max)(),
        (std::numeric_limits<float>::max)() };
    std::array<float, 3> boundsMax{
        (std::numeric_limits<float>::lowest)(),
        (std::numeric_limits<float>::lowest)(),
        (std::numeric_limits<float>::lowest)() };
    for (uint32_t meshletIndex = 0; meshletIndex < meshletCount; ++meshletIndex) {
        MeshletDescriptor descriptor{};
        const size_t descriptorOffset = static_cast<size_t>(meshletIndex) * sizeof(descriptor);
        std::memcpy(&descriptor, blob.data() + descriptorOffset, sizeof(descriptor));
        for (size_t axis = 0; axis < boundsMin.size(); ++axis) {
            if (!std::isfinite(descriptor.boundsMin[axis]) ||
                !std::isfinite(descriptor.boundsMax[axis]) ||
                descriptor.boundsMin[axis] > descriptor.boundsMax[axis])
                return false;
            boundsMin[axis] = (std::min)(boundsMin[axis], descriptor.boundsMin[axis]);
            boundsMax[axis] = (std::max)(boundsMax[axis], descriptor.boundsMax[axis]);
        }
    }
    outMin = boundsMin;
    outMax = boundsMax;
    return true;
}

bool BuildMeshlets(const uint8_t* vertexData, size_t vertexDataBytes,
                                   uint32_t vertexCount, uint32_t vertexStride,
                                   const uint8_t* indexData, size_t indexDataBytes,
                                   uint32_t indexCount, bool terrainCones,
                                   std::vector<uint8_t>& outBlob,
                                   uint32_t& outMeshletCount,
                                   std::span<const StaticClusterRange> clusterRanges = {})
{
    if (!vertexData || !indexData || vertexCount == 0 || vertexStride < 12 ||
        (vertexStride & 3u) != 0 ||
        indexCount == 0 || indexCount % 3 != 0)
        return false;

    size_t requiredVertexBytes = 0;
    size_t requiredIndexBytes = 0;
    if (!CheckedMultiply(vertexCount, vertexStride, requiredVertexBytes) ||
        !CheckedMultiply(indexCount, sizeof(uint32_t), requiredIndexBytes) ||
        requiredVertexBytes > vertexDataBytes || requiredIndexBytes > indexDataBytes ||
        requiredVertexBytes > kMaximumBlobBytes ||
        requiredVertexBytes > (std::numeric_limits<uint32_t>::max)() ||
        requiredIndexBytes > kMaximumBlobBytes)
        return false;
    if (vertexCount > kMaximumBlobBytes / sizeof(uint32_t))
        return false;

    std::vector<StagedDescriptor> stagedDescriptors;
    std::vector<uint32_t> vertexLookup;
    std::vector<std::array<uint32_t, 3>> primitives;
    std::vector<uint8_t> localIndex(vertexCount, kUnassignedLocalIndex);
    WorkingMeshlet current;
    size_t clusterRangeIndex = 0;
    current.vertices.reserve(kMaximumMeshletVertices);
    current.primitives.reserve(kMaximumMeshletPrimitives);
    const size_t estimatedMeshletCount = (indexCount / 3u + kMaximumMeshletPrimitives - 1u) /
                                         kMaximumMeshletPrimitives;
    stagedDescriptors.reserve((std::min)(estimatedMeshletCount,
                                         static_cast<size_t>(kMaximumMeshletCount)));

    auto flushMeshlet = [&]() -> bool {
        if (current.primitives.empty())
            return true;
        if (stagedDescriptors.size() >= kMaximumMeshletCount ||
            vertexLookup.size() > (std::numeric_limits<uint32_t>::max)() ||
            primitives.size() > (std::numeric_limits<uint32_t>::max)())
            return false;

        StagedDescriptor staged{};
        staged.vertexLookupElementOffset = static_cast<uint32_t>(vertexLookup.size());
        staged.primitiveElementOffset = static_cast<uint32_t>(primitives.size());
        staged.descriptor.vertexCount = static_cast<uint32_t>(current.vertices.size());
        staged.descriptor.primitiveCount = static_cast<uint32_t>(current.primitives.size());
        for (size_t axis = 0; axis < 3; ++axis) {
            staged.descriptor.boundsMin[axis] = current.boundsMin[axis];
            staged.descriptor.boundsMax[axis] = current.boundsMax[axis];
        }
        if (terrainCones) {
            TerrainNormalCone cone;
            if (BuildTerrainNormalCone(current, vertexData, vertexStride, cone)) {
                /// @note Terrain uses the non-geometric bound lanes; flat static meshlets leave both lanes zero.
                staged.descriptor.boundsMin[3] = std::bit_cast<float>(cone.packedAxisCutoff);
                staged.descriptor.boundsMax[3] = cone.apexDistance;
            }
        }
        if (!clusterRanges.empty())
            staged.descriptor.boundsMin[3] = std::bit_cast<float>(clusterRanges[clusterRangeIndex].node);
        stagedDescriptors.push_back(staged);
        vertexLookup.insert(vertexLookup.end(), current.vertices.begin(), current.vertices.end());
        primitives.insert(primitives.end(), current.primitives.begin(), current.primitives.end());
        size_t descriptorBytes = 0;
        size_t lookupBytes = 0;
        size_t primitiveBytes = 0;
        size_t stagedBytes = 0;
        if (!CheckedMultiply(stagedDescriptors.size(), sizeof(MeshletDescriptor), descriptorBytes) ||
            !CheckedMultiply(vertexLookup.size(), sizeof(uint32_t), lookupBytes) ||
            !CheckedMultiply(primitives.size(), sizeof(std::array<uint32_t, 3>), primitiveBytes) ||
            !CheckedAdd(descriptorBytes, lookupBytes, stagedBytes) ||
            !CheckedAdd(stagedBytes, primitiveBytes, stagedBytes) ||
            stagedBytes > kMaximumBlobBytes)
            return false;
        for (uint32_t vertexIndex : current.vertices)
            localIndex[vertexIndex] = kUnassignedLocalIndex;
        ResetWorkingMeshlet(current);
        return true;
    };

    for (size_t firstIndex = 0; firstIndex < indexCount; firstIndex += 3) {
        if (!clusterRanges.empty() && firstIndex == clusterRanges[clusterRangeIndex].endIndex) {
            if (!flushMeshlet()) return false;
            ++clusterRangeIndex;
        }
        uint32_t triangle[3]{};
        std::memcpy(triangle, indexData + firstIndex * sizeof(uint32_t),
                    sizeof(triangle));
        for (uint32_t vertexIndex : triangle) {
            if (vertexIndex >= vertexCount)
                return false;
        }

        uint32_t additionalVertices = 0;
        for (size_t corner = 0; corner < 3; ++corner) {
            const uint32_t candidate = triangle[corner];
            if (localIndex[candidate] != kUnassignedLocalIndex)
                continue;
            bool repeated = false;
            for (size_t earlier = 0; earlier < corner; ++earlier)
                repeated = repeated || triangle[earlier] == candidate;
            if (!repeated) ++additionalVertices;
        }

        if (current.primitives.size() == kMaximumMeshletPrimitives ||
            current.vertices.size() + additionalVertices > kMaximumMeshletVertices) {
            if (!flushMeshlet())
                return false;
        }

        std::array<uint32_t, 3> localTriangle{};
        for (size_t corner = 0; corner < 3; ++corner) {
            const uint32_t vertexIndex = triangle[corner];
            uint8_t local = localIndex[vertexIndex];
            if (local == kUnassignedLocalIndex) {
                float position[3]{};
                if (!ReadPosition(vertexData, vertexStride, vertexIndex, position))
                    return false;
                local = static_cast<uint8_t>(current.vertices.size());
                localIndex[vertexIndex] = local;
                current.vertices.push_back(vertexIndex);
                IncludePosition(current, position);
            }
            localTriangle[corner] = local;
        }
        current.primitives.push_back(localTriangle);
    }

    if (!flushMeshlet() || stagedDescriptors.empty() ||
        stagedDescriptors.size() > kMaximumMeshletCount)
        return false;

    size_t descriptorBytes = 0;
    size_t vertexLookupBytes = 0;
    size_t primitiveBytes = 0;
    size_t totalBytes = 0;
    if (!CheckedMultiply(stagedDescriptors.size(), sizeof(MeshletDescriptor), descriptorBytes) ||
        !CheckedMultiply(vertexLookup.size(), sizeof(uint32_t), vertexLookupBytes) ||
        !CheckedMultiply(primitives.size(), sizeof(std::array<uint32_t, 3>), primitiveBytes) ||
        !CheckedAdd(descriptorBytes, vertexLookupBytes, totalBytes) ||
        !CheckedAdd(totalBytes, primitiveBytes, totalBytes) ||
        totalBytes == 0 || totalBytes > kMaximumBlobBytes ||
        totalBytes > (std::numeric_limits<uint32_t>::max)() ||
        (totalBytes & 3u) != 0)
        return false;

    std::vector<MeshletDescriptor> descriptors;
    descriptors.reserve(stagedDescriptors.size());
    for (const StagedDescriptor& staged : stagedDescriptors) {
        const size_t vertexOffset = descriptorBytes +
            static_cast<size_t>(staged.vertexLookupElementOffset) * sizeof(uint32_t);
        const size_t primitiveOffset = descriptorBytes + vertexLookupBytes +
            static_cast<size_t>(staged.primitiveElementOffset) * sizeof(std::array<uint32_t, 3>);
        if (vertexOffset > (std::numeric_limits<uint32_t>::max)() ||
            primitiveOffset > (std::numeric_limits<uint32_t>::max)())
            return false;
        MeshletDescriptor descriptor = staged.descriptor;
        descriptor.vertexOffsetBytes = static_cast<uint32_t>(vertexOffset);
        descriptor.primitiveOffsetBytes = static_cast<uint32_t>(primitiveOffset);
        descriptors.push_back(descriptor);
    }

    std::vector<uint8_t> blob(totalBytes);
    std::memcpy(blob.data(), descriptors.data(), descriptorBytes);
    std::memcpy(blob.data() + descriptorBytes, vertexLookup.data(), vertexLookupBytes);
    std::memcpy(blob.data() + descriptorBytes + vertexLookupBytes,
                primitives.data(), primitiveBytes);
    outBlob.swap(blob);
    outMeshletCount = static_cast<uint32_t>(descriptors.size());
    return true;
}

} /// @note namespace

bool MeshletGeometryBuilder::BuildClusters(std::span<const Vertex> vertices,
    std::span<const uint32_t> indices, MeshletClusterData& output)
{
    StaticClusterBuild hierarchy;
    if (!BuildStaticClusterLod(vertices, indices, hierarchy)) return false;
    const auto* vertexBytes = static_cast<const uint8_t*>(static_cast<const void*>(vertices.data()));
    const auto* indexBytes = static_cast<const uint8_t*>(static_cast<const void*>(hierarchy.indices.data()));
    MeshletClusterData packed;
    if (!BuildMeshlets(vertexBytes, vertices.size_bytes(), static_cast<uint32_t>(vertices.size()), sizeof(Vertex),
        indexBytes, hierarchy.indices.size() * sizeof(uint32_t), static_cast<uint32_t>(hierarchy.indices.size()),
        false, packed.bytes, packed.meshletCount, hierarchy.ranges)) return false;
    const size_t nodeBytes = hierarchy.nodes.size() * sizeof(StaticClusterNode);
    if (nodeBytes > kMaximumBlobBytes - packed.bytes.size()) return false;
    packed.nodeOffsetBytes = static_cast<uint32_t>(packed.bytes.size());
    packed.bytes.resize(packed.bytes.size() + nodeBytes);
    std::memcpy(packed.bytes.data() + packed.nodeOffsetBytes, hierarchy.nodes.data(), nodeBytes);
    output = std::move(packed);
    return true;
}

bool MeshletGeometryBuilder::Build(const uint8_t* vertexData, size_t vertexDataBytes,
                                   uint32_t vertexCount, uint32_t vertexStride,
                                   const uint8_t* indexData, size_t indexDataBytes,
                                   uint32_t indexCount,
                                   std::vector<uint8_t>& outBlob,
                                   uint32_t& outMeshletCount)
{
    return BuildMeshlets(vertexData, vertexDataBytes, vertexCount, vertexStride, indexData,
        indexDataBytes, indexCount, false, outBlob, outMeshletCount);
}

size_t MeshletGeometryCache::InputKeyHash::operator()(const InputKey& key) const
{
    size_t hash = static_cast<size_t>(key.vertexBuffer.id);
    const auto combine = [&hash](uint64_t value) {
        hash ^= std::hash<uint64_t>{}(value) + static_cast<size_t>(0x9e3779b97f4a7c15ull) +
                (hash << 6) + (hash >> 2);
    };
    combine(key.vertexBuffer.gen);
    combine(key.indexBuffer.id);
    combine(key.indexBuffer.gen);
    combine(key.vertexCount);
    combine(key.vertexStride);
    combine(key.indexCount);
    combine(key.terrainCones);
    combine(key.clustered);
    return hash;
}

bool MeshletGeometryCache::Prepare(ResourceManager& resources,
                                   ResourceHandle<BufferTag> vertexBuffer,
                                   ResourceHandle<BufferTag> indexBuffer,
                                   uint32_t vertexCount, uint32_t vertexStride,
                                   uint32_t indexCount, MeshletGeometry& outGeometry,
                                   const MeshletClusterData* clusters)
{
    return PrepareImpl(resources, vertexBuffer, indexBuffer, vertexCount, vertexStride,
        indexCount, false, outGeometry, clusters);
}

bool MeshletGeometryCache::PrepareTerrain(ResourceManager& resources,
    ResourceHandle<BufferTag> vertexBuffer, ResourceHandle<BufferTag> indexBuffer,
    uint32_t indexCount, MeshletGeometry& outGeometry)
{
    return PrepareImpl(resources, vertexBuffer, indexBuffer, 0, kTerrainVertexStride,
        indexCount, true, outGeometry, nullptr);
}

bool MeshletGeometryCache::PrepareImpl(ResourceManager& resources,
    ResourceHandle<BufferTag> vertexBuffer, ResourceHandle<BufferTag> indexBuffer,
    uint32_t vertexCount, uint32_t vertexStride, uint32_t indexCount,
    bool terrainCones, MeshletGeometry& outGeometry, const MeshletClusterData* clusters)
{
    if (!vertexBuffer.IsValid() || !indexBuffer.IsValid() || (!terrainCones && vertexCount == 0) ||
        vertexStride < 12 || (vertexStride & 3u) != 0 ||
        indexCount == 0 || indexCount % 3 != 0 ||
        static_cast<uint64_t>(indexCount / 3u) >
            static_cast<uint64_t>(kMaximumMeshletCount) * kMaximumMeshletPrimitives) {
        return false;
    }

    BufferResourceMetadata vertexMetadata{};
    BufferResourceMetadata indexMetadata{};
    if (!resources.GetBufferMetadata(vertexBuffer, vertexMetadata) ||
        !resources.GetBufferMetadata(indexBuffer, indexMetadata) ||
        !vertexMetadata.cpuReadable || vertexMetadata.gpuWritable || vertexMetadata.indexBuffer ||
        !indexMetadata.cpuReadable || indexMetadata.gpuWritable || !indexMetadata.indexBuffer ||
        vertexMetadata.stride != vertexStride || vertexMetadata.contentVersion == 0 ||
        indexMetadata.contentVersion == 0) {
        return false;
    }

    if (terrainCones) {
        const size_t count = vertexMetadata.sizeBytes / vertexStride;
        if (vertexMetadata.sizeBytes % vertexStride != 0 || count == 0 || count > UINT32_MAX) return false;
        vertexCount = static_cast<uint32_t>(count);
    }
    /// @note A packed import hierarchy indexes the original VB/IB contents. Updated buffers use the legacy path.
    if (clusters && (vertexMetadata.contentVersion != 1 || indexMetadata.contentVersion != 1)) return false;

    size_t requiredVertexBytes = 0;
    size_t requiredIndexBytes = 0;
    if (!CheckedMultiply(vertexCount, vertexStride, requiredVertexBytes) ||
        !CheckedMultiply(indexCount, sizeof(uint32_t), requiredIndexBytes) ||
        requiredVertexBytes > vertexMetadata.sizeBytes ||
        requiredIndexBytes > indexMetadata.sizeBytes ||
        requiredVertexBytes > kMaximumBlobBytes ||
        requiredVertexBytes > (std::numeric_limits<uint32_t>::max)() ||
        (vertexMetadata.sizeBytes & 3u) != 0) {
        return false;
    }

    const InputKey key{ vertexBuffer, indexBuffer, vertexCount, vertexStride, indexCount, terrainCones, clusters != nullptr };
    std::vector<ResourceHandle<BufferTag>> releaseHandles;
    {
        std::lock_guard lock(m_mutex);
        if (m_releasing)
            return false;
        if (m_ownerInstanceId == 0) {
            m_ownerInstanceId = resources.GetInstanceId();
            m_resetVersion = resources.GetResetVersion();
        } else if (m_ownerInstanceId != resources.GetInstanceId() ||
                   m_resetVersion != resources.GetResetVersion()) {
            FBZZ_LOG_ERROR("MeshletGeometryCache: called with a different ResourceManager lifetime");
            return false;
        }

        auto found = m_entries.find(key);
        if (found != m_entries.end() &&
            found->second.vertexVersion == vertexMetadata.contentVersion &&
            found->second.indexVersion == indexMetadata.contentVersion) {
            found->second.lastUseStamp = ++m_useStamp;
            if (found->second.rejected)
                return false;
            outGeometry = found->second.geometry;
            return true;
        }
        if (found != m_entries.end()) {
            m_cachedBytes -= found->second.blobBytes;
            if (found->second.geometry.blob.IsValid())
                releaseHandles.push_back(found->second.geometry.blob);
            m_entries.erase(found);
        }
    }
    ReleaseBlobs(resources, releaseHandles);

    IBuffer* sourceVertexBuffer = resources.Get(vertexBuffer);
    if (!sourceVertexBuffer || sourceVertexBuffer->GetBindlessSrvIndex() == INVALID_BINDLESS_INDEX) return false;

    std::vector<uint8_t> generated;
    uint32_t meshletCount = 0;
    bool validBlob = true;
    if (clusters) {
        meshletCount = clusters->meshletCount;
    } else {
        std::vector<uint8_t> vertexBytes(requiredVertexBytes);
        std::vector<uint8_t> indexBytes(requiredIndexBytes);
        IBuffer* sourceIndexBuffer = resources.Get(indexBuffer);
        if (!sourceIndexBuffer || !sourceVertexBuffer->CopyData(0, requiredVertexBytes, vertexBytes.data())
            || !sourceIndexBuffer->CopyData(0, requiredIndexBytes, indexBytes.data())) return false;
        validBlob = BuildMeshlets(vertexBytes.data(), vertexBytes.size(), vertexCount, vertexStride,
            indexBytes.data(), indexBytes.size(), indexCount, terrainCones, generated, meshletCount);
    }
    const auto& blobBytes = clusters ? clusters->bytes : generated;

    BufferResourceMetadata verifiedVertexMetadata{};
    BufferResourceMetadata verifiedIndexMetadata{};
    if (!resources.GetBufferMetadata(vertexBuffer, verifiedVertexMetadata) ||
        !resources.GetBufferMetadata(indexBuffer, verifiedIndexMetadata) ||
        !SameInputVersions(verifiedVertexMetadata, verifiedIndexMetadata,
                           vertexMetadata.contentVersion, indexMetadata.contentVersion))
        return false;

    if (!validBlob) {
        std::vector<ResourceHandle<BufferTag>> evictedHandles;
        bool reusedGeometry = false;
        bool wasRejected = false;
        bool rememberedFailure = false;
        {
            std::lock_guard lock(m_mutex);
            if (!m_releasing && m_ownerInstanceId == resources.GetInstanceId() &&
                m_resetVersion == resources.GetResetVersion()) {
                auto found = m_entries.find(key);
                if (found != m_entries.end() &&
                    found->second.vertexVersion == vertexMetadata.contentVersion &&
                    found->second.indexVersion == indexMetadata.contentVersion) {
                    found->second.lastUseStamp = ++m_useStamp;
                    wasRejected = found->second.rejected;
                    if (!wasRejected) {
                        outGeometry = found->second.geometry;
                        reusedGeometry = true;
                    }
                } else {
                    if (found != m_entries.end()) {
                        m_cachedBytes -= found->second.blobBytes;
                        if (found->second.geometry.blob.IsValid())
                            evictedHandles.push_back(found->second.geometry.blob);
                        m_entries.erase(found);
                    }
                    while (m_entries.size() >= kMaximumCacheEntries && !m_entries.empty()) {
                        auto oldest = std::min_element(m_entries.begin(), m_entries.end(),
                            [](const auto& left, const auto& right) {
                                return left.second.lastUseStamp < right.second.lastUseStamp;
                            });
                        if (oldest == m_entries.end()) break;
                        m_cachedBytes -= oldest->second.blobBytes;
                        if (oldest->second.geometry.blob.IsValid())
                            evictedHandles.push_back(oldest->second.geometry.blob);
                        m_entries.erase(oldest);
                    }
                    if (m_entries.size() < kMaximumCacheEntries) {
                        Entry entry{};
                        entry.vertexVersion = vertexMetadata.contentVersion;
                        entry.indexVersion = indexMetadata.contentVersion;
                        entry.lastUseStamp = ++m_useStamp;
                        entry.rejected = true;
                        m_entries.emplace(key, entry);
                        rememberedFailure = true;
                    }
                }
            }
        }
        ReleaseBlobs(resources, evictedHandles);
        if (reusedGeometry)
            return true;
        if (rememberedFailure)
            FBZZ_LOG_WARN("MeshletGeometryCache: geometry failed meshlet validation or blob capacity");
        return false;
    }

    std::array<float, 3> aggregateBoundsMin{};
    std::array<float, 3> aggregateBoundsMax{};
    if (!CalculateAggregateBounds(blobBytes, meshletCount,
                                  aggregateBoundsMin, aggregateBoundsMax)) {
        FBZZ_LOG_ERROR("MeshletGeometryCache: packed descriptors contain invalid aggregate bounds");
        return false;
    }

    if (blobBytes.size() > kMaximumCacheBytes)
        return false;
    ResourceHandle<BufferTag> blob = resources.CreateVertexBuffer(
        blobBytes.data(), blobBytes.size(), sizeof(uint32_t));
    if (!blob.IsValid()) {
        FBZZ_LOG_WARN("MeshletGeometryCache: backend failed to create the packed blob buffer");
        return false;
    }

    IBuffer* uploadedBlob = resources.Get(blob);
    if (!uploadedBlob || uploadedBlob->GetBindlessSrvIndex() == INVALID_BINDLESS_INDEX) {
        resources.Release(blob);
        return false;
    }

    BufferResourceMetadata finalVertexMetadata{};
    BufferResourceMetadata finalIndexMetadata{};
    if (!resources.GetBufferMetadata(vertexBuffer, finalVertexMetadata) ||
        !resources.GetBufferMetadata(indexBuffer, finalIndexMetadata) ||
        !SameInputVersions(finalVertexMetadata, finalIndexMetadata,
                           vertexMetadata.contentVersion, indexMetadata.contentVersion)) {
        resources.Release(blob);
        return false;
    }

    MeshletGeometry geometry{ blob, meshletCount, vertexCount, vertexStride, indexCount };
    geometry.boundsMin = aggregateBoundsMin;
    geometry.boundsMax = aggregateBoundsMax;
    geometry.clusterNodeOffsetBytes = clusters ? clusters->nodeOffsetBytes : 0;
    std::vector<ResourceHandle<BufferTag>> evictedHandles;
    bool stored = false;
    bool accepted = false;
    {
        std::lock_guard lock(m_mutex);
        if (!m_releasing && m_ownerInstanceId == resources.GetInstanceId() &&
            m_resetVersion == resources.GetResetVersion()) {
            auto found = m_entries.find(key);
            if (found != m_entries.end() &&
                found->second.vertexVersion == vertexMetadata.contentVersion &&
                found->second.indexVersion == indexMetadata.contentVersion) {
                found->second.lastUseStamp = ++m_useStamp;
                outGeometry = found->second.geometry;
                accepted = true;
            } else {
                if (found != m_entries.end()) {
                    m_cachedBytes -= found->second.blobBytes;
                    if (found->second.geometry.blob.IsValid())
                        evictedHandles.push_back(found->second.geometry.blob);
                    m_entries.erase(found);
                }
                while (!m_entries.empty() &&
                       (m_entries.size() >= kMaximumCacheEntries ||
                        blobBytes.size() > kMaximumCacheBytes - m_cachedBytes)) {
                    auto oldest = std::min_element(m_entries.begin(), m_entries.end(),
                        [](const auto& left, const auto& right) {
                            return left.second.lastUseStamp < right.second.lastUseStamp;
                        });
                    if (oldest == m_entries.end()) break;
                    m_cachedBytes -= oldest->second.blobBytes;
                    if (oldest->second.geometry.blob.IsValid())
                        evictedHandles.push_back(oldest->second.geometry.blob);
                    m_entries.erase(oldest);
                }
                if (m_entries.size() < kMaximumCacheEntries &&
                    blobBytes.size() <= kMaximumCacheBytes - m_cachedBytes) {
                    Entry entry{};
                    entry.vertexVersion = vertexMetadata.contentVersion;
                    entry.indexVersion = indexMetadata.contentVersion;
                    entry.lastUseStamp = ++m_useStamp;
                    entry.blobBytes = blobBytes.size();
                    entry.geometry = geometry;
                    m_entries.emplace(key, entry);
                    m_cachedBytes += blobBytes.size();
                    outGeometry = geometry;
                    stored = true;
                    accepted = true;
                }
            }
        }
    }
    ReleaseBlobs(resources, evictedHandles);
    if (!stored)
        resources.Release(blob);
    return accepted;
}

void MeshletGeometryCache::Invalidate(ResourceManager& resources,
                                      ResourceHandle<BufferTag> buffer)
{
    std::vector<ResourceHandle<BufferTag>> releaseHandles;
    {
        std::lock_guard lock(m_mutex);
        if (m_ownerInstanceId != 0 && m_ownerInstanceId != resources.GetInstanceId()) {
            FBZZ_LOG_ERROR("MeshletGeometryCache: refused invalidation through a different ResourceManager");
            return;
        }
        for (auto entry = m_entries.begin(); entry != m_entries.end();) {
            const bool usesBuffer = entry->first.vertexBuffer == buffer ||
                                    entry->first.indexBuffer == buffer;
            const bool isBlob = entry->second.geometry.blob == buffer;
            if (!usesBuffer && !isBlob) {
                ++entry;
                continue;
            }
            m_cachedBytes -= entry->second.blobBytes;
            const ResourceHandle<BufferTag> blob = entry->second.geometry.blob;
            if (blob.IsValid() && blob != buffer)
                releaseHandles.push_back(blob);
            entry = m_entries.erase(entry);
        }
    }
    ReleaseBlobs(resources, releaseHandles);
}

void MeshletGeometryCache::Release(ResourceManager& resources)
{
    std::vector<ResourceHandle<BufferTag>> releaseHandles;
    {
        std::lock_guard lock(m_mutex);
        if (m_ownerInstanceId != 0 && m_ownerInstanceId != resources.GetInstanceId()) {
            FBZZ_LOG_ERROR("MeshletGeometryCache: refused release through a different ResourceManager");
            return;
        }
        const bool sameReset = m_ownerInstanceId == 0 ||
                               m_resetVersion == resources.GetResetVersion();
        m_releasing = true;
        releaseHandles.reserve(m_entries.size());
        for (const auto& [key, entry] : m_entries) {
            static_cast<void>(key);
            if (sameReset && entry.geometry.blob.IsValid())
                releaseHandles.push_back(entry.geometry.blob);
        }
        m_entries.clear();
        m_cachedBytes = 0;
        m_ownerInstanceId = 0;
        m_resetVersion = 0;
        m_useStamp = 0;
    }
    ReleaseBlobs(resources, releaseHandles);
    {
        std::lock_guard lock(m_mutex);
        m_releasing = false;
    }
}

} /// @note namespace fbzz::renderer
