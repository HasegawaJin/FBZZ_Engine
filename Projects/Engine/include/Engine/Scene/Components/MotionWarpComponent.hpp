/// @file    MotionWarpComponent.hpp
/// @brief   ルートモーションの軌道を指定地点へ寄せる (Motion Warping) 設定
/// @author  Hasegawa Jin
/// @date    2026-08-25
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ObjectReference.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

#include <cstdint>

namespace fbzz::scene {

/// @brief Numeric pose and correction policy for one active warp.
struct MotionWarpTarget {
    bool             active   = false;
    math::Vector3    position = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();

    bool warpPosition = true;
    bool warpRotation = false;

    /// @brief Remaining correction time [s]; reaching zero deactivates the warp.
    float remaining = 0.0f;
    /// @brief Duration [s] captured when the warp begins.
    float duration  = 0.0f;

    /// @brief Per-axis correction weights in [0,1].
    /// @note 既定 y=0 は接地キャラを地面から浮かせないため。段差への飛び乗り等
    /// @note 高さを合わせたいケースだけ明示的に 1 を入れる。
    math::Vector3 positionAxisWeight = { 1.0f, 0.0f, 1.0f };

    /// @brief Maximum correction speed [m/s]; zero disables the limit.
    /// @note 遠い相手をターゲットにしたまま短い duration を渡すと 1 フレームで数メートル
    /// @note 上限を設定すると届かない対象へ大きく滑る動きを抑えられる。
    float maxSpeed = 0.0f;
};

/// @brief Policy for following a configured TargetPoint during a warp.
enum class MotionWarpPointMode : std::uint8_t {
    CAPTURED = 0,
    LIVE = 1,
};

/// @brief Runtime source of the active numeric warp pose.
enum class MotionWarpTargetSource : std::uint8_t {
    NUMERIC = 0,
    POINT_OBJECT = 1,
};

/// @brief Applies a numeric or TargetPoint-backed correction before root-motion output.
struct MotionWarpComponent {
    bool enabled = true;

    MotionWarpTarget target;
    ObjectReference targetReference;
    /// @brief Whether a named target warp captures the pose once or follows it while active.
    MotionWarpPointMode targetPointMode = MotionWarpPointMode::CAPTURED;
    /// @brief Runtime-only source so numeric WarpTo calls do not inherit the configured reference.
    MotionWarpTargetSource runtimeTargetSource = MotionWarpTargetSource::NUMERIC;

    math::Vector3 runtimeLastCorrection    = math::Vector3::ZERO;
    float         runtimeRemainingDistance = 0.0f;
    std::uint64_t runtimeWarpCount         = 0;

    const char* GetTypeName() const { return "Motion Warp"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.TypedObjectReferenceField("targetReference", targetReference, "TargetPointComponent");
        int mode = static_cast<int>(targetPointMode);
        static constexpr const char* MODES[] = { "Captured", "Live" };
        r.Enum("targetPointMode", mode, MODES);
        targetPointMode = static_cast<MotionWarpPointMode>(mode < 0 || mode > 1 ? 0xff : mode);
    }
};

} /// @note namespace fbzz::scene
