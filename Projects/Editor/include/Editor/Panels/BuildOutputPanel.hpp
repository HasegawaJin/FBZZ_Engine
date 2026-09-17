/// @file    BuildOutputPanel.hpp
/// @brief   スクリプト DLL / HLSL コンパイルの診断・ライブログ・ビルド履歴を表示する専用パネル。
/// @author  Hasegawa Jin
/// @date    2026-07-19
///
/// @note 汎用 Console はあらゆるログが混在し、コンパイルエラーが埋もれていた。本パネルはビルド出力だけを
///       対象に、診断を file:line 付きで構造化表示し、ダブルクリックで該当箇所を外部エディタで開けるようにする。
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Util/BuildConsole.hpp>
#include <Editor/Util/LogListView.hpp>

#include <cstddef>
#include <string>

namespace fbzz::editor {

class BuildOutputPanel : public IPanel {
public:
    const char* GetWindowName()      const override { return "Build Output"; }
    /// 起動時は非表示。ビルド失敗の通知やステータスクリックで開く運用にする。
    bool        GetDefaultVisibility() const override { return false; }

    /// 外部 (通知バー / StatusBar クリック) から呼ばれ、パネルを開いて最初のエラーへスクロールさせる。
    void RequestFocusFirstError() { visible = true; m_focusFirstError = true; m_viewHistoryIndex = -1; }

private:
    void OnRenderContent(EditorContext& ctx) override;

    /// 状態 (ビルド中 / 件数) と履歴・操作ボタンの 1 行。
    void DrawHeader(EditorContext& ctx, BuildConsole& console, const BuildRecord* rec);
    /// 表示中レコードの診断を一覧へ流し込む。レコードか件数が変わったときだけ作り直す。
    void SyncDiagnostics(const BuildRecord* rec);
    /// 診断と生ログの境界。ドラッグで m_splitRatio を動かす。
    void DrawSplitter();

    int   m_viewHistoryIndex = -1;    ///< -1 = 最新 (ライブ)。それ以外は History() のインデックス
    bool  m_focusFirstError  = false; ///< 次フレームで最初のエラー行へスクロール
    float m_splitRatio       = 0.45f; ///< 診断一覧が占める高さの比率

    LogListView  m_diagView{ "build_diagnostics" };
    LogListView  m_rawView{ "build_raw_output" };
    BuildLogFeed m_rawFeed;

    const BuildRecord* m_diagRecord = nullptr;
    std::string        m_diagRecordClock;
    std::size_t        m_diagCount  = ~static_cast<std::size_t>(0);
};

} // namespace fbzz::editor
