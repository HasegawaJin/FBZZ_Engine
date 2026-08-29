/// @file    ScriptMotionWarpProxy.hpp
/// @brief   Script から MotionWarpComponent へ寄せ先を渡すショートハンド
/// @author  Hasegawa Jin
/// @date    2026-08-25
#pragma once

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;

struct ScriptMotionWarpProxy {
    Script* script = nullptr;

    /// duration 秒かけて position へ寄せる。攻撃の踏み込み開始時に 1 回呼ぶ。
    void WarpTo(const math::Vector3& position, float duration) const;

    /// 位置に加えて向きも合わせる。斬りつける方向を相手へ向けたいときに使う。
    void WarpToPose(const math::Vector3& position,
                    const math::Quaternion& rotation,
                    float duration) const;

    /// 軸ごとの補正率 [0,1]。既定は { 1, 0, 1 } で高さを動かさない。
    void SetAxisWeight(const math::Vector3& weight) const;

    /// 補正だけで進んでよい速さの上限 [m/s]。0 で無制限。
    void SetMaxSpeed(float metersPerSecond) const;

    /// 寄せを打ち切る。以降のフレームはクリップのルートモーションだけになる。
    void Cancel() const;

    void SetEnabled(bool enabled) const;

    [[nodiscard]] bool  IsWarping() const;
    [[nodiscard]] float GetRemainingTime() const;
    /// 目標までの残り距離 [m]。届いたかの判定に使う。
    [[nodiscard]] float GetRemainingDistance() const;
};

} // namespace fbzz::scene
