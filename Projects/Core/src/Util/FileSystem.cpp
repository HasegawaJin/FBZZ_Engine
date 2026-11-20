/// @file    FileSystem.cpp
/// @brief   ファイル・ディレクトリ操作の Win32 実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note 存在確認、列挙、読み書き、ディレクトリ作成をまとめる。
/// @note 失敗は bool や空配列で返し、例外は使わない。
#include <Core/Util/FileSystem.hpp>
#include <Core/Util/AtomicFileReplace.hpp>
#include <Core/Util/StringUtils.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

/// @note Win32 の A/W マクロが FileSystem の同名メンバーを置換しないようにする。
#ifdef CopyFile
#undef CopyFile
#endif

#ifdef GetCurrentDirectory
#undef GetCurrentDirectory
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <limits>

namespace fbzz::util {
namespace {

HANDLE CreateSiblingTemporaryFile(const std::filesystem::path& target, std::filesystem::path& temporary)
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

        temporary = target.parent_path() / (L"." + target.filename().wstring() + L"."
            + std::to_wstring(::GetCurrentProcessId()) + L"." + std::to_wstring(nonce) + L".tmp");
        /// @note 同じディレクトリに排他的に作成し、名前が衝突した既存ファイルは保持する。
        /// @see https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-createfilew CREATE_NEW の排他的作成。
        const HANDLE file = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            return file;
        }

        const DWORD error = ::GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            return INVALID_HANDLE_VALUE;
        }
    }
    return INVALID_HANDLE_VALUE;
}

/// @note 書き込みに失敗した場合もハンドルを閉じ、呼び出し元が一時ファイルを削除できるようにする。
bool WriteAndCloseTemporaryFile(HANDLE file, const char* data, size_t size)
{
    constexpr size_t WRITE_CHUNK_BYTES = 1024U * 1024U;
    bool writeSucceeded = true;
    size_t offset = 0;
    while (offset < size) {
        const DWORD requestedBytes = static_cast<DWORD>(std::min(size - offset, WRITE_CHUNK_BYTES));
        DWORD writtenBytes = 0;
        if (!::WriteFile(file, data + offset, requestedBytes, &writtenBytes, nullptr) || writtenBytes == 0) {
            writeSucceeded = false;
            break;
        }
        offset += writtenBytes;
    }

    const bool flushed = writeSucceeded && ::FlushFileBuffers(file) != FALSE;
    const bool closed = ::CloseHandle(file) != FALSE;
    return flushed && closed;
}

} /// @note namespace

