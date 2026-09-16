/// @file    UiNavSe.hpp
/// @brief   メニューの移動・決定・戻る。画面をまたいで同じ音を使う。
/// @author  Hasegawa Jin
/// @date    2026-08-29
///
/// SeLibrary の Bank にしていないのは、まだ 1 ファイルずつしか無いため。
/// 変種が増えたら kUiMove / kUiConfirm / kUiCancel として SeLibrary へ移すこと。
#pragma once

#include <string_view>

namespace uinav {
inline constexpr std::string_view kMove    = "Assets/Sound/SE/UI/SE_UI_Move.wav";
inline constexpr std::string_view kConfirm = "Assets/Sound/SE/UI/SE_UI_Confirm.wav";
inline constexpr std::string_view kCancel  = "Assets/Sound/SE/UI/SE_UI_Cancel.wav";
} // namespace uinav
