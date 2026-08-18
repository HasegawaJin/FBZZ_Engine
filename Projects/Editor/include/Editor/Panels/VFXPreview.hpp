// FBZZ Engine
// VFXPreview.hpp | fbzz::editor
// VFX Graph アセット用プレビュー API。
#pragma once

#include <string_view>

namespace fbzz::editor {

struct EditorContext;

// .vfx の構造プレビューを描画し、実時間描画は専用 VFX Editor へ委譲する。
bool DrawVFXPreviewWidget(EditorContext& ctx,
                          std::string_view assetPath,
                          float previewHeight);

} // namespace fbzz::editor
