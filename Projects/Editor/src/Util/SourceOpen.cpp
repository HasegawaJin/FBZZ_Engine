/// @file    SourceOpen.cpp
/// @brief   「ファイル:行」を外部エディターで開く処理と、ログ行の位置プレフィックス解析。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <Editor/Util/SourceOpen.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>

/// @note ShellExecuteW / SearchPathW の呼び出しに使う。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <shellapi.h>
#include <bit>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <unordered_map>
#include <utility>

namespace fbzz::editor {

namespace fs = std::filesystem;

namespace {

bool IsWithinSourceRoot(const std::string& file, const std::string& root)
{
    if (file.empty() || root.empty()) return false;
    std::error_code error;
    const fs::path absoluteFile = fs::absolute(util::FileSystem::PathFromUtf8(file), error).lexically_normal();
    if (error) return false;
    const fs::path absoluteRoot = fs::absolute(util::FileSystem::PathFromUtf8(root), error).lexically_normal();
    if (error) return false;
    auto filePart = absoluteFile.begin();
    auto rootPart = absoluteRoot.begin();
    for (; rootPart != absoluteRoot.end(); ++rootPart, ++filePart) {
        if (filePart == absoluteFile.end()) return false;
        const std::string fileKey = util::StringUtils::ToLower(util::FileSystem::PathToUtf8(*filePart));
        const std::string rootKey = util::StringUtils::ToLower(util::FileSystem::PathToUtf8(*rootPart));
        if (fileKey != rootKey) return false;
    }
    return filePart != absoluteFile.end();
}

} /// @note anonymous namespace

void OpenSourceInExternalEditor(const std::string& file, int line)
{
    if (file.empty()) return;
    const std::wstring wfile = util::StringUtils::ToWide(file);

    /// @note 1) VSCode CLI (code.cmd) を PATH から探す。
    wchar_t codeBuf[MAX_PATH]{};
    if (SearchPathW(nullptr, L"code", L".cmd", MAX_PATH, codeBuf, nullptr)) {
        std::wstring args = L"-g \"" + wfile;
        if (line > 0) args += L":" + std::to_wstring(line);
        args += L"\"";
        /// @note ShellExecute は .cmd をシェル経由で正しく起動できる。SW_HIDE でコンソール点滅を抑える。
        const HINSTANCE r = ShellExecuteW(nullptr, L"open", codeBuf, args.c_str(), nullptr, SW_HIDE);
        if (std::bit_cast<INT_PTR>(r) > 32) return;
    }

    /// @note 2) フォールバック: 既定の関連付けで開く (行ジャンプなし)。
    ShellExecuteW(nullptr, L"open", wfile.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::string ResolveSourceFileByName(const std::string& fileName,
                                    const std::vector<std::string>& roots)
{
    if (fileName.empty()) return {};

    /// @note 既に開けるパスならそのまま使う (ビルド診断はフルパスを持っていることが多い)。
    std::error_code ec;
    if (fs::is_regular_file(fs::path(fileName), ec))
        return fileName;

    /// @note 成否をキャッシュして、同じ名前の探索でソースツリーを再走査しない。
    static std::unordered_map<std::string, std::string> s_cache;
    if (const auto it = s_cache.find(fileName); it != s_cache.end())
        return it->second;

    /// @note 探索対象はソースとシェーダーに限定する。Library/ 等の生成物は除外して走査量を抑える。
    const fs::path wanted = fs::path(fileName).filename();
    std::string found;

    for (const std::string& root : roots) {
        if (!found.empty()) break;
        if (root.empty()) continue;
        if (!fs::is_directory(fs::path(root), ec)) continue;

        fs::recursive_directory_iterator it(fs::path(root),
                                            fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            const fs::path& p = it->path();

            /// @note 生成物ディレクトリへは降りない (Library / Build / .git / intermediate)。
            if (it->is_directory(ec)) {
                const std::string dirName = util::StringUtils::ToLower(
                    util::StringUtils::PathToUtf8(p.filename()));
                if (dirName == "library" || dirName == "build" || dirName == ".git" ||
                    dirName == "out" || dirName == "compiled" || dirName == ".vs")
                    it.disable_recursion_pending();
                continue;
            }

            if (p.filename() == wanted) {
                found = util::StringUtils::PathToUtf8(p);
                break;
            }
        }
    }

    s_cache.emplace(fileName, found);
    return found;
}

std::string ResolveSourceFilePath(const std::string& filePath,
                                  const std::vector<std::string>& roots)
{
    if (filePath.empty()) return {};
    std::error_code error;
    const fs::path requested = util::FileSystem::PathFromUtf8(filePath);
    if (fs::is_regular_file(requested, error) && !error) {
        const fs::path absolute = fs::absolute(requested, error);
        return error ? std::string{} : util::FileSystem::PathToUtf8(absolute);
    }
    if (requested.is_absolute()) return {};
    for (const std::string& root : roots) {
        if (root.empty()) continue;
        const fs::path candidate = util::FileSystem::PathFromUtf8(root) / requested;
        error.clear();
        if (!fs::is_regular_file(candidate, error) || error) continue;
        const fs::path absolute = fs::absolute(candidate, error);
        if (!error) return util::FileSystem::PathToUtf8(absolute);
    }
    return {};
}

bool ParseLogLocationPrefix(const std::string& message,
                            std::string& outFile,
                            int& outLine,
                            std::size_t& outBodyOffset)
{
    /// @note 期待する形式: "[Foo.cpp:123] 本文"  (Logger::BuildLocatedFormat が生成する)
    if (message.size() < 4 || message[0] != '[') return false;

    const std::size_t close = message.find(']');
    if (close == std::string::npos) return false;

    const std::size_t colon = message.rfind(':', close);
    if (colon == std::string::npos || colon <= 1) return false;

    /// @note 位置情報のない括弧付きログを誤認しないよう、行番号が数字のみか確認する。
    if (colon + 1 >= close) return false;
    for (std::size_t i = colon + 1; i < close; ++i)
        if (std::isdigit(static_cast<unsigned char>(message[i])) == 0) return false;

    std::string file = message.substr(1, colon - 1);
    if (file.empty()) return false;

    outFile = std::move(file);
    outLine = std::atoi(message.c_str() + colon + 1);
    outBodyOffset = (close + 2 <= message.size()) ? close + 2 : close + 1;
    return true;
}

std::vector<ManagedSourceLocation> ParseManagedSourceLocations(const std::string& message)
{
    std::vector<ManagedSourceLocation> locations;
    std::size_t searchFrom = 0;
    while (searchFrom < message.size()) {
        const std::size_t marker = message.find(".cs:line ", searchFrom);
        if (marker == std::string::npos) break;
        const std::size_t previousLine = message.rfind('\n', marker);
        const std::size_t frameLineStart = previousLine == std::string::npos ? 0 : previousLine + 1;
        const std::size_t frameStart = message.find(" in ", frameLineStart);
        if (frameStart == std::string::npos || frameStart >= marker) {
            searchFrom = marker + 1;
            continue;
        }
        const std::size_t lineStart = marker + 9;
        std::size_t lineEnd = lineStart;
        int line = 0;
        while (lineEnd < message.size() && message[lineEnd] >= '0' && message[lineEnd] <= '9') {
            const int digit = message[lineEnd] - '0';
            if (line > (INT_MAX - digit) / 10) {
                line = 0;
                break;
            }
            line = line * 10 + digit;
            ++lineEnd;
        }
        std::string file = message.substr(frameStart + 4, marker + 3 - (frameStart + 4));
        while (!file.empty() && (file.back() == ' ' || file.back() == '\t')) file.pop_back();
        if (!file.empty() && line > 0 && lineEnd > lineStart) {
            locations.push_back({std::move(file), line});
        }
        searchFrom = marker + 1;
    }
    return locations;
}

bool ParseManagedSourceLocation(const std::string& message,
                                std::string& outFile,
                                int& outLine)
{
    const std::vector<ManagedSourceLocation> locations = ParseManagedSourceLocations(message);
    if (locations.empty()) return false;
    outFile = locations.front().file;
    outLine = locations.front().line;
    return true;
}

ManagedSourceLocation ResolveManagedSourceLocation(const std::string& message,
                                                    const std::vector<std::string>& roots,
                                                    const std::string& preferredRoot)
{
    ManagedSourceLocation firstResolved;
    for (const ManagedSourceLocation& location : ParseManagedSourceLocations(message)) {
        const std::string resolved = ResolveSourceFilePath(location.file, roots);
        if (resolved.empty()) continue;
        if (firstResolved.file.empty()) firstResolved = {resolved, location.line};
        if (IsWithinSourceRoot(resolved, preferredRoot)) return {resolved, location.line};
    }
    return firstResolved;
}

} /// @note namespace fbzz::editor
