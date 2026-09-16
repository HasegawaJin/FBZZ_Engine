/// @file    BehaviorTreePanel.hpp
/// @brief   .behaviortree を木として編集するパネル。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// WHY 必要か:
/// AssetBrowser の Create > Behavior Tree で .behaviortree を**作れるのに開けなかった**。
/// アセット層 (BTNodeDef / Validate / Warnings / Compile) は揃っていたのに、
/// 編集面だけが無く、TOML を手書きするしかない状態だった。
///
/// WHY 共通 GraphCanvas の上に作るか:
/// Docs/design/graph-editor-framework.md が「フレームワークの最初の実利用者であり
/// 検証台」として想定していたのがこのパネル。木構造なので自動整列がそのまま効き、
/// VFX の DAG より条件が良い。ここで API の穴が出れば framework 側を直す。
///
/// 責務の分担:
/// 描画・パン・ズーム・選択・接続ドラッグ・ショートカット → GraphCanvas
/// 木の妥当性 (親の重複 / 循環 / 子数制限) と Undo と保存 → このパネル
#pragma once
#include <Editor/GraphEditor/GraphCanvas.hpp>
#include <Editor/GraphEditor/GraphView.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <Engine/AI/BehaviorTreeAsset.hpp>

#include <string>
#include <vector>

namespace fbzz::scene { struct BehaviorTreeComponent; }

namespace fbzz::editor {

class BehaviorTreePanel final : public IPanel {
public:
    const char* GetWindowName()        const override { return "Behavior Tree"; }
    bool        GetDefaultVisibility() const override { return false; }
    void OnInit(EditorContext& ctx) override;
    void OnShutdown() override;

    // AssetBrowser のダブルクリックから開く。
    void RequestOpen(const std::string& path) { m_requestedPath = path; }

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    // ── ID 規約 ──
    // ノード id はアセットの BTNodeDef::id をそのまま使う。ピンは GraphIds の
    // 偶奇規約に乗せる (木なので入力 1 本・出力 1 本しか要らない)。
    // WHY 総当りで解決しないか: GraphInteraction が持ち主のノード id を返すように
    //     なったため、ツール側で逆引きする必要が無くなった。
    [[nodiscard]] static int LinkIdOf(int childNodeId) { return childNodeId; }

    bool LoadTree(const std::string& path);
    [[nodiscard]] bool SaveTree();
    void PushUndo();
    void Undo();
    void Redo();

    // 木の不変条件を保ったまま親子を張り替える。失敗理由を返す (成功なら空)。
    [[nodiscard]] std::string TryReparent(int childId, int newParentId);
    void AddNode(fbzz::ai::BTNodeType type, float gridX, float gridY, int parentId);
    void DeleteNode(int nodeId);
    // 選択ノードとその子孫を複製する。GraphSubgraphOps の id 再割当を使う。
    void DuplicateSubtree(int nodeId);
    void AutoLayout();
    void RefreshValidation();

    [[nodiscard]] GraphView BuildView(EditorContext& ctx,
                                      const scene::BehaviorTreeComponent* runtime) const;
    void ApplyInteraction(EditorContext& ctx, const GraphInteraction& interaction);

    void DrawToolbar(EditorContext& ctx);
    void DrawBlackboardSidebar();
    void DrawInspector();
    void DrawNodePalette(float gridX, float gridY, int parentId);
    void DrawValidationBanner();

    // 選択中のエージェントに割り当てられた木なら、その実行状態を返す。
    [[nodiscard]] const scene::BehaviorTreeComponent* FindRuntime(EditorContext& ctx) const;

    GraphCanvas m_graphCanvas;

    fbzz::ai::BehaviorTreeAsset m_asset;
    std::string m_path;
    std::string m_requestedPath;
    bool m_dirty = false;
    std::string m_error;
    std::vector<fbzz::ai::BTWarning> m_warnings;

    // スナップショット Undo。木は最大でも数十ノードなので丸ごと持って問題ない
    // (VFXEditorSession と同じ方式。LambdaCommand 方式より復元の取りこぼしが無い)。
    std::vector<fbzz::ai::BehaviorTreeAsset> m_undoStack;
    std::vector<fbzz::ai::BehaviorTreeAsset> m_redoStack;

    int m_selectedNode = 0;
    int m_paletteParent = 0;
    float m_paletteGridX = 0.0f;
    float m_paletteGridY = 0.0f;
    bool m_openPalette = false;
    // Inspector のアセット欄が要求する projectRoot。描画開始時に一度だけ写す
    // (DrawInspector は EditorContext を受け取らないため)。
    std::string m_projectRoot;
    std::string m_status;
    bool m_showBlackboard = true;
};

} // namespace fbzz::editor
