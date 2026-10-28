/// @file    SceneHierarchyPanel.hpp
/// @brief   シーン内 GameObject をツリー表示し選択状態を EditorContext に書き込む。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Util/HierarchyTraversal.hpp>
#include <Engine/Scene/Entity.hpp>
#include <vector>

namespace fbzz::editor {

class SceneHierarchyPanel : public IPanel {
public:
    const char* GetWindowName() const override { return "Scene Hierarchy"; }
    HotkeyScope GetHotkeyScope() const override { return HotkeyScope::Hierarchy; }

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

    /// @note SetParent 成功後、次フレームで強制 open するノードの EntityID
    scene::EntityID m_pendingExpand;

    std::vector<scene::EntityID> m_revealOpenChain; /// @note 強制 open する祖先 (描画後に空にする)
    scene::EntityID              m_revealScrollTo;  /// @note 行までスクロールする対象
    scene::EntityID m_lastClickedEntity;               /// @note Shift+クリック範囲選択のアンカー
    scene::EntityID m_pendingClickEntity;              /// @note ドラッグにならなかったクリックの保留
    bool            m_hierarchyDragStarted = false;    /// @note 今フレームをまたいでドラッグ中か
    scene::EntityID m_renamingId;                      /// @note F2 リネーム対象
    char            m_renameBuffer[256] = {};
    bool            m_renameFocusPending = false;      /// @note インライン入力欄へ初回フォーカスを移す
    std::vector<scene::EntityID> m_visibleOrder;       /// @note 前フレームの描画順 (Shift+クリック用)

    /// @note 前フレームの並びを描画中に維持し、完了後に swap する。値は毎フレーム作り直す。
    std::vector<scene::EntityID> m_nextVisibleOrder;
    HierarchyTraversal m_traversal;

    char m_searchFilter[128] = {};
};

} // namespace fbzz::editor
