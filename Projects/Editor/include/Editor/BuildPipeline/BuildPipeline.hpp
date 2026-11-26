/// @file    BuildPipeline.hpp
/// @brief   ゲームパッケージングのステートマシン。
/// @author  Hasegawa Jin
/// @date    2026-05-31
/// @note /// @note シングルスレッド方針のため std::thread は使わず、Tick() 毎フレーム 1 ステップで進める。
/// @note 出力先は直接クリアせず _tmp へ書き出し、完了後に rename する (アトミックコミット)。
#pragma once
#include <Editor/BuildPipeline/BuildSettings.hpp>
#include <Editor/BuildPipeline/Compiler.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace fbzz::editor {

class BuildPipeline {
public:
    enum class State { Idle, Running, Done, Failed };

    /// @brief ビルドを開始する。State が Running に移行する。
    /// @param[in] engineRoot 子プロセスへ渡す FBZZ_SDK_ROOT (空なら親の環境をそのまま使う)
    /// @param[in] runAfterBuild true なら Done 後に呼び出し元が exe を起動する ("Build and Run" 用)
    void Start(const BuildSettings& settings,
               const std::string& projectRoot,
               const std::string& buildRoot,
               const std::string& targetName,
               const std::string& engineRoot,
               bool runAfterBuild = false);

    /// @brief 毎フレーム 1 ステップ (CopyFiles は 1 ファイル) 進める。
    /// @brief BuildSettingsPanel::Editor update() から呼ぶ。
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
    /// @return 未存在の要求専用フォルダを排他的に作成した時だけtrue。既存フォルダは変更しない。
    /// @pre outputはValidateOutputPathで検証済みの配布先であること。
    /// @note 成功時のoutは呼出元が所有し、失敗・取消後はそのフォルダだけを後始末する。
    [[nodiscard]] static bool CreateOwnedTempDirectory(const std::filesystem::path& output,
        std::filesystem::path& out, std::string& reason);

private:
    enum class Step {
        /// @note standalone ターゲットを CMake でビルドする
        Compile,
        /// @note 一時ディレクトリを用意する
        PrepareTempDir,
        /// @note standalone exe のビルド完了後に C# をビルドする。
        CompileManaged,
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
        std::filesystem::path src; ///< コピー元となるファイルのパス。
        std::filesystem::path dst; ///< ビルドが所有する一時配布フォルダー内のコピー先。
    };

    /// @brief 現在のステップを 1 フレーム分実行する。完了で true、失敗で false。
    bool ExecuteStep();
    /// @note 現在の Editor catalog は切り替えず、配布候補の source/型 manifest だけを検証する。
    void TickManagedBuild();
    [[nodiscard]] bool CopyManagedPackage(std::string& diagnostic);

    /// @brief .fbzz_proj と ProjectSettings を読み、設定パス・開始シーン・レンダラーを確定する。
    /// @note CopyDlls (DXC の要否判定) と CopyProjectFiles が同じ値を必要とするため、Start で 1 度だけ読む。
    void ResolveProjectLayout();

    /// @brief CopyFiles 専用: Assets / Library/Baked / EngineAssets を m_copyJobs に集める。
    void BeginEnumerateFiles();
    /// @brief CopyFiles 専用: 1 ファイルコピー。ファイルがなくなれば false。
    bool TickCopyOneFile();

    void SetFailed(const std::string& reason);
    void CleanupOwnedTemp();

    State       m_state    = State::Idle; ///< 現在の配布ビルドの開始前・実行中・終端状態。
    Step        m_step     = Step::PrepareTempDir; ///< Tick で進める現在の配布工程。
    float       m_progress = 0.0f; ///< パッケージ作成全体の進み具合 [0,1]。
    std::string m_status; ///< 進行中のビルド段階を UI に表示するメッセージ。
    std::string m_error; ///< ビルドが最後に失敗した理由。
    bool        m_runAfterBuild = false; ///< ビルドが成功した後に呼び出し元から配布 exe を起動するか。

    std::string   m_projectRoot; ///< アセットとプロジェクト設定の取得元となるルートパス。
    std::string   m_buildRoot; ///< ランタイム exe を再ビルドする CMake ビルドツリーのパス。
    std::string   m_engineRoot; ///< FBZZ_SDK_ROOT。CMake の再 configure が走っても SDK を見失わせない
    std::string   m_targetName = "SandboxStandalone"; ///< 配布 exe を生成する CMake ターゲット名。
    std::string   m_runtimeConfiguration = "Development"; ///< 配布する実行ファイルと DLL を選ぶビルド構成名。
    BuildSettings m_settings; ///< このビルド開始時に確定したパッケージ設定。
    Compiler      m_compiler; ///< ランタイム exe を生成する CMake 子プロセスの所有者。
    bool          m_compileStarted = false; ///< 現在のビルド要求で、ランタイム exe のコンパイルを開始済みか。
    bool          m_managedCompileStarted = false; ///< 現在のビルド要求で、C# プロジェクトのビルドを開始済みか。
    std::filesystem::path m_managedProject; ///< dotnet でビルドするゲーム用 csproj のパス。
    std::filesystem::path m_managedHostDirectory; ///< パッケージに同梱するマネージドホストの配置ディレクトリ。
    std::filesystem::path m_managedOutput; ///< dotnet のビルド成果物を取得するディレクトリ。

    std::string   m_settingsRelPath = "ProjectSettings/ProjectSettings.toml"; ///< ResolveProjectLayout() が確定するプロジェクト構成 (すべてプロジェクトルート相対)
    std::string   m_defaultSceneRel = "Assets/Scenes/Main.scene"; ///< .fbzz_proj が指定する既定シーンのプロジェクトルート相対パス。
    std::string   m_startSceneRel; ///< ProjectSettings で選ばれた開始シーンのプロジェクトルート相対パス。
    std::string   m_rendererBackend; ///< ProjectSettings の renderer ("dx12")。

    std::filesystem::path m_outputDir;  ///< 解決済みの出力先
    std::filesystem::path m_tmpDir;     ///< 要求専用の未存在フォルダを排他的に作成する。
    bool m_tmpOwned = false;           ///< 自身が作成したフォルダだけを後始末する。
    std::filesystem::path m_exeSrcPath; ///< standalone exe のフルパス

    std::vector<CopyJob> m_copyJobs; ///< CopyFiles ステップ用
    size_t               m_copyIdx = 0; ///< 次に処理する m_copyJobs の添字。全件完了時は配列の要素数。
};

} // namespace fbzz::editor
