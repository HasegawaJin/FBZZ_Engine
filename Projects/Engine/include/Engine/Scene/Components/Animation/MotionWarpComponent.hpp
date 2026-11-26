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
    bool             active   = false; ///< ルートモーションのワープを実行中かどうか。
    math::Vector3    position = math::Vector3::ZERO; ///< ワープ終了時に合わせる目標のワールド位置 [m]。
    math::Quaternion rotation = math::Quaternion::Identity(); ///< ワープ終了時に合わせる目標のワールド姿勢。

    bool warpPosition = true; ///< ワープでルートモーションの位置を補正するかどうか。
    bool warpRotation = false; ///< ワープでルートモーションの回転を補正するかどうか。

    float remaining = 0.0f; ///< 補正の残り時間 [s]。0 に達したらワープを終了する。
    float duration  = 0.0f; ///< ワープ開始時に確定する継続時間 [s]。

    /// @brief 軸ごとの補正の重み [0, 1]。
    /// @note 既定 y=0 は接地キャラを地面から浮かせないため。段差への飛び乗り等
    /// @note 高さを合わせたいケースだけ明示的に 1 を入れる。
    math::Vector3 positionAxisWeight = { 1.0f, 0.0f, 1.0f }; ///< ワールド XYZ 各軸の位置のずれを補正へ反映する割合。

    /// @brief 補正速度の上限 [m/s]。0 は上限なし。
    /// @note 遠い相手をターゲットにしたまま短い duration を渡すと 1 フレームで数メートル
    /// @note 上限を設定すると届かない対象へ大きく滑る動きを抑えられる。
    float maxSpeed = 0.0f; ///< ワープ補正の移動速度の上限 [m/s]。0 は制限なし。
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
    bool enabled = true; ///< このコンポーネントによるルートモーション補正を有効にするかどうか。

    MotionWarpTarget target; ///< 現在のワープの目標姿勢と残時間。
    ObjectReference targetReference; ///< ワープ先として設定された TargetPoint へのシーン参照。
    MotionWarpPointMode targetPointMode = MotionWarpPointMode::CAPTURED; ///< 名前付きの対象姿勢を開始時に確定するか、ワープの実行中も追従するか。
    MotionWarpTargetSource runtimeTargetSource = MotionWarpTargetSource::NUMERIC; ///< 数値指定の WarpTo に設定済み参照を引き継がせないための実行時専用の取得元。

    math::Vector3 runtimeLastCorrection    = math::Vector3::ZERO; ///< 直近のフレームでルート移動に加えた補正変位 [m]。
    float         runtimeRemainingDistance = 0.0f; ///< ワープの目標位置までの残り距離 [m]。
    std::uint64_t runtimeWarpCount         = 0; ///< ワープの適用回数を数える実行時の診断値。

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
