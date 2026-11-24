/// @file    WindowClientCursor.hpp
/// @brief   Window のクライアント領域とゲームカーソルを接続する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

struct HWND__;

namespace fbzz::core::cursor_detail {

/// @pre 対象ウィンドウ自身のクライアント領域で、既存のウィンドウフックが処理しなかった場合に呼ぶ。
/// @return このウィンドウでゲームカーソルを選択できた場合だけ true。
bool TrySetWindowClientCursor(HWND__* window);

}
