// FBZZ Engine
// AnimationPreviewPanel.hpp | fbzz::editor
// Unity の Animation Preview 相当。選択中のアニメーション (State / Transition / .anim / モデル) を
// スキンメッシュ付きでオフスクリーン再生し、Inspector 下部と独立ウィンドウの両方から確認できる。
// WHY: グラフの数値編集だけでは遷移のブレンド感やクリップの動きを確認できず、
//      Play Mode まで往復する反復コストが大きいため。
#pragma once
#include <Editor/Panels/IPanel.hpp>

namespace fbzz::editor {

struct EditorContext;

// プレビュー対象を現在の選択 (Animation Graph の State / Transition、
// Asset Browser の .anim / .fbx / .fzasset) から解決して描画する共有ウィジェット。
// Inspector 下部と AnimationPreviewPanel の両方が同じ再生状態・カメラを共有する。
// 戻り値: プレビュー対象が存在し描画した場合 true。
bool DrawAnimationPreviewWidget(EditorContext& ctx, float previewHeight);

// 現在プレビュー対象が存在するか (描画せずに問い合わせだけしたい場合用)。
bool HasAnimationPreviewTarget();

class AnimationPreviewPanel final : public IPanel {
public:
    const char* GetWindowName()        const override { return "Animation Preview"; }
    bool        GetDefaultVisibility() const override { return false; }

protected:
    void OnRenderContent(EditorContext& ctx) override;
};

} // namespace fbzz::editor
