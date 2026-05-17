// FBZZ Engine
// KeyCode.hpp | fbzz::input
// キーコード定義 (Win32 仮想キーコードに対応)
#pragma once
#include <cstdint>
#include <Windows.h>

namespace fbzz::input {

enum class KeyCode : uint32_t {
    A = 'A', B = 'B', C = 'C', D = 'D', E = 'E', F = 'F', G = 'G',
    H = 'H', I = 'I', J = 'J', K = 'K', L = 'L', M = 'M', N = 'N',
    O = 'O', P = 'P', Q = 'Q', R = 'R', S = 'S', T = 'T', U = 'U',
    V = 'V', W = 'W', X = 'X', Y = 'Y', Z = 'Z',

    KEY_0 = '0', KEY_1 = '1', KEY_2 = '2', KEY_3 = '3', KEY_4 = '4',
    KEY_5 = '5', KEY_6 = '6', KEY_7 = '7', KEY_8 = '8', KEY_9 = '9',

    ESCAPE    = VK_ESCAPE,
    SPACE     = VK_SPACE,
    ENTER     = VK_RETURN,
    BACKSPACE = VK_BACK,
    TAB       = VK_TAB,
    SHIFT     = VK_SHIFT,
    CTRL      = VK_CONTROL,
    ALT       = VK_MENU,
    LEFT      = VK_LEFT,
    RIGHT     = VK_RIGHT,
    UP        = VK_UP,
    DOWN      = VK_DOWN,

    F1  = VK_F1,  F2  = VK_F2,  F3  = VK_F3,  F4  = VK_F4,
    F5  = VK_F5,  F6  = VK_F6,  F7  = VK_F7,  F8  = VK_F8,
    F9  = VK_F9,  F10 = VK_F10, F11 = VK_F11, F12 = VK_F12,
};

} // namespace fbzz::input
