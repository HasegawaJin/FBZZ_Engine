/// @file    Compiler.cpp
/// @brief   RuntimeBuild 用の CMake 子プロセス管理。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Editor/Compiler.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <string>

namespace fbzz::editor {

Compiler::~Compiler()
{
    if (m_state == State::Building)
        Cancel();
    CloseHandles();
}

bool Compiler::Start(const Config& config)
{
    if (m_state == State::Building) return false;

    CloseHandles();
    m_config   = config;
    m_log.clear();
    m_exitCode = 0;
    m_cleanupRequested = false;

    const auto fail = [this](const char* operation, DWORD error) {
        m_log += "ERROR Compiler " + std::string(operation) + " failed (Windows error " + std::to_string(error) + ")\n";
        m_exitCode = static_cast<int>(error);
        m_state = State::Failed;
        CloseHandles();
        return false;
    };

    /// @note 無名・非継承の Job を使い、Editor が起動していない外部のビルドへ触れない。
    /// @see https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-jobobject_basic_limit_information JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE。
    m_hJob = CreateJobObjectW(nullptr, nullptr);
    if (!m_hJob) return fail("CreateJobObject", GetLastError());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(m_hJob, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return fail("SetInformationJobObject", GetLastError());
    m_hJobEvents = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (!m_hJobEvents) return fail("CreateIoCompletionPort", GetLastError());
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT events{};
    events.CompletionPort = m_hJobEvents;
    if (!SetInformationJobObject(m_hJob, JobObjectAssociateCompletionPortInformation, &events, sizeof(events)))
        return fail("SetJobCompletionPort", GetLastError());

    /// @note stderr も stdout と同じパイプへ流し、ビルドログを UI で一括表示する。
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE stdoutWrite = nullptr;
    if (!CreatePipe(&m_hStdoutRead, &stdoutWrite, &sa, 0))
        return fail("CreatePipe", GetLastError());
    if (!SetHandleInformation(m_hStdoutRead, HANDLE_FLAG_INHERIT, 0)) {
        const DWORD error = GetLastError();
        CloseHandle(stdoutWrite);
        return fail("SetHandleInformation", error);
    }

    const bool usesExplicitCommand = !config.commandLine.empty();
    std::wstring command = config.commandLine;
    if (!usesExplicitCommand) {
        command =
            L"\"" + config.cmakeExe.wstring() + L"\""
            L" --build \"" + config.buildDir.wstring() + L"\""
            L" --target " + util::StringUtils::ToWide(config.target) +
            L" --config " + util::StringUtils::ToWide(config.configuration) +
            /// @note cl.exe の並列度は /MP が持つため MSBuild のノード並列を一つへ制限する。
            L" --parallel 1";
    }

    if (!usesExplicitCommand && (config.skipDeps || config.rebuild)) {
        command += L" --";
        if (config.rebuild) command += L" /t:Rebuild";
        if (config.skipDeps)
            command += L" /p:BuildProjectReferences=false /p:DebugSymbols=false /p:TrackFileAccess=false";
    }

    /// @note 失敗時に target / configuration / buildDir を UI ログだけで特定できるようにする。
    m_log += "> " + util::StringUtils::ToNarrow(command) + "\n";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = stdoutWrite;
    si.hStdError  = stdoutWrite;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    /// @note 古い GameHub 生成物にも /FS を補い、並列 cl.exe の PDB 書き込み競合を防ぐ。
    std::wstring oldCl;
    const DWORD oldClSize = GetEnvironmentVariableW(L"CL", nullptr, 0);
    if (oldClSize > 0) {
        oldCl.resize(static_cast<size_t>(oldClSize));
        GetEnvironmentVariableW(L"CL", oldCl.data(), oldClSize);
        if (!oldCl.empty() && oldCl.back() == L'\0')
            oldCl.pop_back();
    }
    std::wstring childCl = oldCl;
    if (childCl.find(L"/FS") == std::wstring::npos && childCl.find(L"-FS") == std::wstring::npos) {
        if (!childCl.empty())
            childCl += L" ";
        childCl += L"/FS";
    }
    SetEnvironmentVariableW(L"CL", childCl.c_str());

    std::wstring oldSdkRoot;
    const DWORD oldSdkRootSize = GetEnvironmentVariableW(L"FBZZ_SDK_ROOT", nullptr, 0);
    if (oldSdkRootSize > 0) {
        oldSdkRoot.resize(static_cast<size_t>(oldSdkRootSize));
        GetEnvironmentVariableW(L"FBZZ_SDK_ROOT", oldSdkRoot.data(), oldSdkRootSize);
        if (!oldSdkRoot.empty() && oldSdkRoot.back() == L'\0') oldSdkRoot.pop_back();
    }
    if (!config.sdkRoot.empty()) {
        const std::wstring sdkRoot = util::StringUtils::ToWide(config.sdkRoot);
        SetEnvironmentVariableW(L"FBZZ_SDK_ROOT", sdkRoot.c_str());
    }

    const std::wstring workingDirectory = config.workingDirectory.wstring();
    /// @note 子孫の起動前に Job 登録を済ませ、低い優先度をビルドの子孫へ継承する。
    /// @see https://learn.microsoft.com/windows/win32/api/jobapi2/nf-jobapi2-assignprocesstojobobject suspended process の登録。
    const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                                   workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                                   &si, &pi);
    const DWORD createError = ok ? ERROR_SUCCESS : GetLastError();
    if (oldClSize > 0)
        SetEnvironmentVariableW(L"CL", oldCl.c_str());
    else
        SetEnvironmentVariableW(L"CL", nullptr);
    if (oldSdkRootSize > 0)
        SetEnvironmentVariableW(L"FBZZ_SDK_ROOT", oldSdkRoot.c_str());
    else if (!config.sdkRoot.empty())
        SetEnvironmentVariableW(L"FBZZ_SDK_ROOT", nullptr);

    CloseHandle(stdoutWrite);

    if (!ok) return fail("CreateProcess", createError);

    m_hProcess = pi.hProcess;
    if (!AssignProcessToJobObject(m_hJob, pi.hProcess)) {
        const DWORD error = GetLastError();
        /// @note 未開始の自分の子だけを終了する。Job 登録失敗を外部 process の終了で解決しない。
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hThread);
        return fail("AssignProcessToJobObject", error);
    }
    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        const DWORD error = GetLastError();
        TerminateJobObject(m_hJob, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        WaitForJobExit();
        CloseHandle(pi.hThread);
        return fail("ResumeThread", error);
    }
    CloseHandle(pi.hThread);
    m_state = State::Building;
    return true;
}

