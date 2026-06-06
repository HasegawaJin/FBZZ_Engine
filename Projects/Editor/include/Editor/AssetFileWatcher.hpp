// FBZZ Engine
// AssetFileWatcher.hpp | fbzz::editor
// Assets/ ディレクトリの変化をポーリングで検出するファイルシステム監視
// WHY: ReadDirectoryChangesW の非同期オーバーラップモードを使い、
//      std::thread なしで変化通知を取得する。
//      毎フレーム Poll() を呼ぶだけでイベントを取得できる。
//      AGENTS.md の「Step 1〜5 はシングルスレッド」方針に準拠する。
#pragma once
#include <string>
#include <vector>
#include <Windows.h>

namespace fbzz::editor {

class AssetFileWatcher {
public:
    enum class EventType { Added, Modified, Removed, Renamed };

    struct FileEvent {
        EventType   type;
        std::string path;     // rootPath からの相対パス ('/' 区切り)
        std::string oldPath;  // Renamed 時のみ有効
    };

    AssetFileWatcher()  = default;
    ~AssetFileWatcher() { Stop(); }

    AssetFileWatcher(const AssetFileWatcher&)            = delete;
    AssetFileWatcher& operator=(const AssetFileWatcher&) = delete;

    // rootPath 以下を再帰的に監視する。既に起動中なら Stop してから再起動する。
    // @ret 成功なら true
    bool Start(const std::string& rootPath);

    void Stop();

    // 溜まったイベントを返して内部キューをクリアする。毎フレーム呼ぶ。
    // Start() していない場合は空を返す。
    std::vector<FileEvent> Poll();

    [[nodiscard]] bool        IsRunning()  const { return m_handle != INVALID_HANDLE_VALUE; }
    [[nodiscard]] std::string GetRootPath() const { return m_rootPath; }

private:
    // 次の ReadDirectoryChangesW を発行する
    void IssueNextRead();

    // 通知バッファを解析してイベントキューに積む
    void ParseBuffer(DWORD bytesTransferred);

    HANDLE     m_handle    = INVALID_HANDLE_VALUE;
    OVERLAPPED m_overlapped{};
    std::string m_rootPath;

    // WHY: ReadDirectoryChangesW の通知バッファは DWORD アライメントが必要。
    //      FILE_NOTIFY_INFORMATION は可変長のため十分な大きさを確保する。
    alignas(DWORD) uint8_t m_buffer[65536]{};

    std::vector<FileEvent> m_queue;
    bool m_readPending = false;
};

} // namespace fbzz::editor
