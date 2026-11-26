/// @file    Compiler.hpp
/// @brief   RuntimeBuild とアセット生成用の非同期子プロセス管理。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#pragma once

#include <filesystem>
#include <string>

namespace fbzz::editor {

/// @brief CMake または明示コマンドを、所有する子プロセス群として管理する。
/// @note メインスレッドをブロックしないため、Tick() ごとにパイプを短時間だけ読む。
class Compiler {
public:
    enum class State { Idle, Building, Done, Failed, Cancelled };

    struct Config {
        std::filesystem::path cmakeExe; ///< 通常ビルドで起動する cmake.exe のパス。
        std::filesystem::path buildDir; ///< cmake --build に渡すビルドツリーのパス。
        std::filesystem::path exePath; ///< 呼び出し元が期待する実行ファイルの出力先。
        std::string           target; ///< cmake --build に指定するターゲット名。
        std::string           configuration; ///< マルチ構成ビルドで選ぶ構成名。
        /// @note CMake 自動再生成時にも FBZZ_SDK_ROOT を継承する。
        std::string           sdkRoot; ///< 子プロセスの FBZZ_SDK_ROOT として渡す SDK のルート。
        /// @note 空でなければ cmake --build の代わりに完全なコマンドラインを実行する。
        /// @note HLSL ホットリロードは CMake 生成状態に依存せず、統合 bat を直接起動する必要がある。
        std::wstring          commandLine; ///< 直接起動するコマンドライン。空なら CMake のビルドコマンドを組み立てる。
        std::filesystem::path workingDirectory; ///< 子プロセスの作業ディレクトリ。未指定なら既定値を使う。
        /// @note ホットリロード時はエディタプロセスがエンジン DLL をロック中のため、依存ターゲットの再ビルドをスキップしないとリンカが失敗する。
        bool                  skipDeps = false; ///< 稼働中の Editor が参照している依存先を作り直さない設定。
        bool                  rebuild = false; ///< 既存成果物を使わず対象を再ビルドするか。
    };

    Compiler() = default;
    ~Compiler();

    Compiler(const Compiler&)            = delete;
    Compiler& operator=(const Compiler&) = delete;

    /// @return 実行中または子プロセス群を安全に所有できない場合は false。
    /// @note 子を停止状態で作り、Job 登録後にだけ実行を開始する。
    [[nodiscard]] bool Start(const Config& config);

    /// @note 親コマンド終了後は残存 worker の回収をフレームごとに確認し、完了まで Building を保つ。
    void Tick();

    /// @note 自分が開始した子と子孫だけを終了し、回収完了後に Cancelled へ遷移する。
    /// @see https://learn.microsoft.com/windows/win32/procthread/job-objects Job の所有範囲。
    void Cancel();

    /// @note 実行中は状態を変えない。
    void Reset() {
        if (m_state != State::Building) {
            m_state = State::Idle;
        }
    }

    State              GetState()    const { return m_state; }
    const std::string& GetLog()      const { return m_log; }
    int                GetExitCode() const { return m_exitCode; }

    std::filesystem::path GetOutputExePath() const { return m_config.exePath; }

private:
    void PollOutput();
    /// @note Job 自体の signaled 状態は全子終了を表さないため accounting を確認する。
    bool WaitForJobExit();
    void CloseHandles();

    Config      m_config; ///< 現在または直近に起動した子プロセスの実行設定。
    State       m_state    = State::Idle; ///< 子プロセス群の実行と終了回収の状態。
    std::string m_log; ///< 子プロセスから読み取った標準出力と標準エラーのログ。
    int         m_exitCode = 0; ///< 親ビルドプロセスの終了コード。
    bool        m_cleanupRequested = false; ///< 子孫終了まで待ってからハンドルを閉じるための要求状態。

    void* m_hProcess    = nullptr; ///< 所有する Windows handle。Win32 の宣言は実装だけへ閉じる。
    void* m_hStdoutRead = nullptr; ///< 子プロセス群の標準出力とエラーを読む所有パイプハンドル。
    void* m_hJob        = nullptr; ///< 子プロセス群をまとめて終了・回収するための所有 Job ハンドル。
    void* m_hJobEvents  = nullptr; ///< 子プロセス群の終了通知を受け取る所有完了ポートハンドル。
};

}
