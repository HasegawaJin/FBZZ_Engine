/// @file    InspectorAudio.hpp
/// @brief   音源の再生・距離減衰・局所ミックスを共通カードへ接続する。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#pragma once
#include "InspectorCommon.hpp"
namespace fbzz::editor {
void DrawAudioInspectors(scene::GameObject* go, EditorContext& ctx, std::any& componentClipboard,
    const std::type_info*& componentClipboardType);
} /// @note namespace fbzz::editor
