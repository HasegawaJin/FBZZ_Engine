/// @file    BehaviorTreePanel.hpp
/// @brief   .behaviortree を木として編集するパネル。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note アセット層 (BTNodeDef / Validate / Warnings / Compile) は揃っていたが編集面だけが無く、
///       TOML を手書きするしかなかった。Docs/design/graph-editor-framework.md が最初の実利用者として
///       想定していたのがこのパネル。木構造なので自動整列がそのまま効く。
///       責務: 描画・パン・ズーム・選択・接続ドラッグ・ショートカットは GraphCanvas、
///       木の妥当性 (親の重複 / 循環 / 子数制限) と Undo と保存はこのパネル。
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
    HotkeyScope GetHotkeyScope() const override { return m_path.empty() ? HotkeyScope::None : HotkeyScope::BehaviorTree; }
    void OnInit(EditorContext& ctx) override;
    void OnRegisterOperators(OperatorRegistry& registry) override;
    void OnShutdown() override;

    /// AssetBrowser のダブルクリックから開く。
    void RequestOpen(const std::string& path) { m_requestedPath = path; }

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    /// @name ID 規約
    /// @{
    /// ノード id はアセットの BTNodeDef::id をそのまま使う。ピンは GraphIds の
    /// 偶奇規約に乗せる (木なので入力 1 本・出力 1 本しか要らない)。
    /// @note GraphInteraction が持ち主のノード id を返すため、ツール側で総当り逆引きする必要が無い。
    [[nodiscard]] static int LinkIdOf(int childNodeId) { return childNodeId; }

    bool LoadTree(const std::string& path);
    void RequestLoadTree(const std::string& path);
    [[nodiscard]] bool SaveTree();
    void PushUndo();
    void Undo();
    void Redo();

    /// 木の不変条件を保ったまま親子を張り替える。失敗理由を返す (成功なら空)。
    [[nodiscard]] std::string TryReparent(int childId, int newParentId);
    void AddNode(fbzz::ai::BTNodeType type, float gridX, float gridY, int parentId);
    void DeleteNode(int nodeId);
    /// 選択ノードとその子孫を複製する。GraphSubgraphOps の id 再割当を使う。
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

    /// 選択中のエージェントに割り当てられた木なら、その実行状態を返す。
    [[nodiscard]] const scene::BehaviorTreeComponent* FindRuntime(EditorContext& ctx) const;

    GraphCanvas m_graphCanvas; ///< ノードとリンクを描き、クリックやドラッグの入力を処理するキャンバス。

    fbzz::ai::BehaviorTreeAsset m_asset; ///< パネルで編集中の Behavior Tree ドキュメント。
    std::string m_path; ///< 現在開いている Behavior Tree のファイルパス。
    std::string m_requestedPath; ///< 次回のパネル更新時に開くよう要求された Behavior Tree のファイルパス。
    bool m_dirty = false; ///< 現在編集中の Behavior Tree に、ファイルへ保存していない変更があるか。
    std::string m_error; ///< Behavior Tree の読み込み、検証または保存に失敗した理由。
    std::vector<fbzz::ai::BTWarning> m_warnings; ///< 現在の Behavior Tree の検証で見つかった警告。

    /// スナップショット Undo。木は最大でも数十ノードなので丸ごと持って問題ない
    /// (VFXEditorSession と同じ方式。LambdaCommand 方式より復元の取りこぼしが無い)。
    std::vector<fbzz::ai::BehaviorTreeAsset> m_undoStack; ///< 編集前の Behavior Tree を復元するためのスナップショット履歴。
    std::vector<fbzz::ai::BehaviorTreeAsset> m_redoStack; ///< Undo した操作を再適用するための Behavior Tree の控え。

    int m_selectedNode = 0; ///< グラフで現在選択しているノードの ID。
    int m_paletteParent = 0; ///< ノード作成パレットで指定した新しいノードの親 ID。
    float m_paletteGridX = 0.0f; ///< ノード作成パレットから追加するノードのグリッド X 座標。
    float m_paletteGridY = 0.0f; ///< ノード作成パレットから追加するノードのグリッド Y 座標。
    bool m_openPalette = false; ///< 次回の描画時にノード作成パレットを開く要求。
    /// Inspector のアセット欄が要求する projectRoot。描画開始時に一度だけ写す
    /// (DrawInspector は EditorContext を受け取らないため)。
    std::string m_projectRoot; ///< Behavior Tree の参照パスを解決するためのプロジェクトルート。
    std::string m_status; ///< 直近のグラフ編集操作の結果を表示する文言。
    bool m_showBlackboard = true; ///< Behavior Tree の Blackboard 変数欄を表示するか。
    /// @}
};

} // namespace fbzz::editor
