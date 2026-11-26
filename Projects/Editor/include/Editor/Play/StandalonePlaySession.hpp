/// @file    StandalonePlaySession.hpp
/// @brief   Game View のレイアウトから独立した別プロセスの Play セッションを管理する。
/// @author  Hasegawa Jin
/// @date    2026-10-23
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace fbzz::editor {

class OperatorRegistry;
void RegisterStandalonePlayOperators(OperatorRegistry& registry);

enum class PlayExecutionMode { IN_EDITOR, STANDALONE };

struct StandalonePlayRequest {
    std::filesystem::path executable; ///< 起動する Editor ランチャーの絶対パス。
    std::filesystem::path project; ///< 子プロセスが開くプロジェクトルートの絶対パス。
    std::filesystem::path scene; ///< 子プロセスがディスクから開くシーンの絶対パス。
    std::filesystem::path log; ///< 起動診断と実行中のログを書き込む絶対パス。
    bool allowPhysicalInput = true; ///< 実機入力とウィンドウのアクティブ化を許可する。自動 Play では false を渡す。
};

/// @note Start failure must leave no child running. Every operation addresses only that Start's retained process handle.
class IStandalonePlayProcess {
public:
    enum class PollResult { RUNNING, EXITED, FAILED };
    virtual ~IStandalonePlayProcess() = default;
    virtual bool Start(const StandalonePlayRequest& request, std::string& diagnostic) = 0;
    virtual PollResult Poll(uint32_t& exitCode, std::string& diagnostic) = 0;
    virtual bool RequestStop(std::string& diagnostic) = 0;
    virtual bool Terminate(std::string& diagnostic) = 0;
};

/// @note Main-thread service. Stop polls without waiting; destruction terminates only the owned child.
class StandalonePlaySession final {
public:
    StandalonePlaySession();
    explicit StandalonePlaySession(std::unique_ptr<IStandalonePlayProcess> process);
    ~StandalonePlaySession();
    StandalonePlaySession(const StandalonePlaySession&) = delete;
    StandalonePlaySession& operator=(const StandalonePlaySession&) = delete;

    bool Start(const StandalonePlayRequest& request);
    void Stop();
    /// @return True when a running/stopping session finishes or reports a new process error.
    bool Tick(float elapsedSeconds);
    void Shutdown();
    [[nodiscard]] bool IsActive() const { return m_active; }
    [[nodiscard]] bool IsStopping() const { return m_stopRequested && m_active; }
    [[nodiscard]] bool Failed() const { return m_failed; }
    [[nodiscard]] uint32_t ExitCode() const { return m_exitCode; }
    [[nodiscard]] const std::string& Diagnostic() const { return m_diagnostic; }
    [[nodiscard]] const std::filesystem::path& LogPath() const { return m_log; }
    /// @note Uses the Windows argv quoting rules, including embedded quotes and trailing backslashes.
    [[nodiscard]] static std::wstring CommandLine(const StandalonePlayRequest& request);

private:
    std::unique_ptr<IStandalonePlayProcess> m_process; ///< このセッションの子プロセスを制御する所有インターフェース。
    std::filesystem::path m_log; ///< 現在のセッションが出力するログのパス。
    std::string m_diagnostic; ///< 起動、停止、異常終了について直近に発生した診断。
    uint32_t m_exitCode = 0; ///< 終了を確認した子プロセスの終了コード。
    float m_stopSeconds = 0.0f; ///< 停止を要求してからの経過時間 [s]。
    bool m_active = false; ///< 子プロセスが起動済みで終了をまだ確認していないか。
    bool m_stopRequested = false; ///< 子プロセスに通常の停止を要求したか。
    bool m_terminationRequested = false; ///< 停止待ちの制限時間を過ぎ、強制終了を要求したか。
    bool m_failed = false; ///< 現在のセッションでプロセス操作や実行が失敗したか。
};

} // namespace fbzz::editor
