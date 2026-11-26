/// @file    RayPathScene.hpp
/// @brief   Path の内容版と CPU snapshot から求める発光三角形の選択分布。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/RayTracing/Scene/RayScene.hpp>
#include <Graphics/RayTracing/Scene/RayLightInput.hpp>
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
    math::Vector3 direction{0, -1, 0}; ///< 単一の方向光を使う場合のワールド空間での光の向き。
    math::Vector3 directionalRadiance{}; ///< 単一の方向光のシーン線形 RGB 放射輝度。
    math::Vector3 environmentRadiance{}; ///< 定数環境を使う場合のシーン線形 RGB 放射輝度。
    uint64_t environmentContentVersion = 0; ///< 生の環境テクスチャの内容を履歴キーで区別する版。
    float environmentRotation = 0; ///< 環境キューブのワールド Y 軸回転 [rad]。
    float environmentIntensity = 1; ///< 生の環境放射輝度に掛ける倍率。
    bool supported = true; ///< 供給した光源と環境をパス積分で扱えるか。
    /// @note true のとき全光源を lights から積分し、legacy single directional は二重に加算しない。
    std::vector<RayLightInput> lights; ///< `useSceneLights` のときに単一方向光の代わりに積分するシーン光源の列。
    bool useSceneLights = false; ///< 既存の単一方向光に代えて `lights` を積分するか。
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
    std::array<float, 3> v0{}; ///< 発光三角形の先頭のワールド頂点 [m]。
    float area = 0; ///< 発光三角形のワールド面積 [m²]。
    std::array<float, 3> edge1{}; ///< `v0` から第 2 頂点へのワールド辺ベクトル [m]。
    uint32_t instanceId = 0; ///< ヒット表の密なインスタンス添字。仮想面では `UINT32_MAX`。
    std::array<float, 3> edge2{}; ///< `v0` から第 3 頂点へのワールド辺ベクトル [m]。
    uint32_t primitiveId = 0; ///< インスタンス内の三角形番号。仮想面では発光表の一意な添字。
    std::array<float, 3> emission{}; ///< 三角形から出るシーン線形 RGB 放射輝度。
    float selectionPdf = 0; ///< 発光面を 1 枚選ぶとき、この三角形を選ぶ離散確率。
    std::array<float, 3> geometricNormal{}; ///< 頂点の巻き順と表裏判定に対応するワールド幾何法線。
    float selectionCdf = 0; ///< この三角形までを含む発光面選択の累積確率。
    /// @note range=0 は無限。正の range は NEE と BSDF-hit の直前区間へ同じ artist smooth cutoff を適用する。
    float range = 0; ///< 直接サンプリングと BSDF ヒットに共通の寄与範囲 [m]。0 は無限。
    uint32_t flags = 0; ///< 仮想面・両面放射・オーナー付きの面光源代理を区別するビット集合。
    uint32_t objectIndex = 0; ///< 光源とメッシュ所有者を照合する Entity ID の添字部。
    uint32_t objectGeneration = 0; ///< 光源とメッシュ所有者を照合する Entity ID の世代部。
    /// @note Hybrid の直接光だけが使う artist shadow strength。通常の mesh emission と Reference は物理遮蔽を保つ。
    float shadowStrength = 1; ///< Hybrid の直接光で使う作者指定の遮蔽の強さ。
    std::array<uint32_t, 3> reserved{}; ///< GPU レコードの配置を保つ予約領域。
    bool operator==(const RayPathEmitterRecord&) const = default;
};
static_assert(sizeof(RayPathEmitterRecord) == 112);
static_assert(offsetof(RayPathEmitterRecord, shadowStrength) == 96);

/// @note t5 の StructuredBuffer 要素は 64 bytes。type は 0=Point、1=Spot、2=Directional。
/// @note Point / Spot は delta。range=0 は無限、正の range は既存 smooth window、距離二乗は 0.01 で下限を設ける。
struct RayPathDeltaRecord {
    std::array<float, 3> position{}; ///< Point または Spot 光源のワールド位置 [m]。
    float range = 0; ///< Point または Spot の距離減衰範囲 [m]。0 は無限。
    std::array<float, 3> radiance{}; ///< 光源の減衰を評価する前のシーン線形 RGB 放射輝度項。
    uint32_t type = 0; ///< 光源の種別。0 は Point、1 は Spot、2 は Directional。
    std::array<float, 3> direction{}; ///< Spot または Directional 光源のワールド方向。
    float innerCos = 0; ///< Spot 光が一様に寄与する内側半画角の余弦。
    float outerCos = 0; ///< Spot 光の寄与がなくなる外側半画角の余弦。
    /// @note Hybrid のみが使用する。castShadows=false は 0、有限 strength は Raster と同じ [0,1] clamp。
    float shadowStrength = 1; ///< Hybrid で使う遮蔽の強さ [0,1]。影なしは 0。
    std::array<uint32_t, 2> reserved{}; ///< GPU レコードの配置を保つ予約領域。
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
    std::array<float, 3> position{}; ///< Sphere または Tube のワールド中心 [m]。
    float radius = 0; ///< 球またはカプセルの半径 [m]。
    std::array<float, 3> axis{}; ///< Tube の軸方向を表すワールドベクトル。
    float halfLength = 0; ///< Tube の両半球の中心を結ぶ区間の半長 [m]。
    std::array<float, 3> emission{}; ///< 形状表面から出るシーン線形 RGB 放射輝度。
    float area = 0; ///< 形状全体の表面積 [m²]。
    float selectionPdf = 0; ///< 形状光源を選ぶときに、この光源が選ばれる離散確率。
    float selectionCdf = 0; ///< この光源までを含む形状光源選択の累積確率。
    float range = 0; ///< 直接サンプリングと BSDF ヒットに共通の寄与範囲 [m]。0 は無限。
    uint32_t type = 0; ///< 光源の形状。0 は Sphere、1 は Tube。
    uint32_t objectIndex = 0; ///< 形状光源を所有する Entity ID の添字部。
    uint32_t objectGeneration = 0; ///< 形状光源を所有する Entity ID の世代部。
    uint32_t flags = 0; ///< シェーダーへ渡す形状光源のビットフラグ。
    float shadowStrength = 1; ///< Hybrid の直接光で使う作者指定の遮蔽の強さ。
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
    uint32_t instanceId = UINT32_MAX; ///< 問題のあるインスタンスの密な添字。対象なしは `UINT32_MAX`。
    RayPathSceneIssue issue = RayPathSceneIssue::INVALID_INSTANCE; ///< 形状・材質・光源をパス積分できなかった理由。
    RayObjectId objectId; ///< 診断対象のオブジェクトまたは光源の完全 ID。
    bool operator==(const RayPathSceneDiagnostic&) const = default;
};

