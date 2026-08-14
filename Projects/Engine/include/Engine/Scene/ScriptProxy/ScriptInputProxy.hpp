// FBZZ Engine
// ScriptInputProxy.hpp | fbzz::scene
// Script から入力状態を読むためのショートハンド
#pragma once

#include <Math/Vector2.hpp>
#include <cstdint>
#include <string_view>

namespace fbzz::input {
enum class KeyCode : uint32_t;
enum class GamepadButton : uint16_t;
enum class GamepadAxis : uint8_t;
}

namespace fbzz::scene {

class Script;

enum class MouseBtn : int { Left = 0, Right = 1, Middle = 2 };

struct ScriptInputProxy {
    Script* script = nullptr;

    bool GetKey(input::KeyCode key) const;
    bool GetKeyDown(input::KeyCode key) const;
    bool GetKeyUp(input::KeyCode key) const;
    math::Vector2 GetMouseDelta() const;
    math::Vector2 GetMousePosition() const;
    float GetMouseScrollDelta() const;
    bool MouseButton(MouseBtn btn) const;
    bool MouseButtonDown(MouseBtn btn) const;
    bool MouseButtonUp(MouseBtn btn) const;

    // ── アクション層 ──────────────────────────────────────────────────────
    // ProjectSettings/Input.inputactions で定義した論理名で入力を取る。
    // WHY 推奨経路か: GetKey(KeyCode::SPACE) と書くとキーコンフィグもパッド対応も
    //     不可能になる。物理入力に依存しないのはこちらの API のみ。
    bool  GetAction    (std::string_view name) const;
    bool  GetActionDown(std::string_view name) const;
    bool  GetActionUp  (std::string_view name) const;
    float GetActionAxis(std::string_view name) const;

    // 既定の "MoveX"/"MoveY"、"LookX"/"LookY" を 2 軸まとめて取るショートハンド。
    // 半径方向のデッドゾーンが適用されるため、斜め入力の感度が方向で変わらない。
    math::Vector2 GetMoveAxis() const;
    math::Vector2 GetLookAxis() const;

    // ── ゲームパッド直接アクセス ──────────────────────────────────────────
    // WHY アクション層があるのに生アクセスも公開するか:
    //     ボタンアイコンの UI 表示など「どの物理ボタンが押されたか」を
    //     知る必要がある処理は、論理名だけでは表現できない。
    // pad = -1 は「接続中の最初のパッド」を意味する。
    bool  GetPadButton    (input::GamepadButton button, int pad = -1) const;
    bool  GetPadButtonDown(input::GamepadButton button, int pad = -1) const;
    bool  GetPadButtonUp  (input::GamepadButton button, int pad = -1) const;
    float GetPadAxis      (input::GamepadAxis axis,    int pad = -1) const;
    bool  IsPadConnected  (int pad = -1) const;

    // 振動。durationSeconds 経過で自動停止するため、停止呼び出しは不要。
    void SetVibration(float lowFrequency, float highFrequency,
                      float durationSeconds, int pad = -1) const;
    void StopVibration(int pad = -1) const;
};

} // namespace fbzz::scene
