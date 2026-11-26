/// @file    SearchEverythingPanel.hpp
/// @brief   シーン (GameObject) とアセットを横断検索するドッキング可能パネル (Ctrl+Shift+F)。
/// @author  Hasegawa Jin
/// @date    2026-07-20
///
/// @note コマンドパレット (Ctrl+K) は「1 つ選んで即実行」向けで、結果を並べて見比べたい / 開いたまま
///       連続で辿りたい用途には向かない。常設パネルとして結果一覧を保持し、GameObject は選択+フォーカス、
///       アセットは開く/選択へ繋ぐ横断検索を提供する。
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Asset/Browser/AssetQueryCache.hpp>
#include <Editor/Util/RevisionCache.hpp>
#include <Engine/Scene/References/Entity.hpp>
#include <string>
#include <vector>

namespace fbzz::editor {

class SearchEverythingPanel final : public IPanel {
public:
    const char* GetWindowName()      const override { return "Search"; }
    const char* GetViewMenuName()    const override { return "Search"; }
    bool        GetDefaultVisibility() const override { return false; } /// @note 既定は非表示 (Ctrl+Shift+F で開く)

    /// ホットキー / メニューから開く: 表示 ON + 次フレームでフォーカス。
    void RequestOpen() { visible = true; m_requestFocus = true; m_refocusInput = true; }

protected:
    void OnBeforeBegin(EditorContext& ctx) override;
    void OnRenderContent(EditorContext& ctx) override;

private:
    struct ObjectQueryKey {
        std::string query; ///< Scene 内の名前検索に使った検索語。
        uint64_t sceneRevision = 0; ///< 名前索引を検索した時点のリビジョン。
        uint64_t sceneGeneration = 0; ///< 名前索引を検索したシーンの世代番号。
        bool operator==(const ObjectQueryKey&) const = default;
    };
    RevisionCache<ObjectQueryKey> m_objectQuery; ///< 検索語、シーン世代、名前索引の版から結果を再利用するためのキャッシュ。
    std::vector<scene::EntityID> m_objectHits; ///< 現在の名前検索に一致したシーンオブジェクトの EntityID。
    AssetQueryCache m_assetQuery; ///< 現在の検索語とアセット索引の版に対応する検索結果のキャッシュ。
    /// @note 走査・除外規則・一致判定は AssetSearch (Editor/Asset/Browser/AssetSearch.hpp) に集約した。
    ///       以前はこのパネルとアセットピッカーが別々の規則で走査し、同じ語でも結果が違っていた。
    char m_query[128] = {}; ///< アセットとシーンオブジェクトを横断検索する入力バッファ。
    bool m_requestFocus  = false; ///< パネルが開いたら検索欄へフォーカスする要求。
    bool m_refocusInput  = false; ///< 選択操作などの後で検索欄へフォーカスを戻す要求。
};

} // namespace fbzz::editor
