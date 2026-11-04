/// @file    GpuInstanceCulling.hpp
/// @brief   GPU compaction of opaque instance batches and indirect draw generation.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector4.hpp>
#include <memory>
#include <span>

namespace fbzz::math { struct Frustum; }
namespace fbzz::renderer {
class IRenderer;
class ResourceManager;
struct DrawCall;
/// @note The first 128 bytes match PerInstanceData; bounds are a conservative world-space sphere.
struct GpuCullInstance {
    math::Matrix4 world;
    math::Matrix4 worldInvTranspose;
    math::Vector4 bounds;
};
static_assert(sizeof(GpuCullInstance) == 144);

/// @note Layout matches the 144-byte deformed-instance shader input.
struct GpuDeformedInstance {
    math::Matrix4 world, worldInvTranspose;
    uint32_t vertexIndex = 0;
    uint32_t padding[3]{};
};
static_assert(sizeof(GpuDeformedInstance) == 144);

/// @note Compaction may reorder instances and is restricted to opaque, depth-writing batches without LOD fades.
class GpuInstanceCulling {
public:
    FBZZ_GRAPHICS_API GpuInstanceCulling();
    FBZZ_GRAPHICS_API ~GpuInstanceCulling();
    GpuInstanceCulling(const GpuInstanceCulling&) = delete;
    GpuInstanceCulling& operator=(const GpuInstanceCulling&) = delete;
    /// @return Success replaces the instance and indirect buffers; failure leaves call unchanged.
    /// @note GPU output counts stay on the GPU. Separate views and passes acquire separate output buffers.
    /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing - GPU-generated command buffers.
    [[nodiscard]] FBZZ_GRAPHICS_API bool Prepare(IRenderer& renderer, ResourceManager& resources, const math::Frustum& frustum,
        std::span<const GpuCullInstance> instances, DrawCall& call);
    /// @note Stable GPU compaction preserves order. Call owns the deformed-buffer dependencies.
    /// @return Success queues both culling and drawing. Failure submits nothing and leaves call unchanged.
    /// @note Native validation failure draws the original input. Transparent/Velocity paths must not call this.
    [[nodiscard]] FBZZ_GRAPHICS_API bool SubmitDeformed(IRenderer& renderer, ResourceManager& resources, const math::Frustum& frustum,
        std::span<const GpuDeformedInstance> instances, std::span<const math::Vector4> bounds, DrawCall& call);
    /// @note Queues static culling and drawing together. Index ranges are validated on the native thread before GPU work.
    [[nodiscard]] FBZZ_GRAPHICS_API bool SubmitStatic(IRenderer& renderer, ResourceManager& resources, const math::Frustum& frustum,
        std::span<const GpuCullInstance> instances, DrawCall& call);
    FBZZ_GRAPHICS_API void Release(ResourceManager& resources);
private:
    struct State;
    std::unique_ptr<State> m_state;
};
} /// @note namespace fbzz::renderer
