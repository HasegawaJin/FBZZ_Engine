// FBZZ Engine
// BuildOutputPanel.hpp | fbzz::editor
// スクリプト DLL / HLSL コンパイルの診断・ライブログ・ビルド履歴を表示する専用パネル。
//
// WHY: 汎用 Console はあらゆるログが混在し、コンパイルエラーが埋もれていた。
//      本パネルはビルド出力だけを対象に、診断を file:line 付きで構造化表示し、
//      ダブルクリックで該当箇所を外部エディタで開けるようにする。
#pragma once
#include <Editor/Panels/IPanel.hpp>

namespace fbzz::editor {

struct BuildRecord;
class  BuildConsole;

class BuildOutputPanel : public IPanel {
public:
    const char* GetWindowName()      const override { return "Build Output"; }
    // 起動時は非表示。ビルド失敗の通知やステータスクリックで開く運用にする。
    bool        GetDefaultVisibility() const override { return false; }

    // 外部 (通知バー / StatusBar クリック) から呼ばれ、パネルを開いて最初のエラーへスクロールさせる。
    void RequestFocusFirstError() { visible = true; m_focusFirstError = true; m_viewHistoryIndex = -1; }

private:
    void OnRenderContent(EditorContext& ctx) override;

    // 指定レコードの診断リストを描画する。ダブルクリックで OpenInEditor を呼ぶ。
    void DrawDiagnostics(const BuildRecord& rec);
    // 現在ビルド or 選択レコードの生ログを描画する。
    void DrawRawLog(const BuildConsole& console, const BuildRecord* rec);

    int  m_viewHistoryIndex = -1;    // -1 = 最新 (ライブ)。それ以外は History() のインデックス
    bool m_focusFirstError  = false; // 次フレームで最初のエラー行へスクロール
    bool m_autoScroll       = true;
    bool m_errorsOnly       = false;
};

} // namespace fbzz::editor
