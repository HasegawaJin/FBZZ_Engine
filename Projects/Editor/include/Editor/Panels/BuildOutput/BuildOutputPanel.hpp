/// @file    BuildOutputPanel.hpp
/// @brief   スクリプト DLL / HLSL コンパイルの診断・ライブログ・ビルド履歴を表示する専用パネル。
/// @author  Hasegawa Jin
/// @date    2026-07-19
///
/// @note 汎用 Console はあらゆるログが混在し、コンパイルエラーが埋もれていた。本パネルはビルド出力だけを
/// @note 対象に、診断を file:line 付きで構造化表示し、ダブルクリックで該当箇所を外部エディタで開けるようにする。
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Diagnostics/Logging/BuildConsole.hpp>
#include <Editor/Diagnostics/Logging/LogListView.hpp>

#include <cstddef>
#include <string>

namespace fbzz::editor {

class BuildOutputPanel : public IPanel {
public:
    const char* GetWindowName()      const override { return "Build Output"; }
    /// @note 起動時は非表示。ビルド失敗の通知やステータスクリックで開く運用にする。
    bool        GetDefaultVisibility() const override { return false; }
    void OnRegisterOperators(OperatorRegistry& registry) override;

    /// @note 外部 (通知バー / StatusBar クリック) から呼ばれ、パネルを開いて最初のエラーへスクロールさせる。
    void RequestFocusFirstError() { visible = true; m_focusFirstError = true; m_selectedBuild = 0; }

private:
    void DrawManagedBuildStatus(EditorContext& ctx);
    void OnRenderContent(EditorContext& ctx) override;
    const BuildRecord* SelectedRecord(const BuildConsole& console) const;
    void ClearHistory(BuildConsole& console);

    /// @note 状態 (ビルド中 / 件数) と履歴・操作ボタンの 1 行。
    void DrawHeader(EditorContext& ctx, BuildConsole& console, const BuildRecord* rec);
    /// @note 表示中レコードの診断を一覧へ流し込む。レコードか件数が変わったときだけ作り直す。
    void SyncDiagnostics(const BuildRecord* rec);
    /// @note 診断と生ログの境界。ドラッグで m_splitRatio を動かす。
    void DrawSplitter();

    std::string m_managedDiagnosticRequestId;
    std::size_t m_managedDiagnosticPage = 0;
    std::size_t m_managedDiagnosticSelection = 0;

    std::uint64_t m_selectedBuild = 0; ///< @note 0 は最新への追従。履歴の入れ替わりで別のビルドを選ばない。
    bool  m_focusFirstError  = false; ///< @note 次フレームで最初のエラー行へスクロール
    float m_splitRatio       = 0.45f; ///< @note 診断一覧が占める高さの比率

    LogListView  m_diagView{ "build_diagnostics" };
    LogListView  m_rawView{ "build_raw_output" };
    BuildLogFeed m_rawFeed;

    std::uint64_t      m_diagRecord = 0;
    std::size_t        m_diagCount  = ~static_cast<std::size_t>(0);
};

} /// @note namespace fbzz::editor
