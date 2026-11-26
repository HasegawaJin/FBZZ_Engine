/// @file    RayScene.hpp
/// @brief   Scene を参照せず構築する静的レイ形状・完全な object ID・被覆診断。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/RayTracing/AccelerationStructure.hpp>
#include <Graphics/RayTracing/Scene/RayMaterialCapabilities.hpp>
#include <Graphics/RayTracing/Scene/SurfaceMaterialData.hpp>
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
    uint64_t sceneGeneration = 0; ///< 抽出元シーンの生存期間を識別する世代。
    uint32_t index = 0; ///< 抽出元 Entity ID の添字部。
    uint32_t generation = 0; ///< Entity ID の添字再利用を区別する世代部。
    bool operator==(const RayObjectId&) const = default;
};

/// @note ResourceManager / device 世代の異なる cache では共有しない。hash だけで同一性を決めない。
/// @note opacity と両面区分も含むため、同じ mesh の別材質で不適合な BLAS を共有しない。
struct RayGeometryKey {
    ResourceHandle<BufferTag> vertices; ///< 元頂点バッファの非所有ハンドル。
    ResourceHandle<BufferTag> indices; ///< 元インデックスバッファの非所有ハンドル。
    uint64_t vertexContentVersion = 0; ///< ハンドルが同じでも頂点内容の更新を区別する版。
    uint64_t indexContentVersion = 0; ///< ハンドルが同じでもインデックス内容の更新を区別する版。
    uint32_t vertexStride = 0; ///< 頂点レコードの間隔 [byte]。
    uint32_t firstVertex = 0; ///< 元頂点バッファ内でこの形状が始まる添字。
    uint32_t vertexCount = 0; ///< この形状が参照できる頂点の数。
    uint32_t positionOffset = 0; ///< 各頂点レコードの先頭からローカル位置へのオフセット [byte]。
    uint32_t firstIndex = 0; ///< 元インデックスバッファ内でこの形状が始まる添字。
    uint32_t indexCount = 0; ///< この形状が使うインデックス数。
    bool opaque = true; ///< アルファ被覆判定を省略できる形状として構築するか。
    bool doubleSided = false; ///< レイ交差時に裏面も候補となる形状か。
    bool operator==(const RayGeometryKey&) const = default;
};

struct RayGeometryKeyHash {
    [[nodiscard]] FBZZ_GRAPHICS_API size_t operator()(const RayGeometryKey& key) const;
};

struct RaySceneGeometry {
    RayGeometryKey key; ///< 同一の形状資源・描画範囲・被覆設定を照合するキー。
    RayTriangleGeometry triangles; ///< 実際の BLAS 構築で読み取る三角形入力。
};

/// @note 一つの material slot を一つの BLAS instance にする。InstanceID はこの配列の密な添字。
/// @note mask はレイ用途。Engine の layer は収集対象集合へ反映し、8 bit へ切り詰めない。
struct RaySceneInstance {
    RayObjectId objectId; ///< ヒットしたオブジェクトを元のシーンで識別する完全 ID。
    uint32_t geometryIndex = 0; ///< `RayScene::geometries` 内の、このインスタンスが参照する形状の添字。
    uint32_t denseInstanceId = 0; ///< TLAS の InstanceID と GPU ヒット・材質表に共通する密な添字。
    uint32_t sourceItem = 0; ///< 抽出元のメッシュ描画項目の添字。
    uint32_t sourceSubmesh = 0; ///< 元メッシュ内のサブメッシュ番号。
    uint32_t materialSlot = 0; ///< 元オブジェクトの材質スロット番号。
    SurfaceMaterialData surface; ///< この材質スロットから解決したレイ照明用の表面情報。
    math::Matrix4 world = math::Matrix4::Identity(); ///< 当該フレームの形状ローカル空間からワールド空間への変換。
    math::Matrix4 previousWorld = math::Matrix4::Identity(); ///< 前フレームの形状ローカル空間からワールド空間への変換。
    uint8_t mask = RAY_PRIMARY_MASK | RAY_SHADOW_MASK | RAY_SPECULAR_MASK | RAY_DIFFUSE_MASK; ///< このインスタンスに当てられるレイ用途のビット集合。
    /// @note Effective TLAS culling disable; Hybrid candidate policy selects this independently of authored material sidedness.
    bool doubleSided = false; ///< TLAS で裏面カリングを無効にするか。
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
    RayObjectId objectId; ///< 形状・被覆の問題が生じた対象の完全 ID。
    uint32_t sourceItem = UINT32_MAX; ///< 問題のある元描画項目の添字。特定できなければ `UINT32_MAX`。
    RaySceneIssue issue = RaySceneIssue::INVALID_GEOMETRY; ///< レイシーンへの収集を妨げた形状または被覆の理由。
};

