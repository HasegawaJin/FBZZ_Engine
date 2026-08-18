// FBZZ Engine
// AnimationMaskPreviewPanel.hpp | fbzz::editor
// FBX と Avatar Mask の実効ウェイトを同時に確認する独立プレビュー。
#pragma once

#include <Editor/Panels/IPanel.hpp>

namespace fbzz::editor {

// Animation Mask の確認に特化した独立パネル。
// WHY: Inspector はルール編集、Preview は結果確認に分け、長時間の調整でも
//      階層と最終結果を同時に見失わないようにする。
class AnimationMaskPreviewPanel final : public IPanel {
public:
    const char* GetWindowName() const override { return "Animation Mask Preview"; }
    const char* GetViewMenuName() const override { return "Animation Mask Preview"; }
    const char* GetMenuCategory() const override { return "Animation"; }
    bool GetDefaultVisibility() const override { return false; }

protected:
    void OnRenderContent(EditorContext& ctx) override;
};

} // namespace fbzz::editor
