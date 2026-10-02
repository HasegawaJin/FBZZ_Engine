/// @file    RayPathScene.hpp
/// @brief   Path の内容版と CPU snapshot から求める発光三角形の選択分布。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once
#include <Graphics/RayTracing/RayScene.hpp>
#include <Graphics/RayTracing/RayLightInput.hpp>
#include <array>
#include <cstddef>
#include <functional>
#include <vector>

namespace fbzz::renderer {

class IBuffer;
class ResourceManager;

/// @note 放射輝度は scene-linear RGB。environment は初期段の一定背景だけを表す。
/// @note 未対応の光源・環境を supported=false とし、無視して積分しない。
struct RayPathLighting {
    math::Vector3 direction{0, -1, 0};
    math::Vector3 directionalRadiance{};
    math::Vector3 environmentRadiance{};
    uint64_t environmentContentVersion = 0;
    float environmentRotation = 0;
    float environmentIntensity = 1;
    bool supported = true;
    /// @note true のとき全光源を lights から積分し、legacy single directional は二重に加算しない。
    std::vector<RayLightInput> lights;
    bool useSceneLights = false;
};

inline constexpr uint32_t RAY_PATH_EMITTER_VIRTUAL = 1;
inline constexpr uint32_t RAY_PATH_EMITTER_TWO_SIDED = 2;
inline constexpr uint32_t RAY_PATH_EMITTER_MESH_LIGHT_PROXY = 4;

/// @note t3 の StructuredBuffer 要素は 112 bytes。頂点と法線はワールド空間。
/// @note 法線は object winding の逆転置方向。負 determinant の instance でも DXR の front 面と一致する。
/// @note selectionPdf は離散選択確率。面積密度は selectionPdf / area、立体角密度は面積密度 * distance^2 / abs(cosTheta)。
/// @note instanceId / primitiveId はヒット側の emission MIS で同じ選択確率を逆引きする。
/// @note VIRTUAL は instanceId=UINT32_MAX、primitiveId は表内の一意添字。MESH_LIGHT_PROXY owner の非一致面は emission=0。
/// @see https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer PBRT, emission and light-sampling MIS
struct RayPathEmitterRecord {
    std::array<float, 3> v0{};
    float area = 0;
    std::array<float, 3> edge1{};
    uint32_t instanceId = 0;
    std::array<float, 3> edge2{};
    uint32_t primitiveId = 0;
    std::array<float, 3> emission{};
    float selectionPdf = 0;
    std::array<float, 3> geometricNormal{};
    float selectionCdf = 0;
    /// @note range=0 は無限。正の range は NEE と BSDF-hit の直前区間へ同じ artist smooth cutoff を適用する。
    float range = 0;
    uint32_t flags = 0;
    uint32_t objectIndex = 0;
    uint32_t objectGeneration = 0;
    /// @note Hybrid の直接光だけが使う artist shadow strength。通常の mesh emission と Reference は物理遮蔽を保つ。
    float shadowStrength = 1;
    std::array<uint32_t, 3> reserved{};
    bool operator==(const RayPathEmitterRecord&) const = default;
};
static_assert(sizeof(RayPathEmitterRecord) == 112);
static_assert(offsetof(RayPathEmitterRecord, shadowStrength) == 96);

/// @note t5 の StructuredBuffer 要素は 64 bytes。type は 0=Point、1=Spot、2=Directional。
/// @note Point / Spot は delta。range=0 は無限、正の range は既存 smooth window、距離二乗は 0.01 で下限を設ける。
struct RayPathDeltaRecord {
    std::array<float, 3> position{};
    float range = 0;
    std::array<float, 3> radiance{};
    uint32_t type = 0;
    std::array<float, 3> direction{};
    float innerCos = 0;
    float outerCos = 0;
    /// @note Hybrid のみが使用する。castShadows=false は 0、有限 strength は Raster と同じ [0,1] clamp。
    float shadowStrength = 1;
    std::array<uint32_t, 2> reserved{};
    bool operator==(const RayPathDeltaRecord&) const = default;
};
static_assert(sizeof(RayPathDeltaRecord) == 64);
static_assert(offsetof(RayPathDeltaRecord, shadowStrength) == 52);

/// @note t6 は 80 bytes。type=0 は Sphere、1 は両端半球付き Tube (capsule)、寸法はワールド m。
/// @note Sphere の Le=intensity/radius^2。Tube は Point と同じ全 flux を Lambert 面へ配り Le=4*pi*intensity/area。
/// @note Range は実面の NEE / BSDF-hit の直前区間へ同じ artist cutoff を適用する。
/// @see https://pbr-book.org/4ed/Light_Sources/Area_Lights Lambert area flux is pi * area * radiance
/// @see https://pbr-book.org/4ed/Light_Sources/Point_Lights Isotropic point flux is 4*pi*intensity
struct RayPathShapeRecord {
    std::array<float, 3> position{};
    float radius = 0;
    std::array<float, 3> axis{};
    float halfLength = 0;
    std::array<float, 3> emission{};
    float area = 0;
    float selectionPdf = 0;
    float selectionCdf = 0;
    float range = 0;
    uint32_t type = 0;
    uint32_t objectIndex = 0;
    uint32_t objectGeneration = 0;
    uint32_t flags = 0;
    float shadowStrength = 1;
    bool operator==(const RayPathShapeRecord&) const = default;
};
static_assert(sizeof(RayPathShapeRecord) == 80);
static_assert(offsetof(RayPathShapeRecord, shadowStrength) == 76);

enum class RayPathSceneIssue : uint8_t {
    RAY_SCENE_COVERAGE_INCOMPLETE,
    SURFACE_UNSUPPORTED,
    LIGHTING_UNSUPPORTED,
    INVALID_LIGHTING,
    INVALID_INSTANCE,
    EMITTER_DOUBLE_SIDED_UNSUPPORTED,
    BUFFER_READER_UNSUPPORTED,
    BUFFER_CONTENT_MISMATCH,
    INVALID_EMITTER_GEOMETRY,
    INVALID_EMISSION,
    UNREPRESENTABLE_SELECTION_PDF,
    UNSUPPORTED_LIGHT_SOURCE,
    INVALID_LIGHT_SOURCE,
    AREA_MESH_BINDING_UNSUPPORTED,
    SHAPE_MESH_BINDING_UNSUPPORTED,
};

struct RayPathSceneDiagnostic {
    uint32_t instanceId = UINT32_MAX;
    RayPathSceneIssue issue = RayPathSceneIssue::INVALID_INSTANCE;
    RayObjectId objectId;
    bool operator==(const RayPathSceneDiagnostic&) const = default;
};

/// @note contentRevision は builder ごとの実内容版。snapshotSerial / frameStamp / previousWorld / AS アドレスを含めない。
/// @note coverageComplete=false のとき emitter 表を使って Path を部分実行しない。
struct RayPathScene {
    uint64_t sceneGeneration = 0;
    uint64_t contentRevision = 0;
    RayPathLighting lighting;
    std::vector<RayPathEmitterRecord> emitters;
    std::vector<RayPathDeltaRecord> deltaLights;
    std::vector<RayPathShapeRecord> shapes;
    std::vector<RayPathSceneDiagnostic> diagnostics;
    bool coverageComplete = false;
};

using RayPathBufferLookup = std::function<const IBuffer*(ResourceHandle<BufferTag>)>;

/// @note ビューごとに所有する。geometry の CPU 読み出しは内容版付きの exact key で再利用する。
/// @note CPU snapshot を読めない発光 geometry と Area owner の全 geometry は明示診断し、被覆を fail closed にする。
class RayPathSceneBuilder {
public:
    /// @note respectArtistShadows=true は Hybrid 専用。既定の Reference は artist shadow 値を検証・内容キー・transport に使わない。
    [[nodiscard]] RayPathScene Build(const RayScene& scene, ResourceManager& resources,
                                    const RayPathLighting& lighting, bool respectArtistShadows = false);
    /// @note Reference の既定 false は各 record の shadowStrength=1 を維持する。
    [[nodiscard]] RayPathScene Build(const RayScene& scene, const RayPathBufferLookup& lookup,
                                    const RayPathLighting& lighting, bool respectArtistShadows = false);
    /// @note cache を破棄する。次回 Build は内容版を進め、以前の履歴キーを再利用しない。
    void Reset();

private:
    struct LocalTriangle {
        std::array<math::Vector3, 3> vertices;
        uint32_t primitiveId = 0;
    };
    struct CachedGeometry {
        RayGeometryKey key;
        std::vector<LocalTriangle> triangles;
    };
    [[nodiscard]] bool ReadGeometry(const RayGeometryKey& key, const RayPathBufferLookup& lookup,
                                    std::vector<LocalTriangle>& triangles, RayPathSceneIssue& issue) const;
    std::vector<CachedGeometry> m_geometries;
    std::vector<uint32_t> m_contentKey;
    uint64_t m_contentRevision = 0;
};

[[nodiscard]] const char* DescribeRayPathSceneIssue(RayPathSceneIssue issue);

} /// @note namespace fbzz::renderer