/// @note geometry / opacity の diagnostics と分け、遮蔽者になれる材質も surface 未対応を明示する。
struct RaySurfaceDiagnostic {
    RayObjectId objectId; ///< 表面の問題が生じた対象の完全 ID。
    uint32_t sourceItem = UINT32_MAX; ///< 問題のある元描画項目の添字。特定できなければ `UINT32_MAX`。
    SurfaceMaterialIssue issue = SurfaceMaterialIssue::UNSUPPORTED_SHADER; ///< レイ照明で表面を使えない理由。
};

struct RaySceneBuildOptions {
    uint32_t layerMask = UINT32_MAX; ///< 収集する Engine レイヤーのビット集合。
    /// @note Path と対応済み Hybrid 輸送が明示 opt-in する。DXR opaque は alpha 評価不要を意味し、光透過を禁止しない。
    bool allowSolidDielectrics = false; ///< 対応済みの中実誘電体を明示的に収集するか。
    /// @note Hybrid only: dielectric candidates remain two-sided while ordinary surfaces retain hardware backface culling.
    /// @see https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#ray-flags Instance culling disable and nonopaque candidates.
    bool hybridCandidatePolicy = false; ///< Hybrid のレイ候補と裏面カリングの規則を使うか。
};

/// @note GPU ハンドルは非所有。BLAS / TLAS と GPU table は同じ snapshotSerial で公開する。
/// @note diagnostics が空でも surface / BSDF / 光源の完全性を証明せず、Path availability を有効にしない。
struct RayScene {
    uint64_t sceneGeneration = 0; ///< このスナップショットを抽出したシーンの世代。
    uint64_t snapshotSerial = 0; ///< この形状・インスタンス集合を得た公開スナップショット番号。
    uint64_t frameStamp = 0; ///< 形状の抽出元となる描画フレーム番号。
    uint32_t layerMask = UINT32_MAX; ///< この集合へ収集した Engine レイヤーのビット集合。
    bool hybridCandidatePolicy = false; ///< Hybrid 固有の候補選択規則で構築した集合か。
    std::vector<RaySceneGeometry> geometries; ///< インスタンスが添字で参照する BLAS 形状入力。
    std::vector<RaySceneInstance> instances; ///< TLAS に収めるインスタンス。密な InstanceID の順序。
    std::vector<RaySceneDiagnostic> diagnostics; ///< 収集できなかった形状や被覆を記録する診断列。
    std::vector<RaySurfaceDiagnostic> surfaceDiagnostics; ///< レイ照明に対応しない表面を記録する診断列。
};

/// @note カメラの visible / lodVisible / Hi-Z / 距離を使わない。明示的な slot 非表示だけを除外する。
/// @note static layout と保証済み被覆だけ。smooth solid dielectric は明示 opt-in のみ収集する。
/// @see Docs/design/RayTracing.md §RenderScene から Ray Scene を作る
[[nodiscard]] FBZZ_GRAPHICS_API RayScene BuildRayScene(const RenderScene& scene, RaySceneBuildOptions options = {});

[[nodiscard]] FBZZ_GRAPHICS_API const char* DescribeRaySceneIssue(RaySceneIssue issue);

} // namespace fbzz::renderer