std::error_code ReplaceFileAtomic(const std::filesystem::path& temporary, const std::filesystem::path& target)
{
    std::error_code error;
    const auto absoluteTarget = std::filesystem::absolute(target, error);
    if (error) return error;
    const auto& name = absoluteTarget.native();
    if (name.size() > (MAXDWORD - sizeof(FILE_RENAME_INFO) - sizeof(wchar_t)) / sizeof(wchar_t))
        return {ERROR_FILENAME_EXCED_RANGE, std::system_category()};
    FILE_RENAME_INFO rename{};
    rename.Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    rename.FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::vector<uint8_t> bytes(sizeof(rename) + rename.FileNameLength + sizeof(wchar_t), 0);
    std::memcpy(bytes.data(), &rename, sizeof(rename));
    std::memcpy(bytes.data() + offsetof(FILE_RENAME_INFO, FileName), name.data(), rename.FileNameLength);
    const HANDLE source = ::CreateFileW(temporary.c_str(), DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (source == INVALID_HANDLE_VALUE)
        return {static_cast<int>(::GetLastError()), std::system_category()};
    /// @note 削除共有された読み手を保持したまま、対象名を新しいファイルへ切り替える。
    /// @see https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information FILE_RENAME_POSIX_SEMANTICS.
    const BOOL replaced = ::SetFileInformationByHandle(source, FileRenameInfoEx, bytes.data(), static_cast<DWORD>(bytes.size()));
    const DWORD code = replaced ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(source);
    if (replaced) return {};
    if (code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_FUNCTION) {
        std::filesystem::rename(temporary, target, error);
        return error;
    }
    return {static_cast<int>(code), std::system_category()};
}

bool FileSystem::Exists(const std::string& path)
{
    const DWORD attributes = GetFileAttributesW(StringUtils::ToWide(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES;
}

bool FileSystem::IsDirectory(const std::string& path)
{
    const DWORD attributes = GetFileAttributesW(StringUtils::ToWide(path).c_str());
    return (attributes != INVALID_FILE_ATTRIBUTES) && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::string FileSystem::GetExtension(const std::string& path)
{
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    return path.substr(dot);
}

std::string FileSystem::GetFilename(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

std::string FileSystem::GetDirectory(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return "";
    return path.substr(0, slash + 1);
}

std::string FileSystem::NormalizePathSeparators(std::string path, bool trimTrailingSlash)
{
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    if (trimTrailingSlash) {
        while (path.size() > 1 && path.back() == '/')
            path.pop_back();
    }
    return path;
}

bool FileSystem::SamePathText(const std::string& a, const std::string& b)
{
    return SamePathText(a, b, true);
}

bool FileSystem::SamePathText(std::string_view a, std::string_view b, bool trimTrailingSlash)
{
    const auto isSeparator = [](char value) { return value == '/' || value == '\\'; };
    if (trimTrailingSlash) {
        while (a.size() > 1 && isSeparator(a.back())) a.remove_suffix(1);
        while (b.size() > 1 && isSeparator(b.back())) b.remove_suffix(1);
    }
    if (a.size() != b.size()) return false;
    const auto fold = [](char value) {
        if (value == '\\') return '/';
        return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    };
    for (size_t index = 0; index < a.size(); ++index)
        if (fold(a[index]) != fold(b[index])) return false;
    return true;
}

bool FileSystem::IsChildPathText(const std::string& path, const std::string& root)
{
    const std::string normalizedPath = StringUtils::ToLowerCopy(NormalizePathSeparators(path));
    std::string normalizedRoot = StringUtils::ToLowerCopy(NormalizePathSeparators(root));
    if (normalizedRoot.empty()) return false;
    if (normalizedPath == normalizedRoot) return true;

    normalizedRoot += '/';
    return normalizedPath.starts_with(normalizedRoot);
}

std::vector<std::string> FileSystem::ListFiles(const std::string& dir, const std::string& ext)
{
    std::vector<std::string> result;
    const std::wstring searchPattern = StringUtils::ToWide(dir + "\\*");

    WIN32_FIND_DATAW entry;
    const HANDLE search = FindFirstFileW(searchPattern.c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) return result;

    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::string name = StringUtils::ToNarrow(entry.cFileName);
        if (ext.empty() || GetExtension(name) == ext)
            result.push_back(dir + "\\" + name);
    } while (FindNextFileW(search, &entry));

    FindClose(search);
    return result;
}

std::vector<std::filesystem::path> FileSystem::ListFiles(const std::filesystem::path& dir)
{
    std::vector<std::filesystem::path> result;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec))
            result.push_back(entry.path());
    }
    return result;
}

std::vector<std::filesystem::path> FileSystem::ListFilesRecursive(const std::filesystem::path& dir)
{
    std::vector<std::filesystem::path> result;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        dir,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    const std::filesystem::recursive_directory_iterator end;

    while (!ec && it != end) {
        if (it->is_regular_file(ec))
            result.push_back(it->path());
        it.increment(ec);
    }

    return result;
}

std::vector<std::filesystem::path> FileSystem::ListDirectories(const std::filesystem::path& dir)
{
    std::vector<std::filesystem::path> result;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_directory(ec))
            result.push_back(entry.path());
    }
    return result;
}

std::vector<std::string> FileSystem::ListAll(const std::string& dir)
{
    std::vector<std::string> result;
    const std::wstring searchPattern = StringUtils::ToWide(dir + "\\*");

    WIN32_FIND_DATAW entry;
    const HANDLE search = FindFirstFileW(searchPattern.c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) return result;

    do {
        const std::string name = StringUtils::ToNarrow(entry.cFileName);
        if (name == "." || name == "..") continue;
        result.push_back(dir + "\\" + name);
    } while (FindNextFileW(search, &entry));

    FindClose(search);
    return result;
}

