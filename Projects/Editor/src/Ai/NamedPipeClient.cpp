/// @file    NamedPipeClient.cpp
/// @brief   同期Named PipeクライアントのWin32実装。
/// @author  Hasegawa Jin
/// @date    2026-07-20
#include <Editor/Ai/NamedPipeClient.hpp>

#include <Windows.h>
#include <algorithm>
#include <array>

namespace fbzz::editor::ai {

namespace {

constexpr std::size_t MAX_RESPONSE_BYTES = 64u * 1024u * 1024u;

// 部分書き込みを吸収し、要求行を最後まで送信する。
bool WriteAll(HANDLE pipe, const std::string& bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>((std::min<std::size_t>)(
            bytes.size() - offset, static_cast<std::size_t>(MAXDWORD)));
        if (!WriteFile(pipe, bytes.data() + offset, chunk, &written, nullptr) || written == 0)
            return false;
        offset += written;
    }
    return true;
}

} // namespace

bool NamedPipeClient::Request(const std::wstring& pipeName,
                              const std::string& request,
                              std::string& response,
                              std::uint32_t timeoutMs)
{
    response.clear();
    if (!WaitNamedPipeW(pipeName.c_str(), timeoutMs)) return false;

    const HANDLE pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return false;

    std::string line = request;
    line.push_back('\n');
    if (!WriteAll(pipe, line)) {
        CloseHandle(pipe);
        return false;
    }

    std::array<char, 64 * 1024> buffer{};
    bool complete = false;
    while (response.size() <= MAX_RESPONSE_BYTES) {
        DWORD read = 0;
        if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)
            || read == 0)
            break;
        for (DWORD index = 0; index < read; ++index) {
            if (buffer[index] == '\n') {
                complete = true;
                break;
            }
            response.push_back(buffer[index]);
            if (response.size() > MAX_RESPONSE_BYTES) break;
        }
        if (complete) break;
    }
    CloseHandle(pipe);
    if (!complete || response.size() > MAX_RESPONSE_BYTES) {
        response.clear();
        return false;
    }
    return true;
}

} // namespace fbzz::editor::ai
