// FBZZ Engine
// NamedPipeServer.cpp | fbzz::editor::ai
// Win32 Named Pipe の待受・接続処理。オーバーラップド IO + stopEvent でクリーンに停止する。
#include <Editor/Ai/NamedPipeServer.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace fbzz::editor::ai {

namespace {
// 1接続あたりの要求サイズ上限。base64 PNG は応答側なので要求は小さいが、暴走防止に上限を設ける。
constexpr std::size_t kMaxRequestBytes = 8 * 1024 * 1024;
constexpr DWORD       kPipeBufferBytes = 64 * 1024;
} // namespace

struct NamedPipeServer::Impl {
    // メインスレッドの応答を待つ1要求分の受け渡し箱。
    struct Pending {
        std::string               request;
        std::promise<std::string> response;
    };

    std::wstring pipeName;

    std::thread              listener;
    std::atomic<bool>        running{ false };
    HANDLE                   stopEvent = nullptr; // listener / ワーカーの待機を一斉解除する

    std::mutex                             queueMutex;
    std::deque<std::shared_ptr<Pending>>   queue;   // 未処理要求 (メインスレッドが drain)

    std::atomic<int>         activeWorkers{ 0 };
    std::mutex               workerMutex;
    std::condition_variable  workerDone;

    // オーバーラップド操作を stopEvent と同時に待つ。戻り: 0=完了, 1=停止要求, -1=失敗。
    int WaitOverlapped(HANDLE pipe, OVERLAPPED& overlapped, DWORD& bytesTransferred)
    {
        const HANDLE waits[2] = { overlapped.hEvent, stopEvent };
        const DWORD  result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) {
            if (!GetOverlappedResult(pipe, &overlapped, &bytesTransferred, FALSE)) return -1;
            return 0;
        }
        if (result == WAIT_OBJECT_0 + 1) {
            CancelIoEx(pipe, &overlapped);
            return 1;
        }
        return -1;
    }

    // 接続済みパイプから NDJSON 1行を読み、メインスレッドで処理させ、応答を書き戻して閉じる。
    void HandleConnection(HANDLE pipe)
    {
        struct WorkerGuard {
            Impl* self;
            ~WorkerGuard()
            {
                if (self->activeWorkers.fetch_sub(1) == 1) {
                    std::lock_guard<std::mutex> lock(self->workerMutex);
                    self->workerDone.notify_all();
                }
            }
        } guard{ this };

        std::string request;
        if (ReadRequestLine(pipe, request)) {
            const std::string response = DispatchOnMainThread(request);
            if (!response.empty()) WriteResponseLine(pipe, response);
        }
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    // '\n' までを読む。改行は含めない。停止要求・エラー・上限超過で false。
    bool ReadRequestLine(HANDLE pipe, std::string& out)
    {
        std::vector<char> chunk(kPipeBufferBytes);
        for (;;) {
            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (overlapped.hEvent == nullptr) return false;

            DWORD bytesRead = 0;
            const BOOL ok = ReadFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &bytesRead, &overlapped);
            int status = 0;
            if (!ok) {
                const DWORD error = GetLastError();
                if (error == ERROR_IO_PENDING) status = WaitOverlapped(pipe, overlapped, bytesRead);
                else status = -1;
            }
            CloseHandle(overlapped.hEvent);
            if (status != 0) return false;      // 停止 or 失敗
            if (bytesRead == 0) return false;   // 相手が切断

            for (DWORD i = 0; i < bytesRead; ++i) {
                const char c = chunk[i];
                if (c == '\n') return true;     // 1行完成 (以降は 1req/1conn 前提で捨てる)
                out.push_back(c);
                if (out.size() > kMaxRequestBytes) return false;
            }
        }
    }

    void WriteResponseLine(HANDLE pipe, const std::string& response)
    {
        std::string line = response;
        line.push_back('\n');
        std::size_t written = 0;
        while (written < line.size()) {
            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (overlapped.hEvent == nullptr) return;

            DWORD chunkWritten = 0;
            const BOOL ok = WriteFile(pipe, line.data() + written,
                                      static_cast<DWORD>(line.size() - written), &chunkWritten, &overlapped);
            int status = 0;
            if (!ok) {
                const DWORD error = GetLastError();
                if (error == ERROR_IO_PENDING) status = WaitOverlapped(pipe, overlapped, chunkWritten);
                else status = -1;
            }
            CloseHandle(overlapped.hEvent);
            if (status != 0 || chunkWritten == 0) return;
            written += chunkWritten;
        }
    }

    // 要求をキューへ積み、メインスレッド (DrainRequests) が set_value するまで待つ。停止時は空応答。
    std::string DispatchOnMainThread(const std::string& request)
    {
        auto pending = std::make_shared<Pending>();
        pending->request = request;
        std::future<std::string> future = pending->response.get_future();
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (!running.load()) return {};
            queue.push_back(pending);
        }
        // メインスレッドが毎フレーム drain する。停止時は Stop() が空文字で解除する。
        future.wait();
        return future.get();
    }

    void ListenerLoop()
    {
        while (running.load()) {
            HANDLE pipe = CreateNamedPipeW(
                pipeName.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                kPipeBufferBytes, kPipeBufferBytes, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) break;

            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (overlapped.hEvent == nullptr) { CloseHandle(pipe); break; }

            bool connected = false;
            const BOOL ok = ConnectNamedPipe(pipe, &overlapped);
            if (ok) {
                connected = true; // 稀: 同期完了
            } else {
                const DWORD error = GetLastError();
                if (error == ERROR_PIPE_CONNECTED) {
                    connected = true; // Connect 前に相手が既に接続していた
                } else if (error == ERROR_IO_PENDING) {
                    DWORD ignored = 0;
                    connected = (WaitOverlapped(pipe, overlapped, ignored) == 0);
                }
            }
            CloseHandle(overlapped.hEvent);

            if (!running.load()) { CloseHandle(pipe); break; }
            if (!connected)      { CloseHandle(pipe); continue; }

            // 接続毎に短命ワーカーを起こす。detach し、停止時は activeWorkers==0 まで待って安全に解放する。
            activeWorkers.fetch_add(1);
            std::thread([this, pipe] { HandleConnection(pipe); }).detach();
        }
    }
};