bool FileSystem::EnsureDirectory(const std::string& path)
{
    return EnsureDirectory(PathFromUtf8(path));
}

bool FileSystem::EnsureDirectory(const std::filesystem::path& path)
{
    if (path.empty()) return true;
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) return true;
    ec.clear();
    return std::filesystem::create_directories(path, ec) || !ec;
}

bool FileSystem::EnsureParentDirectory(const std::filesystem::path& path)
{
    const std::filesystem::path parent = path.parent_path();
    return parent.empty() || EnsureDirectory(parent);
}

bool FileSystem::CopyFile(const std::filesystem::path& src, const std::filesystem::path& dst, bool overwrite)
{
    if (!EnsureParentDirectory(dst)) return false;
    std::error_code ec;
    const auto options = overwrite
        ? std::filesystem::copy_options::overwrite_existing
        : std::filesystem::copy_options::none;
    std::filesystem::copy_file(src, dst, options, ec);
    return !ec;
}

bool FileSystem::CopyFileA(const std::filesystem::path& src, const std::filesystem::path& dst, bool overwrite)
{
    return CopyFile(src, dst, overwrite);
}

bool FileSystem::CopyFileW(const std::filesystem::path& src, const std::filesystem::path& dst, bool overwrite)
{
    return CopyFile(src, dst, overwrite);
}

bool FileSystem::CopyDirectoryRecursive(const std::filesystem::path& src, const std::filesystem::path& dst, bool overwrite)
{
    if (!EnsureDirectory(dst)) return false;
    std::error_code ec;
    const auto options = std::filesystem::copy_options::recursive |
        (overwrite ? std::filesystem::copy_options::overwrite_existing
                   : std::filesystem::copy_options::none);
    std::filesystem::copy(src, dst, options, ec);
    return !ec;
}

bool FileSystem::RemoveAll(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return true;
    ec.clear();
    std::filesystem::remove_all(path, ec);
    return !ec;
}

bool FileSystem::Rename(const std::filesystem::path& src, const std::filesystem::path& dst)
{
    if (!EnsureParentDirectory(dst)) return false;
    std::error_code ec;
    std::filesystem::rename(src, dst, ec);
    return !ec;
}

bool FileSystem::ReadText(const std::string& path, std::string& out)
{
    const std::wstring widePath = StringUtils::ToWide(path);
    if (widePath.empty()) return false;

    /// @note 文字列版はテキストモードで CRLF を LF に変換する。
    std::ifstream stream(widePath);
    if (!stream.is_open()) return false;
    std::ostringstream contents;
    contents << stream.rdbuf();
    out = contents.str();
    return true;
}

bool FileSystem::WriteText(const std::string& path, const std::string& text)
{
    const std::wstring widePath = StringUtils::ToWide(path);
    if (widePath.empty()) return false;

    /// @note 文字列版はテキストモードで LF を CRLF に変換する。
    std::ofstream stream(widePath);
    if (!stream.is_open()) return false;
    stream << text;
    /// @note バッファの書き出しに伴う失敗も close() 後に確認する。
    stream.close();
    return stream.good();
}

bool FileSystem::WriteTextAtomic(const std::string& path, const std::string& text)
{
    return WriteBinaryAtomic(PathFromUtf8(path), text.data(), text.size());
}

bool FileSystem::WriteBinaryAtomic(const std::filesystem::path& input, const void* data, size_t size)
{
    if (input.empty() || (!data && size > 0)) {
        return false;
    }

    std::error_code error;
    const auto target = std::filesystem::absolute(input, error).lexically_normal();
    if (error || !EnsureParentDirectory(target)) {
        return false;
    }

    const DWORD attributes = ::GetFileAttributesW(target.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return false;
    }

    std::filesystem::path temporary;
    const HANDLE file = CreateSiblingTemporaryFile(target, temporary);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    const bool written = WriteAndCloseTemporaryFile(file, static_cast<const char*>(data), size);
    /// @note MSVC の同一ディレクトリ内の rename は、原本を先に削除せずに置き換える。
    /// @see https://github.com/microsoft/STL/blob/main/stl/src/filesystem.cpp __std_fs_rename。
    if (written && Rename(temporary, target)) {
        return true;
    }

    std::filesystem::remove(temporary, error);
    return false;
}

