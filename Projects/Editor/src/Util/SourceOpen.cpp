// FBZZ Engine
// SourceOpen.cpp | fbzz::editor
// 「ファイル:行」を外部エディターで開く処理と、ログ行の位置プレフィックス解析
#include <Editor/Util/SourceOpen.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <Windows.h>   // ShellExecuteW / SearchPathW
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <unordered_map>
#include <utility>

namespace fbzz::editor {

namespace fs = std::filesystem;

void OpenSourceInExternalEditor(const std::string& file, int line)
{
    if (file.empty()) return;
    const std::wstring wfile = util::StringUtils::ToWide(file);

    // 1) VSCode CLI (code.cmd) を PATH から探す。
    wchar_t codeBuf[MAX_PATH]{};
    if (SearchPathW(nullptr, L"code", L".cmd", MAX_PATH, codeBuf, nullptr)) {
        std::wstring args = L"-g \"" + wfile;
        if (line > 0) args += L":" + std::to_wstring(line);
        args += L"\"";
        // ShellExecute は .cmd をシェル経由で正しく起動できる。SW_HIDE でコンソール点滅を抑える。
        const HINSTANCE r = ShellExecuteW(nullptr, L"open", codeBuf, args.c_str(), nullptr, SW_HIDE);
        if (reinterpret_cast<INT_PTR>(r) > 32) return;
    }

    // 2) フォールバック: 既定の関連付けで開く (行ジャンプなし)。
    ShellExecuteW(nullptr, L"open", wfile.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::string ResolveSourceFileByName(const std::string& fileName,
                                    const std::vector<std::string>& roots)
{
    if (fileName.empty()) return {};

    // 既に開けるパスならそのまま使う (ビルド診断はフルパスを持っていることが多い)。
    std::error_code ec;
    if (fs::is_regular_file(fs::path(fileName), ec))
        return fileName;

    // WHY: 探索は再帰ディレクトリ走査なので、同じ名前を毎回引くとログを 1 行開くたびに
    //      ソースツリー全体を舐めることになる。見つからなかった場合も空文字列で記憶し、
    //      「無いファイル」の連打で走査が繰り返されるのを防ぐ。
    static std::unordered_map<std::string, std::string> s_cache;
    if (const auto it = s_cache.find(fileName); it != s_cache.end())
        return it->second;

    // 探索対象はソースとシェーダーに限定する。Library/ 等の生成物は除外して走査量を抑える。
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

            // 生成物ディレクトリへは降りない (Library / Build / .git / intermediate)。
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

bool ParseLogLocationPrefix(const std::string& message,
                            std::string& outFile,
                            int& outLine,
                            std::size_t& outBodyOffset)
{
    // 期待する形式: "[Foo.cpp:123] 本文"  (Logger::BuildLocatedFormat が生成する)
    if (message.size() < 4 || message[0] != '[') return false;

    const std::size_t close = message.find(']');
    if (close == std::string::npos) return false;

    const std::size_t colon = message.rfind(':', close);
    if (colon == std::string::npos || colon <= 1) return false;

    // 行番号部分がすべて数字であることを確認する。
    // WHY: "[INFO] ..." のような位置情報を持たない括弧付きログを誤って
    //      ファイル参照として扱わないため。
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

} // namespace fbzz::editor
