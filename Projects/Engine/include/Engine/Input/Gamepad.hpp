/// @file    Gamepad.hpp
/// @brief   XInput ゲームパッドのフレーム状態管理と振動制御。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note ポーリング方式: XInput は Win32 メッセージポンプに乗らないため、フレーム先頭で 1 回だけ XInputGetState を呼ぶ。
/// @note DirectInput は不採用: PS 系パッドも Steam Input / DS4Windows 経由で XInput として認識されるため実用上十分。
/// @note デッドゾーン未適用: 適正値はアクションごとに異なるため生値 (-1..1) を返し、InputActionMap に委ねる。
#pragma once
#include "GamepadButton.hpp"
#include <cstdint>

namespace fbzz::input {

class Gamepad {
public:
    /// XInput が扱えるコントローラー数の上限 (XUSER_MAX_COUNT と同値)
    static constexpr int MAX_PADS = 4;

    /// @brief Input::Update() の先頭から呼ばれる。個別に呼ぶ必要はない。
    /// @note 更新経路 (Editor/Standalone/Sandbox) が複数あるため、呼び忘れ事故を避けて 1 箇所に集約する。
    static void Update();

    /// 全状態を初期化する。Play モードの開始・終了など、
    /// 入力の連続性を切りたい境界で呼ぶ。
    static void Reset();

    [[nodiscard]] static bool IsConnected(int pad = 0);

    /// @brief 接続中のパッドのうち最も若いスロット番号。1 台も無ければ -1。
    /// @note スロット 0 決め打ちだと 2 番目のポートに挿した際に無反応になるため使う。
    [[nodiscard]] static int GetFirstConnectedPad();

    [[nodiscard]] static bool ButtonHeld(GamepadButton button, int pad = 0);
    [[nodiscard]] static bool ButtonDown(GamepadButton button, int pad = 0);  ///< 押した瞬間のみ true
    [[nodiscard]] static bool ButtonUp  (GamepadButton button, int pad = 0);  ///< 離した瞬間のみ true

    /// スティックは -1..1、トリガーは 0..1。デッドゾーン未適用の生値。
    [[nodiscard]] static float Axis(GamepadAxis axis, int pad = 0);

    /// @name 振動
    /// @{
    /// @brief lowFrequency / highFrequency はともに 0..1。durationSeconds 経過で自動停止する。
    /// @note 停止を呼び出し側任せにすると呼び忘れで振動しっぱなしになるため、デバイス層で寿命を管理する。
    static void SetVibration(float lowFrequency, float highFrequency,
                             float durationSeconds, int pad = 0);
    static void StopVibration(int pad = 0);
    static void StopAllVibration();
    /// @}
};

} // namespace fbzz::input
