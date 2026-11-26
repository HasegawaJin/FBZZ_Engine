/// @file    FileSystem.cpp
/// @brief   ファイルとディレクトリの列挙・操作の Win32 実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21

#include <Core/Util/FileSystem.hpp>
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

namespace fbzz::util {

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

std::vector<std::string> FileSystem::ListFiles(const std::string& directory, const std::string& extension)
{
    std::vector<std::string> result;
    const std::wstring searchPattern = StringUtils::ToWide(directory + "\\*");

    WIN32_FIND_DATAW entry;
    const HANDLE search = FindFirstFileW(searchPattern.c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        return result;
    }

    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        const std::string name = StringUtils::ToNarrow(entry.cFileName);
        if (extension.empty() || GetExtension(name) == extension)
            result.push_back(directory + "\\" + name);
    } while (FindNextFileW(search, &entry));

    FindClose(search);
    return result;
}

std::vector<std::filesystem::path> FileSystem::ListFiles(const std::filesystem::path& directory)
{
    std::vector<std::filesystem::path> result;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (entry.is_regular_file(error))
            result.push_back(entry.path());
    }
    return result;
}

std::vector<std::filesystem::path> FileSystem::ListFilesRecursive(const std::filesystem::path& directory)
{
    std::vector<std::filesystem::path> result;
    std::error_code error;
    std::filesystem::recursive_directory_iterator entryIterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;

    while (!error && entryIterator != end) {
        if (entryIterator->is_regular_file(error))
            result.push_back(entryIterator->path());
        entryIterator.increment(error);
    }

    return result;
}

std::vector<std::filesystem::path> FileSystem::ListDirectories(const std::filesystem::path& directory)
{
    std::vector<std::filesystem::path> result;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (entry.is_directory(error))
            result.push_back(entry.path());
    }
    return result;
}

std::vector<std::string> FileSystem::ListAll(const std::string& directory)
{
    std::vector<std::string> result;
    const std::wstring searchPattern = StringUtils::ToWide(directory + "\\*");

    WIN32_FIND_DATAW entry;
    const HANDLE search = FindFirstFileW(searchPattern.c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        return result;
    }

    do {
        const std::string name = StringUtils::ToNarrow(entry.cFileName);
        if (name == "." || name == "..") {
            continue;
        }
        result.push_back(directory + "\\" + name);
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
    if (path.empty()) {
        return true;
    }
    std::error_code error;
    if (std::filesystem::is_directory(path, error)) {
        return true;
    }
    error.clear();
    return std::filesystem::create_directories(path, error) || !error;
}

bool FileSystem::EnsureParentDirectory(const std::filesystem::path& path)
{
    const std::filesystem::path parent = path.parent_path();
    return parent.empty() || EnsureDirectory(parent);
}

bool FileSystem::CopyFile(const std::filesystem::path& sourcePath, const std::filesystem::path& destinationPath, bool overwrite)
{
    if (!EnsureParentDirectory(destinationPath)) {
        return false;
    }
    std::error_code error;
    const auto options = overwrite
        ? std::filesystem::copy_options::overwrite_existing
        : std::filesystem::copy_options::none;
    std::filesystem::copy_file(sourcePath, destinationPath, options, error);
    return !error;
}

bool FileSystem::CopyFileA(const std::filesystem::path& sourcePath, const std::filesystem::path& destinationPath, bool overwrite)
{
    return CopyFile(sourcePath, destinationPath, overwrite);
}

bool FileSystem::CopyFileW(const std::filesystem::path& sourcePath, const std::filesystem::path& destinationPath, bool overwrite)
{
    return CopyFile(sourcePath, destinationPath, overwrite);
}

bool FileSystem::CopyDirectoryRecursive(const std::filesystem::path& sourcePath, const std::filesystem::path& destinationPath, bool overwrite)
{
    if (!EnsureDirectory(destinationPath)) {
        return false;
    }
    std::error_code error;
    const auto options = std::filesystem::copy_options::recursive |
        (overwrite ? std::filesystem::copy_options::overwrite_existing
                   : std::filesystem::copy_options::none);
    std::filesystem::copy(sourcePath, destinationPath, options, error);
    return !error;
}

bool FileSystem::RemoveAll(const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return true;
    }
    error.clear();
    std::filesystem::remove_all(path, error);
    return !error;
}

bool FileSystem::Rename(const std::filesystem::path& sourcePath, const std::filesystem::path& destinationPath)
{
    if (!EnsureParentDirectory(destinationPath)) {
        return false;
    }
    std::error_code error;
    std::filesystem::rename(sourcePath, destinationPath, error);
    return !error;
}

bool FileSystem::Exists(const std::filesystem::path& path)
{
    // filesystem::path::c_str() は Windows で const wchar_t* を返すため、
    // GetFileAttributesW に直接渡せて UTF-8 変換が不要。
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES;
}

std::filesystem::path FileSystem::GetCurrentDirectory()
{
    std::error_code error;
    const std::filesystem::path current = std::filesystem::current_path(error);
    return error ? std::filesystem::path{} : current;
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
    std::error_code error;
    const auto writeTime = std::filesystem::last_write_time(path, error);
    return error ? std::filesystem::file_time_type{} : writeTime;
}

std::filesystem::path FileSystem::GetExecutableDirectory()
{
    // GetModuleFileNameW(nullptr) は現在の exe のフルパスを返す。
    // parent_path() でディレクトリを取り出し、アセット・設定ファイルの基点として使う。
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}

}
