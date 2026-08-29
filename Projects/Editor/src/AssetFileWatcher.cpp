/// @file    AssetFileWatcher.cpp
/// @brief   ReadDirectoryChangesW を使った非同期ポーリング型ファイル監視。
/// @author  Hasegawa Jin
/// @date    2026-06-06
#include <Editor/AssetFileWatcher.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <algorithm>

namespace fbzz::editor {

bool AssetFileWatcher::Start(const std::string& rootPath)
{
    Stop();

    // UTF-8 → wchar_t 変換
    const std::wstring wpath = util::StringUtils::ToWide(rootPath);

    m_handle = CreateFileW(
        wpath.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        // WHY: FILE_FLAG_OVERLAPPED で非同期モードにし、スレッドなしでポーリングできる。
        //      FILE_FLAG_BACKUP_SEMANTICS はディレクトリをハンドルとして開くために必要。
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (m_handle == INVALID_HANDLE_VALUE)
    {
        FBZZ_LOG_ERROR("AssetFileWatcher: Failed to open directory: %s (error %lu)",
                       rootPath.c_str(), GetLastError());
        return false;
    }

    m_overlapped = {};
    m_overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_overlapped.hEvent)
    {
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
        return false;
    }

    m_rootPath = util::FileSystem::NormalizePathSeparators(rootPath);
    if (!m_rootPath.empty() && m_rootPath.back() != '/')
        m_rootPath += '/';

    IssueNextRead();
    return true;
}

void AssetFileWatcher::Stop()
{
    if (m_handle == INVALID_HANDLE_VALUE) return;

    CancelIo(m_handle);
    CloseHandle(m_handle);
    m_handle = INVALID_HANDLE_VALUE;

    if (m_overlapped.hEvent)
    {
        CloseHandle(m_overlapped.hEvent);
        m_overlapped.hEvent = nullptr;
    }

    m_readPending = false;
    m_overflowed  = false;
    m_queue.clear();
    m_pendingRenameOld.clear();
}

std::vector<AssetFileWatcher::FileEvent> AssetFileWatcher::Poll()
{
    if (m_handle == INVALID_HANDLE_VALUE || !m_readPending)
        return {};

    DWORD transferred = 0;
    // bWait = FALSE: 完了していなければ即座に返る
    if (!GetOverlappedResult(m_handle, &m_overlapped, &transferred, FALSE))
    {
        const DWORD err = GetLastError();
        if (err == ERROR_IO_INCOMPLETE)
            return {}; // まだ完了していない

        // バッファオーバーフロー: イベントがあったがバッファが小さすぎた
        if (err == ERROR_NOTIFY_ENUM_DIR)
        {
            FBZZ_LOG_WARN("AssetFileWatcher: notification overflow — some events lost");
            m_overflowed = true;
            ResetEvent(m_overlapped.hEvent);
            m_readPending = false;
            IssueNextRead();
            return {};
        }

        FBZZ_LOG_ERROR("AssetFileWatcher: GetOverlappedResult failed (%lu)", err);
        Stop();
        return {};
    }

    // 成功しても転送量 0 は「溜めきれずバッファを捨てた」合図 (ReadDirectoryChangesW の仕様)。
    if (transferred == 0)
    {
        FBZZ_LOG_WARN("AssetFileWatcher: notification buffer discarded — some events lost");
        m_overflowed = true;
    }

    ParseBuffer(transferred);
    ResetEvent(m_overlapped.hEvent);
    m_readPending = false;
    IssueNextRead();

    std::vector<FileEvent> out;
    out.swap(m_queue);
    return out;
}

void AssetFileWatcher::IssueNextRead()
{
    constexpr DWORD kFilter =
        FILE_NOTIFY_CHANGE_FILE_NAME  |   // 追加・削除・リネーム
        FILE_NOTIFY_CHANGE_DIR_NAME   |
        FILE_NOTIFY_CHANGE_LAST_WRITE;    // 上書き保存

    const BOOL ok = ReadDirectoryChangesW(
        m_handle,
        m_buffer, sizeof(m_buffer),
        TRUE,        // bWatchSubtree: 再帰監視
        kFilter,
        nullptr,     // lpBytesReturned: オーバーラップモードでは使用しない
        &m_overlapped,
        nullptr);    // lpCompletionRoutine: イベントベースで使う

    if (!ok)
    {
        FBZZ_LOG_ERROR("AssetFileWatcher: ReadDirectoryChangesW failed (%lu)", GetLastError());
        Stop();
        return;
    }

    m_readPending = true;
}

void AssetFileWatcher::ParseBuffer(DWORD bytesTransferred)
{
    if (bytesTransferred == 0) return;

    const uint8_t* ptr = m_buffer;
    for (;;)
    {
        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(ptr);

        // wchar_t パス → UTF-8 相対パス
        const std::string relPath = util::FileSystem::NormalizePathSeparators(
            util::StringUtils::ToNarrow(info->FileName, static_cast<int>(info->FileNameLength / sizeof(wchar_t))));

        FileEvent ev;

        switch (info->Action)
        {
        case FILE_ACTION_ADDED:
            ev = { EventType::Added, relPath, {} };
            m_queue.push_back(std::move(ev));
            break;

        case FILE_ACTION_REMOVED:
            ev = { EventType::Removed, relPath, {} };
            m_queue.push_back(std::move(ev));
            break;

        case FILE_ACTION_MODIFIED:
            ev = { EventType::Modified, relPath, {} };
            m_queue.push_back(std::move(ev));
            break;

        case FILE_ACTION_RENAMED_OLD_NAME:
            // 次の通知が NEW_NAME のはず。ペアにするために記憶する。
            m_pendingRenameOld = relPath;
            break;

        case FILE_ACTION_RENAMED_NEW_NAME:
            ev = { EventType::Renamed, relPath, m_pendingRenameOld };
            m_queue.push_back(std::move(ev));
            m_pendingRenameOld.clear();
            break;

        default:
            break;
        }

        if (info->NextEntryOffset == 0) break;
        ptr += info->NextEntryOffset;
    }
}

} // namespace fbzz::editor
