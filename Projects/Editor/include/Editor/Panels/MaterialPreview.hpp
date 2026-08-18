// FBZZ Engine
// MaterialPreview.hpp | fbzz::editor
// Material アセット用プレビュー API。
#pragma once

namespace fbzz::asset { struct MaterialAsset; }

namespace fbzz::editor {

struct EditorContext;

// .mat を球体へ適用し、DirectionalLight 付きで描画するプレビュー。
bool DrawMaterialPreviewWidget(EditorContext& ctx,
                               const asset::MaterialAsset& material,
                               float previewHeight);

} // namespace fbzz::editor
