/// @file    InputActionMap.hpp
/// @brief   アクション名でのバインド解決・デッドゾーン処理・リバインド。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// レイヤー構成:
/// Script / Game
/// ↓ GetAction("Jump") / GetAxis2D("MoveX","MoveY")
/// InputActionMap   ← このファイル
/// ↓ Input::KeyHeld / Gamepad::ButtonHeld / Gamepad::Axis
/// Input / Gamepad (デバイス層)
///
/// WHY 静的クラスにするか:
/// 既存の Input / Gamepad と同じ呼び出し様式に揃える。入力は本質的にプロセス唯一の
/// グローバル状態であり、インスタンスを持ち回る利点がない。
#pragma once
#include "InputBinding.hpp"
#include "Math/Vector2.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::input {

class InputActionMap {
public:
    /// 未設定のときだけ既定バインドを作る。起動前に読んだプロジェクト設定は保持する。
    static void Initialize();

    // WASD + XInput の標準バインドを構築する。設定ファイルが無い場合の初期状態。
    static void LoadDefaults();

    // .inputactions (TOML) を読み込む。失敗時は既存のマップを保持したまま false を返す。
    // WHY 失敗時に空にしないか: 設定ファイルが壊れていたときに全操作不能になるより、
    //     直前の (または既定の) バインドで動き続ける方が復旧しやすい。
    [[nodiscard]] static bool LoadFromFile(const std::string& path);
    [[nodiscard]] static bool SaveToFile(const std::string& path);

    // 全アクション/軸を破棄する。
    static void Clear();

    // Input::Update() の直後に毎フレーム呼ぶ。
    // dt はデジタル軸の平滑化 (gravity / sensitivity) に使う。
    static void Update(float dt);

    // 無効時、全アクションは false / 全軸は 0 を返す。
    // WHY 必要か: エディタでシーン編集中に W を押すとプレイヤーが歩き出す事故を防ぐ。
    //     Play モード外では無効にする。
    static void SetEnabled(bool enabled);
    [[nodiscard]] static bool IsEnabled();

    // --- 照会 ---
    [[nodiscard]] static bool  GetAction    (std::string_view name);
    [[nodiscard]] static bool  GetActionDown(std::string_view name);  // 押した瞬間のみ
    [[nodiscard]] static bool  GetActionUp  (std::string_view name);  // 離した瞬間のみ
    [[nodiscard]] static float GetAxis      (std::string_view name);

    // 2 軸をまとめて取得する。デッドゾーンを半径方向に適用してから再マップする。
    // WHY 半径方向か: 軸ごとに独立して切ると正方形のデッドゾーンになり、
    //     スティックを斜めに倒したときの実効感度が方向によって変わる。
    [[nodiscard]] static math::Vector2 GetAxis2D(std::string_view xName, std::string_view yName);

    // --- 編集 ---
    [[nodiscard]] static const std::vector<InputAction>& GetActions();
    [[nodiscard]] static const std::vector<InputAxis>&   GetAxes();

    // 編集用の可変参照。存在しなければ nullptr。
    [[nodiscard]] static InputAction* FindAction(std::string_view name);
    [[nodiscard]] static InputAxis*   FindAxis  (std::string_view name);

    // 既存同名があれば置換する。名前が空の場合は false。
    static bool AddAction(const InputAction& action);
    static bool AddAxis  (const InputAxis& axis);
    static bool RemoveAction(std::string_view name);
    static bool RemoveAxis  (std::string_view name);

    // --- リバインド ---
    // 次に押された任意の物理入力を、指定アクションの bindingIndex 番目へ記録する。
    // bindingIndex が範囲外なら末尾に追加する。
    //
    // WHY ポーリング形式か: 「今から押してください」という UI の状態を伴うため、
    //     1 フレームで完結しない。開始・進行確認・取消を別々の呼び出しに分ける。
    static void BeginRebindAction(std::string_view actionName, int bindingIndex);

    // 軸のリバインド。slot は 0:positive / 1:negative / 2:analog。
    static void BeginRebindAxis(std::string_view axisName, int slot, int bindingIndex);

    [[nodiscard]] static bool IsRebinding();

    // 指定のバインドがリバインド待機中の対象かどうか。
    // slot は軸のみ意味を持つ (0=positive / 1=negative / 2=analog)。
    // アクションのバインドを問い合わせる場合は slot に -1 を渡す。
    // WHY 必要か: エディタ UI が「どのバインドを差し替え待ちか」を行単位で示せないと、
    //     ユーザーが意図しないバインドを潰す事故になる。
    [[nodiscard]] static bool IsRebindTarget(std::string_view name, int slot, int bindingIndex);

    static void CancelRebind();

    // 直近のリバインドが完了したフレームで true を返す (UI の確定演出用)。
    [[nodiscard]] static bool ConsumeRebindCompleted();

    // --- デバッグ / エディタ表示 ---
    // バインド 1 件を "Space" / "Pad0:A" のような人間可読文字列にする。
    [[nodiscard]] static std::string DescribeBinding(const InputBinding& binding);
};

} // namespace fbzz::input
