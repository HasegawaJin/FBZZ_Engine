/// @file    InputBinding.hpp
/// @brief   「アクション名 → 物理入力」のバインド定義。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note スクリプトが KeyCode を直接参照するとキーコンフィグもパッド対応も不可能になるため、
///       「Jump」等の論理名と物理入力の集合を分離し、バインド差し替えがロジックへ影響しないようにする。
#pragma once
#include "GamepadButton.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::input {

/// 1 つの物理入力ソース。
enum class BindingSource : uint8_t {
    KEY = 0,        ///< code = KeyCode (Win32 仮想キーコード)
    MOUSE_BUTTON,   ///< code = 0:左 / 1:右 / 2:中
    GAMEPAD_BUTTON, ///< code = GamepadButton
    GAMEPAD_AXIS,   ///< code = GamepadAxis
    MOUSE_AXIS,     ///< code = MouseAxis
};

/// マウスの移動量を軸として扱うためのソース。
/// @note 一人称/三人称視点は「マウス移動」と「右スティック」を同じ LookX/LookY へ束ねたいため、
///       マウス Delta も軸バインドとして表現できるようにする。
enum class MouseAxis : uint8_t {
    DELTA_X = 0,
    DELTA_Y,
    SCROLL,

    COUNT
};

struct InputBinding {
    BindingSource source = BindingSource::KEY;

    /// source に応じた入力コード。列挙値をそのまま整数として保持する。
    uint32_t code = 0;

    /// 軸の向き反転や感度調整に使う倍率。ボタンとして評価する際は符号のみ意味を持つ。
    float scale = 1.0f;

    /// 参照するゲームパッドのスロット。-1 は「接続中の最初のパッド」。
    /// @note スロット 0 決め打ちだと 2 番目のポートに挿しただけで無反応になるため既定は -1。ローカル対戦時のみ 0..3 を明示する。
    int padIndex = -1;

    /// GAMEPAD_AXIS / MOUSE_AXIS をボタンとして評価する際の発火閾値。
    /// scale の符号と合わせて「左スティックを下に倒したらしゃがむ」等を表現する。
    float buttonThreshold = 0.5f;
};

/// ボタン的アクション ("Jump" / "Attack")。いずれかのバインドが立てば true。
struct InputAction {
    std::string               name;
    std::vector<InputBinding> bindings;
};

/// 1 次元の軸アクション ("MoveX")。
///
/// positive / negative は「押されている間 +1 / -1」を作るデジタル入力。
/// analog は GamepadAxis や MouseAxis を直結するアナログ入力。
/// 両方が定義されている場合、アナログがデッドゾーンを超えていればそちらを優先する。
/// @note パッドとキーボードを同時に接続していても、実際に動かしている側が勝つのが自然なため。
struct InputAxis {
    std::string               name;
    std::vector<InputBinding> positive;
    std::vector<InputBinding> negative;
    std::vector<InputBinding> analog;

    /// アナログ入力のデッドゾーン。GetAxis2D では半径方向に適用される。
    float deadZone = 0.25f;

    /// デジタル入力時に 0 へ戻る速度 [単位/秒]
    float gravity = 8.0f;

    /// デジタル入力時に目標値へ向かう速度 [単位/秒]
    float sensitivity = 8.0f;

    /// 逆方向の入力が来たとき、即座に 0 を経由するか。
    /// @note false だと切り返し操作が緩やかな減速に感じ鈍くなるため、アクションゲームでは true が望ましい。
    bool snap = true;

    /// アナログ入力を生値のまま扱う (平滑化・デッドゾーン再マップを行わない)。
    /// @note マウス Delta はフレーム間の移動量そのものであり、-1..1 正規化やデッドゾーンを適用すると視点操作が破綻するため。
    bool raw = false;
};

} // namespace fbzz::input
