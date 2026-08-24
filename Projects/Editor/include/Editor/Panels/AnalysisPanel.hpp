// FBZZ Engine
// AnalysisPanel.hpp | fbzz::editor
// Profiler と MemoryDebug をまとめて確認するエディター診断パネル
#pragma once

#include <Editor/Panels/IPanel.hpp>

namespace fbzz::editor {

// CPU プロファイルとメモリ統計を Editor UI から確認するパネル。
// WHY: Debug メニュー配下で性能とメモリを同じ文脈に置き、処理の重さと確保状況を同時に追えるようにする。
class AnalysisPanel final : public IPanel {
public:
    const char* GetWindowName()        const override { return "Analysis"; }
    const char* GetViewMenuName()      const override { return "Analysis"; }
    bool        GetDefaultVisibility() const override { return false; }

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    void DrawProfiler();
    void DrawMemory(EditorContext& ctx);
    // フレーム時間・DrawCall / ポリゴン数・GPU パスタイミングを表示するレンダリング統計タブ。
    void DrawRendering(EditorContext& ctx);
};

} // namespace fbzz::editor
