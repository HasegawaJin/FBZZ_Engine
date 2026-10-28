/// @file    Input.hpp
/// @brief   キーボード・マウス入力のフレーム状態管理。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note /// Window の Win32 メッセージから現在状態を更新し、Update で前フレーム状態を保存する。
/// @note KeyDown / KeyUp は 1 フレームだけ true になる。
#pragma once
#include "KeyCode.hpp"
#include "Math/Vector2.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace fbzz::core { struct Cursor; }
namespace fbzz::scene { struct ScriptInputProxy; }
namespace fbzz::input::input_detail { struct GameInputAccess; }

namespace fbzz::input {

class Input {
public:
    /// @note Window 生成後に一度呼ぶ
    static void Init();
    /// @note Explicit host gestures exclude injected input and do not require game ownership.
    static bool PhysicalKeyHeld(KeyCode key);
    static bool PhysicalKeyDown(KeyCode key);
    static bool PhysicalMouseButtonDown(int button);

    /// @note Play セッション切り替え時に入力状態を全てクリアする
    static void Reset();

    /// @note ゲームループ先頭で呼ぶ。前フレームの状態を保存する
    static void Update();

    /// @note キーボード
    static bool KeyDown(KeyCode key);   ///< @note 押した瞬間 (1 フレームのみ true)
    static bool KeyHeld(KeyCode key);   ///< @note 押し続け
    static bool KeyUp  (KeyCode key);   ///< @note 離した瞬間 (1 フレームのみ true)

    /// @note マウスボタン (0=左, 1=右, 2=中)
    static bool MouseButton    (int button);
    static bool MouseButtonDown(int button);
    static bool MouseButtonUp  (int button);

    /// @note マウス座標 (クライアント座標)
    static math::Vector2 MousePosition();
    static math::Vector2 MouseDelta();
    /// @note CursorLockMode::Locked 用に OS カーソルを中央へ戻す前の移動量を注入する。
    /// @note SetCursorPos のワープ量を通常の座標差分に混ぜるとカメラデルタが相殺されるため分離する。
    static void OverrideMouseDelta(const math::Vector2& delta);

    /// @note マウスホイール (1ノッチ = +1.0 / -1.0)
    static float MouseScrollDelta();

    /// @name Window::WndProc から呼ぶ内部関数
    /// @note `msg` は Win32 の `UINT`、`wParam` は `WPARAM` と同じ幅。このヘッダーは
    /// @note エンジン全域へ伝播するため `<Windows.h>` を引かず、幅の一致は Input.cpp の
    /// @note static_assert で担保する。
    ///@{
    static void HandleKeyMessage  (unsigned int msg, std::uintptr_t wParam);
    static void HandleMouseMove   (int x, int y);
    static void HandleMouseButton (unsigned int msg);
    static void HandleMouseScroll (float delta);
    static void HandleTextInput   (wchar_t character);
    /// @note Native key identity allows held acquisition controls to suppress only their own text.
    static void HandleTextInput(wchar_t character, std::uint32_t physicalKey);
    static std::string_view TextInput();
    ///@}

    /// @note AI 自動プレイテスト用の入力注入。通常入力と同じフレーム状態へ合成する。
    /// @note EditorMCP が Pause+Step と組み合わせ、決定的にゲームを操作するために使う。
    static bool InjectKey(uint32_t virtualKey, bool pressed);
    static bool InjectMouseButton(int button, bool pressed);
    static void InjectMousePosition(const math::Vector2& position);
    static void InjectMouseDelta(const math::Vector2& delta);
    static void InjectMouseScroll(float delta);
    static bool SetVirtualAxis(std::string_view name, float value);
    static float GetVirtualAxis(std::string_view name);
    static bool SetVirtualButton(std::string_view name, bool pressed);
    static bool GetVirtualButton(std::string_view name);
    static bool GetVirtualButtonDown(std::string_view name);
    static bool GetVirtualButtonUp(std::string_view name);
    static void ClearInjected();

private:
    friend struct core::Cursor;
    friend struct input_detail::GameInputAccess;
    friend struct scene::ScriptInputProxy;
    /// @note Records held controls for gameplay consumption without changing raw host/UI input.
    static void ConsumeGameInput();
    static void CancelGameInput();
    static std::string_view GameTextInput();
    static void ResetNativeState();
    static bool GameKeyHeld(KeyCode key);
    static bool GameKeyDown(KeyCode key);
    static bool GameKeyUp(KeyCode key);
    static bool GameMouseButton(int button);
    static bool GameMouseButtonDown(int button);
    static bool GameMouseButtonUp(int button);
    static math::Vector2 GameMouseDelta();
    static math::Vector2 GameMousePosition();
    static float GameMouseScrollDelta();
    static std::array<bool, 256> s_previousPhysicalKeys;
    static std::array<bool, 3> s_previousPhysicalMouseButtons;
    static std::string s_gameTextInput;
    static std::array<bool, 256> s_previousInjectedKeys;
    static std::array<bool, 3> s_previousInjectedMouseButtons;
    static math::Vector2 s_injectedMouseDelta;
    static math::Vector2 s_injectedMousePosition;
    static float s_injectedMouseScroll;
    static bool s_hasInjectedMouseDelta;
    static bool s_hasInjectedMousePosition;
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
    static std::array<bool, KEY_COUNT> s_injectedKeys;
    static std::array<bool, 3>         s_injectedMouseButtons;
    static std::unordered_map<std::string, float> s_virtualAxes;
    static std::unordered_map<std::string, bool> s_virtualButtons;
    static std::unordered_map<std::string, bool> s_previousVirtualButtons;
    static std::string s_textInput;
};

} /// @note namespace fbzz::input
