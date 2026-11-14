/// @file    MeshletGeometry.hpp
/// @brief   Static geometry meshlet packing and ResourceManager-owned cache.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Graphics/Api.hpp>

#include <Graphics/Renderer/ResourceHandle.hpp>
#include <Math/Vector3.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace fbzz::renderer {

class ResourceManager;
struct Vertex;
struct SkinnedVertex;

struct MeshletStreamPage {
    uint32_t depth = 0;
    std::vector<uint32_t> words;
};

inline constexpr uint32_t CLUSTER_MIN_TRIANGLES = 1024u;

/// @note Import-owned packed geometry. The GPU blob is created only for a view using cluster LOD.
struct MeshletClusterData {
    std::vector<uint8_t> bytes;
    uint32_t meshletCount = 0;
    uint32_t nodeOffsetBytes = 0;
    std::vector<uint8_t> streamMetadata;
    std::vector<uint32_t> streamTable;
    std::vector<MeshletStreamPage> streamPages;
    uint32_t streamNodeOffsetBytes = 0;
    /// @note Maximum root error in bind-local units, used to request detail before it becomes visible.
    float streamRootError = 0.0f;
    math::Vector3 streamBoundsCenter{};
    float streamBoundsRadius = 0.0f;
    float maximumSkinWeightError = 0.0f;
};

/// @brief GPU meshlet descriptor shared with MeshletGeometry.hlsli.
struct MeshletDescriptor {
    float boundsMin[4]{};
    float boundsMax[4]{};
    uint32_t vertexOffsetBytes = 0;
    uint32_t primitiveOffsetBytes = 0;
    uint32_t vertexCount = 0;
    uint32_t primitiveCount = 0;
};
static_assert(sizeof(MeshletDescriptor) == 48);

/// @brief Uploaded geometry blob, source draw parameters, and aggregate local-space bounds.
struct MeshletGeometry {
    ResourceHandle<BufferTag> blob{};
    uint32_t meshletCount = 0;
    uint32_t vertexCount = 0;
    uint32_t vertexStride = 0;
    uint32_t indexCount = 0;
    /// @note Aggregate position bounds use the source mesh's local space before world transform.
    std::array<float, 3> boundsMin{};
    std::array<float, 3> boundsMax{};
    uint32_t clusterNodeOffsetBytes = 0;
};

/// @brief Deterministic greedy partitioner for indexed triangle geometry.
class MeshletGeometryBuilder {
public:
    /// @return False when the mesh cannot be safely simplified or exceeds packing limits.
    [[nodiscard]] FBZZ_GRAPHICS_API static bool BuildClusters(std::span<const Vertex> vertices,
        std::span<const uint32_t> indices, MeshletClusterData& output);
    [[nodiscard]] FBZZ_GRAPHICS_API static bool BuildClusters(std::span<const SkinnedVertex> vertices,
        std::span<const uint32_t> indices, MeshletClusterData& output);

    /// @brief Builds contiguous meshlets with at most 64 vertices and 126 triangles.
    /// @param vertexData Interleaved vertex bytes with position.xyz at offset zero.
    /// @param vertexStride Distance between records, at least 12 bytes and a multiple of four.
    /// @param indexData Little-endian uint32 triangle indices.
    /// @param indexCount Number of indices; must be a nonzero multiple of three.
    /// @return False for invalid, non-finite, overflowing, or over-capacity input; outputs remain unchanged.
    /// @see https://github.com/microsoft/DirectXMesh/wiki/ComputeMeshlets «meshlet generation inputs and output streams»
    [[nodiscard]] FBZZ_GRAPHICS_API static bool Build(const uint8_t* vertexData, size_t vertexDataBytes,
                                    uint32_t vertexCount, uint32_t vertexStride,
                                    const uint8_t* indexData, size_t indexDataBytes,
                                    uint32_t indexCount,
                                    std::vector<uint8_t>& outBlob,
                                    uint32_t& outMeshletCount);
};

/// @brief ResourceManager-scoped cache for static meshlet blobs.
class MeshletGeometryCache {
public:
    /// @brief Reuses a blob while source handle generations and content versions match.
    /// @param resources Owner of both source buffers and the generated blob.
    /// @param vertexBuffer Raw-SRV-capable vertex buffer with position.xyz at byte offset zero.
    /// @param indexBuffer CPU-readable uint32 index buffer.
    /// @return False when input metadata, CPU shadows, geometry, or GPU capacity is invalid; output remains unchanged.
    /// @pre The caller selects static geometry with no GPU deformation.
    /// @note Cache hits use mirrored metadata; misses synchronize through ResourceManager before reading CPU shadows.
    /// @note Cached blobs are bounded by count and byte budget; least-recently-used entries are evicted.
    [[nodiscard]] FBZZ_GRAPHICS_API bool Prepare(ResourceManager& resources,
                               ResourceHandle<BufferTag> vertexBuffer,
                               ResourceHandle<BufferTag> indexBuffer,
                               uint32_t vertexCount, uint32_t vertexStride,
                               uint32_t indexCount, MeshletGeometry& outGeometry,
                               const MeshletClusterData* clusters = nullptr, bool paged = false);

    /// @brief Caches the selected terrain topology with conservative geometric normal cones.
    /// @note The source terrain vertex buffer uses the 44-byte position/normal/tangent/UV layout.
    [[nodiscard]] FBZZ_GRAPHICS_API bool PrepareTerrain(ResourceManager& resources,
                               ResourceHandle<BufferTag> vertexBuffer,
                               ResourceHandle<BufferTag> indexBuffer,
                               uint32_t indexCount, MeshletGeometry& outGeometry);

    /// @brief Invalidates entries that use a released source or blob handle.
    FBZZ_GRAPHICS_API void Invalidate(ResourceManager& resources, ResourceHandle<BufferTag> buffer);

    /// @brief Releases every cached blob through its owning ResourceManager.
    FBZZ_GRAPHICS_API void Release(ResourceManager& resources);

private:
    [[nodiscard]] bool PrepareImpl(ResourceManager& resources, ResourceHandle<BufferTag> vertexBuffer,
                                  ResourceHandle<BufferTag> indexBuffer, uint32_t vertexCount,
                                  uint32_t vertexStride, uint32_t indexCount, bool terrainCones,
                                  MeshletGeometry& outGeometry, const MeshletClusterData* clusters, bool paged);

    struct InputKey {
        ResourceHandle<BufferTag> vertexBuffer{};
        ResourceHandle<BufferTag> indexBuffer{};
        uint32_t vertexCount = 0;
        uint32_t vertexStride = 0;
        uint32_t indexCount = 0;
        bool terrainCones = false;
        bool clustered = false;
        bool paged = false;

        bool operator==(const InputKey&) const = default;
    };

    struct InputKeyHash {
        [[nodiscard]] FBZZ_GRAPHICS_API size_t operator()(const InputKey& key) const;
    };

    struct Entry {
        uint64_t vertexVersion = 0;
        uint64_t indexVersion = 0;
        uint64_t lastUseStamp = 0;
        size_t blobBytes = 0;
        MeshletGeometry geometry{};
        bool rejected = false;
    };

    std::unordered_map<InputKey, Entry, InputKeyHash> m_entries;
    mutable std::mutex m_mutex;
    uint64_t m_ownerInstanceId = 0;
    uint64_t m_resetVersion = 0;
    uint64_t m_useStamp = 0;
    size_t m_cachedBytes = 0;
    bool m_releasing = false;
};

} /// @note namespace fbzz::renderer
