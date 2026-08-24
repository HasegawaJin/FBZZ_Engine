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

    // 通知バッファが溢れてイベントを取りこぼしたか。true なら 1 回だけ返して印を消す。
    // WHY: 大量のファイルを一度に入れると OS 側の通知バッファが溢れ、その回の変更が
    //      「全部」捨てられる。個々のイベントで追従する仕組みは全て空振りするので、
    //      呼び出し側は一覧とキャッシュを丸ごと作り直す必要がある。
    [[nodiscard]] bool ConsumeOverflow() { const bool o = m_overflowed; m_overflowed = false; return o; }

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
    // FILE_ACTION_RENAMED_OLD_NAME と NEW_NAME が別の通知バッファへ分割されても
    // ペアを失わないため、ParseBuffer をまたいで一時保持する。
    std::string m_pendingRenameOld;
    bool m_readPending = false;
    bool m_overflowed  = false;
};

} // namespace fbzz::editor
