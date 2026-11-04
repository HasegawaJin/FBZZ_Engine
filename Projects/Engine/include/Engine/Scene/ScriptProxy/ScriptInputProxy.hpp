/// @file    ScriptInputProxy.hpp
/// @brief   Script から入力状態を読むためのショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#pragma once
#include <Engine/Api.hpp>

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

/// @note スクリプトから見た 1 件のバインド。

/// @note input::InputBinding をそのまま渡すと、std::vector を持つ型なのでスクリプト DLL と
/// @note エンジンでアロケーターが分かれた瞬間に壊れる。値 (種類と番号) だけを写して渡す。
struct ScriptInputBinding {
    /// @note input::BindingSource と同じ並び。0:Key 1:MouseButton 2:GamepadButton
    /// @note 3:GamepadAxis 4:MouseAxis。範囲外は「バインドが無い」を表す -1。
    int      source = -1;
    uint32_t code   = 0;

    [[nodiscard]] bool IsValid() const { return source >= 0; }
};

struct ScriptInputProxy {
    Script* script = nullptr;

    FBZZ_ENGINE_API bool GetKey(input::KeyCode key) const;
    FBZZ_ENGINE_API bool GetKeyDown(input::KeyCode key) const;
    FBZZ_ENGINE_API bool GetKeyUp(input::KeyCode key) const;
    FBZZ_ENGINE_API math::Vector2 GetMouseDelta() const;
    FBZZ_ENGINE_API math::Vector2 GetMousePosition() const;
    FBZZ_ENGINE_API float GetMouseScrollDelta() const;
    FBZZ_ENGINE_API bool MouseButton(MouseBtn btn) const;
    FBZZ_ENGINE_API bool MouseButtonDown(MouseBtn btn) const;
    FBZZ_ENGINE_API bool MouseButtonUp(MouseBtn btn) const;

    /// @name アクション層
    /// @note ProjectSettings/Input.inputactions で定義した論理名で入力を取る。
    /// @note GetKey(KeyCode::SPACE) と書くとキーコンフィグもパッド対応も不可能になる。
    /// @note 物理入力に依存しないのはこちらの API のみで、推奨経路。
    /// @{
    FBZZ_ENGINE_API bool  GetAction    (std::string_view name) const;
    FBZZ_ENGINE_API bool  GetActionDown(std::string_view name) const;
    FBZZ_ENGINE_API bool  GetActionUp  (std::string_view name) const;
    FBZZ_ENGINE_API float GetActionAxis(std::string_view name) const;

    /// @brief 既定の "MoveX"/"MoveY"、"LookX"/"LookY" を 2 軸まとめて取るショートハンド。
    /// @note 半径方向のデッドゾーンが適用されるため、斜め入力の感度が方向で変わらない。
    FBZZ_ENGINE_API math::Vector2 GetMoveAxis() const;
    FBZZ_ENGINE_API math::Vector2 GetLookAxis() const;
    /// @}

    /// @name ゲームパッド直接アクセス
    /// @note ボタンアイコンの UI 表示など「どの物理ボタンが押されたか」を知る必要がある処理は、
    /// @note アクション層の論理名だけでは表現できないため公開している。
    /// @param pad -1 は「接続中の最初のパッド」を意味する。
    /// @{
    FBZZ_ENGINE_API bool  GetPadButton    (input::GamepadButton button, int pad = -1) const;
    FBZZ_ENGINE_API bool  GetPadButtonDown(input::GamepadButton button, int pad = -1) const;
    FBZZ_ENGINE_API bool  GetPadButtonUp  (input::GamepadButton button, int pad = -1) const;
    FBZZ_ENGINE_API float GetPadAxis      (input::GamepadAxis axis,    int pad = -1) const;
    FBZZ_ENGINE_API bool  IsPadConnected  (int pad = -1) const;

    /// @brief 振動。durationSeconds 経過で自動停止するため、停止呼び出しは不要。
    FBZZ_ENGINE_API void SetVibration(float lowFrequency, float highFrequency,
                      float durationSeconds, int pad = -1) const;
    FBZZ_ENGINE_API void StopVibration(int pad = -1) const;
    /// @}

    /// @name キーコンフィグ
    /// @note アクション 1 つが持つバインドの列を、添字で読み書きする。
    /// @note 保存先はゲームの config。ProjectSettings/Input.inputactions は
    /// @note オーサリング時の既定で、製品版では読み取り専用でありうるため、
    /// @note エンジンは「今の割り当て」の出し入れまでを持ち、保存先はゲームが決める。
    /// @{
    [[nodiscard]] FBZZ_ENGINE_API int GetActionBindingCount(std::string_view action) const;
    /// @note 添字が範囲外なら IsValid() == false のバインドを返す。
    [[nodiscard]] FBZZ_ENGINE_API ScriptInputBinding GetActionBinding(std::string_view action, int index) const;
    /// @note 添字が範囲外なら末尾へ追加する。アクションが無ければ false。
    FBZZ_ENGINE_API bool SetActionBinding(std::string_view action, int index, ScriptInputBinding binding) const;
    /// @note "Space" / "Pad0:A" のような表示用の文字列。無ければ空。
    [[nodiscard]] FBZZ_ENGINE_API std::string DescribeActionBinding(std::string_view action, int index) const;

    /// @brief 次に押された物理入力を index 番目のバインドへ記録する。
    /// @note 1 フレームで完結しないため、開始・完了確認・取消に分かれる。
    FBZZ_ENGINE_API void BeginRebindAction(std::string_view action, int index) const;
    [[nodiscard]] FBZZ_ENGINE_API bool IsRebinding() const;
    /// @note リバインドが完了したフレームで 1 度だけ true。読むと消費される。
    FBZZ_ENGINE_API bool ConsumeRebindCompleted() const;
    FBZZ_ENGINE_API void CancelRebind() const;
    /// @}
};

} /// @note namespace fbzz::scene
