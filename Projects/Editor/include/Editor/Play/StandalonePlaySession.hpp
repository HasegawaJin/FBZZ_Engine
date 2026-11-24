/// @file    StandalonePlaySession.hpp
/// @brief   Owns one separate-process Play session independently of the Game View layout.
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
    std::filesystem::path executable;
    std::filesystem::path project;
    std::filesystem::path scene;
    std::filesystem::path log;
    /// @note Automated Play must not activate its window or acquire physical input; manual Play keeps the default.
    bool allowPhysicalInput = true;
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
    std::unique_ptr<IStandalonePlayProcess> m_process;
    std::filesystem::path m_log;
    std::string m_diagnostic;
    uint32_t m_exitCode = 0;
    float m_stopSeconds = 0.0f;
    bool m_active = false;
    bool m_stopRequested = false;
    bool m_terminationRequested = false;
    bool m_failed = false;
};

} /// @note namespace fbzz::editor
