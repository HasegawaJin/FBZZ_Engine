/// @file    MaterialPreview.hpp
/// @brief   Material アセット用プレビュー API。
/// @author  Hasegawa Jin
/// @date    2026-08-19
#pragma once

namespace fbzz::asset { struct MaterialAsset; }

namespace fbzz::editor {

struct EditorContext;

// .mat を球体へ適用し、DirectionalLight 付きで描画するプレビュー。
bool DrawMaterialPreviewWidget(EditorContext& ctx,
                               const asset::MaterialAsset& material,
                               float previewHeight);

} // namespace fbzz::editor
