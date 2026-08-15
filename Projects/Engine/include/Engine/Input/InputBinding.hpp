// FBZZ Engine
// InputBinding.hpp | fbzz::input
// 「アクション名 → 物理入力」のバインド定義
//
// WHY この層が必要か:
//   スクリプトが KeyCode::SPACE を直接参照すると、キーコンフィグもゲームパッド対応も
//   原理的に不可能になる。「Jump」という論理名と、それを発火させる物理入力の集合を
//   分離することで、バインドの差し替えがゲームロジックに一切影響しなくなる。
#pragma once
#include "GamepadButton.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::input {

// 1 つの物理入力ソース。
enum class BindingSource : uint8_t {
    KEY = 0,        // code = KeyCode (Win32 仮想キーコード)
    MOUSE_BUTTON,   // code = 0:左 / 1:右 / 2:中
    GAMEPAD_BUTTON, // code = GamepadButton
    GAMEPAD_AXIS,   // code = GamepadAxis
    MOUSE_AXIS,     // code = MouseAxis
};

// マウスの移動量を軸として扱うためのソース。
// WHY: 一人称/三人称の視点操作は「マウス移動」と「右スティック」を同じ LookX/LookY
//      アクションへ束ねたい。マウス Delta を軸バインドとして表現できないと、
//      視点操作だけアクション層の外に取り残される。
enum class MouseAxis : uint8_t {
    DELTA_X = 0,
    DELTA_Y,
    SCROLL,

    COUNT
};

struct InputBinding {
    BindingSource source = BindingSource::KEY;

    // source に応じた入力コード。列挙値をそのまま整数として保持する。
    uint32_t code = 0;

    // 軸の向き反転や感度調整に使う倍率。ボタンとして評価する際は符号のみ意味を持つ。
    float scale = 1.0f;

    // 参照するゲームパッドのスロット。-1 は「接続中の最初のパッド」。
    // WHY 既定を -1 にするか: シングルプレイヤーでスロット 0 決め打ちにすると、
    //     2 番目のポートに挿しただけで無反応になる。ローカル対戦で明示したい場合のみ
    //     0..3 を指定する。
    int padIndex = -1;

    // GAMEPAD_AXIS / MOUSE_AXIS をボタンとして評価する際の発火閾値。
    // scale の符号と合わせて「左スティックを下に倒したらしゃがむ」等を表現する。
    float buttonThreshold = 0.5f;
};

// ボタン的アクション ("Jump" / "Attack")。いずれかのバインドが立てば true。
struct InputAction {
    std::string               name;
    std::vector<InputBinding> bindings;
};

// 1 次元の軸アクション ("MoveX")。
//
// positive / negative は「押されている間 +1 / -1」を作るデジタル入力。
// analog は GamepadAxis や MouseAxis を直結するアナログ入力。
// 両方が定義されている場合、アナログがデッドゾーンを超えていればそちらを優先する。
// WHY: パッドとキーボードを同時に接続していても、実際に動かしている側が勝つのが自然。
struct InputAxis {
    std::string               name;
    std::vector<InputBinding> positive;
    std::vector<InputBinding> negative;
    std::vector<InputBinding> analog;

    // アナログ入力のデッドゾーン。GetAxis2D では半径方向に適用される。
    float deadZone = 0.25f;

    // デジタル入力時に 0 へ戻る速度 [単位/秒]
    float gravity = 8.0f;

    // デジタル入力時に目標値へ向かう速度 [単位/秒]
    float sensitivity = 8.0f;

    // 逆方向の入力が来たとき、即座に 0 を経由するか。
    // WHY: false だと右移動中に左を押しても +1 → -1 へ緩やかに減速するため、
    //      切り返しの操作感が鈍くなる。アクションゲームでは true が望ましい。
    bool snap = true;

    // アナログ入力を生値のまま扱う (平滑化・デッドゾーン再マップを行わない)。
    // WHY: マウス Delta は既にフレーム間の移動量であり、
    //      -1..1 への正規化やデッドゾーンを適用すると視点操作が破綻する。
    bool raw = false;
};

} // namespace fbzz::input
