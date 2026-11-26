/// @file    WaterSpline.hpp
/// @brief   水面スプラインの制作値と不変の CPU 水面スナップショット。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once
#include <Math/LinearAlgebra/Vector4.hpp>
#include <Engine/Api.hpp>
#include <Math/LinearAlgebra/Vector2.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fbzz::scene {
struct IReflector;
struct EntityID;
struct WaterComponent;
struct Transform;

/// @brief 現在の照会または固定ステップで参照する水面ジオメトリの読み取り境界。
enum class WaterSurfaceReadView : std::uint8_t {
    SIMULATION,
    LIVE_QUERY,
};

/// @brief 水面スプラインの制御点数の上限。
inline constexpr std::uint32_t WATER_SPLINE_MAX_CONTROL_POINTS = 256;
/// @brief 制御点区間あたりの Catmull-Rom サンプル数の上限。
inline constexpr std::uint32_t WATER_SPLINE_MAX_SAMPLES_PER_SEGMENT = 64;
/// @brief 水面スプラインのスナップショットに生成できる頂点数の上限。
inline constexpr std::size_t WATER_SPLINE_MAX_VERTICES = 32768;
/// @brief 描画用の重力波速度を求める代表波長 [m]。
inline constexpr math::Vector2 WATER_RIVER_WAVE_REFERENCE_LENGTHS{6.0f, 2.0f};
/// @brief 反射の符号付き整数フィールドに収まる最大 ID。
inline constexpr std::uint32_t WATER_SPLINE_MAX_POINT_ID = 0x7fffffffu;
/// @brief 二次計算量の検証に渡せる湖の外周サンプル頂点数の上限。
inline constexpr std::size_t WATER_SPLINE_MAX_LAKE_BOUNDARY_VERTICES = 2048;
/// @brief 二次計算量の検証に渡せる河岸の外周サンプル頂点数の上限。
inline constexpr std::size_t WATER_SPLINE_MAX_RIVER_BOUNDARY_VERTICES = 4096;

/// @brief WaterComponent が選択する水面の平面形状。
enum class WaterSurfaceShape : std::uint8_t {
    RECTANGLE = 0,
    SPLINE_LAKE = 1,
    SPLINE_RIVER = 2,
};

/// @brief 湖の外周または河川の中心線を表す安定 ID 付きの制御点。
struct WaterSplinePoint {
    std::uint32_t id = 0; ///< 点の並べ替えで変わらない非ゼロの識別子。
    math::Vector3 localPosition = math::Vector3::ZERO; ///< コンポーネントのローカル座標 [m]。
    float width = 0.0f; ///< 河川の全幅 [ローカル m]。湖では使わないが有限値が必要。
    float depth = 0.0f; ///< コンポーネントの上限適用前の水柱の深さ [ワールド m]。
    float flowSpeed = 0.0f; ///< スプライン接線方向の符号付き流速 [ワールド m/s]。

    /// @brief 制作とシーン保存に使う安定した制御点フィールドを反射する。
    void Reflect(IReflector& reflector);
    bool operator==(const WaterSplinePoint& other) const = default;
};

inline constexpr std::size_t WATER_LAKE_MAX_ISLAND_POINTS = 512;
inline constexpr int WATER_LAKE_MAX_ISLANDS = 16;

/// @brief 水面を除外する閉じた輪郭の頂点 [所有者ローカル m]。
struct WaterLakeIslandPoint {
    int ring = 0;
    math::Vector3 localPosition = math::Vector3::ZERO;
    bool operator==(const WaterLakeIslandPoint&) const = default;
};

/// @brief 描画と水面照会が共有するスプラインのサンプル頂点。
struct WaterSplineVertex {
    math::Vector3 localPosition = math::Vector3::ZERO;
    math::Vector3 worldPosition = math::Vector3::ZERO;
    math::Vector3 worldVelocity = math::Vector3::ZERO;
    float baseHeightOffset = 0.0f;
    float depth = 0.0f;
    float riverDistance = 0.0f; ///< 河川中心線の始点からサンプルに沿った距離 [ワールド m]。
    float horizonMaterialWeight = 0.0f;
    float riverTravelTime = 0.0f; ///< 河川中心線に沿う基本波の符号付き伝播時間 [s]。
    float riverShortTravelTime = 0.0f; ///< 河川中心線に沿う短波長帯の符号付き伝播時間 [s]。
};

