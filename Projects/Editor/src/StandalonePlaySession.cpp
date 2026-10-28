/// @file    StandalonePlaySession.cpp
/// @brief   Handle-based Standalone Play launch, nonblocking Stop and owned-child cleanup.
/// @author  Hasegawa Jin
/// @date    2026-10-23
#include <Editor/StandalonePlaySession.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Util/StringUtils.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <algorithm>
#include <utility>

namespace fbzz::editor {
namespace {

thread_local DWORD s_closingStandalonePid = 0;

std::wstring QuoteArgument(const std::wstring& value)
{
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') { ++backslashes; continue; }
        result.append(backslashes * (character == L'"' ? 2 : 1), L'\\');
        backslashes = 0;
        if (character == L'"') result.push_back(L'\\');
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::string Win32Error(const char* operation)
{
    return std::string(operation) + " failed (Win32 error " + std::to_string(GetLastError()) + ").";
}

class OwnedStandaloneProcess final : public IStandalonePlayProcess {
public:
    ~OwnedStandaloneProcess() override { Close(); }

    bool Start(const StandalonePlayRequest& request, std::string& diagnostic) override
    {
        Close();
        m_job = CreateJobObjectW(nullptr, nullptr);
        if (!m_job) { diagnostic = Win32Error("CreateJobObject"); return false; }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            diagnostic = Win32Error("SetInformationJobObject"); Close(); return false;
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        std::wstring command = StandalonePlaySession::CommandLine(request);
        /// @note Suspend until job assignment so launch failure and Editor death cannot orphan a running game.
        if (!CreateProcessW(request.executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED, nullptr, request.project.c_str(), &startup, &process)) {
            diagnostic = Win32Error("CreateProcess"); Close(); return false;
        }
        m_process = process.hProcess;
        m_pid = process.dwProcessId;
        if (!AssignProcessToJobObject(m_job, m_process) || ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
            diagnostic = Win32Error("Assign/Resume Standalone Play");
            TerminateProcess(m_process, 1);
            CloseHandle(process.hThread);
            Close();
            return false;
        }
        CloseHandle(process.hThread);
        return true;
    }

    PollResult Poll(uint32_t& exitCode, std::string& diagnostic) override
    {
        if (!m_process) { diagnostic = "Standalone Play has no owned process handle."; return PollResult::FAILED; }
        const DWORD wait = WaitForSingleObject(m_process, 0);
        if (wait == WAIT_TIMEOUT) return PollResult::RUNNING;
        if (wait != WAIT_OBJECT_0) { diagnostic = Win32Error("Poll Standalone Play"); return PollResult::FAILED; }
        DWORD code = 0;
        if (!GetExitCodeProcess(m_process, &code)) { diagnostic = Win32Error("Read Standalone exit code"); return PollResult::FAILED; }
        exitCode = code;
        Close();
        return PollResult::EXITED;
    }

    bool RequestStop(std::string& diagnostic) override
    {
        if (!m_process) { diagnostic = "Standalone Play has no owned process handle."; return false; }
        s_closingStandalonePid = m_pid;
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            (void)parameter;
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            if (pid == s_closingStandalonePid) PostMessageW(window, WM_CLOSE, 0, 0);
            return TRUE;
        }, 0);
        s_closingStandalonePid = 0;
        return true;
    }

    bool Terminate(std::string& diagnostic) override
    {
        if (!m_process || WaitForSingleObject(m_process, 0) == WAIT_OBJECT_0) return true;
        if (TerminateProcess(m_process, 1)) return true;
        diagnostic = Win32Error("Terminate owned Standalone Play");
        return false;
    }

private:
    void Close()
    {
        /// @note Closing this private job stops only this launch and its descendants, including on Editor crash.
        if (m_job) { CloseHandle(m_job); m_job = nullptr; }
        if (m_process) { CloseHandle(m_process); m_process = nullptr; }
        m_pid = 0;
    }
    HANDLE m_job = nullptr;
    HANDLE m_process = nullptr;
    DWORD m_pid = 0;
};

} /// @note namespace

StandalonePlaySession::StandalonePlaySession() : StandalonePlaySession(std::make_unique<OwnedStandaloneProcess>()) {}
StandalonePlaySession::StandalonePlaySession(std::unique_ptr<IStandalonePlayProcess> process) : m_process(std::move(process)) {}
StandalonePlaySession::~StandalonePlaySession() { Shutdown(); }

