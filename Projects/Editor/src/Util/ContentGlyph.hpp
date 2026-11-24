/// @file    ContentGlyph.hpp
/// @brief   階層行とアセットフォルダーで使う縮尺可変のピクトグラム。
/// @author  Hasegawa Jin
/// @date    2026-11-24
#pragma once
#include <imgui.h>
#include <string_view>

namespace fbzz::editor::ui {

enum class ContentGlyph {
    NONE, OBJECT, MESH, CAMERA, LIGHT, TERRAIN, WATER, PREFAB, MATERIAL, SCRIPT, SHADER,
    IMAGE, SCENE, AUDIO, ANIMATION, FOLDER, EFFECTS, CANVAS, FONT, DATA, PHYSICS, EYE, EYE_OFF, ENABLED, DISABLED, LOCK, UNLOCK
};

/// @note 座標は中心、size は表示枠の一辺。ImGui の最終項目や入力状態には触れない。
void DrawContentGlyph(ContentGlyph type, ImVec2 center, float size, ImU32 color);
/// @note 標準フォルダー名を手掛かりに表示する。ディレクトリの中身は走査しない。
[[nodiscard]] ContentGlyph FolderGlyph(std::string_view name);

}
