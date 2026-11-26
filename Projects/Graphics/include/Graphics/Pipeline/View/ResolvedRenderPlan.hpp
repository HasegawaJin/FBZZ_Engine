/// @file    ResolvedRenderPlan.hpp
/// @brief   描画の要求を能力・被覆・準備結果から実行可能な構成へ解決する。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#pragma once
#include <Graphics/Api.hpp>

#include <Graphics/Renderer/Platform/GraphicsCapabilities.hpp>
#include <Graphics/Renderer/View/OpaqueRenderPlan.hpp>
#include <Graphics/Renderer/View/RenderMode.hpp>

namespace fbzz::renderer {

struct RenderSettings;

enum class RenderPlanReason : uint8_t {
    NONE,
    INVALID_REQUEST,
    NO_RAY_EFFECT_REQUESTED,
    INLINE_RAY_QUERY_UNAVAILABLE,
    BINDLESS_UNAVAILABLE,
    RAY_SCENE_UNAVAILABLE,
    RAY_COVERAGE_INCOMPLETE,
    RAY_PIPELINE_UNAVAILABLE,
    RASTER_SURFACE_UNAVAILABLE,
    RASTER_PIPELINE_UNAVAILABLE,
    REQUIRED_OUTPUT_UNAVAILABLE,
    DEVELOPER_MODE_REQUIRED,
};

/// @note 効果に届く形状・opacity・必要な BSDF / 光源の対応を呼び出し側で証明する。
/// @note path は profile の対応範囲と、GAME で許可した近似を解決した結果。
struct SceneRayCoverage {
    bool shadow = false; ///< 影を落とすシーン要素をレイ追跡で扱えるか。
    bool reflection = false; ///< 反射に必要な形状とマテリアルをレイ追跡で扱えるか。
    bool diffuseGi = false; ///< 間接拡散光に必要な形状とマテリアルをレイ追跡で扱えるか。
    bool path = false; ///< 要求したプロファイルでシーンをパストレースできるか。
};

/// @note 当該ビュー・Scene / device 世代・寸法の準備結果。ハンドルの存在だけで true にしない。
/// @note パスの実装・シェーダー・必須資源が揃った場合だけ pipelineReady を true にする。
struct RenderAvailability {
    /// @note ホストのセッション方針。false なら保存された RT 要求を保持したまま Raster へ戻す。
    bool experimentalRayTracingEnabled = false;
    bool outputsReady = false; ///< このビューが要求する出力面の準備ができているか。
    bool rasterPipelineReady = false; ///< フォールバック先にも使うラスタ描画経路が準備できているか。
    OpaqueRenderAvailability opaque; ///< 不透明物の描画方式ごとの利用可否。
    bool clusteredLightingReady = false; ///< クラスター照明の計算と参照に必要な資源を使えるか。
    bool raySceneReady = false; ///< 現在のシーンに対応するレイ追跡資源を使えるか。
    bool shadowPipelineReady = false; ///< レイ追跡による影を記録できるか。
    bool reflectionPipelineReady = false; ///< レイ追跡による反射を記録できるか。
    bool diffuseGiPipelineReady = false; ///< レイ追跡による間接拡散光を記録できるか。
    bool pathPipelineReady = false; ///< パストレーシングのパスを記録できるか。
    bool rasterSurfaceReady = false; ///< レイ描画の入力になるラスタ描画面が揃っているか。
};

struct RayEffectPlan {
    bool enabled = false; ///< この効果で実際にレイ追跡を使うか。
    RenderPlanReason fallbackReason = RenderPlanReason::NONE; ///< 効果を要求した方式で実行できなかった理由。
    bool operator==(const RayEffectPlan&) const = default;
};

struct ResolvedRenderPlan {
    RenderMode requestedMode = RenderMode::RASTER; ///< 描画設定から要求されたモード。
    RenderMode effectiveMode = RenderMode::RASTER; ///< デバイス能力と資源の状態を反映して実行する描画モード。
    PathTracingProfile pathProfile = PathTracingProfile::REFERENCE; ///< パストレーシングで採用する対応範囲と品質のプロファイル。
    PrimaryVisibility primaryVisibility = PrimaryVisibility::RASTER; ///< 最初に見える面を決める描画方式。
    RayExecution rayExecution = RayExecution::NONE; ///< このビューで実際に記録するレイ追跡の実行方式。
    OpaqueRenderPlan rasterPlan; ///< 不透明物をどのラスタ経路で描くかを決めた結果。
    /// @note Raster の受け手向け。Path のヒット照明は RayLightTable を使う。
    bool clusteredLighting = false;
    RayEffectPlan shadow; ///< 影のレイ追跡を実行できるかと、代替経路を使う理由。
    RayEffectPlan reflection; ///< 反射のレイ追跡を実行できるかと、代替経路を使う理由。
    RayEffectPlan diffuseGi; ///< 間接拡散光のレイ追跡を実行できるかと、代替経路を使う理由。
    RenderPlanReason fallbackReason = RenderPlanReason::NONE; ///< 要求した方式から別の描画方式に切り替えた理由。
    RenderPlanReason failureReason = RenderPlanReason::REQUIRED_OUTPUT_UNAVAILABLE; ///< 代替を含めて描画計画を実行できない理由。NONE なら実行可能。
    bool operator==(const ResolvedRenderPlan&) const = default;

    [[nodiscard]] bool IsValid() const { return failureReason == RenderPlanReason::NONE; }
    [[nodiscard]] bool NeedsRayScene() const
    {
        return IsValid() && rayExecution != RayExecution::NONE;
    }
};

/// @note GPU 資源の生成・Scene 走査・設定変更を行わない。初期 RT は Inline と bindless を要求する。
/// @note GAME は専用 GBuffer の準備を要求する。通常の Forward プリパスでは代用しない。
/// @return 復帰先も未準備なら無効な Plan。fallbackReason と failureReason を別々に保持する。
/// @see Docs/design/RayTracing.md
[[nodiscard]] FBZZ_GRAPHICS_API ResolvedRenderPlan ResolveRenderPlan(
    const RenderSettings& settings, const RenderModeRequest& request,
    const GraphicsCapabilities& capabilities, const SceneRayCoverage& coverage,
    const RenderAvailability& availability);

[[nodiscard]] FBZZ_GRAPHICS_API const char* DescribeRenderPlanReason(RenderPlanReason reason);

}
