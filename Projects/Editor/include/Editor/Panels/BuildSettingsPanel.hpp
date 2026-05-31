// FBZZ Engine
// BuildSettingsPanel.hpp | fbzz::editor
// ゲームパッケージング設定パネル
//
// WHAT: Unity の "File → Build Settings" に相当する ImGui パネル。
//       ビルドに含めるシーン一覧、出力先、製品名を編集し、
//       BuildPipeline を通じてゲームをパッケージングする。
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Editor/BuildSettings.hpp>
#include <Editor/BuildPipeline.hpp>

namespace fbzz::editor {

class BuildSettingsPanel final : public IPanel {
public:
    const char* GetWindowName()  const override { return "Build Settings"; }
    bool        ShowInViewMenu() const override { return false; }
    bool        CanClose()       const override { return true; }

    void OnInit(EditorContext& ctx) override;
    void OnRenderContent(EditorContext& ctx) override;

private:
    // シーンリスト (追加・削除・並び替え)
    void DrawScenesInBuild(EditorContext& ctx);
    // 出力先・製品名・バージョン設定
    void DrawOutputSettings(EditorContext& ctx);
    // 進捗バーとビルドボタン
    void DrawProgressAndActions(EditorContext& ctx);

    BuildSettings m_settings;
    BuildPipeline m_pipeline;
    int           m_selectedSceneIdx = -1;
    bool          m_settingsLoaded   = false;
};

} // namespace fbzz::editor