void RegisterStandalonePlayOperators(OperatorRegistry& registry)
{
    EditorOperator operation;
    operation.id = "play.set_execution_mode";
    operation.label = "Play Execution Mode";
    operation.category = "Play";
    operation.desc = "Select Editor Game View or a separate Standalone process for the next Play session.";
    operation.kind = OpKind::Action;
    OpParam mode;
    mode.name = "mode";
    mode.type = OpParamType::String;
    mode.enumValues = { "inEditor", "standalone" };
    operation.params = { mode };
    operation.poll = [](const OpContext& context, const OpArgs&) {
        return (!context.ctx.playMode || context.ctx.playMode->IsInEditor())
            && (!context.ctx.standalonePlay || !context.ctx.standalonePlay->IsActive());
    };
    operation.exec = [](OpContext& context, const OpArgs& arguments) {
        context.ctx.playExecutionMode = arguments.GetString("mode") == "standalone"
            ? PlayExecutionMode::STANDALONE : PlayExecutionMode::IN_EDITOR;
        context.ctx.requestEditorSettingsSave = true;
        return OpResult::Ok();
    };
    registry.Register(std::move(operation));
}

std::wstring StandalonePlaySession::CommandLine(const StandalonePlayRequest& request)
{
    return QuoteArgument(request.executable.wstring()) + L" --standalone --editor-play --project "
        + QuoteArgument(request.project.wstring()) + L" --play-scene " + QuoteArgument(request.scene.wstring())
        + L" --play-log " + QuoteArgument(request.log.wstring());
}

bool StandalonePlaySession::Start(const StandalonePlayRequest& request)
{
    if (m_active) { m_diagnostic = "Standalone Play is already running; stop it before starting another session."; return false; }
    m_failed = false;
    m_exitCode = 0;
    m_log = request.log;
    m_diagnostic.clear();
    std::error_code error;
    const auto isFile = [&error](const std::filesystem::path& path) {
        error.clear(); return std::filesystem::is_regular_file(path, error) && !error;
    };
    if (!request.executable.is_absolute() || !request.project.is_absolute() || !request.scene.is_absolute()
        || !request.log.is_absolute() || !isFile(request.executable) || !isFile(request.project / L".fbzz_proj")
        || !isFile(request.scene)) {
        m_diagnostic = "Standalone Play requires an existing executable, project and saved scene, with absolute paths.";
        m_failed = true; return false;
    }
    std::filesystem::create_directories(request.log.parent_path(), error);
    if (error) { m_diagnostic = "Could not create the Standalone Play log directory: " + error.message(); m_failed = true; return false; }
    if (!m_process || !m_process->Start(request, m_diagnostic)) {
        if (m_diagnostic.empty()) m_diagnostic = "Could not start Standalone Play.";
        m_failed = true; return false;
    }
    m_active = true;
    m_stopRequested = false;
    m_terminationRequested = false;
    m_stopSeconds = 0.0f;
    m_diagnostic = "Standalone Play started. Log: " + util::StringUtils::PathToUtf8(m_log);
    return true;
}

void StandalonePlaySession::Stop()
{
    if (!m_active || m_stopRequested) return;
    m_stopRequested = true;
    m_stopSeconds = 0.0f;
    m_diagnostic = "Stopping owned Standalone Play...";
    if (!m_process->RequestStop(m_diagnostic)) m_failed = true;
}

bool StandalonePlaySession::Tick(float elapsedSeconds)
{
    if (!m_active) return false;
    std::string diagnostic;
    const auto result = m_process->Poll(m_exitCode, diagnostic);
    if (result == IStandalonePlayProcess::PollResult::EXITED) {
        m_active = false;
        m_failed = !m_stopRequested && m_exitCode != 0;
        m_diagnostic = m_stopRequested ? "Standalone Play stopped." :
            (m_failed ? "Standalone Play exited with code " + std::to_string(m_exitCode) + "." : "Standalone Play finished.");
        m_diagnostic += " Log: " + util::StringUtils::PathToUtf8(m_log);
        return true;
    }
    if (m_stopRequested && !m_terminationRequested) {
        m_stopSeconds += (std::max)(0.0f, elapsedSeconds);
        if (m_stopSeconds >= 2.0f) {
            m_terminationRequested = m_process->Terminate(diagnostic);
            if (!m_terminationRequested) m_failed = true;
        }
    }
    if (!diagnostic.empty() && diagnostic != m_diagnostic) {
        m_diagnostic = std::move(diagnostic); m_failed = true; return true;
    }
    return false;
}

void StandalonePlaySession::Shutdown()
{
    if (!m_active) return;
    std::string diagnostic;
    if (!m_process->Terminate(diagnostic)) { m_failed = true; m_diagnostic = std::move(diagnostic); return; }
    m_active = false;
}

} /// @note namespace fbzz::editor