/// @note contentRevision は builder ごとの実内容版。snapshotSerial / frameStamp / previousWorld / AS アドレスを含めない。
/// @note coverageComplete=false のとき emitter 表を使って Path を部分実行しない。
struct RayPathScene {
    uint64_t sceneGeneration = 0; ///< 積分対象のシーンの世代。
    uint64_t contentRevision = 0; ///< 同一ビルダーの積分入力の実内容を識別する版。
    RayPathLighting lighting; ///< パス積分に渡した光源・環境の設定。
    std::vector<RayPathEmitterRecord> emitters; ///< 発光三角形と面の代理形状をサンプリングするレコード列。
    std::vector<RayPathDeltaRecord> deltaLights; ///< 点光源・スポット光源・方向光のレコード列。
    std::vector<RayPathShapeRecord> shapes; ///< 球とカプセル形状の光源をサンプリングするレコード列。
    std::vector<RayPathSceneDiagnostic> diagnostics; ///< 積分入力の不足や未対応を記録する診断列。
    bool coverageComplete = false; ///< 必要な入力を揃え、部分実行を避けられるか。
};

using RayPathBufferLookup = std::function<const IBuffer*(ResourceHandle<BufferTag>)>;

/// @note ビューごとに所有する。geometry の CPU 読み出しは内容版付きの exact key で再利用する。
/// @note CPU snapshot を読めない発光 geometry と Area owner の全 geometry は明示診断し、被覆を fail closed にする。
class RayPathSceneBuilder {
public:
    /// @note respectArtistShadows=true は Hybrid 専用。既定の Reference は artist shadow 値を検証・内容キー・transport に使わない。
    [[nodiscard]] FBZZ_GRAPHICS_API RayPathScene Build(const RayScene& scene, ResourceManager& resources,
                                    const RayPathLighting& lighting, bool respectArtistShadows = false);
    /// @note Reference の既定 false は各 record の shadowStrength=1 を維持する。
    [[nodiscard]] FBZZ_GRAPHICS_API RayPathScene Build(const RayScene& scene, const RayPathBufferLookup& lookup,
                                    const RayPathLighting& lighting, bool respectArtistShadows = false);
    /// @note cache を破棄する。次回 Build は内容版を進め、以前の履歴キーを再利用しない。
    FBZZ_GRAPHICS_API void Reset();

private:
    struct LocalTriangle {
        std::array<math::Vector3, 3> vertices; ///< 発光候補となる三角形のローカル頂点 3 点。
        uint32_t primitiveId = 0; ///< 元の BLAS 形状内でこの三角形を識別する添字。
    };
    struct CachedGeometry {
        RayGeometryKey key; ///< CPU 読み出しを再利用できる頂点・添字資源と内容版のキー。
        std::vector<LocalTriangle> triangles; ///< 照合済みの形状から読んだローカル三角形の列。
    };
    [[nodiscard]] FBZZ_GRAPHICS_API bool ReadGeometry(const RayGeometryKey& key, const RayPathBufferLookup& lookup,
                                    std::vector<LocalTriangle>& triangles, RayPathSceneIssue& issue) const;
    std::vector<CachedGeometry> m_geometries; ///< 内容版が一致する場合に読み出しを再利用する CPU 形状キャッシュ。
    std::vector<uint32_t> m_contentKey; ///< 前回構築した積分入力の実内容を照合するビット列。
    uint64_t m_contentRevision = 0; ///< 積分入力の実内容が変わるたびに更新する版。
};

[[nodiscard]] FBZZ_GRAPHICS_API const char* DescribeRayPathSceneIssue(RayPathSceneIssue issue);

} // namespace fbzz::renderer