/// @brief 共有水面スナップショットの三角形。
struct WaterSplineTriangle {
    std::array<std::uint32_t, 3> indices = {};
    math::Vector2 baseHeightGradient = math::Vector2::ZERO;
};

/// @brief スナップショット内の XZ 三角形を厳密に検索する階層のノード。
struct WaterSplineBvhNode {
    math::Vector2 boundsMin = math::Vector2::ZERO;
    math::Vector2 boundsMax = math::Vector2::ZERO;
    std::uint32_t left = 0;
    std::uint32_t right = 0;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

/// @brief 水面描画、照会、物理が共有する不変の数値形状。
struct WaterSplineSnapshot {
    WaterSurfaceShape shape = WaterSurfaceShape::RECTANGLE;
    std::vector<WaterSplineVertex> vertices;
    std::vector<WaterSplineTriangle> triangles;
    std::vector<WaterSplineTriangle> footprintTriangles; ///< 共有辺の細分化前に保持する水面範囲の三角形分割。
    std::vector<std::uint32_t> outlineIndices; ///< BuildWaterSplineSnapshot は左岸を点順、右岸を逆順で格納する。
    std::vector<std::vector<math::Vector3>> islandOutlines;
    std::vector<WaterSplineBvhNode> bvhNodes;
    std::vector<std::uint32_t> triangleOrder;
    math::Vector3 localBoundsMin = math::Vector3::ZERO;
    math::Vector3 localBoundsMax = math::Vector3::ZERO;
    math::Vector3 worldBoundsMin = math::Vector3::ZERO;
    math::Vector3 worldBoundsMax = math::Vector3::ZERO;
    float worldBaseY = 0.0f;
    float minimumBaseHeightOffset = 0.0f;
    float maximumBaseHeightOffset = 0.0f;
    float maximumDepth = 0.0f;
    float maximumWorldEdgeLength = 0.0f;
    float riverWaveReferenceSpeed = 0.0f; ///< 弧長で重み付けした基本波の位相速度の大きさ [m/s]。
    math::Vector4 horizonMouth{}; ///< ワールド XZ の中心と平坦部・フェード部の半径 [m]。
    bool meshQualityLimited = false;
    std::uint64_t signature = 0;
};

/// @brief ワールド XZ 座標で補間したスプライン水面の値。
struct WaterSplineSample {
    float baseHeight = 0.0f;
    float baseHeightOffset = 0.0f;
    float depth = 0.0f;
    math::Vector3 worldVelocity = math::Vector3::ZERO;
    math::Vector2 baseHeightGradient = math::Vector2::ZERO;
    float riverDistance = 0.0f;
    float horizonMaterialWeight = 0.0f;
};

/// @brief 水面形状と UV マッピングに使うコンポーネント中心基準のローカル XZ の広がりを返す。
/// @param[in] water 選択形状で平面範囲が決まる水面。
/// @return 制作時の矩形範囲、またはスプラインの対称なローカル境界の広がり。
[[nodiscard]] FBZZ_ENGINE_API math::Vector2 WaterSurfaceLocalExtent(const WaterComponent& water,
    WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY);

/// @brief 選択した読み取り境界で確定した形状を返す。
[[nodiscard]] FBZZ_ENGINE_API WaterSurfaceShape WaterSurfaceEffectiveShape(const WaterComponent& water,
    WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY);

/// @brief 水面ジオメトリが制作時の Rectangle 格子を置き換えるかを返す。
/// @note 無効な制作スプラインもジオメトリ選択を維持し、矩形への代替を防ぐ。
[[nodiscard]] FBZZ_ENGINE_API bool UsesWaterSurfaceGeometry(const WaterComponent& water,
    WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY);

/// @brief 有効な不変ジオメトリを返す。選択中の形状が無効なら null。
/// @note Zone の所有者は制作時の水面形状を変えずに共有スナップショットを公開する。
[[nodiscard]] FBZZ_ENGINE_API const WaterSplineSnapshot* WaterSurfaceGeometry(const WaterComponent& water,
    WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY);

/// @brief 選択した水面ジオメトリと同時に確定した所有者の姿勢を返す。
/// @param[in] water シミュレーション姿勢を保持する Water コンポーネント。
/// @param[in] currentTransform LIVE_QUERY または初回のシミュレーション公開前に使う現在の姿勢。
/// @param[in] view 姿勢を読むジオメトリの境界。
[[nodiscard]] FBZZ_ENGINE_API const Transform& WaterSurfaceTransform(const WaterComponent& water,
    const Transform& currentTransform, WaterSurfaceReadView view);

/// @brief 選択した水面ジオメトリと同時に確定した Zone 所属を返す。
/// @param[in] water 現在とシミュレーションの所属を保持する Water コンポーネント。
/// @param[in] view Zone 所有者を読むジオメトリの境界。
[[nodiscard]] FBZZ_ENGINE_API EntityID WaterSurfaceZoneOwner(const WaterComponent& water,
    WaterSurfaceReadView view);

/// @brief スプラインの UV とアトラスが使うローカル、拡縮後、回転後の共有範囲を検証する。
/// @param[in] snapshot ベイク済みのスプライン境界。
/// @param[in] transform 中心基準の境界を写すワールド姿勢。
/// @return すべての範囲が有限で float に収まるときだけ true。
[[nodiscard]] FBZZ_ENGINE_API bool HasValidWaterSplineExtents(const WaterSplineSnapshot& snapshot, const Transform& transform);

/// @brief スナップショットの構築に使う全制作値とワールド変換をハッシュする。
/// @param[in] shape 選択した湖または河川のスプライン形状。
/// @param[in] points コンポーネントのローカル空間にある安定 ID 付き制御点。
/// @param[in] samplesPerSegment 制御点区間あたりの Catmull-Rom サンプル数。
/// @param[in] transform 位置と流速ベクトルのベイクに使う現在のワールド変換。
/// @param[in] maximumWaterDepth 物理と描画が共有する全体の深さ上限。
/// @param[in] maximumWorldCellSize 共有辺を中点で細分化した後に求めるワールド空間の最大辺長。
/// @return キャッシュの失効判定に使う安定した署名。
[[nodiscard]] FBZZ_ENGINE_API std::uint64_t WaterSplineSnapshotSignature(WaterSurfaceShape shape,
    std::span<const WaterSplinePoint> points, std::uint32_t samplesPerSegment,
    const Transform& transform, float maximumWaterDepth = 10.0f,
    float maximumWorldCellSize = 2.0f, std::span<const WaterLakeIslandPoint> islands = {});

/// @brief 選択した湖または河川の上限付き三角形スナップショットを描画、照会、物理向けに構築する。
/// @param[in] points コンポーネントのローカル空間にある安定 ID 付き制御点。
/// @param[in] samplesPerSegment Catmull-Rom の分割数。切り詰めずに検証する。
/// @param[in] transform 位置と水平流速に使う、上下軸を保つワールド姿勢。
/// @param[in] maximumWaterDepth 物理と描画が共有するワールド空間の深さ上限。
/// @param[in] maximumWorldCellSize メッシュと波のサンプリングで共有する、細分化後のワールド辺長の目安。
/// @return 無効または上限超過で false。out は未変更。成功時は out を一括で置き換える。
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/water-body-actors-in-unreal-engine Epic Games, Water Body Actors.
[[nodiscard]] FBZZ_ENGINE_API bool BuildWaterSplineSnapshot(WaterSurfaceShape shape,
    std::span<const WaterSplinePoint> points, std::uint32_t samplesPerSegment,
    const Transform& transform, WaterSplineSnapshot& out, float maximumWaterDepth = 10.0f,
    float maximumWorldCellSize = 2.0f, std::span<const WaterLakeIslandPoint> islands = {});

/// @brief 制作時の数値メッシュの境界、共有辺の細分化、勾配、BVH を再計算する。
/// @return 無効または上限超過の形状で false。out は部分的に更新され得る。
[[nodiscard]] FBZZ_ENGINE_API bool FinalizeWaterSplineSnapshot(WaterSplineSnapshot& snapshot,
    float maximumWorldCellSize);

/// @brief 頂点を細分化せず、スナップショットの境界、三角形の勾配、検索階層を再計算する。
[[nodiscard]] FBZZ_ENGINE_API bool RebuildWaterSplineSnapshotDerivedData(WaterSplineSnapshot& snapshot);

/// @brief レンダラーが使う三角形水面をサンプリングする。
/// @param[in] snapshot 不変のワールド空間スプライン形状。
/// @param[in] worldX,worldZ 照会位置のワールド座標。
/// @param[out] out 成功時に補間した高さ、深さ、流速を格納する。
/// @return 水面範囲外、無効なスナップショット、無効な照会で false。out は未変更。
[[nodiscard]] FBZZ_ENGINE_API bool TrySampleWaterSpline(const WaterSplineSnapshot& snapshot,
    float worldX, float worldZ, WaterSplineSample& out);
}
