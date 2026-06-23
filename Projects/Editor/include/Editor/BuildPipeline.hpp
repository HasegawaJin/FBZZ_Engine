// FBZZ Engine
// BuildPipeline.hpp | fbzz::editor
// ゲームパッケージングのステートマシン
//
// WHAT: ゲームを配布可能な形にパッケージングするビルドパイプライン。
//       standalone ターゲットを CMake で再コンパイルし、バイナリ + アセット + プロジェクトファイルを出力する。
//
// WHY (シングルスレッド + ステートマシン設計):
//   AGENTS.md のスレッドモデル制約 (Step 1〜5 はシングルスレッド) に従い、
//   std::thread は使わない。代わりに Tick() を毎フレーム呼んで 1 ファイルずつ
//   コピーすることで ImGui の描画をブロックしない。
//
// WHY (アトミックコミット設計):
//   出力先を直接クリアせず _tmp ディレクトリに書き出し、完了後に rename する。
//   コピー途中でエラーが起きても旧ビルドは保持される。
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
    // @param runAfterBuild true なら Done 後に呼び出し元が exe を起動する ("Build and Run" 用)
    void Start(const BuildSettings& settings,
               const std::string& projectRoot,
               const std::string& buildRoot,
               const std::string& targetName,
               const std::string& scriptsDllPath,
               bool runAfterBuild = false);

    // 毎フレーム 1 ステップ (CopyAssets は 1 ファイル) 進める。
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

    // 出力先の <ProductName>.exe フルパスを返す
    std::string GetOutputExePath() const;

private:
    enum class Step {
        Compile,          // standalone ターゲットを CMake でビルドする
        PrepareTempDir,   // 一時ディレクトリを用意する
        CopyExecutable,   // standalone exe を <ProductName>.exe としてコピー
        CopyDlls,         // 必要な DLL をコピー
        EnumerateAssets,  // コピー対象ファイル一覧を収集する
        CopyAssets,       // ファイルを 1 つずつコピー (複数フレーム)
        CopyProjectFiles, // .fbzz_proj / ProjectSettings.toml / シーンをコピー
        WriteManifest,    // game.manifest.toml を生成する
        CommitOutput,     // _tmp → 出力先に atomic rename
        Done,
    };

    // 現在のステップを 1 フレーム分実行する。完了で true、失敗で false。
    bool ExecuteStep();

    // CopyAssets 専用: ファイル一覧を m_assetFiles に収集する (1 フレームで完了)
    void BeginEnumerateAssets();
    // CopyAssets 専用: 1 ファイルコピー。ファイルがなくなれば false。
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
    std::string   m_targetName = "SandboxStandalone";
    std::string   m_scriptsDllSrcPath; // 配布物にコピーするスクリプト DLL の絶対パス
    BuildSettings m_settings;
    Compiler      m_compiler;
    bool          m_compileStarted = false;

    std::filesystem::path m_outputDir;  // 解決済みの出力先
    std::filesystem::path m_tmpDir;     // 作業用一時ディレクトリ (outputDir + "_tmp")
    std::filesystem::path m_exeSrcPath; // standalone exe のフルパス

    // CopyAssets ステップ用
    std::vector<std::filesystem::path> m_assetFiles; // コピー対象ファイル一覧
    size_t                             m_assetIdx = 0;
};

} // namespace fbzz::editor
