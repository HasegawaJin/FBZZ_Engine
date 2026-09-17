/// @file    ScriptInputProxy.hpp
/// @brief   Script から入力状態を読むためのショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#pragma once

#include <Math/Vector2.hpp>
#include <cstdint>
#include <string>
#include <string_view>

namespace fbzz::input {
enum class KeyCode : uint32_t;
enum class GamepadButton : uint16_t;
enum class GamepadAxis : uint8_t;
}

namespace fbzz::scene {

class Script;

enum class MouseBtn : int { Left = 0, Right = 1, Middle = 2 };

/// スクリプトから見た 1 件のバインド。
///
/// @note input::InputBinding をそのまま渡すと、std::vector を持つ型なのでスクリプト DLL と
///       エンジンでアロケーターが分かれた瞬間に壊れる。値 (種類と番号) だけを写して渡す。
struct ScriptInputBinding {
    /// input::BindingSource と同じ並び。0:Key 1:MouseButton 2:GamepadButton
    /// 3:GamepadAxis 4:MouseAxis。範囲外は「バインドが無い」を表す -1。
    int      source = -1;
    uint32_t code   = 0;

    [[nodiscard]] bool IsValid() const { return source >= 0; }
};

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

    /// @name アクション層
    /// ProjectSettings/Input.inputactions で定義した論理名で入力を取る。
    /// @note GetKey(KeyCode::SPACE) と書くとキーコンフィグもパッド対応も不可能になる。
    ///       物理入力に依存しないのはこちらの API のみで、推奨経路。
    ///@{
    bool  GetAction    (std::string_view name) const;
    bool  GetActionDown(std::string_view name) const;
    bool  GetActionUp  (std::string_view name) const;
    float GetActionAxis(std::string_view name) const;

    /// @brief 既定の "MoveX"/"MoveY"、"LookX"/"LookY" を 2 軸まとめて取るショートハンド。
    /// @note 半径方向のデッドゾーンが適用されるため、斜め入力の感度が方向で変わらない。
    math::Vector2 GetMoveAxis() const;
    math::Vector2 GetLookAxis() const;
    ///@}

    /// @name ゲームパッド直接アクセス
    /// ボタンアイコンの UI 表示など「どの物理ボタンが押されたか」を知る必要がある処理は、
    /// アクション層の論理名だけでは表現できないため公開している。
    /// @param pad -1 は「接続中の最初のパッド」を意味する。
    ///@{
    bool  GetPadButton    (input::GamepadButton button, int pad = -1) const;
    bool  GetPadButtonDown(input::GamepadButton button, int pad = -1) const;
    bool  GetPadButtonUp  (input::GamepadButton button, int pad = -1) const;
    float GetPadAxis      (input::GamepadAxis axis,    int pad = -1) const;
    bool  IsPadConnected  (int pad = -1) const;

    /// @brief 振動。durationSeconds 経過で自動停止するため、停止呼び出しは不要。
    void SetVibration(float lowFrequency, float highFrequency,
                      float durationSeconds, int pad = -1) const;
    void StopVibration(int pad = -1) const;
    ///@}

    /// @name キーコンフィグ
    /// アクション 1 つが持つバインドの列を、添字で読み書きする。
    /// @note 保存先はゲームの config。ProjectSettings/Input.inputactions は
    ///       オーサリング時の既定で、製品版では読み取り専用でありうるため、
    ///       エンジンは「今の割り当て」の出し入れまでを持ち、保存先はゲームが決める。
    ///@{
    [[nodiscard]] int GetActionBindingCount(std::string_view action) const;
    /// 添字が範囲外なら IsValid() == false のバインドを返す。
    [[nodiscard]] ScriptInputBinding GetActionBinding(std::string_view action, int index) const;
    /// 添字が範囲外なら末尾へ追加する。アクションが無ければ false。
    bool SetActionBinding(std::string_view action, int index, ScriptInputBinding binding) const;
    /// "Space" / "Pad0:A" のような表示用の文字列。無ければ空。
    [[nodiscard]] std::string DescribeActionBinding(std::string_view action, int index) const;

    /// @brief 次に押された物理入力を index 番目のバインドへ記録する。
    /// @note 1 フレームで完結しないため、開始・完了確認・取消に分かれる。
    void BeginRebindAction(std::string_view action, int index) const;
    [[nodiscard]] bool IsRebinding() const;
    /// リバインドが完了したフレームで 1 度だけ true。読むと消費される。
    bool ConsumeRebindCompleted() const;
    void CancelRebind() const;
    ///@}
};

} // namespace fbzz::scene
