/// @file    ResolvedRenderPlan.hpp
/// @brief   描画の要求を能力・被覆・準備結果から実行可能な構成へ解決する。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#pragma once

#include <Graphics/Renderer/GraphicsCapabilities.hpp>
#include <Graphics/Renderer/OpaqueRenderPlan.hpp>
#include <Graphics/Renderer/RenderMode.hpp>

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
};

/// @note 効果に届く形状・opacity・必要な BSDF / 光源の対応を呼び出し側で証明する。
/// @note path は profile の対応範囲と、GAME で許可した近似を解決した結果。
struct SceneRayCoverage {
    bool shadow = false;
    bool reflection = false;
    bool diffuseGi = false;
    bool path = false;
};

/// @note 当該ビュー・Scene / device 世代・寸法の準備結果。ハンドルの存在だけで true にしない。
/// @note パスの実装・シェーダー・必須資源が揃った場合だけ pipelineReady を true にする。
struct RenderAvailability {
    bool outputsReady = false;
    bool rasterPipelineReady = false;
    OpaqueRenderAvailability opaque;
    bool clusteredLightingReady = false;
    bool raySceneReady = false;
    bool shadowPipelineReady = false;
    bool reflectionPipelineReady = false;
    bool diffuseGiPipelineReady = false;
    bool pathPipelineReady = false;
    bool rasterSurfaceReady = false;
};

struct RayEffectPlan {
    bool enabled = false;
    RenderPlanReason fallbackReason = RenderPlanReason::NONE;
    bool operator==(const RayEffectPlan&) const = default;
};

struct ResolvedRenderPlan {
    RenderMode requestedMode = RenderMode::RASTER;
    RenderMode effectiveMode = RenderMode::RASTER;
    PathTracingProfile pathProfile = PathTracingProfile::REFERENCE;
    PrimaryVisibility primaryVisibility = PrimaryVisibility::RASTER;
    RayExecution rayExecution = RayExecution::NONE;
    OpaqueRenderPlan rasterPlan;
    /// @note Raster の受け手向け。Path のヒット照明は RayLightTable を使う。
    bool clusteredLighting = false;
    RayEffectPlan shadow;
    RayEffectPlan reflection;
    RayEffectPlan diffuseGi;
    RenderPlanReason fallbackReason = RenderPlanReason::NONE;
    RenderPlanReason failureReason = RenderPlanReason::REQUIRED_OUTPUT_UNAVAILABLE;
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
[[nodiscard]] ResolvedRenderPlan ResolveRenderPlan(
    const RenderSettings& settings, const RenderModeRequest& request,
    const GraphicsCapabilities& capabilities, const SceneRayCoverage& coverage,
    const RenderAvailability& availability);

[[nodiscard]] const char* DescribeRenderPlanReason(RenderPlanReason reason);

}
