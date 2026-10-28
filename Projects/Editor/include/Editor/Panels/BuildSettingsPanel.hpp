/// @file    BuildSettingsPanel.hpp
/// @brief   ゲームパッケージング設定パネル。
/// @author  Hasegawa Jin
/// @date    2026-05-31
/// @note /// @note Unity の "File → Build Settings" に相当する ImGui パネル。ビルドに含めるシーン一覧・出力先・
/// @note 製品名を編集し、BuildPipeline を通じてゲームをパッケージングする。
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Editor/PackageBuildService.hpp>
#include <Editor/BuildPipeline.hpp>
#include <Editor/Util/BuildConsole.hpp>
#include <Editor/Util/LogListView.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::editor {

class BuildSettingsPanel final : public IPanel {
public:
    explicit BuildSettingsPanel(PackageBuildService& build)
        : m_build(build), m_settings(build.Settings()), m_pipeline(build.Pipeline()),
          m_checks(build.Checks()), m_lastOutputDir(build.LastOutputDir()) {}
    const char* GetWindowName()        const override { return "Build Settings"; }
    bool        ShowInViewMenu()       const override { return false; }
    bool        GetDefaultVisibility() const override { return false; }
    bool        CanClose()       const override { return true; }

    void OnRenderContent(EditorContext& ctx) override;
    void OnLoadSettings(const EditorSettings& settings) override;
    void OnSaveSettings(EditorSettings& settings) const override;

private:
    using Check = PackageBuildService::Check;

    /// @note シーンリスト (追加・削除・並び替え)
    void DrawScenesInBuild(EditorContext& ctx);
    /// @note 出力先・製品名・バージョン設定
    void DrawOutputSettings(EditorContext& ctx);
    /// @note exe に焼くアイコン画像の指定 (パス欄 + プレビュー)
    void DrawIconSetting(EditorContext& ctx);
    /// @note 配布物に何が入るかの事前チェック
    void DrawPackageChecks(EditorContext& ctx);
    /// @note 進捗バーとビルドボタン
    void DrawProgressAndActions(EditorContext& ctx);

    /// @note ビルド開始の共通処理 (設定の永続化 → パイプライン起動)
    void StartBuild(EditorContext& ctx, bool runAfterBuild);

    /// @note m_checks を作り直す。ディスクを読むのでパネルを開いた時とビルド前後だけ呼ぶ。
    void RefreshChecks(EditorContext& ctx);

    /// @note シーンをプロジェクト相対で重複なく追加する。追加したら true。
    bool AddScene(const EditorContext& ctx, const std::string& absolutePath);

    /// @note runtime.start_scene (未指定なら project.default_scene) を使う。
    std::string ResolveStartScene(const EditorContext& ctx) const;

    PackageBuildService& m_build;
    BuildSettings& m_settings;
    const BuildPipeline& m_pipeline;
    int           m_selectedSceneIdx = -1;

    /// @note アイコンプレビューの読み込み結果。
    /// @note ResourceManager::LoadTexture は解決に失敗するとログへ書くため、毎フレーム引くと
    /// @note 読めないパスを入れている間だけコンソールが埋まる。結果を持ち回して再試行を避ける。
    std::string                                m_iconPreviewPath;
    renderer::ResourceHandle<renderer::TextureTag> m_iconPreviewTexture{};
    std::uint64_t                              m_iconPreviewResetVersion = 0;
    /// @note .ico は自前でデコードして CreateTexture するため、実体の所有権をこちらが持つ。
    bool                                       m_iconPreviewOwnsTexture = false;

    const std::vector<Check>& m_checks;
    bool               m_checksValid    = false;

    /// @note Reset 後も「どこに出たか」を残し、Explorer で開けるようにする。
    const std::string& m_lastOutputDir;

    /// @note CMake / MSBuild の出力。Build Output と同じ操作 (複数選択・コピー・検索) で読ませる。
    LogListView   m_buildLogView{ "runtime_build_log" };
    BuildLogFeed  m_buildLogFeed;
    std::uint64_t m_buildLogGeneration = 0;     ///< @note ビルドを始めるたびに進め、前回の行を捨てさせる
    std::string   m_buildLogRequestId;
};

} /// @note namespace fbzz::editor
