/// @file    ScriptMotionWarpProxy.hpp
/// @brief   Script から MotionWarpComponent へ寄せ先を渡すショートハンド
/// @author  Hasegawa Jin
/// @date    2026-08-25
#pragma once
#include <Engine/Api.hpp>

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Engine/Scene/Entity.hpp>
#include <cstdint>

namespace fbzz::scene {

class Script;
class GameObject;
enum class MotionWarpPointMode : std::uint8_t;

struct ScriptMotionWarpProxy {
    Script* script = nullptr;

    /// @note duration秒でworld位置へ寄せる。配置点のLIVE追従から数値入力へ切り替える。
    FBZZ_ENGINE_API void WarpTo(const math::Vector3& position, float duration) const;

    /// @note 位置と回転はworld。配置点のLIVE追従から数値入力へ切り替える。
    FBZZ_ENGINE_API void WarpToPose(const math::Vector3& position,
                    const math::Quaternion& rotation,
                    float duration) const;

    /// @brief 同じSceneのTargetPointを配置目標へ設定する。INVALIDは明示解除。
    /// @return 無効な対象・modeならfalseで接続と進行中warpは未変更。
    [[nodiscard]] FBZZ_ENGINE_API bool SetTargetPoint(EntityID target, MotionWarpPointMode mode) const;
    [[nodiscard]] FBZZ_ENGINE_API bool SetTargetPoint(const GameObject& target, MotionWarpPointMode mode) const;
    /// @brief 設定済み配置点のworld姿勢を捕捉して新しいwarpを始める。
    /// @return 未解決・不正な入力ならfalseで進行中warpは未変更。
    [[nodiscard]] FBZZ_ENGINE_API bool WarpToTarget(float duration) const;
    [[nodiscard]] FBZZ_ENGINE_API GameObject* GetTargetPoint() const;
    [[nodiscard]] FBZZ_ENGINE_API MotionWarpPointMode GetTargetPointMode() const;

    /// @note 軸ごとの補正率 [0,1]。既定は { 1, 0, 1 } で高さを動かさない。
    FBZZ_ENGINE_API void SetAxisWeight(const math::Vector3& weight) const;

    /// @note 補正だけで進んでよい速さの上限 [m/s]。0 で無制限。
    FBZZ_ENGINE_API void SetMaxSpeed(float metersPerSecond) const;

    /// @note 寄せを打ち切る。以降のフレームはクリップのルートモーションだけになる。
    FBZZ_ENGINE_API void Cancel() const;

    FBZZ_ENGINE_API void SetEnabled(bool enabled) const;

    [[nodiscard]] FBZZ_ENGINE_API bool  IsWarping() const;
    [[nodiscard]] FBZZ_ENGINE_API float GetRemainingTime() const;
    /// @note 目標までの残り距離 [m]。届いたかの判定に使う。
    [[nodiscard]] FBZZ_ENGINE_API float GetRemainingDistance() const;
};

} /// @note namespace fbzz::scene
