/// @file    WaterSurfaceQuery.hpp
/// @brief   水面の描画・物理・水中判定が共有するワールド座標の照会契約。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once
#include <Engine/Api.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <vector>

namespace fbzz::scene {
struct ActiveFlowField;
struct Transform;
struct WaterComponent;

struct WaterSurfaceHeightSample {
    math::Vector3 position;
    float depth = 0.0f; ///< @note 表面から入力位置までの鉛直距離 [m]。水面より上なら負。
    bool insideSurface = false;
    bool underwater = false; ///< @note footprint 内かつ 0 < depth <= waterDepthLimit。
    float displacement = 0.0f; ///< @note Component の worldY に加える基準高さ差と表面変位 [world m]。
    float waterDepthLimit = 0.0f; ///< @note この XZ で浮力が届く表面下の深さ [world m]。
    float baseHeightOffset = 0.0f; ///< @note Spline 三角形の基準 worldY 差。絶対 worldY から分離して精度を保つ。
    math::Vector2 baseHeightGradient = math::Vector2::ZERO; ///< @note Spline の基準面 dY/d(worldX,worldZ)。
    bool excluded = false; ///< @note 3D exclusion box contains the queried world point, or its snapshot is invalid.
};

struct WaterSurfaceSample {
    WaterSurfaceHeightSample height;
    math::Vector3 normal;
    math::Vector3 flowVelocity;
};

/// @return 合成形状を持たない Rectangle の Horizon 指定なら true。
/// @note Horizon の水平所属はカメラ非依存。水深と除外領域は有限設定を共有する。
[[nodiscard]] FBZZ_ENGINE_API bool WaterSurfaceUsesHorizon(const WaterComponent& water,
    WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY);

/// @return 有限で可逆の水平 TRS と有限の水域サイズ・深さ・実効波なら true。
/// @note yaw・親 TRS・負の非一様 scale に対応。pitch/roll は現行 world XZ 波と法線の契約外。
[[nodiscard]] FBZZ_ENGINE_API bool IsWaterSurfaceSupported(const WaterComponent& water, const Transform& transform,
    WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY);

/// @return Finite は Gerstner 逆写像の footprint 内、Horizon は全 XZ で true。無効入力は false。
/// @note 描画カメラと遠景 LOD を参照せず、物理 substep の時刻で所属を求める。
[[nodiscard]] FBZZ_ENGINE_API bool WaterSurfaceContainsXZ(const WaterComponent& water, const Transform& transform,
    float worldX, float worldZ, float time,
    WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY);

/// @return 無効 TRS・非有限入力/結果では false、out 未変更。
/// @note footprint 外でも表面高さは外挿する。既存 C# 高さ API は外挿値、所属を要する消費者は insideSurface を使う。
/// @note position は worldXZ 鉛直線の表面。基準面 worldY に world m の変位を足し、scale.y は波高に掛けない。
[[nodiscard]] FBZZ_ENGINE_API bool TrySampleWaterSurfaceHeightWorld(const WaterComponent& water, const Transform& transform,
    const math::Vector3& worldPosition, float time, WaterSurfaceHeightSample& out,
    WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY);

/// @return 高さ・上向き単位法線・その点の水平流速 [m/s]。無効入力/結果では false、out 未変更。
/// @note fields は現在フレームの不変 snapshot。波と流速は WaterSystem が解決した実効値を読み、Scene を参照しない。
/// @note WaterComponent の波キャッシュを更新するため、同じ水面 snapshot の並行評価は禁止。
/// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models GPU Gems 1.2.2, Differentials of Height Functions。
[[nodiscard]] FBZZ_ENGINE_API bool TrySampleWaterSurfaceWorld(const WaterComponent& water, const Transform& transform,
    const std::vector<ActiveFlowField>& fields, const math::Vector3& worldPosition, float time,
    WaterSurfaceSample& out, WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY);
} /// @note namespace fbzz::scene
