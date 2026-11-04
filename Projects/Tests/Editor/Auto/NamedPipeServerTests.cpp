/// @file    NamedPipeServerTests.cpp
/// @brief   実 Named Pipe を通した応答・取消し・ワーカー終了・再起動の契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-01
#include <TestKit/TestKit.hpp>
#include <Editor/Ai/NamedPipeServer.hpp>
#include <array>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace fbzz::tests {
namespace {

class NamedPipeServerTest : public testkit::Fixture {
protected:
    editor::ai::NamedPipeServer m_server;
    std::wstring m_name;
    std::vector<HANDLE> m_clients;

    void SetUp() override
    {
        Fixture::SetUp();
        static std::atomic<unsigned> sequence{0};
        m_name = L"\\\\.\\pipe\\FBZZNamedPipeTest-" + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(sequence.fetch_add(1));
        ASSERT_TRUE(m_server.Start(m_name));
    }

    void TearDown() override
    {
        m_server.Stop();
        for (HANDLE client : m_clients) CloseHandle(client);
        Fixture::TearDown();
    }

    HANDLE Connect()
    {
        HANDLE client = CreateFileW(m_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (client == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY) {
            if (WaitNamedPipeW(m_name.c_str(), 5000))
                client = CreateFileW(m_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        }
        if (client != INVALID_HANDLE_VALUE) m_clients.push_back(client);
        return client;
    }

    DWORD Transfer(HANDLE client, char* data, DWORD count, bool write)
    {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return 0;
        DWORD bytes = 0;
        const BOOL complete = write ? WriteFile(client, data, count, &bytes, &overlapped)
            : ReadFile(client, data, count, &bytes, &overlapped);
        if (!complete && GetLastError() == ERROR_IO_PENDING) {
            /// @note timeout は失敗時の後始末用で、スケジュール順の期待値には使わない。
            if (WaitForSingleObject(overlapped.hEvent, 5000) != WAIT_OBJECT_0) {
                CancelIoEx(client, &overlapped);
                GetOverlappedResult(client, &overlapped, &bytes, TRUE);
                bytes = 0;
            } else if (!GetOverlappedResult(client, &overlapped, &bytes, FALSE)) bytes = 0;
        } else if (!complete) bytes = 0;
        CloseHandle(overlapped.hEvent);
        return bytes;
    }

    void Send(HANDLE client, std::string text = "request\n")
    {
        ASSERT_EQ(Transfer(client, text.data(), static_cast<DWORD>(text.size()), true), text.size());
    }

    void Pump(std::size_t count, const editor::ai::NamedPipeServer::RequestHandler& handler)
    {
        std::size_t handled = 0;
        while (handled < count) {
            m_server.DrainRequests([&](const std::string& request) {
                ++handled;
                return handler(request);
            });
            std::this_thread::yield();
        }
    }

    std::string ReadLine(HANDLE client)
    {
        std::string line;
        std::array<char, 4096> bytes;
        for (;;) {
            const DWORD count = Transfer(client, bytes.data(), static_cast<DWORD>(bytes.size()), false);
            if (!count) return line;
            line.append(bytes.data(), count);
            if (line.back() == '\n') return line;
        }
    }
};

TEST_F(NamedPipeServerTest, DeliversTheWholeResponseBeforeClientDisconnect)
{
    const HANDLE client = Connect();
    ASSERT_NE(client, INVALID_HANDLE_VALUE);
    Send(client);
    const std::string response(256 * 1024, 'x');
    Pump(1, [&](const auto& request) { EXPECT_EQ(request, "request"); return response; });
    EXPECT_EQ(ReadLine(client), response + "\n");
}

TEST_F(NamedPipeServerTest, StopCancelsPartialRequestAndJoinsBeforeRestart)
{
    for (int index = 0; index < 8; ++index) {
        const HANDLE client = Connect();
        ASSERT_NE(client, INVALID_HANDLE_VALUE);
        Send(client, "partial");
        m_server.Stop();
        EXPECT_FALSE(m_server.IsRunning());
        m_server.Stop();
        ASSERT_TRUE(m_server.Start(m_name));
    }
    const HANDLE client = Connect();
    ASSERT_NE(client, INVALID_HANDLE_VALUE);
    Send(client);
    Pump(1, [](const auto&) { return std::string("after-restart"); });
    EXPECT_EQ(ReadLine(client), "after-restart\n");
}

TEST_F(NamedPipeServerTest, StopUnblocksAResponseLargerThanAnUnreadPipeBuffer)
{
    const HANDLE client = Connect();
    ASSERT_NE(client, INVALID_HANDLE_VALUE);
    Send(client);
    Pump(1, [](const auto&) { return std::string(2 * 1024 * 1024, 'x'); });
    DWORD available = 0;
    while (available == 0) {
        ASSERT_TRUE(PeekNamedPipe(client, nullptr, 0, nullptr, &available, nullptr));
        std::this_thread::yield();
    }
    m_server.Stop();
    EXPECT_FALSE(m_server.IsRunning());
    ASSERT_TRUE(m_server.Start(m_name));
}

TEST_F(NamedPipeServerTest, HandlerCanStopWithoutWaitingForItsOwnResponse)
{
    const HANDLE client = Connect();
    ASSERT_NE(client, INVALID_HANDLE_VALUE);
    Send(client);
    Pump(1, [&](const auto&) { m_server.Stop(); return std::string("ignored"); });
    EXPECT_FALSE(m_server.IsRunning());
    ASSERT_TRUE(m_server.Start(m_name));
}

TEST_F(NamedPipeServerTest, StopJoinsConcurrentConnectionsWaitingForMainThread)
{
    for (int index = 0; index < 8; ++index) {
        const HANDLE client = Connect();
        ASSERT_NE(client, INVALID_HANDLE_VALUE);
        Send(client);
    }
    m_server.Stop();
    EXPECT_FALSE(m_server.IsRunning());
    std::size_t handled = 0;
    m_server.DrainRequests([&](const auto&) { ++handled; return std::string{}; });
    EXPECT_EQ(handled, 0u);
}

TEST_F(NamedPipeServerTest, DestructionStopsAConnectedClientWaitingForMoreInput)
{
    const std::wstring name = m_name + L"-destructor";
    {
        editor::ai::NamedPipeServer server;
        ASSERT_TRUE(server.Start(name));
        const HANDLE client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        ASSERT_NE(client, INVALID_HANDLE_VALUE);
        m_clients.push_back(client);
        Send(client, "partial");
    }
    editor::ai::NamedPipeServer replacement;
    ASSERT_TRUE(replacement.Start(name));
}

} /// @note namespace
} /// @note namespace fbzz::tests
