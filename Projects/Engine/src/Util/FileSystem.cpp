// FBZZ Engine
// FileSystem.cpp | fbzz::util
// ファイル・ディレクトリ操作の Win32 実装
// 存在確認、列挙、読み書き、ディレクトリ作成をまとめる。
// 失敗は bool や空配列で返し、例外は使わない。
#include <Engine/Util/FileSystem.hpp>
#include <Windows.h>
#include <fstream>
#include <sstream>
#include <string>

namespace {
// UTF-8 文字列をワイド文字列に変換する。
// WHY: std::ifstream(std::string) は Windows ANSI (CP_ACP) でパスを解釈するため、
//      UTF-8 の多バイト文字を含むパスが正しく開けない。
//      ワイド文字列を使うと Win32 Unicode API 経由で開くため常に正しく動く。
std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}
} // namespace

namespace fbzz::util {

bool FileSystem::Exists(const std::string& path)
{
    DWORD attr = GetFileAttributesW(Utf8ToWide(path).c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
}

bool FileSystem::IsDirectory(const std::string& path)
{
    DWORD attr = GetFileAttributesW(Utf8ToWide(path).c_str());
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
    std::wstring pattern = Utf8ToWide(dir + "\\*");

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return result;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = WideToUtf8(fd.cFileName);
        if (ext.empty() || GetExtension(name) == ext)
            result.push_back(dir + "\\" + name);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return result;
}

std::vector<std::string> FileSystem::ListAll(const std::string& dir)
{
    std::vector<std::string> result;
    std::wstring pattern = Utf8ToWide(dir + "\\*");

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return result;

    do {
        std::string name = WideToUtf8(fd.cFileName);
        if (name == "." || name == "..") continue;
        result.push_back(dir + "\\" + name);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return result;
}

bool FileSystem::EnsureDirectory(const std::string& path)
{
    if (IsDirectory(path)) return true;
    BOOL ok = CreateDirectoryW(Utf8ToWide(path).c_str(), nullptr);
    return ok || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool FileSystem::ReadText(const std::string& path, std::string& out)
{
    std::ifstream f(Utf8ToWide(path));
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool FileSystem::WriteText(const std::string& path, const std::string& text)
{
    std::ofstream f(Utf8ToWide(path));
    if (!f.is_open()) return false;
    f << text;
    return true;
}

} // namespace fbzz::util
