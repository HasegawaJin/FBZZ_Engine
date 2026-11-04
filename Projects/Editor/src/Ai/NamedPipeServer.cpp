/// @file    NamedPipeServer.cpp
/// @brief   Win32 Named Pipe の待受・接続処理と、I/O 完了を含むワーカーの停止。
/// @author  Hasegawa Jin
/// @date    2026-07-20
#include <Editor/Ai/NamedPipeServer.hpp>

#include <algorithm>
#include <atomic>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace fbzz::editor::ai {
namespace {
constexpr std::size_t MAX_REQUEST_BYTES = 8 * 1024 * 1024;
constexpr DWORD PIPE_BUFFER_BYTES = 64 * 1024;

/// @note 完了前に OVERLAPPED・イベント・転送バッファを破棄しない。
/// @see https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex CancelIoEx remarks
int WaitOverlapped(HANDLE pipe, OVERLAPPED& overlapped, HANDLE stopEvent, DWORD& bytesTransferred)
{
    const HANDLE waits[] = {stopEvent, overlapped.hEvent};
    const DWORD result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (result == WAIT_OBJECT_0 + 1)
        return GetOverlappedResult(pipe, &overlapped, &bytesTransferred, FALSE) ? 0 : -1;
    CancelIoEx(pipe, &overlapped);
    GetOverlappedResult(pipe, &overlapped, &bytesTransferred, TRUE);
    return result == WAIT_OBJECT_0 ? 1 : -1;
}
} /// @note namespace

struct NamedPipeServer::Impl {
    struct Pending {
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::string request;
        std::string response;
        ~Pending() { if (ready) CloseHandle(ready); }
    };

    struct Worker {
        std::thread thread;
        std::atomic<bool> finished{false};
    };

    std::wstring pipeName;
    std::thread listener;
    std::atomic<bool> running{false};
    HANDLE stopEvent = nullptr;
    std::mutex queueMutex;
    std::deque<std::shared_ptr<Pending>> queue;
    /// @note listener だけが変更し、Stop は listener の join 後に全 worker を join する。
    std::vector<std::unique_ptr<Worker>> workers;

