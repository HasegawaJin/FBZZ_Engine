/// @file    RayReflectionMediumCache.hpp
/// @brief   CPU-readable solid geometry bounds proving that one camera origin starts in air.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#pragma once
#include <Graphics/RayTracing/RayScene.hpp>
#include <array>
#include <functional>
#include <vector>

namespace fbzz::renderer {

class IBuffer;
class ResourceManager;
using RayReflectionMediumBufferLookup = std::function<const IBuffer*(ResourceHandle<BufferTag>)>;

/// @note Only the supplied origin is proved; orthographic pixel origins and secondary ray origins require independent initialization.
/// @note Authored closed, consistently wound solids remain a transport precondition; this cache does not validate watertight topology.
class RayReflectionMediumCache {
public:
    /// @return true only when the origin is strictly outside every validated solid owner's conservative world bounds; false means unknown or inside.
    /// @note Uses current live CPU-readable, non-UAV buffer versions and never reads back GPU-only geometry.
    [[nodiscard]] bool IsOriginProvenAir(const RayScene& scene, ResourceManager& resources,
                                       const math::Vector3& origin);
    /// @pre The lookup and CPU buffer updates run on the rendering thread; unchanged nonzero versions denote unchanged content.
    [[nodiscard]] bool IsOriginProvenAir(const RayScene& scene, const RayReflectionMediumBufferLookup& lookup,
                                       const math::Vector3& origin);
    void Reset();

private:
    struct Bounds {
        std::array<double, 3> lower{}, upper{};
    };
    struct GeometryEntry {
        RayGeometryKey key;
        const IBuffer* vertices = nullptr;
        const IBuffer* indices = nullptr;
        Bounds local;
        double minimumNormalLength = 0, maximumNormalLength = 0;
        double minimumCrossLength = 0, maximumCrossLength = 0;
        double relativeCrossError = 0;
        double minimumNormalizedProjection = 0;
    };
    [[nodiscard]] bool IsOriginProvenAirImpl(const RayScene& scene,
        const RayReflectionMediumBufferLookup& lookup, const math::Vector3& origin);
    [[nodiscard]] const GeometryEntry* ReadGeometry(const RaySceneGeometry& geometry,
        const RayReflectionMediumBufferLookup& lookup);
    [[nodiscard]] bool WorldBounds(const GeometryEntry& geometry, const math::Matrix4& world, Bounds& bounds) const;
    std::vector<GeometryEntry> m_geometry;
    const ResourceManager* m_resources = nullptr;
    uint64_t m_resetVersion = 0;
    uint64_t m_sceneGeneration = 0;
};

} /// @note namespace fbzz::renderer
