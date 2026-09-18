/// @file    AssetFileWatcher.hpp
/// @brief   Assets/ ディレクトリの変化をポーリングで検出するファイルシステム監視。
/// @author  Hasegawa Jin
/// @date    2026-06-06
///
/// @note ReadDirectoryChangesW の非同期オーバーラップモードで std::thread なしに変化通知を取得する (シングルスレッド方針に準拠)。
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
        std::string path;     ///< rootPath からの相対パス ('/' 区切り)
        std::string oldPath;  ///< Renamed 時のみ有効
    };

    AssetFileWatcher()  = default;
    ~AssetFileWatcher() { Stop(); }

    AssetFileWatcher(const AssetFileWatcher&)            = delete;
    AssetFileWatcher& operator=(const AssetFileWatcher&) = delete;

    /// rootPath 以下を再帰的に監視する。既に起動中なら Stop してから再起動する。
    /// @return 成功なら true
    bool Start(const std::string& rootPath);

    void Stop();

    /// 溜まったイベントを返して内部キューをクリアする。毎フレーム呼ぶ。
    /// Start() していない場合は空を返す。
    std::vector<FileEvent> Poll();

    [[nodiscard]] bool        IsRunning()  const { return m_handle != INVALID_HANDLE_VALUE; }
    [[nodiscard]] std::string GetRootPath() const { return m_rootPath; }

    /// 通知バッファが溢れてイベントを取りこぼしたか。true なら 1 回だけ返して印を消す。
    /// @note オーバーフロー時は個々のイベントで追従できないため、呼び出し側は一覧とキャッシュを丸ごと作り直す。
    [[nodiscard]] bool ConsumeOverflow() { const bool o = m_overflowed; m_overflowed = false; return o; }

private:
    /// 次の ReadDirectoryChangesW を発行する
    void IssueNextRead();

    /// 通知バッファを解析してイベントキューに積む
    void ParseBuffer(DWORD bytesTransferred);

    HANDLE     m_handle    = INVALID_HANDLE_VALUE;
    OVERLAPPED m_overlapped{};
    std::string m_rootPath;

    /// @note ReadDirectoryChangesW の通知バッファは DWORD アライメント必須。可変長の FILE_NOTIFY_INFORMATION 用に十分なサイズを確保する。
    alignas(DWORD) uint8_t m_buffer[65536]{};

    std::vector<FileEvent> m_queue;
    /// FILE_ACTION_RENAMED_OLD_NAME と NEW_NAME が別の通知バッファへ分割されても
    /// ペアを失わないため、ParseBuffer をまたいで一時保持する。
    std::string m_pendingRenameOld;
    bool m_readPending = false;
    bool m_overflowed  = false;
};

} // namespace fbzz::editor
