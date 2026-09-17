/// @file    AssetSearch.cpp
/// @brief   アセット索引の構築・差分更新と、スコア付きマッチングの実装。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <Editor/Util/AssetSearch.hpp>

#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_set>

namespace fbzz::editor {

namespace {

/// 索引に入れないディレクトリ名。
/// @note 従来はプロジェクトルート全体を走査し build 出力・ThirdParty・Git 管理ファイルまで候補に並んでいた。数万ファイルを舐め、ピッカーを開くたび体感できる待ちが出ていた。
const std::unordered_set<std::string>& ExcludedDirectories()
{
    static const std::unordered_set<std::string> kExcluded = {
        /// @note ビルド生成物
        "build", "out", "bin", "obj", "x64", "Debug", "Release", "Development",
        /// @note 依存ライブラリ
        "ThirdParty", "node_modules",
        /// @note バージョン管理・IDE
        ".git", ".vs", ".vscode", ".idea",
        /// @note エンジンが生成する中間物 (原本から再生成されるため検索対象にしない)
        "Library", "Baked", "Temp", "Intermediate",
    };
    return kExcluded;
}

/// 索引に入れない拡張子。生成物・バイナリ中間物を除く。
/// @note .meta は原本と 1 対 1 のサイドカーで、検索結果に原本と 2 件並ぶとノイズにしかならないため除く。
const std::unordered_set<std::string>& ExcludedExtensions()
{
    static const std::unordered_set<std::string> kExcluded = {
        ".meta",
        ".cso", ".pdb", ".ilk", ".exp", ".obj", ".lib", ".dll", ".exe",
        ".tlog", ".idb", ".ipdb", ".iobj", ".log", ".tmp",
    };
    return kExcluded;
}

/// 索引の上限。これを超えたら打ち切って警告する。
/// @note 誤って巨大なディレクトリをプロジェクトルートに指定した場合に、エディタが起動不能なほど固まるのを防ぐ。
constexpr std::size_t MAX_INDEXED_FILES = 20000;

struct SearchContext {
    std::string projectRoot;
    std::vector<AssetSearchEntry> entries;
};

/// @note 関数内 static にする: Editor は静的ライブラリではなく exe に取り込まれるが、他の Util (AssetDirtyRegistry 等) と様式を揃え初期化順序の問題も避ける。
SearchContext& Ctx()
{
    static SearchContext context;
    return context;
}

std::string NormalizeSlashes(std::string path)
{
    for (char& c : path)
        if (c == '\\') c = '/';
    return path;
}

/// 1 ファイルぶんのエントリを組み立てる。索引対象外なら false。
bool MakeEntry(const std::filesystem::path& absolutePath,
               const std::filesystem::path& root,
               AssetSearchEntry& out)
{
    const std::string extension = util::StringUtils::ToLower(
        util::FileSystem::PathToUtf8(absolutePath.extension()));
    if (ExcludedExtensions().count(extension) != 0) return false;

    const std::string filename = util::FileSystem::PathToUtf8(absolutePath.filename());
    /// @note ドット始まりは隠しファイル扱いで索引しない (.gitignore / .fbzz_proj 等)。.fbzz_proj はプロジェクトファイルでアセットスロットへの割り当て対象になり得ないため同様に除く。
    if (!filename.empty() && filename[0] == '.') return false;

    std::error_code error;
    const std::filesystem::path relative = std::filesystem::relative(absolutePath, root, error);

    out.absolutePath = util::FileSystem::PathToUtf8(absolutePath);
    out.relativePath = error ? out.absolutePath
                             : NormalizeSlashes(util::FileSystem::PathToUtf8(relative));
    out.filename  = filename;
    out.extension = extension;
    return true;
}

std::string ToLowerCopy(std::string_view text)
{
    std::string lower(text);
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower;
}

/// query の全文字が text にこの順で現れるか (連続でなくてよい)。
/// 一致した場合、文字同士がどれだけ密集しているかをスコアに反映する。
/// 引数はどちらも小文字化済みであること。
int SubsequenceScore(std::string_view text, std::string_view query)
{
    std::size_t textIndex = 0;
    std::size_t lastMatch = 0;
    int gapPenalty = 0;
    bool first = true;

    for (const char target : query) {
        bool found = false;
        for (; textIndex < text.size(); ++textIndex) {
            if (text[textIndex] != target) continue;
            if (!first) gapPenalty += static_cast<int>(textIndex - lastMatch - 1);
            lastMatch = textIndex;
            first = false;
            ++textIndex;
            found = true;
            break;
        }
        if (!found) return 0;
    }

    /// @note 密集しているほど高い。最低でも 1 は返す (一致はしているため)。
    return std::max(1, 200 - gapPenalty);
}

} // namespace

/// @name 索引

void AssetSearch::SetProjectRoot(const std::string& projectRoot)
{
    /// @note 空のルートは無視する: 索引はプロセス全体で 1 つ。配線し忘れたウィジェットが空文字で呼ぶと Rebuild() が索引を空にして早期 return し、全画面の検索が死ぬ。
    /// @note 索引を捨てるのは Rebuild() を明示的に呼んだときだけにする (毎フレーム全再走査になるのを避ける)。
    if (projectRoot.empty()) return;

    SearchContext& context = Ctx();
    if (context.projectRoot == projectRoot) return;

    context.projectRoot = projectRoot;
    Rebuild();
}

void AssetSearch::Rebuild()
{
    SearchContext& context = Ctx();
    context.entries.clear();

    if (context.projectRoot.empty()) return;

    namespace fs = std::filesystem;
    const fs::path root = util::FileSystem::PathFromUtf8(context.projectRoot);
    if (!util::FileSystem::Exists(root)) return;

    std::error_code error;
    fs::recursive_directory_iterator iterator(
        root, fs::directory_options::skip_permission_denied, error);
    if (error) {
        FBZZ_LOG_WARN("AssetSearch: %s を走査できません", context.projectRoot.c_str());
        return;
    }

    const fs::recursive_directory_iterator end;
    bool truncated = false;

    for (; iterator != end; iterator.increment(error)) {
        if (error) {
            /// @note 個別のエントリが読めなくても走査は続ける (権限・シンボリックリンク等)。
            error.clear();
            continue;
        }

        const fs::directory_entry& entry = *iterator;

        if (entry.is_directory(error)) {
            const std::string name = util::FileSystem::PathToUtf8(entry.path().filename());
            /// @note 除外ディレクトリはその配下ごと降りない: 中へ入ってから弾くと build/ 以下の数万ファイルを走査するコストを払うことになる。
            if (ExcludedDirectories().count(name) != 0 || (!name.empty() && name[0] == '.'))
                iterator.disable_recursion_pending();
            continue;
        }

        if (!entry.is_regular_file(error)) continue;

        AssetSearchEntry indexed;
        if (!MakeEntry(entry.path(), root, indexed)) continue;

        context.entries.push_back(std::move(indexed));
        if (context.entries.size() >= MAX_INDEXED_FILES) { truncated = true; break; }
    }

    /// @note パス順に並べておく。query が空のときの表示順を安定させる。
    std::sort(context.entries.begin(), context.entries.end(),
        [](const AssetSearchEntry& lhs, const AssetSearchEntry& rhs) {
            return lhs.relativePath < rhs.relativePath;
        });

    if (truncated) {
        FBZZ_LOG_WARN("AssetSearch: ファイル数が上限 %zu に達したため索引を打ち切りました",
                      MAX_INDEXED_FILES);
    }
}

void AssetSearch::ApplyFileEvents(std::span<const AssetFileWatcher::FileEvent> events)
{
    if (events.empty()) return;

    SearchContext& context = Ctx();
    if (context.projectRoot.empty()) return;

    namespace fs = std::filesystem;
    const fs::path root = util::FileSystem::PathFromUtf8(context.projectRoot);

    /// @note 監視は Assets/ を起点にしているため、イベントの相対パスもそこからになる。
    ///       索引はプロジェクトルート基準なので、突き合わせは絶対パスで行う。
    const auto findEntry = [&context](const std::string& absolutePath) {
        return std::find_if(context.entries.begin(), context.entries.end(),
            [&absolutePath](const AssetSearchEntry& entry) {
                return entry.absolutePath == absolutePath;
            });
    };

    const auto watcherRoot = [&]() -> fs::path {
        /// @note AssetFileWatcher は Assets/ を監視する。相対パスの起点を合わせる。
        const fs::path assets = root / L"Assets";
        return util::FileSystem::Exists(assets) ? assets : root;
    }();

    bool dirty = false;

    for (const AssetFileWatcher::FileEvent& event : events) {
        const fs::path absolute = watcherRoot / util::FileSystem::PathFromUtf8(event.path);
        const std::string absoluteUtf8 = util::FileSystem::PathToUtf8(absolute);

        switch (event.type) {
        case AssetFileWatcher::EventType::Removed: {
            const auto it = findEntry(absoluteUtf8);
            if (it != context.entries.end()) { context.entries.erase(it); dirty = true; }
            break;
        }
        case AssetFileWatcher::EventType::Renamed: {
            if (!event.oldPath.empty()) {
                const fs::path oldAbsolute = watcherRoot
                    / util::FileSystem::PathFromUtf8(event.oldPath);
                const auto it = findEntry(util::FileSystem::PathToUtf8(oldAbsolute));
                if (it != context.entries.end()) { context.entries.erase(it); dirty = true; }
            }
            [[fallthrough]];
        }
        case AssetFileWatcher::EventType::Added: {
            std::error_code error;
            if (!fs::is_regular_file(absolute, error)) break;
            if (findEntry(absoluteUtf8) != context.entries.end()) break;

            AssetSearchEntry indexed;
            if (!MakeEntry(absolute, root, indexed)) break;
            context.entries.push_back(std::move(indexed));
            dirty = true;
            break;
        }
        case AssetFileWatcher::EventType::Modified:
            /// @note 内容の変更は索引 (パスと名前のみ) に影響しない。
            break;
        }
    }

    if (dirty) {
        std::sort(context.entries.begin(), context.entries.end(),
            [](const AssetSearchEntry& lhs, const AssetSearchEntry& rhs) {
                return lhs.relativePath < rhs.relativePath;
            });
    }
}

const std::vector<AssetSearchEntry>& AssetSearch::Entries() { return Ctx().entries; }
std::size_t AssetSearch::Count()                            { return Ctx().entries.size(); }
const std::string& AssetSearch::ProjectRoot()               { return Ctx().projectRoot; }

/// @name マッチング

int AssetSearch::Match(std::string_view text, std::string_view query)
{
    if (query.empty()) return 1;
    if (text.empty())  return 0;

    const std::string lowerText  = ToLowerCopy(text);
    const std::string lowerQuery = ToLowerCopy(query);

    /// @note 完全一致
    if (lowerText == lowerQuery) return 10000;

    /// @note 前方一致。同じ前方一致なら短い名前を優先する
    ///       ("Player" と "PlayerController" なら前者)。
    ///       下限を設けて、名前が長くても部分一致より下に落ちないようにする。
    if (lowerText.starts_with(lowerQuery))
        return std::max(5000 - static_cast<int>(lowerText.size()), 2000);

    /// @note 部分一致。出現位置が前ほど高スコア。
    if (const std::size_t position = lowerText.find(lowerQuery);
        position != std::string::npos) {
        return std::max(1000 - static_cast<int>(position) * 5, 300);
    }

    /// @note 部分列一致 (曖昧検索)
    return SubsequenceScore(lowerText, lowerQuery);
}

std::vector<AssetSearchHit> AssetSearch::Query(std::string_view query,
                                               std::span<const std::string> extensions,
                                               std::size_t maxResults)
{
    const SearchContext& context = Ctx();

    std::vector<AssetSearchHit> hits;
    hits.reserve(std::min(maxResults, context.entries.size()));

    const auto extensionAllowed = [&extensions](const std::string& extension) {
        if (extensions.empty()) return true;
        for (const std::string& allowed : extensions)
            if (allowed == extension) return true;
        return false;
    };

    for (const AssetSearchEntry& entry : context.entries) {
        if (!extensionAllowed(entry.extension)) continue;

        /// @note ファイル名を主対象にし、当たらなければ相対パスでも見る ("UI/Button" のようにフォルダ名で絞りたい場合があるため)。パス一致はファイル名一致より弱く扱う (スコアを割り引く)。
        int score = Match(entry.filename, query);
        if (score == 0) {
            const int pathScore = Match(entry.relativePath, query);
            score = pathScore == 0 ? 0 : pathScore / 4;
        }
        if (score == 0) continue;

        hits.push_back({ &entry, score });
    }

    /// @note スコア降順。同点はパス順で安定させる: 順序が揺れると同じ検索語でも開くたびに並びが変わり「さっき上にあった項目」が消えたように見える。
    std::stable_sort(hits.begin(), hits.end(),
        [](const AssetSearchHit& lhs, const AssetSearchHit& rhs) {
            if (lhs.score != rhs.score) return lhs.score > rhs.score;
            return lhs.entry->relativePath < rhs.entry->relativePath;
        });

    if (hits.size() > maxResults) hits.resize(maxResults);
    return hits;
}

std::vector<std::string> AssetSearch::ParseExtensionFilter(std::string_view csv)
{
    std::vector<std::string> result;
    std::string current;

    const auto flush = [&]() {
        if (current.empty()) return;
        std::string extension = util::StringUtils::ToLower(current);
        /// @note ドットを補う ("mat" → ".mat")
        if (extension[0] != '.') extension.insert(extension.begin(), '.');
        result.push_back(std::move(extension));
        current.clear();
    };

    for (const char c : csv) {
        if (c == ',' || c == ';' || c == ' ') flush();
        else current.push_back(c);
    }
    flush();

    return result;
}

} // namespace fbzz::editor
