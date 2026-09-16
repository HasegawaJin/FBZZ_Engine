/// @file    SceneHierarchyPanel.hpp
/// @brief   シーン内 GameObject をツリー表示し選択状態を EditorContext に書き込む。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Editor/Panels/IPanel.hpp>
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
    // パネル内部の状態を要する要求 (F2 リネーム) だけをここで処理する。
    //
    // WHY: Delete / Ctrl+D / Ctrl+C / Ctrl+V / Ctrl+A / Esc はすべて
    //      EditorApp::RegisterDefaultHotkeys へ移した。以前はこの 3 つの表示モード
    //      それぞれに同じ判定が丸ごとコピーされており、片方だけ直る事故の温床だった。
    //      リネームだけは編集バッファとフォーカス制御がこのパネル内にあるため残す。
    //      フォーカスの申告は GetHotkeyScope() 経由で IPanel が行う。
    void HandlePanelRequests(EditorContext& ctx);

    // 他の面 (Scene View / 検索 / Map / AI) が選んだ対象を、このツリー上で見えるようにする。
    // EditorContext::hierarchyRevealTarget を消費し、祖先チェーンとスクロール対象を決める。
    void ConsumeRevealRequest(EditorContext& ctx);

    // ↑↓ / Home / End で選択を動かす。並びは m_visibleOrder (前フレームの描画順) が正本。
    //
    // WHY キーで動かせる必要があるか: 数百 GO のシーンでは «1 つ下» を選ぶだけでも
    //     行を目で探して狙う必要があり、リネーム → 隣を選ぶ → リネーム、のような
    //     連続作業が全部マウスの往復になる。検索欄は既にあるので、足りないのは移動だけ。
    void HandleKeyboardNavigation(EditorContext& ctx);

    // SetParent 成功後、次フレームで強制 open するノードの EntityID
    scene::EntityID m_pendingExpand;

    std::vector<scene::EntityID> m_revealOpenChain; // 強制 open する祖先 (描画後に空にする)
    scene::EntityID              m_revealScrollTo;  // 行までスクロールする対象
    scene::EntityID m_lastClickedEntity;               // Shift+クリック範囲選択のアンカー
    scene::EntityID m_pendingClickEntity;              // ドラッグにならなかったクリックの保留
    bool            m_hierarchyDragStarted = false;    // 今フレームをまたいでドラッグ中か
    scene::EntityID m_renamingId;                      // F2 リネーム対象
    char            m_renameBuffer[256] = {};
    bool            m_renameFocusPending = false;      // インライン入力欄へ初回フォーカスを移す
    std::vector<scene::EntityID> m_visibleOrder;       // 前フレームの描画順 (Shift+クリック用)

    char m_searchFilter[128] = {};
};

} // namespace fbzz::editor
