/// @file    BuildSettingsPanel.hpp
/// @brief   ゲームパッケージング設定パネル。
/// @author  Hasegawa Jin
/// @date    2026-05-31
/// @note /// @note Unity の "File → Build Settings" に相当する ImGui パネル。ビルドに含めるシーン一覧・出力先・
/// @note 製品名を編集し、BuildPipeline を通じてゲームをパッケージングする。
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Editor/BuildPipeline/PackageBuildService.hpp>
#include <Editor/BuildPipeline/BuildPipeline.hpp>
#include <Editor/Diagnostics/Logging/BuildConsole.hpp>
#include <Editor/Diagnostics/Logging/LogListView.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>

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

    PackageBuildService& m_build; ///< EditorApp 所有の配布ビルドサービスを借りる。パネルを閉じてもビルドは進む。
    BuildSettings& m_settings; ///< m_build が保持する、現在編集中の共有ビルド設定への参照。
    const BuildPipeline& m_pipeline; ///< m_build が保持する、進捗と出力を読み取る配布パイプラインへの参照。
    int           m_selectedSceneIdx = -1; ///< 配布設定のシーン一覧で選択した行の添字。未選択なら -1。

    /// @note アイコンプレビューの読み込み結果。
    /// @note ResourceManager::LoadTexture は解決に失敗するとログへ書くため、毎フレーム引くと
    /// @note 読めないパスを入れている間だけコンソールが埋まる。結果を持ち回して再試行を避ける。
    std::string                                m_iconPreviewPath; ///< 現在のアイコンプレビューに読み込んだ元ファイルのパス。
    renderer::ResourceHandle<renderer::TextureTag> m_iconPreviewTexture{}; ///< アイコンプレビューに表示する画像のテクスチャハンドル。
    std::uint64_t                              m_iconPreviewResetVersion = 0; ///< アイコンプレビューの画像を取得した ResourceManager のリセット世代。
    bool                                       m_iconPreviewOwnsTexture = false; ///< .ico は自前でデコードして CreateTexture するため、実体の所有権をこちらが持つ。

    const std::vector<Check>& m_checks; ///< m_build が保持する配布設定の事前確認結果への参照。
    bool               m_checksValid    = false; ///< 事前確認結果が現在表示するビルド設定に対応しているか。

    const std::string& m_lastOutputDir; ///< Reset 後も「どこに出たか」を残し、Explorer で開けるようにする。

    LogListView   m_buildLogView{ "runtime_build_log" }; ///< CMake / MSBuild の出力。Build Output と同じ操作 (複数選択・コピー・検索) で読ませる。
    BuildLogFeed  m_buildLogFeed; ///< 配布ビルドの増分ログを表示行へ取り込む状態。
    std::uint64_t m_buildLogGeneration = 0;     ///< ビルドを始めるたびに進め、前回の行を捨てさせる
    std::string   m_buildLogRequestId; ///< m_buildLogFeed が表示中の配布ビルドの要求 ID。
};

} // namespace fbzz::editor
