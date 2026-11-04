/// @file    Input.hpp
/// @brief   キーボード・マウス入力のフレーム状態管理。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note /// Window の Win32 メッセージから現在状態を更新し、Update で前フレーム状態を保存する。
/// @note KeyDown / KeyUp は 1 フレームだけ true になる。
#pragma once
#include <Engine/Api.hpp>
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
    FBZZ_ENGINE_API static void Init();
    /// @note Explicit host gestures exclude injected input and do not require game ownership.
    FBZZ_ENGINE_API static bool PhysicalKeyHeld(KeyCode key);
    FBZZ_ENGINE_API static bool PhysicalKeyDown(KeyCode key);
    FBZZ_ENGINE_API static bool PhysicalMouseButtonDown(int button);

    /// @note Play セッション切り替え時に入力状態を全てクリアする
    FBZZ_ENGINE_API static void Reset();

    /// @note ゲームループ先頭で呼ぶ。前フレームの状態を保存する
    FBZZ_ENGINE_API static void Update();

    /// @note キーボード
    FBZZ_ENGINE_API static bool KeyDown(KeyCode key);   ///< @note 押した瞬間 (1 フレームのみ true)
    FBZZ_ENGINE_API static bool KeyHeld(KeyCode key);   ///< @note 押し続け
    FBZZ_ENGINE_API static bool KeyUp  (KeyCode key);   ///< @note 離した瞬間 (1 フレームのみ true)

    /// @note マウスボタン (0=左, 1=右, 2=中)
    FBZZ_ENGINE_API static bool MouseButton    (int button);
    FBZZ_ENGINE_API static bool MouseButtonDown(int button);
    FBZZ_ENGINE_API static bool MouseButtonUp  (int button);

    /// @note マウス座標 (クライアント座標)
    FBZZ_ENGINE_API static math::Vector2 MousePosition();
    FBZZ_ENGINE_API static math::Vector2 MouseDelta();
    /// @note CursorLockMode::Locked 用に OS カーソルを中央へ戻す前の移動量を注入する。
    /// @note SetCursorPos のワープ量を通常の座標差分に混ぜるとカメラデルタが相殺されるため分離する。
    FBZZ_ENGINE_API static void OverrideMouseDelta(const math::Vector2& delta);

    /// @note マウスホイール (1ノッチ = +1.0 / -1.0)
    FBZZ_ENGINE_API static float MouseScrollDelta();

    /// @name Window::WndProc から呼ぶ内部関数
    /// @note `msg` は Win32 の `UINT`、`wParam` は `WPARAM` と同じ幅。このヘッダーは
    /// @note エンジン全域へ伝播するため `<Windows.h>` を引かず、幅の一致は Input.cpp の
    /// @note static_assert で担保する。
    /// @{
    FBZZ_ENGINE_API static void HandleKeyMessage  (unsigned int msg, std::uintptr_t wParam);
    FBZZ_ENGINE_API static void HandleMouseMove   (int x, int y);
    FBZZ_ENGINE_API static void HandleMouseButton (unsigned int msg);
    FBZZ_ENGINE_API static void HandleMouseScroll (float delta);
    FBZZ_ENGINE_API static void HandleTextInput   (wchar_t character);
    /// @note Native key identity allows held acquisition controls to suppress only their own text.
    FBZZ_ENGINE_API static void HandleTextInput(wchar_t character, std::uint32_t physicalKey);
    FBZZ_ENGINE_API static std::string_view TextInput();
    /// @}

    /// @note AI 自動プレイテスト用の入力注入。通常入力と同じフレーム状態へ合成する。
    /// @note EditorMCP が Pause+Step と組み合わせ、決定的にゲームを操作するために使う。
    FBZZ_ENGINE_API static bool InjectKey(uint32_t virtualKey, bool pressed);
    FBZZ_ENGINE_API static bool InjectMouseButton(int button, bool pressed);
    FBZZ_ENGINE_API static void InjectMousePosition(const math::Vector2& position);
    FBZZ_ENGINE_API static void InjectMouseDelta(const math::Vector2& delta);
    FBZZ_ENGINE_API static void InjectMouseScroll(float delta);
    FBZZ_ENGINE_API static bool SetVirtualAxis(std::string_view name, float value);
    FBZZ_ENGINE_API static float GetVirtualAxis(std::string_view name);
    FBZZ_ENGINE_API static bool SetVirtualButton(std::string_view name, bool pressed);
    FBZZ_ENGINE_API static bool GetVirtualButton(std::string_view name);
    FBZZ_ENGINE_API static bool GetVirtualButtonDown(std::string_view name);
    FBZZ_ENGINE_API static bool GetVirtualButtonUp(std::string_view name);
    FBZZ_ENGINE_API static void ClearInjected();

