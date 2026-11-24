/// @file    MotionWarpComponent.hpp
/// @brief   ルートモーションの軌道を指定地点へ寄せる (Motion Warping) 設定
/// @author  Hasegawa Jin
/// @date    2026-08-25
#pragma once

#include <Engine/Reflection/Reflection.hpp>
#include <Engine/Scene/References/ObjectReference.hpp>
#include <Math/LinearAlgebra/Quaternion.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>

#include <cstdint>

namespace fbzz::scene {

/// @brief 実行中のワープ一件分の数値姿勢と補正方針。
struct MotionWarpTarget {
    bool             active   = false;
    math::Vector3    position = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();

    bool warpPosition = true;
    bool warpRotation = false;

    /// @brief 補正の残り時間 [s]。0 に達したらワープを終了する。
    float remaining = 0.0f;
    /// @brief ワープ開始時に確定する継続時間 [s]。
    float duration  = 0.0f;

    /// @brief 軸ごとの補正の重み [0, 1]。
    /// @note 既定 y=0 は接地キャラを地面から浮かせないため。段差への飛び乗り等
    /// @note 高さを合わせたいケースだけ明示的に 1 を入れる。
    math::Vector3 positionAxisWeight = { 1.0f, 0.0f, 1.0f };

    /// @brief 補正速度の上限 [m/s]。0 は上限なし。
    /// @note 遠い相手をターゲットにしたまま短い duration を渡すと 1 フレームで数メートル
    /// @note 上限を設定すると届かない対象へ大きく滑る動きを抑えられる。
    float maxSpeed = 0.0f;
};

/// @brief ワープ中に設定済みの TargetPoint を追従する方針。
enum class MotionWarpPointMode : std::uint8_t {
    CAPTURED = 0,
    LIVE = 1,
};

/// @brief 実行中のワープ姿勢の取得元。
enum class MotionWarpTargetSource : std::uint8_t {
    NUMERIC = 0,
    POINT_OBJECT = 1,
};

/// @brief ルートモーションを出力する前に、数値指定または TargetPoint による補正を適用する。
struct MotionWarpComponent {
    bool enabled = true;

    MotionWarpTarget target;
    ObjectReference targetReference;
    /// @brief 名前付きの対象姿勢を開始時に確定するか、ワープの実行中も追従するか。
    MotionWarpPointMode targetPointMode = MotionWarpPointMode::CAPTURED;
    /// @brief 数値指定の WarpTo に設定済み参照を引き継がせないための実行時専用の取得元。
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

}
