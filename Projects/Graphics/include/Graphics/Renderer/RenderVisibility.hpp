/// @file    RenderVisibility.hpp
/// @brief   Scene に依存しないビュー単位の可視性判定。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#pragma once

#include <Math/Vector3.hpp>

namespace fbzz::math { struct Frustum; }

namespace fbzz::renderer {

struct RenderCullingView {
    math::Vector3 position{};
    math::Vector3 forward{ 0.0f, 0.0f, 1.0f };
    /// @note 非所有。判定中は有効であること。nullptr は視錐台判定なし。
    const math::Frustum* frustum = nullptr;
    float projectionScaleY = 0.0f;
    float smallObjectScreenHeight = 0.0f;
    bool distanceSpherical = true;
    bool orthographic = false;
};

struct RenderCullingItem {
    /// @note ワールド空間の球。余白は抽出側で半径へ加算済み。
    math::Vector3 center{};
    /// @note 0 以下は境界未確定。可視性判定では安全側に倒して描く。
    float radius = 0.0f;
    /// @note レイヤー別設定を解決済みの距離 [m]。0 以下は距離制限なし。
    float maxDrawDistance = 0.0f;
};

enum class VisibilityResult {
    VISIBLE,
    DISTANCE_CULLED,
    SMALL_OBJECT_CULLED,
    FRUSTUM_CULLED,
};

/// @note 距離、極小、視錐台の順で最初の除外理由を返す。統計は呼び出し側が加算する。
[[nodiscard]] VisibilityResult EvaluateVisibility(
    const RenderCullingView& view, const RenderCullingItem& item);

/// @note 距離制限だけを評価する。半径未確定でも距離判定する既存の補助パス用。
[[nodiscard]] bool IsWithinDrawDistance(
    const RenderCullingView& view, const RenderCullingItem& item);

} /// @note namespace fbzz::renderer
