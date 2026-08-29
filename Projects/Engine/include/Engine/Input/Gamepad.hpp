/// @file    Gamepad.hpp
/// @brief   XInput ゲームパッドのフレーム状態管理と振動制御。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// WHY ポーリング方式か:
/// ゲームパッドは Win32 メッセージポンプに乗らない。XInputGetState を明示的に呼ぶ以外に
/// 状態を得る手段がないため、既存 Input と同じ「フレーム先頭で現在状態を確定し、
/// 前フレーム状態を保存する」モデルに合わせてフレーム先頭で 1 回だけポーリングする。
///
/// WHY XInput のみで DirectInput を採用しないか:
/// DirectInput は汎用性と引き換えにデバイス列挙・軸マッピング・フォースフィードバックの
/// 複雑さが跳ね上がる。PS 系パッドも Steam Input / DS4Windows 経由で XInput として
/// 認識されるため、実用上の欠落は小さい。ポートフォリオでの投資対効果を優先した。
///
/// WHY デッドゾーンを適用しないか:
/// デッドゾーンの適正値はアクションごとに異なる (移動は大きめ、カメラは小さめ)。
/// デバイス層で潰すと後段で元の値を復元できないため、生の -1..1 を返し、
/// デッドゾーン処理は InputActionMap に委ねる。
#pragma once
#include "GamepadButton.hpp"
#include <cstdint>

namespace fbzz::input {

class Gamepad {
public:
    // XInput が扱えるコントローラー数の上限 (XUSER_MAX_COUNT と同値)
    static constexpr int MAX_PADS = 4;

    // Input::Update() の先頭から呼ばれる。個別に呼ぶ必要はない。
    // WHY Input::Update() に内包するか: 更新経路は Editor / Standalone / Sandbox に
    //     散らばっており、それぞれに追記させると呼び忘れで「パッドだけ効かない」
    //     再現困難な不具合になる。既存の入力更新点 1 箇所に寄せる。
    static void Update();

    // 全状態を初期化する。Play モードの開始・終了など、
    // 入力の連続性を切りたい境界で呼ぶ。
    static void Reset();

    [[nodiscard]] static bool IsConnected(int pad = 0);

    // 接続中のパッドのうち最も若いスロット番号。1 台も無ければ -1。
    // WHY: シングルプレイヤーゲームで「どのスロットに挿さっていても動く」挙動を
    //      実現するために使う。スロット 0 決め打ちだと 2 番目のポートに挿した際に無反応になる。
    [[nodiscard]] static int GetFirstConnectedPad();

    [[nodiscard]] static bool ButtonHeld(GamepadButton button, int pad = 0);
    [[nodiscard]] static bool ButtonDown(GamepadButton button, int pad = 0);  // 押した瞬間のみ true
    [[nodiscard]] static bool ButtonUp  (GamepadButton button, int pad = 0);  // 離した瞬間のみ true

    // スティックは -1..1、トリガーは 0..1。デッドゾーン未適用の生値。
    [[nodiscard]] static float Axis(GamepadAxis axis, int pad = 0);

    // --- 振動 ---
    // lowFrequency / highFrequency はともに 0..1。durationSeconds 経過で自動停止する。
    // WHY 自動停止させるか: 停止呼び出しを呼び出し側の責任にすると、
    //     ゲームオーバーやシーン遷移の経路で必ず呼び忘れが起き、
    //     コントローラーが振動しっぱなしになる。デバイス層で寿命を持たせる。
    static void SetVibration(float lowFrequency, float highFrequency,
                             float durationSeconds, int pad = 0);
    static void StopVibration(int pad = 0);
    static void StopAllVibration();

    // --- テスト用フック ---
    // 実機を接続せずに Gamepad の状態遷移を検証するための注入口。
    // 有効化すると Update() は XInput をポーリングせず、注入値のみを反映する。
    // WHY 必要か: CI や自動テストに実 XInput デバイスは存在しない。
    //             注入口が無いと Gamepad のロジックが一切テストできなくなる。
    static void SetSimulationEnabled(bool enabled);
    [[nodiscard]] static bool IsSimulationEnabled();
    static void SimulateConnected(int pad, bool connected);
    static void SimulateButton(GamepadButton button, bool pressed, int pad = 0);
    static void SimulateAxis(GamepadAxis axis, float value, int pad = 0);
};

} // namespace fbzz::input
