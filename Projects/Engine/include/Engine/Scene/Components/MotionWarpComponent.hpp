/// @file MotionWarpComponent.hpp
/// @brief ルートモーションの軌道を指定地点へ寄せる (Motion Warping) 設定
/// @author Hasegawa Jin
/// @date 2026-08-25
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

#include <cstdint>

namespace fbzz::scene {

/// 1 件の寄せ先。攻撃 1 回につき 1 つをスクリプトから設定する。
struct MotionWarpTarget {
    bool             active   = false;
    math::Vector3    position = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();

    bool warpPosition = true;
    bool warpRotation = false;

    /// 補正を配りきるまでの残り時間 [s]。0 以下になった時点で active を落とす。
    float remaining = 0.0f;
    /// 設定時の duration [s]。診断表示にだけ使う。
    float duration  = 0.0f;

    /// 軸ごとの補正率 [0,1]。y を 0 にすると高さは動かさない。
    ///
    /// WHY 既定で y=0 か: 接地しているキャラを地面から浮かせないため。高さを合わせたい
    ///     ケース (段差への飛び乗り) だけ明示的に 1 を入れる。
    math::Vector3 positionAxisWeight = { 1.0f, 0.0f, 1.0f };

    /// 補正だけで進んでよい速さの上限 [m/s]。0 で無制限。
    ///
    /// WHY: 遠い相手をターゲットにしたまま短い duration を渡すと、1 フレームで
    ///     数メートル滑る。上限を切ると「届かないものには届かない」で済む。
    float maxSpeed = 0.0f;
};

/// AnimatorComponent と同じ GameObject に置き、ルートモーション適用の直前に効く。
struct MotionWarpComponent {
    bool enabled = true;

    MotionWarpTarget target;

    math::Vector3 runtimeLastCorrection    = math::Vector3::ZERO;
    float         runtimeRemainingDistance = 0.0f;
    std::uint64_t runtimeWarpCount         = 0;

    const char* GetTypeName() const { return "Motion Warp"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
    }
};

} // namespace fbzz::scene