NamedPipeServer::NamedPipeServer() : m_impl(std::make_unique<Impl>()) {}

NamedPipeServer::~NamedPipeServer() { Stop(); }

bool NamedPipeServer::Start(const std::wstring& pipeName)
{
    if (m_impl->running.load()) return true;

    m_impl->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); // manual-reset
    if (m_impl->stopEvent == nullptr) return false;

    m_impl->pipeName = pipeName;
    m_impl->running.store(true);
    m_impl->listener = std::thread([this] { m_impl->ListenerLoop(); });
    return true;
}

void NamedPipeServer::Stop()
{
    if (!m_impl || !m_impl->running.exchange(false)) {
        // 既に停止済みでも stopEvent の後始末だけは行う。
        if (m_impl && m_impl->stopEvent) { CloseHandle(m_impl->stopEvent); m_impl->stopEvent = nullptr; }
        return;
    }

    if (m_impl->stopEvent) SetEvent(m_impl->stopEvent);

    // 待機中ワーカーを空応答で解除し、キューを空にする。
    {
        std::lock_guard<std::mutex> lock(m_impl->queueMutex);
        for (auto& pending : m_impl->queue) pending->response.set_value(std::string{});
        m_impl->queue.clear();
    }

    if (m_impl->listener.joinable()) m_impl->listener.join();

    // detach 済みワーカーが Impl を触り終える (activeWorkers==0) まで待つ。
    {
        std::unique_lock<std::mutex> lock(m_impl->workerMutex);
        m_impl->workerDone.wait(lock, [this] { return m_impl->activeWorkers.load() == 0; });
    }

    if (m_impl->stopEvent) { CloseHandle(m_impl->stopEvent); m_impl->stopEvent = nullptr; }
}

bool NamedPipeServer::IsRunning() const
{
    return m_impl && m_impl->running.load();
}

void NamedPipeServer::DrainRequests(const RequestHandler& handler)
{
    // キューをローカルへ取り出してからロック外で処理する (handler 実行中に IO スレッドを止めない)。
    std::deque<std::shared_ptr<Impl::Pending>> pendingList;
    {
        std::lock_guard<std::mutex> lock(m_impl->queueMutex);
        pendingList.swap(m_impl->queue);
    }
    for (auto& pending : pendingList) {
        std::string response = handler ? handler(pending->request) : std::string{};
        pending->response.set_value(std::move(response));
    }
}

} // namespace fbzz::editor::ai
