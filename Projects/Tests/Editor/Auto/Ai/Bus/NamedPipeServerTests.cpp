/// @file    NamedPipeServerTests.cpp
/// @brief   実 Named Pipe を通した応答・取消し・ワーカー終了・再起動の契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-01
#include <TestKit/TestKit.hpp>
#include <Editor/Ai/Bus/NamedPipeServer.hpp>
#include <array>
#include <atomic>
#include <chrono>
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

    /// @brief 使用可能なパイプのインスタンスに接続する。
    /// @param[out] client 接続ハンドル。成功時はフィクスチャが閉じ、失敗時は INVALID_HANDLE_VALUE。
    /// @return 接続に成功。失敗時は接続番号と Win32 の診断を返す。待機と再試行は合わせて 5 秒まで。
    ::testing::AssertionResult Connect(HANDLE& client)
    {
        using Clock = std::chrono::steady_clock;
        constexpr auto CONNECT_TIMEOUT = std::chrono::seconds{5};
        const auto deadline = Clock::now() + CONNECT_TIMEOUT;
        const std::size_t connectionNumber = m_clients.size() + 1;
        std::size_t attemptCount = 0;
        DWORD openError = ERROR_SUCCESS;
        DWORD lastWaitError = ERROR_SUCCESS;

        do {
            ++attemptCount;
            client = CreateFileW(m_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (client != INVALID_HANDLE_VALUE) {
                m_clients.push_back(client);
                return ::testing::AssertionSuccess();
            }
            openError = GetLastError();
            if (openError != ERROR_PIPE_BUSY && openError != ERROR_FILE_NOT_FOUND) {
                return ::testing::AssertionFailure() << "Connection #" << connectionNumber
                    << ", attempt " << attemptCount << ": CreateFileW failed (GetLastError=" << openError << ')';
            }

            // 次のインスタンスが開くまで、接続用の残り時間だけ待つ。0 ms は Win32 で既定値を意味する。
            const auto remaining = deadline - Clock::now();
            if (remaining <= Clock::duration::zero()) {
                break;
            }
            const auto waitMilliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
            if (WaitNamedPipeW(m_name.c_str(), static_cast<DWORD>(waitMilliseconds))) {
                continue;
            }
            lastWaitError = GetLastError();
            if (lastWaitError != ERROR_FILE_NOT_FOUND && lastWaitError != ERROR_SEM_TIMEOUT) {
                return ::testing::AssertionFailure() << "Connection #" << connectionNumber
                    << ", attempt " << attemptCount << ": WaitNamedPipeW failed (GetLastError=" << lastWaitError
                    << ", CreateFileW GetLastError=" << openError << ')';
            }

            // 待機が即時に戻る場合も、リスナーが次のインスタンスを作成できるようにする。
            std::this_thread::yield();
        } while (Clock::now() < deadline);

        return ::testing::AssertionFailure() << "Connection #" << connectionNumber
            << " timed out after " << attemptCount << " attempts (last CreateFileW GetLastError=" << openError
            << ", last WaitNamedPipeW failure GetLastError=" << lastWaitError << ')';
    }

    DWORD Transfer(HANDLE client, char* data, DWORD count, bool write)
    {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) {
            return 0;
        }
        DWORD bytes = 0;
        const BOOL complete = write ? WriteFile(client, data, count, &bytes, &overlapped)
            : ReadFile(client, data, count, &bytes, &overlapped);
        if (!complete && GetLastError() == ERROR_IO_PENDING) {
            // timeout は失敗時の後始末用で、スケジュール順の期待値には使わない。
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
            if (!count) {
                return line;
            }
            line.append(bytes.data(), count);
            if (line.back() == '\n') {
                return line;
            }
        }
    }
};

/// @brief Delivers the whole response before client disconnect.
TEST_F(NamedPipeServerTest, DeliversTheWholeResponseBeforeClientDisconnect)
{
    HANDLE client = INVALID_HANDLE_VALUE;
    ASSERT_TRUE(Connect(client));
    Send(client);
    const std::string response(256 * 1024, 'x');
    Pump(1, [&](const auto& request) { EXPECT_EQ(request, "request"); return response; });
    EXPECT_EQ(ReadLine(client), response + "\n");
}

/// @brief Stop cancels partial request and joins before restart.
TEST_F(NamedPipeServerTest, StopCancelsPartialRequestAndJoinsBeforeRestart)
{
    for (int index = 0; index < 8; ++index) {
        HANDLE client = INVALID_HANDLE_VALUE;
        ASSERT_TRUE(Connect(client));
        Send(client, "partial");
        m_server.Stop();
        EXPECT_FALSE(m_server.IsRunning());
        m_server.Stop();
        ASSERT_TRUE(m_server.Start(m_name));
    }
    HANDLE client = INVALID_HANDLE_VALUE;
    ASSERT_TRUE(Connect(client));
    Send(client);
    Pump(1, [](const auto&) { return std::string("after-restart"); });
    EXPECT_EQ(ReadLine(client), "after-restart\n");
}

/// @brief Checks stop unblocks a response larger than an unread pipe buffer.
TEST_F(NamedPipeServerTest, StopUnblocksAResponseLargerThanAnUnreadPipeBuffer)
{
    HANDLE client = INVALID_HANDLE_VALUE;
    ASSERT_TRUE(Connect(client));
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

/// @brief Handler can stop without waiting for its own response.
TEST_F(NamedPipeServerTest, HandlerCanStopWithoutWaitingForItsOwnResponse)
{
    HANDLE client = INVALID_HANDLE_VALUE;
    ASSERT_TRUE(Connect(client));
    Send(client);
    Pump(1, [&](const auto&) { m_server.Stop(); return std::string("ignored"); });
    EXPECT_FALSE(m_server.IsRunning());
    ASSERT_TRUE(m_server.Start(m_name));
}

/// @brief Stop joins concurrent connections waiting for main thread.
TEST_F(NamedPipeServerTest, StopJoinsConcurrentConnectionsWaitingForMainThread)
{
    for (int index = 0; index < 8; ++index) {
        HANDLE client = INVALID_HANDLE_VALUE;
        ASSERT_TRUE(Connect(client));
        Send(client);
    }
    m_server.Stop();
    EXPECT_FALSE(m_server.IsRunning());
    std::size_t handled = 0;
    m_server.DrainRequests([&](const auto&) { ++handled; return std::string{}; });
    EXPECT_EQ(handled, 0u);
}

/// @brief Destruction stops a connected client waiting for more input.
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
