/// @file    GamepadButton.hpp
/// @brief   ゲームパッドのボタン・軸の論理定義。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note XInput のビットフラグ (XINPUT_GAMEPAD_*) は配列添字に使えないため、独自の連番へ写像し
///       Gamepad 側も std::array<bool, COUNT> で統一する。
#pragma once
#include <cstdint>

namespace fbzz::input {

/// ゲームパッドのボタン。値は配列添字として使うため 0 から連続していること。
/// COUNT は要素数の取得専用であり、実ボタンとして渡してはならない。
enum class GamepadButton : uint16_t {
    A = 0,
    B,
    X,
    Y,

    DPAD_UP,
    DPAD_DOWN,
    DPAD_LEFT,
    DPAD_RIGHT,

    LEFT_SHOULDER,
    RIGHT_SHOULDER,

    /// スティックの押し込み (L3 / R3)
    LEFT_STICK,
    RIGHT_STICK,

    START,
    BACK,

    /// アナログトリガーを閾値ベースのボタンとしても扱える。
    /// @note 呼び出し側で毎回閾値比較すると値がばらつくため、デバイス層で一意の閾値に統一する。
    LEFT_TRIGGER,
    RIGHT_TRIGGER,

    COUNT
};

/// ゲームパッドのアナログ軸。値は配列添字として使うため 0 から連続していること。
enum class GamepadAxis : uint8_t {
    LEFT_STICK_X = 0,  ///< -1(左) 〜 +1(右)
    LEFT_STICK_Y,      ///< -1(下) 〜 +1(上)
    RIGHT_STICK_X,
    RIGHT_STICK_Y,
    LEFT_TRIGGER,      ///< 0 〜 1
    RIGHT_TRIGGER,     ///< 0 〜 1

    COUNT
};

/// トリガーをボタンとみなす閾値。
/// @note 実機では誤タッチで 0.1 前後まで上がり、0.8 だと反応が鈍く感じられるため中間の 0.5 を取る。
inline constexpr float GAMEPAD_TRIGGER_BUTTON_THRESHOLD = 0.5f;

/// 列挙名を UI 表示・シリアライズに使うための文字列化。
/// 未知の値には "None" を返し、呼び出し側が nullptr チェックを書かなくて済むようにする。
[[nodiscard]] const char* ToString(GamepadButton button) noexcept;
[[nodiscard]] const char* ToString(GamepadAxis axis) noexcept;

/// ToString の逆変換。未知の文字列に対しては false を返し、out を書き換えない。
/// @note .inputactions の手編集ミスは回復可能なため、assert でなく戻り値で表す。
/// @see Docs/conventions/error_handling.md
[[nodiscard]] bool ParseGamepadButton(const char* name, GamepadButton& out) noexcept;
[[nodiscard]] bool ParseGamepadAxis(const char* name, GamepadAxis& out) noexcept;

} // namespace fbzz::input
