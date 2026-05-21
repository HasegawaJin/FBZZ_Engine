// FBZZ Engine
// FileSystem.cpp | fbzz::util
// ファイル・ディレクトリ操作 (Win32)
#include <Engine/Util/FileSystem.hpp>
#include <Windows.h>
#include <fstream>
#include <sstream>

namespace fbzz::util {

bool FileSystem::Exists(const std::string& path)
{
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
}

bool FileSystem::IsDirectory(const std::string& path)
{
    DWORD attr = GetFileAttributesA(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
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

std::vector<std::string> FileSystem::ListFiles(const std::string& dir, const std::string& ext)
{
    std::vector<std::string> result;
    std::string pattern = dir + "\\*";

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return result;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = fd.cFileName;
        if (ext.empty() || GetExtension(name) == ext)
            result.push_back(dir + "\\" + name);
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    return result;
}

bool FileSystem::EnsureDirectory(const std::string& path)
{
    if (IsDirectory(path)) return true;
    BOOL ok = CreateDirectoryA(path.c_str(), nullptr);
    return ok || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool FileSystem::ReadText(const std::string& path, std::string& out)
{
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool FileSystem::WriteText(const std::string& path, const std::string& text)
{
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << text;
    return true;
}

} // namespace fbzz::util
