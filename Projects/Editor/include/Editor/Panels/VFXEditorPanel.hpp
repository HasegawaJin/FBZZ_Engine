// FBZZ Engine
// VFXEditorPanel.hpp | fbzz::editor
// VFX (Particle) 専用エディター。実シミュレーション直結のトランスポート、
// Burst マーカー付きタイムライン、SubEmitter ツリー、モジュールスタックを備える。
// WHY: プレビューはシーンビューの実レンダリングをそのまま使う (Unity Shuriken 方式)。
//      専用 RT プレビューを持たないことで、ソフトパーティクル・GPU シミュレーション・
//      SubEmitter 連鎖を含む「ゲームと同じ見た目」での編集を保証する。
#pragma once

#include <Editor/Panels/EditorToolPanel.hpp>
#include <Engine/Scene/Entity.hpp>
#include <vector>

namespace fbzz::scene { struct ParticleEmitter; }

namespace fbzz::editor {

class VFXEditorPanel final : public EditorToolPanel {
public:
    const char* GetWindowName() const override { return "VFX Editor"; }
    const char* GetEditorType() const override { return "VFX Editor"; }
    bool GetDefaultVisibility() const override { return false; }

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    // 選択エミッターから SubEmitter 名前参照と子 GameObject を辿った「1エフェクト」の集合。
    // トランスポート操作 (Pause / Restart / スクラブ) はこの集合全体へ適用する。
    std::vector<scene::EntityID> BuildEffectGroup(EditorContext& ctx) const;

    void DrawTransport(EditorContext& ctx, const std::vector<scene::EntityID>& group,
                       scene::ParticleEmitter* root);
    void DrawHierarchy(EditorContext& ctx);
    void DrawEmitterNode(EditorContext& ctx, scene::EntityID id, int depth,
                         std::vector<scene::EntityID>& visited);
    void DrawTimeline(EditorContext& ctx, const std::vector<scene::EntityID>& group,
                      scene::ParticleEmitter* root);
    void DrawStats(EditorContext& ctx, const std::vector<scene::EntityID>& group,
                   scene::ParticleEmitter* root);

    // グループ全エミッターへスクラブを要求する (決定論的な再シミュレーション)。
    void RequestScrub(EditorContext& ctx, const std::vector<scene::EntityID>& group, float targetTime);

    bool  m_paused       = false; // トランスポートの一時停止 (editorTimeScale = 0 を注入)
    float m_previewSpeed = 1.0f;  // プレビュー再生速度倍率
};

} // namespace fbzz::editor
