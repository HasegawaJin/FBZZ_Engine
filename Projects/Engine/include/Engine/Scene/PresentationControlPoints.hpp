/// @file    PresentationControlPoints.hpp
/// @brief   表示用の参照先を不変の数値入力に解決する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/Components/MotionWarpComponent.hpp>
#include <Engine/Scene/ControlPointResolver.hpp>
#include <Engine/Scene/ControlPointModes.hpp>
#include <Math/Vector3.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::scene {

class Scene;
struct LineRendererComponent;
enum class LineSpace : int;

inline constexpr std::uint32_t LINE_CONTROL_POINT_ADAPTER_VERSION = 1;

/// @brief Line の点入力元の検証と解決の結果。
enum class LineControlPointStatus : std::uint8_t {
    SUCCESS,
    INVALID_SOURCE_MODE,
    INVALID_RUNTIME_MODE,
    INVALID_NUMERIC_DATA,
    MISSING_BAKE,
    STALE_BAKE,
    RESOLUTION_FAILED,
};

/// @brief Line 形状の生成に使う実効の順序付き点列。
struct LineControlPointSnapshot {
    std::vector<math::Vector3> points;
    LineSpace space = static_cast<LineSpace>(0);
    LineControlPointStatus status = LineControlPointStatus::SUCCESS;
    ControlPointResolveStatus resolveStatus = ControlPointResolveStatus::SUCCESS;
    std::uint64_t inputSignature = 0;
    bool bakeStale = false;
    bool valid = false;
};

/// @brief 現在の制作時の入力元を解決し、正規化した数値署名を取得する。
/// @note 成功時の署名は順序付きの安定 ID と数値座標を使い、個体 GUID や Scene キャッシュを含めない。
[[nodiscard]] FBZZ_ENGINE_API LineControlPointSnapshot ResolveLineControlPointAuthoring(
    Scene& scene, EntityID ownerId, LineRendererComponent& line,
    ControlPointRuntimeMode signatureRuntimeMode);

/// @brief Line の現在の入力元を解決するか、記録済みの静的な数値を返す。
/// @note RUNTIME は参照先の個体を解決せず、BAKED_STATIC の数値を読む。
[[nodiscard]] FBZZ_ENGINE_API LineControlPointSnapshot ResolveLineControlPoints(
    Scene& scene, EntityID ownerId, LineRendererComponent& line,
    ControlPointReadContext context = ControlPointReadContext::AUTHORING);

/// @brief 設定された参照先の姿勢を記録し、個体参照を使う MotionWarp を一回開始する。
/// @note LIVE の参照先は所有者の階層外に置く。CAPTURED の参照先には子孫も使える。
[[nodiscard]] FBZZ_ENGINE_API bool BeginMotionWarpToTarget(Scene& scene, EntityID ownerId,
    MotionWarpComponent& warp, float duration);

/// @brief 実行中の個体参照型 LIVE MotionWarp の姿勢だけを更新する。
/// @return 参照が解決できないか所有者の階層内に入った場合は false を返し、実行中のワープを取り消す。
[[nodiscard]] FBZZ_ENGINE_API bool RefreshLiveMotionWarpTarget(Scene& scene, EntityID ownerId,
    MotionWarpComponent& warp);

}
