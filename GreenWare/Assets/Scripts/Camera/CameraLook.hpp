/// @file    CameraLook.hpp
/// @brief   視点入力 (マウス / 右スティック) を Option の設定込みで旋回量へ直す
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// @note TPS/FPS で回転式を共有する。個別実装だと感度・不感帯の調整が片方だけずれても気付けない。
/// @note GameSettingsComponent に依存するため、Game/ を知らない Utils/ でなく Camera/ に置く。
/// @note マウス Delta ([px/frame]) とスティック (-1..1) は単位が違うため、感度をデバイスごとに分ける。
#pragma once

#include <Engine/Input/Input.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector2.hpp>
#include <Scripts/Game/GameSettingsComponent.hpp>

namespace sandbox::cameralook {

/// マウスの移動量に感度を掛けた旋回量 (度)。
/// sensitivity は「Option が既定値のときにどれだけ回るか」で、設定はこれに掛かる。
[[nodiscard]] inline fbzz::math::Vector2 MouseDelta(
    const fbzz::scene::ScriptInputProxy& input, float sensitivity)
{
    return input.GetMouseDelta() * (sensitivity * GameSettingsComponent::MouseSensScale());
}

/// @brief 右スティックの倒し量を、不感帯と応答カーブを通した -1..1 へ落とす。無入力なら (0,0)。
/// @note 不感帯は半径で切る。軸ごとに切ると正方形になり、斜め入力の実効感度が方向で変わる。
/// @note 不感帯・曲線は Option の値をそのまま使う (倍率で掛けない)。設定と実際の効きの対応を保つため。
[[nodiscard]] inline fbzz::math::Vector2 PadAxis(const fbzz::scene::ScriptInputProxy& input)
{
    using namespace fbzz::math;
    const Vector2 raw{ input.GetPadAxis(fbzz::input::GamepadAxis::RIGHT_STICK_X),
                       input.GetPadAxis(fbzz::input::GamepadAxis::RIGHT_STICK_Y) };

    const InputConfig& settings = GameSettingsComponent::InputOrDefault();
    const float magnitude = raw.Length();
    const float dead      = Clamp01(settings.deadzone);
    if (magnitude <= dead) return Vector2::ZERO;

    /// @note 不感帯の外側を 0..1 へ引き直す。境界を跨いだ瞬間に速度が飛ばない。
    const float normalized = Clamp01((magnitude - dead) / Max(1.0f - dead, EPSILON));
    const float curved     = Pow(normalized, Max(CurveExponent(settings.curve), 1.0f));
    return raw * (curved / magnitude);
}

/// 右スティックの旋回速度 (度/秒)。padLookSpeed は Option が既定値のときの速さ。
[[nodiscard]] inline float PadLookSpeed(float padLookSpeed)
{
    return padLookSpeed * GameSettingsComponent::StickSensScale();
}

} // namespace sandbox::cameralook
