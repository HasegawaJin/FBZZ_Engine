/// @file    PreviewPanel.hpp
/// @brief   Animation / Material の共通ルーターとプレビューウィンドウの宣言。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// 各アセット種別の描画実装は AnimationPreview / MaterialPreview へ分離する。
/// WHY: グラフの数値編集だけでは遷移のブレンド感やクリップの動きを確認できず、
/// Play Mode まで往復する反復コストが大きいため。
#pragma once
#include <Editor/Panels/AnimationMaskPreview.hpp>
#include <Editor/Panels/AnimationPreview.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Panels/IPreviewPanel.hpp>
#include <Editor/Panels/MaterialPreview.hpp>
#include <string_view>

namespace fbzz::editor {

struct EditorContext;

class PreviewPanel final : public IPanel, public IPreviewPanel {
public:
    const char* GetWindowName()        const override { return "Preview"; }
    bool        GetDefaultVisibility() const override { return false; }

    const char* GetPreviewName() const override { return "Preview"; }
    [[nodiscard]] bool Supports(std::string_view extension) const override;
    [[nodiscard]] bool DrawPreview(EditorContext& ctx,
                                   std::string_view assetPath,
                                   float previewHeight) override;

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    AnimationMaskPreview m_animationMaskPreview;
};

} // namespace fbzz::editor
