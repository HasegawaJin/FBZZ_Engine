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
#include <Engine/Renderer/ResourceHandle.hpp>
#include <string>
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

    // Animation Preview の表示トグル・カメラ角を EditorLocalState.toml と往復させる。
    void OnLoadSettings(const EditorSettings& settings) override;
    void OnSaveSettings(EditorSettings& settings) const override;

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    // .ico は 1 ファイルに複数サイズが入るので、面積最大のフレームを展開して持つ。
    struct IconPreview {
        std::string                                    path;
        renderer::ResourceHandle<renderer::TextureTag> texture;
        int         width      = 0;
        int         height     = 0;
        int         frameCount = 0;
        bool        resolved   = false;
        std::string error;
    };

    bool DrawIconPreview(EditorContext& ctx, std::string_view assetPath, float previewHeight);

    IconPreview          m_iconPreview;
    AnimationMaskPreview m_animationMaskPreview;
    // WHY パネルごとに持つか: Inspector 内のプレビューと視点・照明を共有させると、
    //      片方を回したときにもう片方まで動き、2 つ並べて比べられない。
    MaterialPreviewView  m_materialPreview;
};

} // namespace fbzz::editor
