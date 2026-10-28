/// @file    InputActionMap.hpp
/// @brief   アクション名でのバインド解決・デッドゾーン処理・リバインド。
/// @author  Hasegawa Jin
/// @date    2026-08-12
/// @note /// レイヤー構成:
/// @note Script / Game
/// @note ↓ GetAction("Jump") / GetAxis2D("MoveX","MoveY")
/// @note InputActionMap   ← このファイル
/// @note ↓ Input::KeyHeld / Gamepad::ButtonHeld / Gamepad::Axis
/// @note Input / Gamepad (デバイス層)
/// @note /// @note 入力はプロセス唯一のグローバル状態のため、既存の Input / Gamepad と同じ静的クラス様式に揃える。
#pragma once
#include "InputBinding.hpp"
#include "Math/Vector2.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::input {

class InputActionMap {
public:
    /// @note 未設定のときだけ既定バインドを作る。起動前に読んだプロジェクト設定は保持する。
    static void Initialize();

    /// @note WASD + XInput の標準バインドを構築する。設定ファイルが無い場合の初期状態。
    static void LoadDefaults();

    /// @note .inputactions (TOML) を読み込む。失敗時は既存のマップを保持したまま false を返す。
    /// @note 壊れた設定で全操作不能になるより、直前 (または既定) のバインドで動き続ける方が復旧しやすい。
    [[nodiscard]] static bool LoadFromFile(const std::string& path);
    [[nodiscard]] static bool SaveToFile(const std::string& path);

    /// @note 全アクション/軸を破棄する。
    static void Clear();

    /// @note Input::Update() の直後に毎フレーム呼ぶ。
    /// @note dt はデジタル軸の平滑化 (gravity / sensitivity) に使う。
    static void Update(float dt);

    /// @note 無効時、全アクションは false / 全軸は 0 を返す。
    /// @note エディタでシーン編集中に W を押すとプレイヤーが歩き出す事故を防ぐため、Play モード外では無効にする。
    static void SetEnabled(bool enabled);
    /// @brief Clears evaluated input at host ownership handoffs, preserving settings, bindings and requested enabled state.
    static void ResetRuntime();
    [[nodiscard]] static bool IsEnabled();

    /// @name 照会
    /// @{
    [[nodiscard]] static bool  GetAction    (std::string_view name);
    [[nodiscard]] static bool  GetActionDown(std::string_view name);  ///< @note 押した瞬間のみ
    [[nodiscard]] static bool  GetActionUp  (std::string_view name);  ///< @note 離した瞬間のみ
    [[nodiscard]] static float GetAxis      (std::string_view name);

    /// @note 2 軸をまとめて取得する。デッドゾーンを半径方向に適用してから再マップする。
    /// @note 軸ごとに独立して切ると正方形のデッドゾーンになり、斜め入力の実効感度が方向で変わるため。
    [[nodiscard]] static math::Vector2 GetAxis2D(std::string_view xName, std::string_view yName);
    /// @}

    /// @name 編集
    /// @{
    [[nodiscard]] static const std::vector<InputAction>& GetActions();
    [[nodiscard]] static const std::vector<InputAxis>&   GetAxes();

    /// @note 編集用の可変参照。存在しなければ nullptr。
    [[nodiscard]] static InputAction* FindAction(std::string_view name);
    [[nodiscard]] static InputAxis*   FindAxis  (std::string_view name);

    /// @note 既存同名があれば置換する。名前が空の場合は false。
    static bool AddAction(const InputAction& action);
    static bool AddAxis  (const InputAxis& axis);
    static bool RemoveAction(std::string_view name);
    static bool RemoveAxis  (std::string_view name);
    /// @}

    /// @name リバインド
    /// @{
    /// @note 次に押された任意の物理入力を、指定アクションの bindingIndex 番目へ記録する。
    /// @note bindingIndex が範囲外なら末尾に追加する。
    /// @note 「今から押してください」という UI 状態を伴い 1 フレームで完結しないため、開始・進行確認・取消を分ける。
    static void BeginRebindAction(std::string_view actionName, int bindingIndex);

    /// @note 軸のリバインド。slot は 0:positive / 1:negative / 2:analog。
    static void BeginRebindAxis(std::string_view axisName, int slot, int bindingIndex);

    [[nodiscard]] static bool IsRebinding();

    /// @note 指定のバインドがリバインド待機中の対象かどうか。
    /// @note slot は軸のみ意味を持つ (0=positive / 1=negative / 2=analog)。
    /// @note アクションのバインドを問い合わせる場合は slot に -1 を渡す。
    /// @note エディタ UI が行単位で示せないと、意図しないバインドを潰す事故につながる。
    [[nodiscard]] static bool IsRebindTarget(std::string_view name, int slot, int bindingIndex);

    static void CancelRebind();

    /// @note 直近のリバインドが完了したフレームで true を返す (UI の確定演出用)。
    [[nodiscard]] static bool ConsumeRebindCompleted();
    /// @}

    /// @name デバッグ / エディタ表示
    /// @{
    /// @note バインド 1 件を "Space" / "Pad0:A" のような人間可読文字列にする。
    [[nodiscard]] static std::string DescribeBinding(const InputBinding& binding);
    /// @}
};

} /// @note namespace fbzz::input
