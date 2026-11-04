/// @file    InspectorCore.hpp
/// @brief   Transform / Script の Inspector 描画。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#pragma once

#include "InspectorCommon.hpp"

namespace fbzz::editor {
/// @brief 画像原寸をストレッチしていない軸へ適用し、変更をUndoへ記録する。
bool SetUIImageNativeSize(scene::GameObject& object, EditorContext& context);
} /// @note namespace fbzz::editor
