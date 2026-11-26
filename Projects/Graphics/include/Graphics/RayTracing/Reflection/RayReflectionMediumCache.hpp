/// @file    RayReflectionMediumCache.hpp
/// @brief   CPU-readable solid geometry bounds proving that one camera origin starts in air.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/RayTracing/Scene/RayScene.hpp>
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
    [[nodiscard]] FBZZ_GRAPHICS_API bool IsOriginProvenAir(const RayScene& scene, ResourceManager& resources,
                                       const math::Vector3& origin);
    /// @pre The lookup and CPU buffer updates run on the rendering thread; unchanged nonzero versions denote unchanged content.
    [[nodiscard]] FBZZ_GRAPHICS_API bool IsOriginProvenAir(const RayScene& scene, const RayReflectionMediumBufferLookup& lookup,
                                       const math::Vector3& origin);
    FBZZ_GRAPHICS_API void Reset();

private:
    struct Bounds {
        std::array<double, 3> lower{}; ///< 境界箱の各軸の下端。座標空間は境界を作る処理で決まる。
        std::array<double, 3> upper{}; ///< 境界箱の各軸の上端。座標空間は境界を作る処理で決まる。
    };
    struct GeometryEntry {
        RayGeometryKey key; ///< 読み出した頂点・添字資源と内容版を照合するキー。
        const IBuffer* vertices = nullptr; ///< 頂点の読み出し元と照合する非所有ポインター。
        const IBuffer* indices = nullptr; ///< インデックスの読み出し元と照合する非所有ポインター。非インデックス形状では null。
        Bounds local; ///< 頂点位置から求めたローカル空間の境界箱。
        double minimumNormalLength = 0; ///< 頂点に記録された法線のうち長さが最小の値。
        double maximumNormalLength = 0; ///< 頂点に記録された法線のうち長さが最大の値。
        double minimumCrossLength = 0; ///< 非退化三角形の辺の外積の長さの最小値。
        double maximumCrossLength = 0; ///< 非退化三角形の辺の外積の長さの最大値。
        double relativeCrossError = 0; ///< 辺の外積の長さに対する丸め誤差の相対上界。
        double minimumNormalizedProjection = 0; ///< 頂点法線を面法線の方向へ射影した正規化値の下界。
    };
    [[nodiscard]] FBZZ_GRAPHICS_API bool IsOriginProvenAirImpl(const RayScene& scene,
        const RayReflectionMediumBufferLookup& lookup, const math::Vector3& origin);
    [[nodiscard]] FBZZ_GRAPHICS_API const GeometryEntry* ReadGeometry(const RaySceneGeometry& geometry,
        const RayReflectionMediumBufferLookup& lookup);
    [[nodiscard]] FBZZ_GRAPHICS_API bool WorldBounds(const GeometryEntry& geometry, const math::Matrix4& world, Bounds& bounds) const;
    std::vector<GeometryEntry> m_geometry; ///< 内容版が一致する場合に再利用するローカル形状検証結果。
    const ResourceManager* m_resources = nullptr; ///< キャッシュの有効期間を照合する資源管理器の非所有参照。
    uint64_t m_resetVersion = 0; ///< 資源管理器がリセットされる前後を区別する版。
    uint64_t m_sceneGeneration = 0; ///< 検証結果を作ったシーンの世代。
};

} // namespace fbzz::renderer
