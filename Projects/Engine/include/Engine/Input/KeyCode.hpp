/// @file    KeyCode.hpp
/// @brief   Win32 仮想キーコードに対応するキー定義。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// ゲーム側が OS の定数を直接見ないための薄い列挙。
/// 値は Win32 メッセージからそのまま変換できるように保つ。
#pragma once
#include <cstdint>

namespace fbzz::input {

/// 値は winuser.h の `VK_*` と一致する。`<Windows.h>` をこのヘッダーへ持ち込むと
/// 200 近い翻訳単位へ 35 万行が伝播するため、マクロではなく数値で持つ。
/// 一致は Input.cpp の static_assert が検証する。
enum class KeyCode : uint32_t {
    A = 'A', B = 'B', C = 'C', D = 'D', E = 'E', F = 'F', G = 'G',
    H = 'H', I = 'I', J = 'J', K = 'K', L = 'L', M = 'M', N = 'N',
    O = 'O', P = 'P', Q = 'Q', R = 'R', S = 'S', T = 'T', U = 'U',
    V = 'V', W = 'W', X = 'X', Y = 'Y', Z = 'Z',

    KEY_0 = '0', KEY_1 = '1', KEY_2 = '2', KEY_3 = '3', KEY_4 = '4',
    KEY_5 = '5', KEY_6 = '6', KEY_7 = '7', KEY_8 = '8', KEY_9 = '9',

    ESCAPE    = 0x1B,  ///< VK_ESCAPE
    SPACE     = 0x20,  ///< VK_SPACE
    ENTER     = 0x0D,  ///< VK_RETURN
    BACKSPACE = 0x08,  ///< VK_BACK
    TAB       = 0x09,  ///< VK_TAB
    SHIFT     = 0x10,  ///< VK_SHIFT
    CTRL      = 0x11,  ///< VK_CONTROL
    ALT       = 0x12,  ///< VK_MENU
    LEFT      = 0x25,  ///< VK_LEFT
    RIGHT     = 0x27,  ///< VK_RIGHT
    UP        = 0x26,  ///< VK_UP
    DOWN      = 0x28,  ///< VK_DOWN

    /// VK_F1..VK_F12 は 0x70 から連番。
    F1  = 0x70, F2  = 0x71, F3  = 0x72, F4  = 0x73,
    F5  = 0x74, F6  = 0x75, F7  = 0x76, F8  = 0x77,
    F9  = 0x78, F10 = 0x79, F11 = 0x7A, F12 = 0x7B,

    /// マウスボタン — VK_LBUTTON/RBUTTON/MBUTTON はキーボード配列(0-255)内に収まる。
    /// KeyDown / KeyHeld / KeyUp で通常キーと同じように使える。
    MouseLeft   = 0x01,  ///< VK_LBUTTON
    MouseRight  = 0x02,  ///< VK_RBUTTON
    MouseMiddle = 0x04,  ///< VK_MBUTTON
};

} // namespace fbzz::input
