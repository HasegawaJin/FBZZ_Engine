/// @file    FileSystemAtomicWrite.cpp
/// @brief   一時ファイルへの書き込みと同一ディレクトリ内での公開。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Core/Util/FileSystem.hpp>
#include <Core/Util/AtomicFileReplace.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace fbzz::util {

namespace {

HANDLE CreateSiblingTemporaryFile(const std::filesystem::path& targetPath, std::filesystem::path& temporaryPath)
{
    constexpr unsigned MAX_ATTEMPTS = 64;
    constexpr uint64_t EXHAUSTED_NONCE = std::numeric_limits<uint64_t>::max();
    static std::atomic<uint64_t> nextNonce{1};

    for (unsigned attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
        uint64_t nonce = nextNonce.load(std::memory_order_relaxed);
        while (nonce != EXHAUSTED_NONCE
            && !nextNonce.compare_exchange_weak(nonce, nonce + 1, std::memory_order_relaxed)) {
        }
        if (nonce == EXHAUSTED_NONCE) {
            return INVALID_HANDLE_VALUE;
        }

        temporaryPath = targetPath.parent_path() / (L"." + targetPath.filename().wstring() + L"."
            + std::to_wstring(::GetCurrentProcessId()) + L"." + std::to_wstring(nonce) + L".tmp");
        /// @note 同じディレクトリに排他的に作成し、名前が衝突した既存ファイルは保持する。
        /// @see https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-createfilew CREATE_NEW の排他的作成。
        const HANDLE temporaryHandle = ::CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (temporaryHandle != INVALID_HANDLE_VALUE) {
            return temporaryHandle;
        }

        const DWORD error = ::GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            return INVALID_HANDLE_VALUE;
        }
    }
    return INVALID_HANDLE_VALUE;
}

/// @note 書き込みに失敗した場合もハンドルを閉じ、呼び出し元が一時ファイルを削除できるようにする。
bool WriteAndCloseTemporaryFile(HANDLE temporaryHandle, const char* data, size_t byteCount)
{
    constexpr size_t WRITE_CHUNK_BYTES = 1024U * 1024U;
    bool writeSucceeded = true;
    size_t byteOffset = 0;
    while (byteOffset < byteCount) {
        const DWORD requestedBytes = static_cast<DWORD>(std::min(byteCount - byteOffset, WRITE_CHUNK_BYTES));
        DWORD writtenBytes = 0;
        if (!::WriteFile(temporaryHandle, data + byteOffset, requestedBytes, &writtenBytes, nullptr) || writtenBytes == 0) {
            writeSucceeded = false;
            break;
        }
        byteOffset += writtenBytes;
    }

    const bool flushed = writeSucceeded && ::FlushFileBuffers(temporaryHandle) != FALSE;
    const bool closed = ::CloseHandle(temporaryHandle) != FALSE;
    return flushed && closed;
}

} /// @note namespace

std::error_code ReplaceFileAtomic(const std::filesystem::path& temporaryPath, const std::filesystem::path& targetPath)
{
    std::error_code error;
    const auto absoluteTarget = std::filesystem::absolute(targetPath, error);
    if (error) return error;
    const auto& absoluteTargetName = absoluteTarget.native();
    if (absoluteTargetName.size() > (MAXDWORD - sizeof(FILE_RENAME_INFO) - sizeof(wchar_t)) / sizeof(wchar_t))
        return {ERROR_FILENAME_EXCED_RANGE, std::system_category()};
    FILE_RENAME_INFO renameInfo{};
    renameInfo.Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    renameInfo.FileNameLength = static_cast<DWORD>(absoluteTargetName.size() * sizeof(wchar_t));
    std::vector<uint8_t> renamePayload(sizeof(renameInfo) + renameInfo.FileNameLength + sizeof(wchar_t), 0);
    std::memcpy(renamePayload.data(), &renameInfo, sizeof(renameInfo));
    std::memcpy(renamePayload.data() + offsetof(FILE_RENAME_INFO, FileName), absoluteTargetName.data(), renameInfo.FileNameLength);
    const HANDLE temporaryHandle = ::CreateFileW(temporaryPath.c_str(), DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (temporaryHandle == INVALID_HANDLE_VALUE)
        return {static_cast<int>(::GetLastError()), std::system_category()};
    /// @note 削除共有された読み手を保持したまま、対象名を新しいファイルへ切り替える。
    /// @see https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information FILE_RENAME_POSIX_SEMANTICS.
    const BOOL replaced = ::SetFileInformationByHandle(temporaryHandle, FileRenameInfoEx, renamePayload.data(), static_cast<DWORD>(renamePayload.size()));
    const DWORD statusCode = replaced ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(temporaryHandle);
    if (replaced) return {};
    if (statusCode == ERROR_INVALID_PARAMETER || statusCode == ERROR_NOT_SUPPORTED || statusCode == ERROR_INVALID_FUNCTION) {
        std::filesystem::rename(temporaryPath, targetPath, error);
        return error;
    }
    return {static_cast<int>(statusCode), std::system_category()};
}

bool FileSystem::WriteTextAtomic(const std::string& path, const std::string& text)
{
    return WriteBinaryAtomic(PathFromUtf8(path), text.data(), text.size());
}

bool FileSystem::WriteBinaryAtomic(const std::filesystem::path& outputPath, const void* data, size_t byteCount)
{
    if (outputPath.empty() || (!data && byteCount > 0)) {
        return false;
    }

    std::error_code error;
    const auto targetPath = std::filesystem::absolute(outputPath, error).lexically_normal();
    if (error || !EnsureParentDirectory(targetPath)) {
        return false;
    }

    const DWORD attributes = ::GetFileAttributesW(targetPath.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return false;
    }

    std::filesystem::path temporaryPath;
    const HANDLE temporaryHandle = CreateSiblingTemporaryFile(targetPath, temporaryPath);
    if (temporaryHandle == INVALID_HANDLE_VALUE) {
        return false;
    }

    const bool written = WriteAndCloseTemporaryFile(temporaryHandle, static_cast<const char*>(data), byteCount);
    /// @note MSVC の同一ディレクトリ内の rename は、原本を先に削除せずに置き換える。
    /// @see https://github.com/microsoft/STL/blob/main/stl/src/filesystem.cpp __std_fs_rename。
    if (written && Rename(temporaryPath, targetPath)) {
        return true;
    }

    std::filesystem::remove(temporaryPath, error);
    return false;
}

}
