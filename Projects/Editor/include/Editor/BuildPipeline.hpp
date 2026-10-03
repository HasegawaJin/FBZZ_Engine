/// @file    BuildPipeline.hpp
/// @brief   ゲームパッケージングのステートマシン。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// @note シングルスレッド方針のため std::thread は使わず、Tick() 毎フレーム 1 ステップで進める。
/// @note 出力先は直接クリアせず _tmp へ書き出し、完了後に rename する (アトミックコミット)。
#pragma once
#include <Editor/BuildSettings.hpp>
#include <Editor/Compiler.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace fbzz::editor {

class BuildPipeline {
public:
    enum class State { Idle, Running, Done, Failed };

    /// @brief ビルドを開始する。State が Running に移行する。
    /// @param scriptsDllPath コンパイル済みスクリプト DLL の絶対パス (空の場合はコピーをスキップ)
    /// @param engineRoot     子プロセスへ渡す FBZZ_SDK_ROOT (空なら親の環境をそのまま使う)
    /// @param runAfterBuild true なら Done 後に呼び出し元が exe を起動する ("Build and Run" 用)
    void Start(const BuildSettings& settings,
               const std::string& projectRoot,
               const std::string& buildRoot,
               const std::string& targetName,
               const std::string& scriptsDllPath,
               const std::string& engineRoot,
               bool runAfterBuild = false);

    /// @brief 毎フレーム 1 ステップ (CopyFiles は 1 ファイル) 進める。
    /// @brief BuildSettingsPanel::OnRenderContent() から呼ぶ。
    void Tick();

    /// @brief State をリセットして次のビルドに備える (Build and Run の重複起動防止)
    void Reset();

    /// @brief 実行中の RuntimeBuild コンパイルをキャンセルする。
    void Cancel();

    State       GetState()    const { return m_state; }
    float       GetProgress() const { return m_progress; }
    const char* GetStatus()   const { return m_status.c_str(); }
    const char* GetError()    const { return m_error.c_str(); }
    bool        WantsRunAfter() const { return m_runAfterBuild; }
    const std::string& GetBuildLog() const { return m_compiler.GetLog(); }
    /// @brief 実際にビルドした CMake 構成 (Development / Release / Debug)。
    const std::string& GetRuntimeConfiguration() const { return m_runtimeConfiguration; }

    /// @brief 出力先の `<ProductName>`.exe フルパスを返す
    std::string GetOutputExePath() const;
    /// @brief 出力先ディレクトリ (Explorer で開く導線が使う)
    std::string GetOutputDir() const;

    /// @return 必須配置、SDK の公開構成または配布ハッシュが不一致なら false とファイル名付き理由。
    /// @note 最終配布では構成に関わらず SDK Layers を拒否する。
    [[nodiscard]] static bool ValidateRuntimePackage(const std::filesystem::path& exePath,
        const std::filesystem::path& sdkRoot, const std::string& configuration,
        bool finalDistribution, std::string& reason, bool* dx12Enabled = nullptr);

private:
    enum class Step {
        /// @note standalone ターゲットを CMake でビルドする
        Compile,
        /// @note 一時ディレクトリを用意する
        PrepareTempDir,
        /// @note standalone exe を `<ProductName>`.exe としてコピー
        CopyExecutable,
        /// @note Build Settings のアイコン画像を exe のリソースへ焼く
        ApplyIcon,
        /// @note exe 隣のランタイム DLL をコピー
        CopyDlls,
        /// @note コピー対象ファイル一覧を収集する
        EnumerateFiles,
        /// @note ファイルを 1 つずつコピー (複数フレーム)
        CopyFiles,
        /// @note .fbzz_proj / ProjectSettings.toml / シーンをコピー
        CopyProjectFiles,
        /// @note game.manifest.toml を生成する
        WriteManifest,
        /// @note _tmp → 出力先に atomic rename
        CommitOutput,
        Done,
    };

    /// @brief コピー 1 件分。列挙時に出力先まで決めておき、Tick では copy だけを行う。
    struct CopyJob {
        std::filesystem::path src;
        std::filesystem::path dst;
    };

    /// @brief 現在のステップを 1 フレーム分実行する。完了で true、失敗で false。
    bool ExecuteStep();

    /// @brief .fbzz_proj と ProjectSettings を読み、設定パス・開始シーン・レンダラーを確定する。
    /// @note CopyDlls (DXC の要否判定) と CopyProjectFiles が同じ値を必要とするため、Start で 1 度だけ読む。
    void ResolveProjectLayout();

    /// @brief CopyFiles 専用: Assets / Library/Baked / EngineAssets を m_copyJobs に集める。
    void BeginEnumerateFiles();
    /// @brief CopyFiles 専用: 1 ファイルコピー。ファイルがなくなれば false。
    bool TickCopyOneFile();

    void SetFailed(const std::string& reason);

    State       m_state    = State::Idle;
    Step        m_step     = Step::PrepareTempDir;
    float       m_progress = 0.0f;
    std::string m_status;
    std::string m_error;
    bool        m_runAfterBuild = false;

    std::string   m_projectRoot;
    std::string   m_buildRoot;
    std::string   m_engineRoot; ///< @note FBZZ_SDK_ROOT。CMake の再 configure が走っても SDK を見失わせない
    std::string   m_targetName = "SandboxStandalone";
    std::string   m_runtimeConfiguration = "Development";
    std::string   m_scriptsDllSrcPath; ///< @note 配布物にコピーするスクリプト DLL の絶対パス
    BuildSettings m_settings;
    Compiler      m_compiler;
    bool          m_compileStarted = false;

    /// @brief ResolveProjectLayout() が確定するプロジェクト構成 (すべてプロジェクトルート相対)
    std::string   m_settingsRelPath = "ProjectSettings/ProjectSettings.toml";
    std::string   m_defaultSceneRel = "Assets/Scenes/Main.scene";
    std::string   m_startSceneRel;
    std::string   m_rendererBackend; ///< @note ProjectSettings の renderer ("dx12")。

    std::filesystem::path m_outputDir;  ///< @note 解決済みの出力先
    std::filesystem::path m_tmpDir;     ///< @note 作業用一時ディレクトリ (outputDir + "_tmp")
    std::filesystem::path m_exeSrcPath; ///< @note standalone exe のフルパス

    /// @brief CopyFiles ステップ用
    std::vector<CopyJob> m_copyJobs;
    size_t               m_copyIdx = 0;
};

} /// @note namespace fbzz::editor
