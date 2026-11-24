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
        std::filesystem::path cmakeExe;
        std::filesystem::path buildDir;
        std::filesystem::path exePath;
        std::string           target;
        std::string           configuration;
        /// @note CMake 自動再生成時にも FBZZ_SDK_ROOT を継承する。
        std::string           sdkRoot;
        /// @note 空でなければ cmake --build の代わりに完全なコマンドラインを実行する。
        /// @note HLSL ホットリロードは CMake 生成状態に依存せず、統合 bat を直接起動する必要がある。
        std::wstring          commandLine;
        std::filesystem::path workingDirectory;
        /// @note ホットリロード時はエディタプロセスがエンジン DLL をロック中のため、依存ターゲットの再ビルドをスキップしないとリンカが失敗する。
        bool                  skipDeps = false;
        bool                  rebuild = false;
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
    void Reset() { if (m_state != State::Building) m_state = State::Idle; }

    State              GetState()    const { return m_state; }
    const std::string& GetLog()      const { return m_log; }
    int                GetExitCode() const { return m_exitCode; }

    std::filesystem::path GetOutputExePath() const { return m_config.exePath; }

private:
    void PollOutput();
    /// @note Job 自体の signaled 状態は全子終了を表さないため accounting を確認する。
    bool WaitForJobExit();
    void CloseHandles();

    Config      m_config;
    State       m_state    = State::Idle;
    std::string m_log;
    int         m_exitCode = 0;
    bool        m_cleanupRequested = false;

    /// @note 所有する Windows handle。Win32 の宣言は実装だけへ閉じる。
    void* m_hProcess    = nullptr;
    void* m_hStdoutRead = nullptr;
    void* m_hJob        = nullptr;
    void* m_hJobEvents  = nullptr;
};

}
