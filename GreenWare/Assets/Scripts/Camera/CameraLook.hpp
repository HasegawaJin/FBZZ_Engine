/// @file CameraLook.hpp
/// @brief 視点入力 (マウス / 右スティック) を Option の設定込みで旋回量へ直す
/// @author Hasegawa Jin
/// @date 2026-08-26
///
/// WHY カメラの外へ出すか:
///   TPS と FPS で違うのは「目をどこに置くか」だけで、視点の回り方は同じでなければ
///   ならない。同じ式を 2 つ持つと、感度や不感帯の扱いを片方だけ直したときに、
///   カメラを差し替えただけで視点の効きが変わる。しかもコンパイルは通るので、
///   気付けるのは「FPS だけスティックが重い」という遊んだ感触だけになる。
///
/// WHY Utils/ ではなく Camera/ に置くか:
///   Option (GameSettingsComponent) に依存する。Utils/ のヘルパーはどれも Game/ を
///   知らない側にあり、そこへ混ぜると依存の向きが 1 箇所だけ逆になる。読む側は
///   カメラしか居ないので、カメラの隣に置く。
///
/// WHY LookX / LookY (アクション層) を使わないか:
///   マウス Delta は「1 フレームに何ピクセル動いたか」、スティックは「-1..1 の倒し量」で
///   単位が違う。1 本の感度で両方を扱うと、マウスに合わせればスティックが動かず、
///   スティックに合わせればマウスが暴れる。デバイスごとに感度を持たせるしかない。
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

/// 右スティックの倒し量を、不感帯と応答カーブを通した -1..1 へ落とす。無入力なら (0,0)。
///
/// WHY 軸ごとではなく半径で不感帯を切るか: 軸ごとに切ると正方形の不感帯になり、
///     斜めに倒したときの実効感度が方向によって変わる。
///
/// 不感帯と曲線は Option の値をそのまま使う。倍率で掛けないのは、どちらも
/// 「どこから効き始めるか」「どんな効き方か」という形そのもので、スクリプト側の
/// 値と混ぜると、設定を見ても実際の効きが判らなくなるため。
[[nodiscard]] inline fbzz::math::Vector2 PadAxis(const fbzz::scene::ScriptInputProxy& input)
{
    using namespace fbzz::math;
    const Vector2 raw{ input.GetPadAxis(fbzz::input::GamepadAxis::RIGHT_STICK_X),
                       input.GetPadAxis(fbzz::input::GamepadAxis::RIGHT_STICK_Y) };

    const InputConfig& settings = GameSettingsComponent::InputOrDefault();
    const float magnitude = raw.Length();
    const float dead      = Clamp01(settings.deadzone);
    if (magnitude <= dead) return Vector2::ZERO;

    // 不感帯の外側を 0..1 へ引き直す。境界を跨いだ瞬間に速度が飛ばない。
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
