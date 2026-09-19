/// @file    BuildSettingsPanel.hpp
/// @brief   ゲームパッケージング設定パネル。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// @note Unity の "File → Build Settings" に相当する ImGui パネル。ビルドに含めるシーン一覧・出力先・
///       製品名を編集し、BuildPipeline を通じてゲームをパッケージングする。
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Editor/BuildSettings.hpp>
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
    const char* GetWindowName()        const override { return "Build Settings"; }
    bool        ShowInViewMenu()       const override { return false; }
    bool        GetDefaultVisibility() const override { return false; }
    bool        CanClose()       const override { return true; }

    void OnRenderContent(EditorContext& ctx) override;
    void OnLoadSettings(const EditorSettings& settings) override;
    void OnSaveSettings(EditorSettings& settings) const override;

private:
    /// 配布物が成立するかの事前チェック 1 件。
    /// @note 欠けているものは「ビルドは成功したのに起動すると何も出ない」形で現れるため、
    ///       押す前に何が入って何が入らないかを名指しで見せる。
    struct Check {
        enum class Level { Ok, Warn, Error };
        Level       level = Level::Ok;
        std::string label;
        std::string detail;
    };

    /// シーンリスト (追加・削除・並び替え)
    void DrawScenesInBuild(EditorContext& ctx);
    /// 出力先・製品名・バージョン設定
    void DrawOutputSettings(EditorContext& ctx);
    /// exe に焼くアイコン画像の指定 (パス欄 + プレビュー)
    void DrawIconSetting(EditorContext& ctx);
    /// 配布物に何が入るかの事前チェック
    void DrawPackageChecks(EditorContext& ctx);
    /// 進捗バーとビルドボタン
    void DrawProgressAndActions(EditorContext& ctx);

    /// ビルド開始の共通処理 (設定の永続化 → パイプライン起動)
    void StartBuild(EditorContext& ctx, bool runAfterBuild);

    /// m_checks を作り直す。ディスクを読むのでパネルを開いた時とビルド前後だけ呼ぶ。
    void RefreshChecks(EditorContext& ctx);

    /// シーンをプロジェクト相対で重複なく追加する。追加したら true。
    bool AddScene(const EditorContext& ctx, const std::string& absolutePath);

    /// 実際に開始シーンとして使われるパス (リスト先頭、無ければ ProjectSettings)。
    std::string ResolveStartScene(const EditorContext& ctx) const;

    BuildSettings m_settings;
    BuildPipeline m_pipeline;
    int           m_selectedSceneIdx = -1;

    /// アイコンプレビューの読み込み結果。
    /// @note ResourceManager::LoadTexture は解決に失敗するとログへ書くため、毎フレーム引くと
    ///       読めないパスを入れている間だけコンソールが埋まる。結果を持ち回して再試行を避ける。
    std::string                                m_iconPreviewPath;
    renderer::ResourceHandle<renderer::TextureTag> m_iconPreviewTexture{};
    std::uint64_t                              m_iconPreviewResetVersion = 0;
    /// .ico は自前でデコードして CreateTexture するため、実体の所有権をこちらが持つ。
    bool                                       m_iconPreviewOwnsTexture = false;

    std::vector<Check> m_checks;
    bool               m_checksValid    = false;
    std::string        m_standaloneExeDir; ///< ビルド成果物の出力先 (DLL / EngineAssets の在処)

    /// Reset 後も「どこに出たか」を残し、Explorer で開けるようにする。
    std::string m_lastOutputDir;

    /// CMake / MSBuild の出力。Build Output と同じ操作 (複数選択・コピー・検索) で読ませる。
    LogListView   m_buildLogView{ "runtime_build_log" };
    BuildLogFeed  m_buildLogFeed;
    std::uint64_t m_buildLogGeneration = 0;     ///< ビルドを始めるたびに進め、前回の行を捨てさせる
    bool          m_buildLogWasRunning = false;
};

} // namespace fbzz::editor
