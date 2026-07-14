// FBZZ Engine
// Compiler.hpp | fbzz::editor
// RuntimeBuild 用の CMake 子プロセス管理
#pragma once

#include <Windows.h>
#include <filesystem>
#include <string>

namespace fbzz::editor {

/// cmake --build を非同期子プロセスとして起動し、stdout / stderr を ImGui 表示用ログへ蓄積する。
/// WHY: エディタのメインスレッドをブロックしないため、Tick() ごとにパイプを短時間だけ読む。
class Compiler {
public:
    enum class State { Idle, Building, Done, Failed, Cancelled };

    struct Config {
        std::filesystem::path cmakeExe;
        std::filesystem::path buildDir;
        std::filesystem::path exePath;
        std::string           target;
        std::string           configuration;
        std::string           sdkRoot; // CMake 自動再生成時にも FBZZ_SDK_ROOT を継承させる
        // WHY: ホットリロード時はエディタプロセスがエンジン DLL をロック中のため、
        //      依存ターゲットの再ビルドをスキップしないとリンカが失敗する。
        bool                  skipDeps = false;
    };

    Compiler() = default;
    ~Compiler();

    Compiler(const Compiler&)            = delete;
    Compiler& operator=(const Compiler&) = delete;

    /// CMake ビルドを開始する。既にビルド中なら false を返す。
    [[nodiscard]] bool Start(const Config& config);

    /// stdout を回収し、子プロセス終了時に Done / Failed へ遷移する。
    void Tick();

    /// 実行中ビルドを強制終了し、Cancelled へ遷移する。
    void Cancel();

    /// Done / Failed / Cancelled の後、次のビルドに備えて Idle へ戻す。
    void Reset() { if (m_state != State::Building) m_state = State::Idle; }

    State              GetState()    const { return m_state; }
    const std::string& GetLog()      const { return m_log; }
    int                GetExitCode() const { return m_exitCode; }

    /// build.config 由来の standalone exe パスを返す。
    std::filesystem::path GetOutputExePath() const { return m_config.exePath; }

private:
    void PollOutput();
    void CloseHandles();

    Config      m_config;
    State       m_state    = State::Idle;
    std::string m_log;
    int         m_exitCode = 0;

    HANDLE m_hProcess    = INVALID_HANDLE_VALUE;
    HANDLE m_hStdoutRead = INVALID_HANDLE_VALUE;
};

} // namespace fbzz::editor
