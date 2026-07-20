// FBZZ Engine
// VFXEditorPanel.hpp | fbzz::editor
// VFX (Particle) 専用エディター。実シミュレーション直結のトランスポート、
// Burst マーカー付きタイムライン、SubEmitter ツリー、モジュールスタックを備える。
// WHY: グラフ、Inspector、決定論プレビューを OS の独立ウィンドウへまとめることで、
//      メイン Scene View のレイアウトと分離し、AI capture も専用 RT だけを対象にできる。
#pragma once

#include <Editor/Panels/EditorToolPanel.hpp>
#include <Editor/VFX/VFXGraphEditor.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Entity.hpp>
#include <string>
#include <vector>

namespace fbzz::scene { struct ParticleEmitter; }
struct ImNodesContext;
struct ImNodesEditorContext;

namespace fbzz::editor {

class VFXEditorPanel final : public EditorToolPanel {
public:
    const char* GetWindowName() const override { return "VFX Editor"; }
    const char* GetEditorType() const override { return "VFX Editor"; }
    bool GetDefaultVisibility() const override { return false; }
    void OnInit(EditorContext& ctx) override;
    void OnShutdown() override;
    // 背面・最小化・画面外へ移動した独立viewportをEditor側から確実に復帰させる。
    void RequestFocusAndReveal(bool resetPlacement = false);
    // 独立ApplicationではPanelをメインOSウィンドウ全面に固定し、secondary viewport化を無効にする。
    void SetStandaloneApplicationMode(bool enabled) { m_standaloneApplicationMode = enabled; }
    // 独立AppのOS D&Dを、Graphを開く操作またはPreview Emitter生成へ振り分ける。
    void OpenDroppedAsset(EditorContext& ctx, const std::string& path);
    [[nodiscard]] bool HasUnsavedChanges() const { return m_graphDirty; }
    [[nodiscard]] bool SaveCurrentGraph();
    [[nodiscard]] bool ReloadCurrentGraphFromDisk(const std::string& changedPath = {});

    // EditorApp が所有する専用プレビュー RT。Panel は表示サイズだけを返し、GPU 所有権を持たない。
    renderer::ResourceHandle<renderer::RenderTargetTag> previewRT;
    float previewWidth = 640.0f;
    float previewHeight = 360.0f;
    [[nodiscard]] bool WantsPreviewRender() const { return visible && m_previewRequested; }
    [[nodiscard]] bool UsesIsolatedPreviewWorld() const { return true; }
    [[nodiscard]] bool IsPreviewHovered() const { return m_previewHovered; }

protected:
    void OnRenderContent(EditorContext& ctx) override;
    void OnBeforeBegin(EditorContext& ctx) override;
    ImGuiWindowFlags GetWindowFlags() const override
    {
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_MenuBar;
        if (m_standaloneApplicationMode)
            flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove
                | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
        return flags;
    }
    bool CanClose() const override { return !m_standaloneApplicationMode; }

private:
    friend class VFXGraphEditor;
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

    // .vfxアセット選択中はParticle単体UIから独立グラフEditorへ切り替える。
    void DrawGraphEditor(EditorContext& ctx);
    void DrawGraphMenuBar(EditorContext& ctx);
    void DrawGraphStatusBar();
    void DrawPreviewViewport(EditorContext& ctx, bool fillAvailable = false);
    void DrawGraphToolbar(EditorContext& ctx);
    void DrawGraphCanvas(EditorContext& ctx);
    void DrawGraphInspector(EditorContext& ctx);
    void DrawGraphParameters();
    void DrawAddNodeMenu();
    void AddGraphNode(asset::VFXNodeType type);
    void AddGraphNodeFromAsset(const std::string& path);
    void CreatePreviewEmitterFromAsset(EditorContext& ctx, const std::string& path);
    void DeleteSelectedGraphNode();
    void PushGraphUndo();
    void PushGraphUndo(const asset::VFXGraphAsset& before);
    void UndoGraphEdit();
    void RedoGraphEdit();
    bool SaveGraph();
    bool LoadGraph(const std::string& path);

    // グループ全エミッターへスクラブを要求する (決定論的な再シミュレーション)。
    void RequestScrub(EditorContext& ctx, const std::vector<scene::EntityID>& group, float targetTime);

    bool  m_paused       = false; // トランスポートの一時停止 (editorTimeScale = 0 を注入)
    float m_previewSpeed = 1.0f;  // プレビュー再生速度倍率
    ImNodesContext* m_nodesContext = nullptr;
    ImNodesEditorContext* m_graphEditorContext = nullptr;
    asset::VFXGraphAsset m_graph;
    std::string m_graphPath;
    std::string m_graphError;
    std::string m_requestedAssetPath;
    int m_selectedGraphNodeId = -1;
    int m_selectedGraphLinkIndex = -1;
    bool m_graphDirty = false;
    bool m_graphPositionsPending = false;
    bool m_previewRequested = false;
    bool m_previewHovered = false;
    bool m_graphMode = false;
    bool m_previousGraphMode = false;
    bool m_showMiniMap = true;
    bool m_restartPreviewRequested = false;
    bool m_focusSelectionRequested = false;
    float m_emitterHierarchyWidth = 240.0f;
    float m_emitterPreviewHeight = 360.0f;
    scene::EntityID m_previewSelectedEntity = scene::EntityID::INVALID;
    scene::EntityID m_graphPreviewEntity = scene::EntityID::INVALID;
    // Scene Undoと混ぜず、独立アセットEditor内で完結するスナップショット履歴。
    std::vector<asset::VFXGraphAsset> m_graphUndo;
    std::vector<asset::VFXGraphAsset> m_graphRedo;
    bool m_graphEditInProgress = false;
    bool m_focusWindowRequested = false;
    bool m_resetWindowPlacementRequested = false;
    bool m_standaloneApplicationMode = false;
    VFXGraphEditor m_graphEditorUi;
};

} // namespace fbzz::editor
