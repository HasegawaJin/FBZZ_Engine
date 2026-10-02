/// @file    AccelerationStructure.hpp
/// @brief   Backend-independent immutable triangle BLAS and instance TLAS contracts.
/// @author  Hasegawa Jin
/// @date    2026-09-30
#pragma once
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <Math/Matrix4.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::renderer {

enum class AccelerationStructureKind : uint8_t {
    BOTTOM_LEVEL,
    TOP_LEVEL,
};

/// @note Positions are float3 in object space; indices are uint32 relative to firstVertex.
/// @pre Position components must be finite; indices must be less than vertexCount.
/// @note Non-indexed geometry uses vertexCount / 3 triangles. Opaque skips candidate alpha testing.
struct RayTriangleGeometry {
    ResourceHandle<BufferTag> vertices;
    ResourceHandle<BufferTag> indices;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t positionOffset = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    bool opaque = true;
};

/// @note transform uses Math's column-vector convention and must be finite, affine and invertible.
/// @note instanceId is a dense table index limited to 24 bits; mask contains ray purposes, not Engine layers.
/// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_raytracing_instance_desc DXR instance layout
struct RayInstance {
    ResourceHandle<AccelerationStructureTag> bottomLevel;
    math::Matrix4 transform = math::Matrix4::Identity();
    uint32_t instanceId = 0;
    uint8_t mask = 0xFF;
    bool doubleSided = false;
    bool frontCounterClockwise = false;
};

/// @note Exactly one nonempty input array must match kind. Inputs are copied at creation.
/// @note Static structures prefer tracing speed; update and compaction are not supported yet.
struct AccelerationStructureDesc {
    AccelerationStructureKind kind = AccelerationStructureKind::BOTTOM_LEVEL;
    std::vector<RayTriangleGeometry> geometries;
    std::vector<RayInstance> instances;
};

class IAccelerationStructure {
public:
    virtual ~IAccelerationStructure() = default;
    [[nodiscard]] virtual AccelerationStructureKind GetKind() const = 0;
    [[nodiscard]] virtual size_t GetSize() const = 0;
    /// @note True after a successful build recording; completion follows the renderer's queue order.
    [[nodiscard]] virtual bool IsBuilt() const = 0;
    /// @return Only a built TLAS exposes a shader-readable descriptor; otherwise UINT32_MAX.
    [[nodiscard]] virtual uint32_t GetBindlessIndex() const = 0;
};

}
