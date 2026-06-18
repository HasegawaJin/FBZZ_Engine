// FBZZ Engine
// FileSystem.cpp | fbzz::util
// ファイル・ディレクトリ操作の Win32 実装
// 存在確認、列挙、読み書き、ディレクトリ作成をまとめる。
// 失敗は bool や空配列で返し、例外は使わない。
#include <Engine/Util/FileSystem.hpp>
#include <Windows.h>

// WHY: Windows.h は CopyFile / GetCurrentDirectory を A / W サフィックス付き関数へ置換する。
//      FileSystem のメンバー関数名まで置換されると、ヘッダ宣言と実装名がずれて
//      MSVC が FileSystem::CopyFileA などを探してしまうため、Win32 API を直接呼ばない本ファイルでは解除する。
#ifdef CopyFile
#undef CopyFile
#endif

#ifdef GetCurrentDirectory
#undef GetCurrentDirectory
#endif

#include <algorithm>
#include <cctype>
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
    std::string lhs = NormalizePathSeparators(a);
    std::string rhs = NormalizePathSeparators(b);
    std::transform(lhs.begin(), lhs.end(), lhs.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(rhs.begin(), rhs.end(), rhs.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lhs == rhs;
}

bool FileSystem::IsChildPathText(const std::string& path, const std::string& root)
{
    std::string normalizedPath = NormalizePathSeparators(path);
    std::string normalizedRoot = NormalizePathSeparators(root);
    std::transform(normalizedPath.begin(), normalizedPath.end(), normalizedPath.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(normalizedRoot.begin(), normalizedRoot.end(), normalizedRoot.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (normalizedRoot.empty()) return false;
    if (normalizedPath == normalizedRoot) return true;
    normalizedRoot += "/";
    return normalizedPath.rfind(normalizedRoot, 0) == 0;
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

std::ofstream FileSystem::OpenBinaryWriter(const std::filesystem::path& path)
{
    if (!EnsureParentDirectory(path)) return {};
    return std::ofstream(path, std::ios::binary);
}

bool FileSystem::ReadBinary(const std::filesystem::path& path, std::vector<uint8_t>& out)
{
    out.clear();

    // WHY: バイナリアセットの読み込み経路を FileSystem に集約し、Hub / Editor / Engine で
    //      Windows の wchar_t パス対応と失敗時 bool 戻り値の方針を揃える。
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;

    const std::streamsize size = f.tellg();
    if (size <= 0) return false;

    out.resize(static_cast<size_t>(size));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(out.data()), size);
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

bool FileSystem::WriteText(const std::filesystem::path& path, const std::string& text)
{
    if (!EnsureParentDirectory(path)) return false;
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f << text;
    return true;
}

std::filesystem::path FileSystem::PathFromUtf8(const std::string& path)
{
    return std::filesystem::path(Utf8ToWide(path));
}

std::string FileSystem::PathToUtf8(const std::filesystem::path& path)
{
    return NormalizePathSeparators(WideToUtf8(path.wstring()), false);
}

std::filesystem::path FileSystem::RelativePath(const std::filesystem::path& path, const std::filesystem::path& root)
{
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(path, root, ec);
    return ec ? std::filesystem::path{} : rel;
}

std::filesystem::path FileSystem::MakeAbsolute(const std::filesystem::path& path)
{
    // WHY: 失敗時は入力をそのまま返し、呼び出し元にフォールバック処理を課さない。
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
    // WHY: GetModuleFileNameW(nullptr) は現在の exe のフルパスを返す。
    //      parent_path() でディレクトリを取り出し、アセット・設定ファイルの基点として使う。
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}

} // namespace fbzz::util