void Compiler::Tick()
{
    if (m_state != State::Building) return;

    PollOutput();

    if (!m_cleanupRequested && WaitForSingleObject(m_hProcess, 0) == WAIT_OBJECT_0) {
        PollOutput();

        DWORD exitCode = 0;
        GetExitCodeProcess(m_hProcess, &exitCode);
        m_exitCode = static_cast<int>(exitCode);
        /// @note 正常に戻ったツールが残す自己起動 worker も、次の build の前に回収する。
        const bool requestedCleanup = TerminateJobObject(m_hJob, exitCode) != FALSE;
        if (!requestedCleanup)
            m_log += "ERROR Compiler TerminateJobObject failed (Windows error " + std::to_string(GetLastError()) + ")\n";
        if (!requestedCleanup) {
            if (m_exitCode == 0) m_exitCode = 1;
            CloseHandles();
            m_state = State::Failed;
            return;
        }
        m_cleanupRequested = true;
    }
    if (!m_cleanupRequested) return;

    /// @note 子孫の終了は非同期。UI スレッドで待たず、全子終了を次フレーム以降にも確認する。
    /// @see https://learn.microsoft.com/windows/win32/procthread/terminating-a-process TerminateProcess の非同期終了。
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (!QueryInformationJobObject(m_hJob, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) {
        m_log += "ERROR Compiler QueryJobAccounting failed (Windows error " + std::to_string(GetLastError()) + ")\n";
        if (m_exitCode == 0) m_exitCode = 1;
        CloseHandles();
        m_state = State::Failed;
        return;
    }
    if (accounting.ActiveProcesses != 0) return;

    PollOutput();
    CloseHandles();
    m_state = m_exitCode == 0 ? State::Done : State::Failed;
}

void Compiler::Cancel()
{
    if (m_state != State::Building) return;

    /// @see https://learn.microsoft.com/windows/win32/api/jobapi2/nf-jobapi2-terminatejobobject Job の全子孫だけを終了する。
    TerminateJobObject(m_hJob, 1);
    WaitForSingleObject(m_hProcess, INFINITE);
    WaitForJobExit();
    PollOutput();
    CloseHandles();
    m_state = State::Cancelled;
    m_exitCode = 1;
}

void Compiler::PollOutput()
{
    if (!m_hStdoutRead) return;

    DWORD available = 0;
    while (PeekNamedPipe(m_hStdoutRead, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
        std::string buffer(available, '\0');
        DWORD read = 0;
        if (!ReadFile(m_hStdoutRead, buffer.data(), available, &read, nullptr) || read == 0)
            break;
        m_log.append(buffer.data(), read);
    }
}

bool Compiler::WaitForJobExit()
{
    if (!m_hJob) return true;
    for (;;) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!QueryInformationJobObject(m_hJob, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) {
            m_log += "ERROR Compiler QueryJobAccounting failed (Windows error " + std::to_string(GetLastError()) + ")\n";
            return false;
        }
        if (accounting.ActiveProcesses == 0) return true;
        DWORD message = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* process = nullptr;
        /// @note 通知の欠落でも accounting を再確認する。Job handle の signaled 状態へ依存しない。
        /// @see https://learn.microsoft.com/windows/win32/procthread/job-objects Job Limits and Notifications。
        GetQueuedCompletionStatus(m_hJobEvents, &message, &key, &process, 50);
    }
}

void Compiler::CloseHandles()
{
    if (m_hJob) {
        CloseHandle(m_hJob);
        m_hJob = nullptr;
    }
    if (m_hJobEvents) {
        CloseHandle(m_hJobEvents);
        m_hJobEvents = nullptr;
    }
    if (m_hProcess) {
        CloseHandle(m_hProcess);
        m_hProcess = nullptr;
    }
    if (m_hStdoutRead) {
        CloseHandle(m_hStdoutRead);
        m_hStdoutRead = nullptr;
    }
}

}