private:
    friend struct core::Cursor;
    friend struct input_detail::GameInputAccess;
    friend struct scene::ScriptInputProxy;
    /// @note Records held controls for gameplay consumption without changing raw host/UI input.
    FBZZ_ENGINE_API static void ConsumeGameInput();
    FBZZ_ENGINE_API static void CancelGameInput();
    FBZZ_ENGINE_API static std::string_view GameTextInput();
    FBZZ_ENGINE_API static void ResetNativeState();
    FBZZ_ENGINE_API static bool GameKeyHeld(KeyCode key);
    FBZZ_ENGINE_API static bool GameKeyDown(KeyCode key);
    FBZZ_ENGINE_API static bool GameKeyUp(KeyCode key);
    FBZZ_ENGINE_API static bool GameMouseButton(int button);
    FBZZ_ENGINE_API static bool GameMouseButtonDown(int button);
    FBZZ_ENGINE_API static bool GameMouseButtonUp(int button);
    FBZZ_ENGINE_API static math::Vector2 GameMouseDelta();
    FBZZ_ENGINE_API static math::Vector2 GameMousePosition();
    FBZZ_ENGINE_API static float GameMouseScrollDelta();
    static std::array<bool, 256> s_previousPhysicalKeys;
    static std::array<bool, 3> s_previousPhysicalMouseButtons;
    FBZZ_ENGINE_API static std::string s_gameTextInput;
    static std::array<bool, 256> s_previousInjectedKeys;
    static std::array<bool, 3> s_previousInjectedMouseButtons;
    FBZZ_ENGINE_API static math::Vector2 s_injectedMouseDelta;
    FBZZ_ENGINE_API static math::Vector2 s_injectedMousePosition;
    FBZZ_ENGINE_API static float s_injectedMouseScroll;
    FBZZ_ENGINE_API static bool s_hasInjectedMouseDelta;
    FBZZ_ENGINE_API static bool s_hasInjectedMousePosition;
    static constexpr int KEY_COUNT = 256;

    static std::array<bool, KEY_COUNT> s_current;
    static std::array<bool, KEY_COUNT> s_previous;
    static std::array<bool, 3>         s_mouseCurrent;
    static std::array<bool, 3>         s_mousePrevious;
    FBZZ_ENGINE_API static math::Vector2               s_mousePos;
    FBZZ_ENGINE_API static math::Vector2               s_prevMousePos;
    FBZZ_ENGINE_API static math::Vector2               s_overrideMouseDelta;
    FBZZ_ENGINE_API static float                       s_scrollDelta;
    FBZZ_ENGINE_API static bool                        s_hasOverrideMouseDelta;
    static std::array<bool, KEY_COUNT> s_injectedKeys;
    static std::array<bool, 3>         s_injectedMouseButtons;
    static std::unordered_map<std::string, float> s_virtualAxes;
    static std::unordered_map<std::string, bool> s_virtualButtons;
    static std::unordered_map<std::string, bool> s_previousVirtualButtons;
    FBZZ_ENGINE_API static std::string s_textInput;
};

} /// @note namespace fbzz::input