    bool ReadRequestLine(HANDLE pipe, std::string& out)
    {
        std::vector<char> chunk(PIPE_BUFFER_BYTES);
        while (running.load()) {
            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!overlapped.hEvent) return false;
            DWORD bytesRead = 0;
            const BOOL ok = ReadFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &bytesRead, &overlapped);
            int status = 0;
            if (!ok)
                status = GetLastError() == ERROR_IO_PENDING ? WaitOverlapped(pipe, overlapped, stopEvent, bytesRead) : -1;
            CloseHandle(overlapped.hEvent);
            if (status != 0 || bytesRead == 0) return false;
            for (DWORD index = 0; index < bytesRead; ++index) {
                if (chunk[index] == '\n') return true;
                out.push_back(chunk[index]);
                if (out.size() > MAX_REQUEST_BYTES) return false;
            }
        }
        return false;
    }

    bool WriteResponseLine(HANDLE pipe, const std::string& response)
    {
        std::string line = response;
        line.push_back('\n');
        std::size_t written = 0;
        while (running.load() && written < line.size()) {
            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!overlapped.hEvent) return false;
            DWORD bytesWritten = 0;
            const DWORD count = static_cast<DWORD>(std::min<std::size_t>(line.size() - written, PIPE_BUFFER_BYTES));
            const BOOL ok = WriteFile(pipe, line.data() + written, count, &bytesWritten, &overlapped);
            int status = 0;
            if (!ok)
                status = GetLastError() == ERROR_IO_PENDING ? WaitOverlapped(pipe, overlapped, stopEvent, bytesWritten) : -1;
            CloseHandle(overlapped.hEvent);
            if (status != 0 || bytesWritten == 0) return false;
            written += bytesWritten;
        }
        return written == line.size();
    }

    /// @note NDJSON クライアントは応答を読んで切断する。停止不能な FlushFileBuffers を使わず切断を待つ。
    /// @see https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-operations Named Pipe Operations
    void WaitForDisconnect(HANDLE pipe)
    {
        char ignored[256];
        while (running.load()) {
            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!overlapped.hEvent) return;
            DWORD bytesRead = 0;
            const BOOL ok = ReadFile(pipe, ignored, sizeof(ignored), &bytesRead, &overlapped);
            int status = 0;
            if (!ok)
                status = GetLastError() == ERROR_IO_PENDING ? WaitOverlapped(pipe, overlapped, stopEvent, bytesRead) : -1;
            CloseHandle(overlapped.hEvent);
            if (status != 0 || bytesRead == 0) return;
        }
    }

    std::string DispatchOnMainThread(const std::string& request)
    {
        auto pending = std::make_shared<Pending>();
        if (!pending->ready) return {};
        pending->request = request;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (!running.load()) return {};
            queue.push_back(pending);
        }
        /// @note handler 内の Stop も待ちを解除する。Pending は handler と worker の両方が使い終わるまで生存する。
        const HANDLE waits[] = {stopEvent, pending->ready};
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return {};
        return pending->response;
    }

    void HandleConnection(HANDLE pipe)
    {
        std::string request;
        if (ReadRequestLine(pipe, request)) {
            const std::string response = DispatchOnMainThread(request);
            if (!response.empty() && WriteResponseLine(pipe, response)) WaitForDisconnect(pipe);
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    void ReapWorkers()
    {
        std::erase_if(workers, [](const auto& worker) {
            if (!worker->finished.load(std::memory_order_acquire)) return false;
            worker->thread.join();
            return true;
        });
    }

    void ListenerLoop(std::promise<bool> started)
    {
        bool published = false;
        while (running.load()) {
            ReapWorkers();
            HANDLE pipe = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
                PIPE_BUFFER_BYTES, PIPE_BUFFER_BYTES, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) break;
            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!overlapped.hEvent) { CloseHandle(pipe); break; }
            if (!published) { started.set_value(true); published = true; }

            bool connected = ConnectNamedPipe(pipe, &overlapped) != FALSE;
            if (!connected) {
                const DWORD error = GetLastError();
                if (error == ERROR_PIPE_CONNECTED) connected = true;
                else if (error == ERROR_IO_PENDING) {
                    DWORD ignored = 0;
                    connected = WaitOverlapped(pipe, overlapped, stopEvent, ignored) == 0;
                }
            }
            CloseHandle(overlapped.hEvent);
            if (!running.load()) { CloseHandle(pipe); break; }
            if (!connected) { CloseHandle(pipe); continue; }

            auto worker = std::make_unique<Worker>();
            Worker* state = worker.get();
            state->thread = std::thread([this, pipe, state] {
                HandleConnection(pipe);
                state->finished.store(true, std::memory_order_release);
            });
            workers.push_back(std::move(worker));
        }
        if (!published) started.set_value(false);
    }
};

NamedPipeServer::NamedPipeServer() : m_impl(std::make_unique<Impl>()) {}
NamedPipeServer::~NamedPipeServer() { Stop(); }

bool NamedPipeServer::Start(const std::wstring& pipeName)
{
    if (m_impl->running.load()) return true;
    m_impl->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_impl->stopEvent) return false;
    m_impl->pipeName = pipeName;
    m_impl->running.store(true);
    std::promise<bool> started;
    auto ready = started.get_future();
    m_impl->listener = std::thread([this, started = std::move(started)]() mutable {
        m_impl->ListenerLoop(std::move(started));
    });
    if (ready.get()) return true;
    Stop();
    return false;
}

void NamedPipeServer::Stop()
{
    if (!m_impl || !m_impl->running.exchange(false)) return;
    SetEvent(m_impl->stopEvent);
    {
        std::lock_guard<std::mutex> lock(m_impl->queueMutex);
        m_impl->queue.clear();
    }
    if (m_impl->listener.joinable()) m_impl->listener.join();
    for (const auto& worker : m_impl->workers) worker->thread.join();
    m_impl->workers.clear();
    CloseHandle(m_impl->stopEvent);
    m_impl->stopEvent = nullptr;
}

bool NamedPipeServer::IsRunning() const
{
    return m_impl && m_impl->running.load();
}

void NamedPipeServer::DrainRequests(const RequestHandler& handler)
{
    std::deque<std::shared_ptr<Impl::Pending>> pendingList;
    {
        std::lock_guard<std::mutex> lock(m_impl->queueMutex);
        pendingList.swap(m_impl->queue);
    }
    for (auto& pending : pendingList) {
        if (!m_impl->running.load()) break;
        pending->response = handler ? handler(pending->request) : std::string{};
        SetEvent(pending->ready);
    }
}

} /// @note namespace fbzz::editor::ai
