// FBZZ Engine
// GamepadButton.hpp | fbzz::input
// ゲームパッドのボタン・軸の論理定義
// XInput の生ビットマスクをそのまま外へ出さず、エンジン独自の連番へ写像する。
// WHY: XINPUT_GAMEPAD_* はビットフラグのため配列添字として使えず、
//      「前フレーム状態を配列で持って差分を取る」既存 Input の実装様式に噛み合わない。
//      連番にすることで Gamepad 側も std::array<bool, COUNT> で統一できる。
#pragma once
#include <cstdint>

namespace fbzz::input {

// ゲームパッドのボタン。値は配列添字として使うため 0 から連続していること。
// COUNT は要素数の取得専用であり、実ボタンとして渡してはならない。
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

    // スティックの押し込み (L3 / R3)
    LEFT_STICK,
    RIGHT_STICK,

    START,
    BACK,

    // アナログトリガーをボタンとしても扱えるようにする。
    // WHY: 「RT で射撃」のような用途では、呼び出し側が毎回閾値比較を書くのは冗長で
    //      閾値がバラつく原因になる。デバイス層で一意の閾値を定義して吸収する。
    LEFT_TRIGGER,
    RIGHT_TRIGGER,

    COUNT
};

// ゲームパッドのアナログ軸。値は配列添字として使うため 0 から連続していること。
enum class GamepadAxis : uint8_t {
    LEFT_STICK_X = 0,  // -1(左) 〜 +1(右)
    LEFT_STICK_Y,      // -1(下) 〜 +1(上)
    RIGHT_STICK_X,
    RIGHT_STICK_Y,
    LEFT_TRIGGER,      // 0 〜 1
    RIGHT_TRIGGER,     // 0 〜 1

    COUNT
};

// トリガーをボタンとみなす閾値。
// WHY 0.5 か: Xbox コントローラーの実機では、意図せず触れた程度で 0.1 前後まで上がる。
//             一方 0.8 まで上げると「奥まで引き切らないと反応しない」と感じられる。
//             中間の 0.5 が誤爆と応答性のバランス点。
inline constexpr float GAMEPAD_TRIGGER_BUTTON_THRESHOLD = 0.5f;

// 列挙名を UI 表示・シリアライズに使うための文字列化。
// 未知の値には "None" を返し、呼び出し側が nullptr チェックを書かなくて済むようにする。
[[nodiscard]] const char* ToString(GamepadButton button) noexcept;
[[nodiscard]] const char* ToString(GamepadAxis axis) noexcept;

// ToString の逆変換。未知の文字列に対しては false を返し、out を書き換えない。
// WHY bool 戻り値か: 不正な入力は .inputactions の手編集ミスであり回復可能。
//                    Docs/conventions/error_handling.md に従い assert ではなく戻り値で表す。
[[nodiscard]] bool ParseGamepadButton(const char* name, GamepadButton& out) noexcept;
[[nodiscard]] bool ParseGamepadAxis(const char* name, GamepadAxis& out) noexcept;

} // namespace fbzz::input
