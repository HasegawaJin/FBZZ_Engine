/// @file    PreviewPanel.hpp
/// @brief   Animation / Material の共通ルーターとプレビューウィンドウの宣言。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// 各アセット種別の描画実装は AnimationPreview / MaterialPreview へ分離する。
/// @note グラフの数値編集だけでは遷移のブレンド感やクリップの動きを確認できず、
///       Play Mode まで往復する反復コストが大きい。
#pragma once
#include <Editor/Panels/Animation/AnimationMaskPreview.hpp>
#include <Editor/Panels/Animation/AnimationPreview.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Panels/Preview/IPreviewPanel.hpp>
#include <Editor/Panels/Preview/MaterialPreview.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
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

    /// Animation Preview の表示トグル・カメラ角を EditorLocalState.toml と往復させる。
    void OnLoadSettings(const EditorSettings& settings) override;
    void OnSaveSettings(EditorSettings& settings) const override;
    void OnRegisterOperators(OperatorRegistry& registry) override;

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    /// .ico は 1 ファイルに複数サイズが入るので、面積最大のフレームを展開して持つ。
    struct IconPreview {
        std::string                                    path; ///< 現在調べているアイコンファイルのパス。
        renderer::ResourceHandle<renderer::TextureTag> texture; ///< デコードしたアイコンのプレビュー用テクスチャハンドル。
        int         width      = 0; ///< 表示に選んだアイコン画像の幅 [px]。
        int         height     = 0; ///< 表示に選んだアイコン画像の高さ [px]。
        int         frameCount = 0; ///< ICO ファイル内に検出した画像の数。
        bool        resolved   = false; ///< 現在のアイコンパスを取得しようと試行済みか。
        std::string error; ///< アイコンを読み込めない場合に表示する診断。
    };

    bool DrawIconPreview(EditorContext& ctx, std::string_view assetPath, float previewHeight);

    IconPreview          m_iconPreview; ///< 現在のアイコンプレビューで使う画像・読込状態。
    AnimationMaskPreview m_animationMaskPreview; ///< Animation Mask を骨の状態として可視化する所有プレビュー。
    /// @note Inspector 内のプレビューと視点・照明を共有させると、片方を回したときにもう片方まで
    ///       動き、2 つ並べて比べられないため、パネルごとに持つ。
    MaterialPreviewView  m_materialPreview; ///< 現在表示している材質の GPU 表示と操作状態を持つプレビュー。
};

} // namespace fbzz::editor
