/// @file    SearchEverythingPanel.hpp
/// @brief   シーン (GameObject) とアセットを横断検索するドッキング可能パネル (Ctrl+Shift+F)。
/// @author  Hasegawa Jin
/// @date    2026-07-20
///
/// WHY: コマンドパレット (Ctrl+K) は「1 つ選んで即実行」に最適化されているが、結果を並べて
/// 見比べたい / 開いたまま連続で辿りたい用途には向かない。常設パネルとして結果一覧を保持し、
/// GameObject は選択+フォーカス、アセットは開く/選択へ繋ぐ横断検索を提供する。
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <string>
#include <vector>

namespace fbzz::editor {

class SearchEverythingPanel final : public IPanel {
public:
    const char* GetWindowName()      const override { return "Search"; }
    const char* GetViewMenuName()    const override { return "Search"; }
    bool        GetDefaultVisibility() const override { return false; } // 既定は非表示 (Ctrl+Shift+F で開く)

    // ホットキー / メニューから開く: 表示 ON + 次フレームでフォーカス。
    void RequestOpen() { visible = true; m_requestFocus = true; m_refocusInput = true; }

protected:
    void OnBeforeBegin(EditorContext& ctx) override;
    void OnRenderContent(EditorContext& ctx) override;

private:
    // WHY 自前の索引を持たなくなったか: 走査・除外規則・一致判定は
    //     AssetSearch (Editor/Util/AssetSearch.hpp) に集約した。
    //     以前はこのパネルとアセットピッカーが別々の規則で走査しており、
    //     同じ語で検索しても画面によって結果が違っていた。
    char m_query[128] = {};
    bool m_requestFocus  = false; // 次フレームでウィンドウをフォーカス
    bool m_refocusInput  = false; // 次フレームで入力欄へキーボードフォーカス
};

} // namespace fbzz::editor
