/// @file    SceneHierarchyPanel.hpp
/// @brief   シーン内 GameObject をツリー表示し選択状態を EditorContext に書き込む。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Scene/Editing/HierarchyTraversal.hpp>
#include <Engine/Scene/References/Entity.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::scene { class Scene; }

namespace fbzz::editor {

class SceneHierarchyPanel : public IPanel {
public:
    const char* GetWindowName() const override { return "Scene Hierarchy"; }
    HotkeyScope GetHotkeyScope() const override { return HotkeyScope::Hierarchy; }
    void OnRegisterOperators(OperatorRegistry& registry) override;

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    /// @note パネル内部の状態を要する要求 (F2 リネーム) だけをここで処理する。
    /// @note Delete / Ctrl+D / Ctrl+C / Ctrl+V / Ctrl+A / Esc は EditorApp::RegisterDefaultHotkeys へ
    /// @note       移した。リネームだけは編集バッファとフォーカス制御がこのパネル内にあるため残す。
    void HandlePanelRequests(EditorContext& ctx);

    /// @note 他の面 (Scene View / 検索 / Map / AI) が選んだ対象を、このツリー上で見えるようにする。
    /// @note EditorContext::hierarchyRevealTarget を消費し、祖先チェーンとスクロール対象を決める。
    void ConsumeRevealRequest(EditorContext& ctx);

    /// @note ↑↓ / Home / End で選択を動かす。並びは m_visibleOrder (前フレームの描画順) が正本。
    /// @note 数百 GO のシーンでは «1 つ下» を選ぶだけでも行を目で探す必要があり、
    /// @note       リネーム→隣を選ぶ→リネームの連続作業が全部マウス往復になっていた。
    void HandleKeyboardNavigation(EditorContext& ctx);

    scene::EntityID m_pendingExpand; ///< SetParent 成功後、次フレームで強制 open するノードの EntityID

    std::vector<scene::EntityID> m_revealOpenChain; ///< 表示要求を受けた行へ辿るために開く祖先 EntityID の列。
    scene::EntityID              m_revealScrollTo; ///< 祖先を開いた後に表示位置までスクロールする対象の EntityID。
    scene::EntityID m_lastClickedEntity; ///< 範囲選択の起点に使う最後のクリック対象の EntityID。
    scene::EntityID m_pendingClickEntity; ///< クリック確定まで保持する EntityID。ドラッグへ移った場合は選択しない。
    bool            m_hierarchyDragStarted = false; ///< 現在の押下で行のドラッグを開始したか。
    scene::EntityID m_renamingId; ///< インラインで名前を変更している EntityID。
    char            m_renameBuffer[256] = {}; ///< オブジェクトの新しい名前を入力するバッファ。
    bool            m_renameFocusPending = false; ///< インライン入力欄を表示したらフォーカスする要求。
    std::vector<scene::EntityID> m_visibleOrder; ///< 範囲選択に使う、Hierarchy に表示された行の EntityID 順。

    std::vector<scene::EntityID> m_nextVisibleOrder; ///< 前フレームの並びを描画中に維持し、完了後に swap する。値は毎フレーム作り直す。
    HierarchyTraversal m_traversal; ///< 現在のシーンの階層索引と、一回の表示巡回の作業状態。

    char m_searchFilter[128] = {}; ///< Hierarchy の名前絞り込み用の文字列バッファ。
    const scene::Scene* m_expansionScene = nullptr; ///< 展開・折りたたみ要求の対象を照合するために借りるシーン。
    std::unordered_map<std::string, bool> m_expansionRequests; ///< 次回の描画で反映する、オブジェクトの GUID ごとの開閉要求。
};

} // namespace fbzz::editor
