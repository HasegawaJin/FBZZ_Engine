/// @file    AnalysisPanel.hpp
/// @brief   Profiler と MemoryDebug をまとめて確認するエディター診断パネル。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#pragma once

#include <Editor/Panels/IPanel.hpp>

namespace fbzz::editor {

/// @brief CPU プロファイルとメモリ統計を Editor UI から確認するパネル。
/// @note Debug メニュー配下に性能とメモリを同居させ、重さと確保状況を同時に追えるようにする。
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
    /// フレーム時間・DrawCall / ポリゴン数・GPU パスタイミングを表示するレンダリング統計タブ。
    void DrawRendering(EditorContext& ctx);
};

} // namespace fbzz::editor
