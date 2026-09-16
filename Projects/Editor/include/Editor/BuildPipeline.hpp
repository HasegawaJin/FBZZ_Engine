/// @file    BuildPipeline.hpp
/// @brief   ゲームパッケージングのステートマシン。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// WHAT: ゲームを配布可能な形にパッケージングするビルドパイプライン。
/// standalone ターゲットを CMake で再コンパイルし、バイナリ + アセット + プロジェクトファイルを出力する。
///
/// WHY (シングルスレッド + ステートマシン設計):
/// AGENTS.md のスレッドモデル制約 (Step 1〜5 はシングルスレッド) に従い、
/// std::thread は使わない。代わりに Tick() を毎フレーム呼んで 1 ファイルずつ
/// コピーすることで ImGui の描画をブロックしない。
///
/// WHY (アトミックコミット設計):
/// 出力先を直接クリアせず _tmp ディレクトリに書き出し、完了後に rename する。
/// コピー途中でエラーが起きても旧ビルドは保持される。
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

    // ビルドを開始する。State が Running に移行する。
    // @param scriptsDllPath コンパイル済みスクリプト DLL の絶対パス (空の場合はコピーをスキップ)
    // @param engineRoot     子プロセスへ渡す FBZZ_SDK_ROOT (空なら親の環境をそのまま使う)
    // @param runAfterBuild true なら Done 後に呼び出し元が exe を起動する ("Build and Run" 用)
    void Start(const BuildSettings& settings,
               const std::string& projectRoot,
               const std::string& buildRoot,
               const std::string& targetName,
               const std::string& scriptsDllPath,
               const std::string& engineRoot,
               bool runAfterBuild = false);

    // 毎フレーム 1 ステップ (CopyFiles は 1 ファイル) 進める。
    // BuildSettingsPanel::OnRenderContent() から呼ぶ。
    void Tick();

    // State をリセットして次のビルドに備える (Build and Run の重複起動防止)
    void Reset();

    // 実行中の RuntimeBuild コンパイルをキャンセルする。
    void Cancel();

    State       GetState()    const { return m_state; }
    float       GetProgress() const { return m_progress; }
    const char* GetStatus()   const { return m_status.c_str(); }
    const char* GetError()    const { return m_error.c_str(); }
    bool        WantsRunAfter() const { return m_runAfterBuild; }
    const std::string& GetBuildLog() const { return m_compiler.GetLog(); }
    // 実際にビルドした CMake 構成 (Development / Release / Debug)。
    const std::string& GetRuntimeConfiguration() const { return m_runtimeConfiguration; }

    // 出力先の <ProductName>.exe フルパスを返す
    std::string GetOutputExePath() const;
    // 出力先ディレクトリ (Explorer で開く導線が使う)
    std::string GetOutputDir() const;

private:
    enum class Step {
        Compile,          // standalone ターゲットを CMake でビルドする
        PrepareTempDir,   // 一時ディレクトリを用意する
        CopyExecutable,   // standalone exe を <ProductName>.exe としてコピー
        ApplyIcon,        // Build Settings のアイコン画像を exe のリソースへ焼く
        CopyDlls,         // exe 隣のランタイム DLL をコピー
        EnumerateFiles,   // コピー対象ファイル一覧を収集する
        CopyFiles,        // ファイルを 1 つずつコピー (複数フレーム)
        CopyProjectFiles, // .fbzz_proj / ProjectSettings.toml / シーンをコピー
        WriteManifest,    // game.manifest.toml を生成する
        CommitOutput,     // _tmp → 出力先に atomic rename
        Done,
    };

    // コピー 1 件分。列挙時に出力先まで決めておき、Tick では copy だけを行う。
    struct CopyJob {
        std::filesystem::path src;
        std::filesystem::path dst;
    };

    // 現在のステップを 1 フレーム分実行する。完了で true、失敗で false。
    bool ExecuteStep();

    // .fbzz_proj と ProjectSettings を読み、設定パス・開始シーン・レンダラーを確定する。
    // WHY Start で 1 度だけ行うか: CopyDlls (DXC の要否判定) と CopyProjectFiles の
    //     両方が同じ値を必要とするため、ステップ間で読み直すと食い違いが起きる。
    void ResolveProjectLayout();

    // CopyFiles 専用: Assets / Library/Baked / EngineAssets を m_copyJobs に集める。
    void BeginEnumerateFiles();
    // CopyFiles 専用: 1 ファイルコピー。ファイルがなくなれば false。
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
    std::string   m_engineRoot; // FBZZ_SDK_ROOT。CMake の再 configure が走っても SDK を見失わせない
    std::string   m_targetName = "SandboxStandalone";
    std::string   m_runtimeConfiguration = "Development";
    std::string   m_scriptsDllSrcPath; // 配布物にコピーするスクリプト DLL の絶対パス
    BuildSettings m_settings;
    Compiler      m_compiler;
    bool          m_compileStarted = false;

    // ResolveProjectLayout() が確定するプロジェクト構成 (すべてプロジェクトルート相対)
    std::string   m_settingsRelPath = "ProjectSettings/ProjectSettings.toml";
    std::string   m_defaultSceneRel = "Assets/Scenes/Main.scene";
    std::string   m_startSceneRel;
    std::string   m_rendererBackend; ///< ProjectSettings の renderer ("dx12")。

    std::filesystem::path m_outputDir;  // 解決済みの出力先
    std::filesystem::path m_tmpDir;     // 作業用一時ディレクトリ (outputDir + "_tmp")
    std::filesystem::path m_exeSrcPath; // standalone exe のフルパス

    // CopyFiles ステップ用
    std::vector<CopyJob> m_copyJobs;
    size_t               m_copyIdx = 0;
};

} // namespace fbzz::editor
