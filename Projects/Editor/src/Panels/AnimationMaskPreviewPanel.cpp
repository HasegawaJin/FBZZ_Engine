/// @file    AnimationMaskPreviewPanel.cpp
/// @brief   FBX と Avatar Mask の実効ウェイトを同時に確認する独立プレビュー。
/// @author  Hasegawa Jin
/// @date    2026-08-19
#include <Editor/Panels/AnimationMaskPreviewPanel.hpp>
#include <Editor/Panels/AnimationPreview.hpp>

namespace fbzz::editor {

void AnimationMaskPreviewPanel::OnRenderContent(EditorContext& ctx)
{
    DrawAnimationMaskPreviewPanelContent(ctx);
}

} // namespace fbzz::editor
