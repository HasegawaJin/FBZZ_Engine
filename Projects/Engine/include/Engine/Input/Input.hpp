// FBZZ Engine
// Input.hpp | fbzz::input
// キーボード・マウス入力のフレーム状態管理
// Window の Win32 メッセージから現在状態を更新し、Update で前フレーム状態を保存する。
// KeyDown / KeyUp は 1 フレームだけ true になる。
#pragma once
#include "KeyCode.hpp"
#include "Math/Vector2.hpp"

#include <array>
#include <Windows.h>

namespace fbzz::input {

class Input {
public:
    // Window 生成後に一度呼ぶ
    static void Init();

    // Play セッション切り替え時に入力状態を全てクリアする
    static void Reset();

    // ゲームループ先頭で呼ぶ。前フレームの状態を保存する
    static void Update();

    // キーボード
    static bool KeyDown(KeyCode key);   // 押した瞬間 (1 フレームのみ true)
    static bool KeyHeld(KeyCode key);   // 押し続け
    static bool KeyUp  (KeyCode key);   // 離した瞬間 (1 フレームのみ true)

    // マウスボタン (0=左, 1=右, 2=中)
    static bool MouseButton    (int button);
    static bool MouseButtonDown(int button);
    static bool MouseButtonUp  (int button);

    // マウス座標 (クライアント座標)
    static math::Vector2 MousePosition();
    static math::Vector2 MouseDelta();
    // CursorLockMode::Locked 用に OS カーソルを中央へ戻す前の移動量を注入する。
    // WHY: SetCursorPos によるワープを通常の座標差分へ混ぜると、ゲームカメラのデルタが相殺されるため。
    static void OverrideMouseDelta(const math::Vector2& delta);

    // マウスホイール (1ノッチ = +1.0 / -1.0)
    static float MouseScrollDelta();

    // Window::WndProc から呼ぶ内部関数
    static void HandleKeyMessage  (UINT msg, WPARAM wParam);
    static void HandleMouseMove   (int x, int y);
    static void HandleMouseButton (UINT msg);
    static void HandleMouseScroll (float delta);

private:
    static constexpr int KEY_COUNT = 256;

    static std::array<bool, KEY_COUNT> s_current;
    static std::array<bool, KEY_COUNT> s_previous;
    static std::array<bool, 3>         s_mouseCurrent;
    static std::array<bool, 3>         s_mousePrevious;
    static math::Vector2               s_mousePos;
    static math::Vector2               s_prevMousePos;
    static math::Vector2               s_overrideMouseDelta;
    static float                       s_scrollDelta;
    static bool                        s_hasOverrideMouseDelta;
};

} // namespace fbzz::input
