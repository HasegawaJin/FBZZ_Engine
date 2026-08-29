/// @file    BuildSettingsPanel.hpp
/// @brief   ゲームパッケージング設定パネル。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// WHAT: Unity の "File → Build Settings" に相当する ImGui パネル。
/// ビルドに含めるシーン一覧、出力先、製品名を編集し、
/// BuildPipeline を通じてゲームをパッケージングする。
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Editor/BuildSettings.hpp>
#include <Editor/BuildPipeline.hpp>

#include <string>
#include <vector>

namespace fbzz::editor {

class BuildSettingsPanel final : public IPanel {
public:
    const char* GetWindowName()        const override { return "Build Settings"; }
    bool        ShowInViewMenu()       const override { return false; }
    bool        GetDefaultVisibility() const override { return false; }
    bool        CanClose()       const override { return true; }

    void OnRenderContent(EditorContext& ctx) override;
    void OnLoadSettings(const EditorSettings& settings) override;
    void OnSaveSettings(EditorSettings& settings) const override;

private:
    // 配布物が成立するかの事前チェック 1 件。
    // WHY: 欠けているものは「ビルドは成功したのに起動すると何も出ない」形で現れる。
    //      押す前に、何が入って何が入らないかを名指しで見せる。
    struct Check {
        enum class Level { Ok, Warn, Error };
        Level       level = Level::Ok;
        std::string label;
        std::string detail;
    };

    // シーンリスト (追加・削除・並び替え)
    void DrawScenesInBuild(EditorContext& ctx);
    // 出力先・製品名・バージョン設定
    void DrawOutputSettings(EditorContext& ctx);
    // 配布物に何が入るかの事前チェック
    void DrawPackageChecks(EditorContext& ctx);
    // 進捗バーとビルドボタン
    void DrawProgressAndActions(EditorContext& ctx);

    // ビルド開始の共通処理 (設定の永続化 → パイプライン起動)
    void StartBuild(EditorContext& ctx, bool runAfterBuild);

    // m_checks を作り直す。ディスクを読むのでパネルを開いた時とビルド前後だけ呼ぶ。
    void RefreshChecks(EditorContext& ctx);

    // シーンをプロジェクト相対で重複なく追加する。追加したら true。
    bool AddScene(const EditorContext& ctx, const std::string& absolutePath);

    // 実際に開始シーンとして使われるパス (リスト先頭、無ければ ProjectSettings)。
    std::string ResolveStartScene(const EditorContext& ctx) const;

    BuildSettings m_settings;
    BuildPipeline m_pipeline;
    int           m_selectedSceneIdx = -1;

    std::vector<Check> m_checks;
    bool               m_checksValid    = false;
    std::string        m_standaloneExeDir; // ビルド成果物の出力先 (DLL / EngineAssets の在処)

    // Reset 後も「どこに出たか」を残し、Explorer で開けるようにする。
    std::string m_lastOutputDir;
};

} // namespace fbzz::editor