std::ofstream FileSystem::OpenBinaryWriter(const std::filesystem::path& path)
{
    if (!EnsureParentDirectory(path)) return {};
    return std::ofstream(path, std::ios::binary);
}

bool FileSystem::ReadBinary(const std::filesystem::path& path, std::vector<uint8_t>& out)
{
    out.clear();

    /// @note バイナリアセットの読み込み経路を FileSystem に集約し、Hub/Editor/Engine で
    /// @note       Windows の wchar_t パス対応と失敗時 bool 戻り値の方針を揃える。
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;

    const std::streamsize size = f.tellg();
    if (size <= 0) return false;

    out.resize(static_cast<size_t>(size));
    f.seekg(0, std::ios::beg);
    f.read(static_cast<char*>(static_cast<void*>(out.data())), size);
    if (!f.good()) {
        out.clear();
        return false;
    }

    return true;
}

bool FileSystem::WriteBinary(const std::filesystem::path& path, const void* data, size_t size)
{
    if (!data && size > 0) return false;
    std::ofstream f = OpenBinaryWriter(path);
    if (!f.is_open()) return false;
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return f.good();
}

/// @name std::filesystem::path オーバーロード

bool FileSystem::Exists(const std::filesystem::path& path)
{
    /// @note filesystem::path::c_str() は Windows で const wchar_t* を返すため、
    /// @note       GetFileAttributesW に直接渡せて UTF-8 変換が不要。
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
}

bool FileSystem::ReadText(const std::filesystem::path& path, std::string& out)
{
    /// @note std::ifstream(filesystem::path) は Windows で wchar_t パスを使うため
    /// @note       マルチバイト文字を含むパスも正しく開ける。
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool FileSystem::WriteText(const std::filesystem::path& path, const std::string& text)
{
    if (!EnsureParentDirectory(path)) return false;
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f << text;
    f.close();
    return f.good();
}

std::filesystem::path FileSystem::PathFromUtf8(const std::string& path)
{
    return std::filesystem::path(StringUtils::ToWide(path));
}

std::string FileSystem::PathToUtf8(const std::filesystem::path& path)
{
    /// @note StringUtils::PathToUtf8 が正規化まで担うため、重複実装を排除して委譲する。
    return util::StringUtils::PathToUtf8(path);
}

std::filesystem::path FileSystem::RelativePath(const std::filesystem::path& path, const std::filesystem::path& root)
{
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(path, root, ec);
    return ec ? std::filesystem::path{} : rel;
}

std::filesystem::path FileSystem::MakeAbsolute(const std::filesystem::path& path)
{
    /// @note 失敗時は入力をそのまま返し、呼び出し元にフォールバック処理を課さない。
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    return ec ? path : absolute.lexically_normal();
}

bool FileSystem::SamePath(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code ec;
    if (std::filesystem::exists(a, ec) && std::filesystem::exists(b, ec)) {
        ec.clear();
        const bool equivalent = std::filesystem::equivalent(a, b, ec);
        if (!ec) return equivalent;
    }

    return SamePathText(PathToUtf8(a.lexically_normal()), PathToUtf8(b.lexically_normal()));
}

std::filesystem::path FileSystem::GetCurrentDirectory()
{
    std::error_code ec;
    const std::filesystem::path current = std::filesystem::current_path(ec);
    return ec ? std::filesystem::path{} : current;
}

std::filesystem::path FileSystem::GetCurrentDirectoryA()
{
    return GetCurrentDirectory();
}

std::filesystem::path FileSystem::GetCurrentDirectoryW()
{
    return GetCurrentDirectory();
}

std::filesystem::file_time_type FileSystem::LastWriteTime(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    return ec ? std::filesystem::file_time_type{} : t;
}

std::filesystem::path FileSystem::GetExecutableDirectory()
{
    /// @note GetModuleFileNameW(nullptr) は現在の exe のフルパスを返す。
    /// @note       parent_path() でディレクトリを取り出し、アセット・設定ファイルの基点として使う。
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}

} /// @note namespace fbzz::util
