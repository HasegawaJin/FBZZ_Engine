// FBZZ Engine
// Compiler.cpp | fbzz::editor
// RuntimeBuild 用の CMake 子プロセス管理
#include <Editor/Compiler.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Windows.h>

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

    // WHAT: stderr も stdout と同じパイプへ流し、ビルドログを UI で一括表示する。
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE stdoutWrite = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&m_hStdoutRead, &stdoutWrite, &sa, 0))
        return false;
    SetHandleInformation(m_hStdoutRead, HANDLE_FLAG_INHERIT, 0);

    std::wstring command =
        L"\"" + config.cmakeExe.wstring() + L"\""
        L" --build \"" + config.buildDir.wstring() + L"\""
        L" --target " + util::StringUtils::ToWide(config.target) +
        L" --config " + util::StringUtils::ToWide(config.configuration) +
        L" --parallel";  // MSBuild: /m — 全 CPU コアで並列コンパイル

    if (config.skipDeps)
        command += L" -- /p:BuildProjectReferences=false /p:DebugSymbols=false /p:TrackFileAccess=false";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = stdoutWrite;
    si.hStdError  = stdoutWrite;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(stdoutWrite);

    if (!ok) {
        CloseHandles();
        return false;
    }

    m_hProcess = pi.hProcess;
    CloseHandle(pi.hThread);
    m_state = State::Building;
    return true;
}

void Compiler::Tick()
{
    if (m_state != State::Building) return;

    PollOutput();

    if (WaitForSingleObject(m_hProcess, 0) == WAIT_OBJECT_0) {
        PollOutput();

        DWORD exitCode = 0;
        GetExitCodeProcess(m_hProcess, &exitCode);
        m_exitCode = static_cast<int>(exitCode);
        m_state = (exitCode == 0) ? State::Done : State::Failed;

        CloseHandles();
    }
}

void Compiler::Cancel()
{
    if (m_state != State::Building) return;

    TerminateProcess(m_hProcess, 1);
    WaitForSingleObject(m_hProcess, INFINITE);
    PollOutput();
    CloseHandles();
    m_state = State::Cancelled;
}

void Compiler::PollOutput()
{
    if (m_hStdoutRead == INVALID_HANDLE_VALUE) return;

    DWORD available = 0;
    while (PeekNamedPipe(m_hStdoutRead, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
        std::string buffer(available, '\0');
        DWORD read = 0;
        if (!ReadFile(m_hStdoutRead, buffer.data(), available, &read, nullptr) || read == 0)
            break;
        m_log.append(buffer.data(), read);
    }
}

void Compiler::CloseHandles()
{
    if (m_hProcess != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hProcess);
        m_hProcess = INVALID_HANDLE_VALUE;
    }
    if (m_hStdoutRead != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hStdoutRead);
        m_hStdoutRead = INVALID_HANDLE_VALUE;
    }
}

} // namespace fbzz::editor
