/// @file    RayScene.hpp
/// @brief   Scene を参照せず構築する静的レイ形状・完全な object ID・被覆診断。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#pragma once
#include <Graphics/Renderer/AccelerationStructure.hpp>
#include <Graphics/RayTracing/RayMaterialCapabilities.hpp>
#include <Graphics/RayTracing/SurfaceMaterialData.hpp>
#include <cstdint>
#include <vector>

namespace fbzz::renderer {

struct RenderScene;

inline constexpr uint8_t RAY_PRIMARY_MASK = 1;
inline constexpr uint8_t RAY_SHADOW_MASK = 2;
inline constexpr uint8_t RAY_SPECULAR_MASK = 4;
inline constexpr uint8_t RAY_DIFFUSE_MASK = 8;

/// @note Scene の Clear / 移動代入で sceneGeneration が変わり、再利用された Entity と区別する。
struct RayObjectId {
    uint64_t sceneGeneration = 0;
    uint32_t index = 0;
    uint32_t generation = 0;
    bool operator==(const RayObjectId&) const = default;
};

/// @note ResourceManager / device 世代の異なる cache では共有しない。hash だけで同一性を決めない。
/// @note opacity と両面区分も含むため、同じ mesh の別材質で不適合な BLAS を共有しない。
struct RayGeometryKey {
    ResourceHandle<BufferTag> vertices;
    ResourceHandle<BufferTag> indices;
    uint64_t vertexContentVersion = 0;
    uint64_t indexContentVersion = 0;
    uint32_t vertexStride = 0;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t positionOffset = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    bool opaque = true;
    bool doubleSided = false;
    bool operator==(const RayGeometryKey&) const = default;
};

struct RayGeometryKeyHash {
    [[nodiscard]] size_t operator()(const RayGeometryKey& key) const;
};

struct RaySceneGeometry {
    RayGeometryKey key;
    RayTriangleGeometry triangles;
};

/// @note 一つの material slot を一つの BLAS instance にする。InstanceID はこの配列の密な添字。
/// @note mask はレイ用途。Engine の layer は収集対象集合へ反映し、8 bit へ切り詰めない。
struct RaySceneInstance {
    RayObjectId objectId;
    uint32_t geometryIndex = 0;
    uint32_t denseInstanceId = 0;
    uint32_t sourceItem = 0;
    uint32_t sourceSubmesh = 0;
    uint32_t materialSlot = 0;
    SurfaceMaterialData surface;
    math::Matrix4 world = math::Matrix4::Identity();
    math::Matrix4 previousWorld = math::Matrix4::Identity();
    uint8_t mask = RAY_PRIMARY_MASK | RAY_SHADOW_MASK | RAY_SPECULAR_MASK | RAY_DIFFUSE_MASK;
    bool doubleSided = false;
};

enum class RaySceneIssue : uint8_t {
    SCENE_ID_UNAVAILABLE,
    INVALID_OBJECT_RANGE,
    INVALID_TRANSFORM,
    INVALID_GEOMETRY,
    CONTENT_VERSION_UNAVAILABLE,
    MATERIAL_UNRESOLVED,
    SKINNED_UNSUPPORTED,
    CUSTOM_GEOMETRY_UNSUPPORTED,
    ALPHA_TEST_UNSUPPORTED,
    TRANSPARENT_UNSUPPORTED,
    NONMESH_GEOMETRY_UNSUPPORTED,
    INSTANCE_LIMIT_EXCEEDED,
    SOLID_DIELECTRIC_UNSUPPORTED,
    LOD_SELECTION_UNRESOLVED,
};

struct RaySceneDiagnostic {
    RayObjectId objectId;
    uint32_t sourceItem = UINT32_MAX;
    RaySceneIssue issue = RaySceneIssue::INVALID_GEOMETRY;
};

/// @note geometry / opacity の diagnostics と分け、遮蔽者になれる材質も surface 未対応を明示する。
struct RaySurfaceDiagnostic {
    RayObjectId objectId;
    uint32_t sourceItem = UINT32_MAX;
    SurfaceMaterialIssue issue = SurfaceMaterialIssue::UNSUPPORTED_SHADER;
};

struct RaySceneBuildOptions {
    uint32_t layerMask = UINT32_MAX;
    /// @note Path と対応済み Hybrid 輸送が明示 opt-in する。DXR opaque は alpha 評価不要を意味し、光透過を禁止しない。
    bool allowSolidDielectrics = false;
};

/// @note GPU ハンドルは非所有。BLAS / TLAS と GPU table は同じ snapshotSerial で公開する。
/// @note diagnostics が空でも surface / BSDF / 光源の完全性を証明せず、Path availability を有効にしない。
struct RayScene {
    uint64_t sceneGeneration = 0;
    uint64_t snapshotSerial = 0;
    uint64_t frameStamp = 0;
    uint32_t layerMask = UINT32_MAX;
    std::vector<RaySceneGeometry> geometries;
    std::vector<RaySceneInstance> instances;
    std::vector<RaySceneDiagnostic> diagnostics;
    std::vector<RaySurfaceDiagnostic> surfaceDiagnostics;
};

/// @note カメラの visible / lodVisible / Hi-Z / 距離を使わない。明示的な slot 非表示だけを除外する。
/// @note static layout と保証済み被覆だけ。smooth solid dielectric は明示 opt-in のみ収集する。
/// @see Docs/design/RayTracing.md §RenderScene から Ray Scene を作る
[[nodiscard]] RayScene BuildRayScene(const RenderScene& scene, RaySceneBuildOptions options = {});

[[nodiscard]] const char* DescribeRaySceneIssue(RaySceneIssue issue);

} /// @note namespace fbzz::renderer
