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
    // WHY: MultiByteToWideChar は -1 指定時に終端 NUL も含めて n 文字を書き込む。
    //      n - 1 だけ確保して n を渡すと 1 文字分オーバーランし、ReadText などの呼び出し元でクラッシュする。
    std::wstring w(static_cast<size_t>(n), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (written <= 0) return {};
    w.resize(static_cast<size_t>(written - 1));
    return w;
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    // WHY: WideCharToMultiByte も終端 NUL を含めて n バイトを書き込むため、同じく n 分を確保してから縮める。
    std::string s(static_cast<size_t>(n), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    if (written <= 0) return {};
    s.resize(static_cast<size_t>(written - 1));
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
    const std::wstring widePath = Utf8ToWide(path);
    if (widePath.empty()) return false;

    std::ifstream f(widePath);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool FileSystem::WriteText(const std::string& path, const std::string& text)
{
    const std::wstring widePath = Utf8ToWide(path);
    if (widePath.empty()) return false;

    std::ofstream f(widePath);
    if (!f.is_open()) return false;
    f << text;
    return true;
}

// --- std::filesystem::path オーバーロード ---

bool FileSystem::Exists(const std::filesystem::path& path)
{
    // WHY: filesystem::path::c_str() は Windows で const wchar_t* を返すため、
    //      GetFileAttributesW に直接渡せて UTF-8 変換が不要。
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
}

bool FileSystem::ReadText(const std::filesystem::path& path, std::string& out)
{
    // WHY: std::ifstream(filesystem::path) は Windows で wchar_t パスを使うため
    //      マルチバイト文字を含むパスも正しく開ける。
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::filesystem::path FileSystem::MakeAbsolute(const std::filesystem::path& path)
{
    // WHY: 失敗時は入力をそのまま返し、呼び出し元にフォールバック処理を課さない。
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    return ec ? path : absolute.lexically_normal();
}

std::filesystem::path FileSystem::GetExecutableDirectory()
{
    // WHY: GetModuleFileNameW(nullptr) は現在の exe のフルパスを返す。
    //      parent_path() でディレクトリを取り出し、アセット・設定ファイルの基点として使う。
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}

} // namespace fbzz::util
