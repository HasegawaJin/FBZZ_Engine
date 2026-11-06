/// @file    AssetDatabase.cpp
/// @brief   GUID ⇄ アセットパスの双方向インデックス実装。
/// @author  Hasegawa Jin
/// @date    2026-07-08
#include <Engine/Asset/ModelSource.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/MetaFile.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Concurrency/TaskSystem.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <random>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fbzz::asset {

namespace {

/// @note 双方向インデックス。キーは正規化 (小文字ドライブ + '/' 区切り) した絶対パス。
std::unordered_map<std::string, std::string> s_guidToPath;
std::unordered_map<std::string, std::string> s_pathToGuid;
/// @brief Immutable Engine assets stay outside the project index and its writeback file.
std::unordered_map<std::string, std::string> s_sharedGuidToPath;
std::unordered_map<std::string, std::string> s_sharedPathToGuid;
std::unordered_set<std::string> s_ambiguousSharedGuids;
/// @note リネーム直後に開いている Scene が旧パスを保持していても、保存時に GUID 化できるよう
/// @note 旧パスを一時的に GUID へ解決する。新しいアセットが同じ場所へ作られたら消費する。
std::unordered_map<std::string, std::string> s_movedPathAliases;
std::mutex  s_mutex;
uint64_t    s_revision = 0;
bool        s_initialized = false;
std::string s_assetsRoot;  ///< @note Init に渡された Assets ルート (末尾 '/' 付き正規化)
std::string s_sharedAssetsRoot; ///< @brief 読み取り専用 Engine Assets ルート (末尾 '/' 付き正規化)
/// @note 索引が最後の書き出し以降に変わったか。FlushIndexFile が見る。
bool        s_indexDirty = false;
/// @note 先勝ちで弾いた重複の記録。修復ツールと通知バーが読む。
std::vector<AssetDatabase::GuidConflict> s_conflicts;

/// @pre 呼び出し側が s_mutex を保持していること。
void SetMappingLocked(std::unordered_map<std::string, std::string>& index,
                       const std::string& key, const std::string& value, bool overwrite = true)
{
    const auto [entry, inserted] = index.try_emplace(key, value);
    if (inserted) {
        ++s_revision;
    } else if (overwrite && entry->second != value) {
        entry->second = value;
        ++s_revision;
    }
}

/// @pre 呼び出し側が s_mutex を保持していること。
void EraseMappingLocked(std::unordered_map<std::string, std::string>& index,
                         const std::string& key)
{
    if (index.erase(key) != 0) ++s_revision;
}

/// @pre 呼び出し側が s_mutex を保持していること。
void ClearSharedMappingsLocked()
{
    if (!s_sharedGuidToPath.empty() || !s_sharedPathToGuid.empty() || !s_ambiguousSharedGuids.empty())
        ++s_revision;
    s_sharedGuidToPath.clear();
    s_sharedPathToGuid.clear();
    s_ambiguousSharedGuids.clear();
}

std::string NormalizePath(std::string p)
{
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

/// @note パス索引キー。Windows はパス大文字小文字非区別のため小文字化して同一視する。
std::string PathKey(const std::string& absPath)
{
    return util::StringUtils::ToLowerCopy(NormalizePath(absPath));
}

/// @note `<asset>.meta` から guid を読む。無ければ空文字列。
std::string ReadGuidFromMeta(const std::string& metaPath)
{
    std::string text;
    if (!util::FileSystem::ReadText(metaPath, text)) return {};
    std::istringstream iss(text);
    const auto parsed = toml::parse(iss);
    if (!parsed) return {};
    if (auto v = parsed.table()["meta"]["guid"].value<std::string>())
        return util::StringUtils::ToLowerCopy(*v);
    return {};
}

/// @note .meta に [meta] guid を書き込む。既存の他セクション ([texture] 等) は保持する。
bool WriteGuidToMeta(const std::string& metaPath, const std::string& guid)
{
    toml::table root;
    std::string existing;
    if (util::FileSystem::ReadText(metaPath, existing)) {
        std::istringstream iss(existing);
        const auto parsed = toml::parse(iss);
        if (parsed) root = parsed.table();
    }

    auto* meta = root["meta"].as_table();
    if (!meta) {
        toml::table metaTbl;
        metaTbl.insert("guid", guid);
        root.insert("meta", std::move(metaTbl));
    } else {
        meta->insert_or_assign("guid", guid);
    }

    std::ostringstream ss;
    ss << root;
    return MetaFile::Save(metaPath, ss.str());
}

/// @brief `Foo.fbx` のインポートで生成される従属フォルダ `Foo/` か判定する。
/// @note 従属フォルダは原本モデルから再生成されるため独立した guid を持たせない。持たせると
/// @note 再インポートのたびに孤児 .meta が残り索引が汚れる。
bool IsGeneratedModelPackageDir(const std::filesystem::path& dir)
{
    namespace fs = std::filesystem;
    const std::string stem = util::FileSystem::PathToUtf8(dir.filename());
    if (stem.empty()) return false;

    /// @note 兄弟に同名のモデル原本があれば、このフォルダはその出力先。
    static constexpr const char* kModelExts[] = { ".fbx", ".obj", ".gltf", ".glb" };
    for (const char* ext : kModelExts) {
        if (util::FileSystem::Exists(dir.parent_path() / (stem + ext))) return true;
    }
    /// @note 原本が消えていても、内部コンテナが残っていれば生成物と分かる。
    std::error_code ec;
    return fs::exists(dir / (stem + ".fzasset"), ec);
}

/// @brief 本体が存在しない孤児 .meta か。索引には載せず、削除もしない。
/// @note 大容量バイナリ (png/fbx/wav) を .gitignore しつつ .meta だけ追跡するリポジトリでは
/// @note clone 直後に本体不在の .meta が孤児として見える。ここで消すと guid 参照が一斉に切れ、
/// @note 入れ直しても新しい乱数 guid になり二度と戻らない。残せば本体が戻った時点で同じ
/// @note guid のまま復帰する (エディター内の削除は本体と対で移すため、ここへ来るのは
/// @note git / エクスプローラーで消された場合だけ)。代償として、同じパスへ別のアセットを
/// @note 作り直すと残った guid を引き継ぐ (ファイル入れ替えなら望ましい挙動として受け入れる)。
bool IsOrphanMeta(const std::filesystem::path& metaPath)
{
    const std::string metaUtf8 = util::FileSystem::PathToUtf8(metaPath);
    /// @note "Foo.png.meta" → "Foo.png" / "Textures.meta" → "Textures" (ディレクトリ)
    return !util::FileSystem::Exists(metaUtf8.substr(0, metaUtf8.size() - 5));
}

/// @brief ロック取得済み前提。同じ (guid, 弾かれたパス) を二重に積まない。
/// @note 索引の再構築や watcher の再通知で同じ衝突が何度も通る。件数がそのたびに
/// @note 増えると、通知バーの「N 件」が実際の重複数と合わなくなる。
void RecordConflictLocked(const std::string& guid, const std::string& keptPath,
                          const std::string& duplicatePath)
{
    const std::string dupKey = PathKey(duplicatePath);
    for (const AssetDatabase::GuidConflict& c : s_conflicts) {
        if (c.guid == guid && PathKey(c.duplicatePath) == dupKey) return;
    }
    s_conflicts.push_back({ guid, NormalizePath(keptPath), NormalizePath(duplicatePath) });
}

/// @brief `…/Assets/` 以降を返す。含まなければ空。
/// @note 索引には絶対パスが載るが、参照解決は `Assets/Models/Foo.fbx` のような論理パスの
/// @note まま入ってくることがあるため、先頭の `Assets/` も見る。区切りだけを探すと、
/// @note 先頭に来た `Assets/` を取りこぼして «別物» と判定する。
std::string RelativeToAnyAssetsRoot(const std::string& absPath)
{
    static constexpr std::string_view kLeading = "assets/";
    static constexpr std::string_view kEmbedded = "/assets/";

    const std::string lower = util::StringUtils::ToLowerCopy(NormalizePath(absPath));
    if (lower.rfind(kLeading, 0) == 0) return lower.substr(kLeading.size());
    const size_t pos = lower.rfind(kEmbedded);
    if (pos == std::string::npos) return {};
    return lower.substr(pos + kEmbedded.size());
}

/// @brief 同じ guid を名乗る 2 つのパスが、同じアセットの «コピー» か。
/// @note エンジンは共通シェーダーをプロジェクトの Assets へ配る。コピーは .meta ごと複製
/// @note されるので guid も同一になるが、これは «同じアセットが 2 箇所にある» のであって
/// @note 参照が別物へ吸われる本物の衝突ではない。区別せずに重複として鳴らすと、起動の
/// @note たびにエラーが出るうえ «直す» と guid が振り直されてどちらかの参照が本当に壊れる。
bool IsSameAssetCopyLocked(const std::string& a, const std::string& b)
{
    const std::string relA = RelativeToAnyAssetsRoot(a);
    return !relA.empty() && relA == RelativeToAnyAssetsRoot(b);
}

/// @note absPath が «開いているプロジェクトの» Assets 配下か。
bool IsUnderProjectAssetsLocked(const std::string& absPath)
{
    if (s_assetsRoot.empty()) return false;
    const std::string key = PathKey(absPath);
    return key.rfind(util::StringUtils::ToLowerCopy(s_assetsRoot), 0) == 0;
}

/// @brief 共有 Engine Assets 配下か。
/// @pre 呼び出し側がロックを保持していること。
bool IsUnderSharedAssetsLocked(const std::string& absPath)
{
    if (s_sharedAssetsRoot.empty()) return false;
    std::error_code error;
    const auto path = util::FileSystem::PathFromUtf8(absPath);
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    const std::string key = PathKey(util::FileSystem::PathToUtf8(
        error ? path.lexically_normal() : canonical));
    const std::string root = util::StringUtils::ToLowerCopy(s_sharedAssetsRoot);
    return key == root.substr(0, root.size() - 1) || key.rfind(root, 0) == 0;
}

/// @brief 読み取り専用の共有索引へ登録する。
/// @note 異なる実体が同じ GUID を名乗る場合は解決を拒否する。
void RegisterSharedLocked(const std::string& guid, const std::string& absPath)
{
    const std::string key = PathKey(absPath);
    if (s_ambiguousSharedGuids.contains(guid)) return;

    if (const auto project = s_guidToPath.find(guid); project != s_guidToPath.end()) {
        const std::string projectKey = PathKey(project->second);
        std::error_code equivalentError;
        const bool sameFile = std::filesystem::equivalent(
            util::FileSystem::PathFromUtf8(project->second),
            util::FileSystem::PathFromUtf8(absPath), equivalentError) && !equivalentError;
        if (projectKey == key || sameFile
            || IsSameAssetCopyLocked(project->second, absPath)) {
            SetMappingLocked(s_sharedPathToGuid, key, guid, false);
            return;
        }

        FBZZ_LOG_ERROR("AssetDatabase: duplicate guid [%s]\n  kept: %s\n  dup : %s",
                       guid.c_str(), project->second.c_str(), absPath.c_str());
        RecordConflictLocked(guid, project->second, absPath);
        SetMappingLocked(s_sharedPathToGuid, key, guid, false);
        return;
    }

    const auto it = s_sharedGuidToPath.find(guid);
    if (it != s_sharedGuidToPath.end() && PathKey(it->second) != key) {
        if (std::error_code error; std::filesystem::equivalent(
                util::FileSystem::PathFromUtf8(it->second),
                util::FileSystem::PathFromUtf8(absPath), error) && !error) {
            SetMappingLocked(s_sharedPathToGuid, key, guid, false);
            return;
        }

        s_sharedGuidToPath.erase(it);
        ++s_revision;
        if (s_ambiguousSharedGuids.insert(guid).second) ++s_revision;
        EraseMappingLocked(s_sharedPathToGuid, PathKey(absPath));
        FBZZ_LOG_ERROR("AssetDatabase: ambiguous shared Engine guid [%s]", guid.c_str());
        return;
    }

    SetMappingLocked(s_sharedGuidToPath, guid, NormalizePath(absPath), false);
    SetMappingLocked(s_sharedPathToGuid, key, guid, false);
}

/// @brief ロック取得済み前提でインデックスに登録する。guid 重複はエラーログを出し先勝ち。
/// @note primary=false のときは「別名」として扱い、path → guid の逆引きは上書きしない。
/// @note 移行期には 1 つの実体を 2 つの GUID が指しうる (旧: .meta に書かれた乱数 / 新: 原本
/// @note から導出)。参照側は旧 GUID で書かれているのでどちらからも引けなければならないが、
/// @note 保存する値は新 GUID に一本化したい。guid→path は多対一を許し、path→guid は
/// @note primary だけが書く。
void RegisterLocked(const std::string& guid, const std::string& absPath, bool primary = true)
{
    const std::string key = PathKey(absPath);
    EraseMappingLocked(s_movedPathAliases, key);
    s_indexDirty = true;
    const auto it = s_guidToPath.find(guid);
    if (it != s_guidToPath.end() && PathKey(it->second) != key) {
        /// @note 外部ツールの移動で Renamed 通知が欠落した場合、古い実体が消えていれば
        /// @note 新しいパスを同じアセットとして受け入れる。両方が存在する場合は本当の
        /// @note GUID 重複なので、先勝ちを維持して誤った参照の乗っ取りを防ぐ。
        if (!util::FileSystem::Exists(it->second)) {
            EraseMappingLocked(s_pathToGuid, PathKey(it->second));
            SetMappingLocked(s_guidToPath, guid, NormalizePath(absPath));
            SetMappingLocked(s_pathToGuid, key, guid, primary);
            return;
        }
        /// @note 同じ実体を 2 通りのパス表記で登録しただけなら、そもそも衝突ではない。相対と
        /// @note 絶対、ドライブ文字や中間ディレクトリの大文字小文字、ジャンクション経由 —
        /// @note どれも «違う文字列で同じファイル» を作り、PathKey の正規化 (小文字化と区切りの
        /// @note 統一) では吸収できないため、ファイルシステムに «同じものか» を直接聞く。
        if (std::error_code ec; std::filesystem::equivalent(
                util::FileSystem::PathFromUtf8(it->second),
                util::FileSystem::PathFromUtf8(absPath), ec) && !ec) {
            /// @note 実体は 1 つ。guid→path は先に入った表記を保ち、逆引きだけ増やす。
            SetMappingLocked(s_pathToGuid, key, guid, primary);
            return;
        }
        /// @note エンジン内蔵 Assets とプロジェクト Assets に配られた同じアセット。
        /// @note 衝突ではないので記録も警告もせず、プロジェクト側を «実体» として採用する。
        if (IsSameAssetCopyLocked(it->second, absPath)) {
            if (IsUnderProjectAssetsLocked(absPath)
                && !IsUnderProjectAssetsLocked(it->second))
                SetMappingLocked(s_guidToPath, guid, NormalizePath(absPath));
            /// @note 逆引きはどちらのパスからも引けてよい。参照はパスで来ることがある。
            SetMappingLocked(s_pathToGuid, key, guid, primary);
            return;
        }
        FBZZ_LOG_ERROR("AssetDatabase: duplicate guid [%s]\n  kept: %s\n  dup : %s",
                       guid.c_str(), it->second.c_str(), absPath.c_str());
        RecordConflictLocked(guid, it->second, NormalizePath(absPath));
        return;
    }
    SetMappingLocked(s_guidToPath, guid, NormalizePath(absPath));
    SetMappingLocked(s_pathToGuid, key, guid, primary);
}

}

bool AssetDatabase::ShouldHaveMeta(std::string_view lowerFileName)
{
    /// @note 拡張子を持たないものは対象外。`.gitignore` のような管理ファイルと `LICENSE` の
    /// @note ような無拡張ファイルがここに落ちる。`.playmode_snapshot.scene` のようにドット
    /// @note 始まりでも拡張子を持つ実アセットがあるため「ドット始まりは全部除外」にはできない。
    const size_t dot = lowerFileName.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return false;
    const std::string_view ext = lowerFileName.substr(dot);

    /// @note 拡張子として妥当な形だけを通す。EncodeGuidRefs はドキュメント中の全文字列に
    /// @note この判定を掛けるため、`Player.001` や `Score: 0.5` のような値までアセット候補
    /// @note としてディスクを叩きに行かないよう、英字を含む短い英数字列に限る。
    if (ext.size() < 2 || ext.size() > 17) return false;
    bool hasAlpha = false;
    for (const char c : ext.substr(1)) {
        const auto uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc)) return false;
        if (std::isalpha(uc)) hasAlpha = true;
    }
    if (!hasAlpha) return false;

    /// @note 許可リストではなく除外リストにする。「Asset Browser には出るし Inspector でも
    /// @note 選べるのに ShouldHaveMeta に無く、guid が振られないので参照が壊れる」不具合を
    /// @note 拡張子を足すたびに繰り返してきた (Docs/design/audio-system.md の .flac)。
    /// @note .md / .txt / .pdf のような資料も含め、原本は等しく GUID を持たせ、
    /// @note 再生成できる派生物だけを名指しで除外する。
    static constexpr std::string_view kExcludedExts[] = {
        ".meta",
        /// @note import / shader compile の生成物。GUID は原本の GUID から導出される (DeriveGuid)。
        ".fzasset", ".mesh", ".skel", ".cso",
        /// @note ビルド生成物と作業用の一時ファイル。
        ".dll", ".lib", ".pdb", ".exp", ".ilk", ".exe", ".tmp", ".bak",
    };
    for (const auto e : kExcludedExts)
        if (ext == e) return false;

    /// @note ScriptCodeGen の出力。原本の .hpp から再生成されるので独立した GUID を持たない。
    static constexpr std::string_view kExcludedSuffixes[] = { ".generated.hpp" };
    for (const auto s : kExcludedSuffixes)
        if (lowerFileName.size() >= s.size()
            && lowerFileName.substr(lowerFileName.size() - s.size()) == s) return false;

    /// @note 原子的な保存の途中経過 (`Stage_01.scene.tmp.<pid>.<hash>`)。一時名は .tmp の
    /// @note «後ろ» に pid とハッシュを足すため、末尾の `.tmp` 判定では足りず拡張子として
    /// @note 見えるのは `.<hash>` になる。英字を含む短い英数字列なので上の書式検査を通って
    /// @note しまい、rename で消える相手に .meta を発行して孤児が残る。
    if (lowerFileName.find(".tmp.") != std::string_view::npos) return false;

    return true;
}

bool AssetDatabase::ShouldHaveFolderMeta(std::string_view folderName)
{
    if (folderName.empty()) return false;

    /// @note ドット始まりは VCS / エディター内部の管理ディレクトリ (.git / .import_presets 等)。
    if (folderName.front() == '.') return false;

    /// @note 再生成される中間・出力物。guid を振っても参照先として意味を持たない。
    static constexpr std::string_view kExcluded[] = {
        "library", "build", "temp", "obj", "bin", "intermediate", "compiled",
    };
    std::string lower;
    lower.reserve(folderName.size());
    for (const char c : folderName)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    for (const auto e : kExcluded)
        if (lower == e) return false;

    return true;
}

std::string AssetDatabase::GenerateGuid()
{
    /// @note 128bit 乱数 → 32 桁 hex (Unity と同形式)。暗号強度は不要なので mt19937_64 で足りる。
    static std::mt19937_64 gen{ std::random_device{}() };
    static std::mutex genMutex;
    std::lock_guard lock(genMutex);
    const uint64_t hi = gen();
    const uint64_t lo = gen();
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  static_cast<unsigned long long>(hi),
                  static_cast<unsigned long long>(lo));
    return std::string(buf, 32);
}

std::string AssetDatabase::DeriveGuid(std::string_view sourceGuid, std::string_view subKey)
{
    if (sourceGuid.empty() || subKey.empty()) return {};

    /// @note FNV-1a を 2 系統 (offset basis 違い) で回して 128bit を作る。GuidRefCodec /
    /// @note FbxMetaSerializer が既に FNV-1a を使っており、新しいハッシュを持ち込まずに済み
    /// @note 実装の妥当性を読み手が確かめやすい。
    constexpr uint64_t kPrime = 1099511628211ull;
    auto fold = [&](uint64_t hash) {
        for (const char c : sourceGuid) { hash ^= static_cast<unsigned char>(c); hash *= kPrime; }
        hash ^= '/'; hash *= kPrime;
        for (const char c : subKey)     { hash ^= static_cast<unsigned char>(c); hash *= kPrime; }
        return hash;
    };
    const uint64_t hi = fold(14695981039346656037ull);
    const uint64_t lo = fold(1469598103934665603ull);

    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  static_cast<unsigned long long>(hi),
                  static_cast<unsigned long long>(lo));
    return std::string(buf, 32);
}

/// @brief s_mutex を呼び出し側が保持している前提の実体。
/// @note Init は索引の登録中にロックを持つため、そこから公開 API を呼ぶと std::mutex は
/// @note 再帰不可なのでデッドロックする。分けて内部専用にすることでそれを避ける。
static void IndexBakedLibraryLocked(const std::string& libraryBakedRoot)
{
    namespace fs = std::filesystem;
    if (libraryBakedRoot.empty()) return;
    const fs::path root = util::FileSystem::PathFromUtf8(libraryBakedRoot);
    if (!util::FileSystem::Exists(root)) return;

    size_t indexed = 0;

    /// @note `Library/Baked/<sourceGuid>/<subKey...>`
    /// @note ディレクトリ名がそのまま原本の GUID。.meta は読まない (存在しない)。
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), last; !ec && it != last; it.increment(ec)) {
        std::error_code dirEc;
        if (!it->is_directory(dirEc) || dirEc) continue;

        const std::string sourceGuid =
            util::StringUtils::ToLowerCopy(util::FileSystem::PathToUtf8(it->path().filename()));
        /// @note guid ディレクトリ以外は無視
        if (sourceGuid.size() != 32) continue;

        for (const fs::path& file : util::FileSystem::ListFilesRecursive(it->path())) {
            const std::string absPath = NormalizePath(util::FileSystem::PathToUtf8(file));
            const std::string ext = util::StringUtils::ToLowerCopy(util::FileSystem::GetExtension(absPath));
            /// @note 参照されうるものだけ索引する。.fzasset/.mesh/.skel は原本の GUID から
            /// @note 引かれるので個別の GUID を持たない。
            /// @note 画像は .mat が guid: で参照するため索引が要る。
            static constexpr std::string_view kIndexed[] = {
                ".anim", ".mat",
                ".png", ".jpg", ".jpeg", ".tga", ".dds", ".bmp", ".hdr", ".exr",
            };
            bool indexable = false;
            for (const auto candidate : kIndexed)
                if (ext == candidate) { indexable = true; break; }
            if (!indexable) continue;

            /// @note 専用の error_code を使う。外側のイテレータ用 ec を使い回すと、relative の
            /// @note 失敗でループ条件 (!ec) が偽になり走査が途中で止まる。
            std::error_code relEc;
            const std::string subKey = util::FileSystem::NormalizePathSeparators(
                util::FileSystem::PathToUtf8(fs::relative(file, it->path(), relEc)));
            if (relEc || subKey.empty()) continue;

            RegisterLocked(AssetDatabase::DeriveGuid(sourceGuid, subKey), absPath);
            ++indexed;
        }
    }

    if (indexed > 0)
        FBZZ_LOG_INFO("AssetDatabase: indexed %zu baked sub-assets under [%s]",
                      indexed, libraryBakedRoot.c_str());
}

void AssetDatabase::IndexBakedLibrary(const std::string& libraryBakedRoot)
{
    std::lock_guard lock(s_mutex);
    IndexBakedLibraryLocked(libraryBakedRoot);
}

namespace {

/// @brief Shared roots may be the Assets directory itself or a staged EngineAssets parent.
std::string NormalizeSharedAssetsRoot(const std::string& sharedAssetsRoot)
{
    if (sharedAssetsRoot.empty()) return {};

    std::filesystem::path root = util::FileSystem::PathFromUtf8(sharedAssetsRoot);
    std::error_code error;
    const std::filesystem::path nestedAssets = root / L"Assets";
    if (std::filesystem::is_directory(nestedAssets, error) && !error)
        root = nestedAssets;

    error.clear();
    const auto canonical = std::filesystem::weakly_canonical(root, error);
    root = error ? root.lexically_normal() : canonical;

    std::string normalized = NormalizePath(util::FileSystem::PathToUtf8(root));
    if (!normalized.empty() && normalized.back() != '/') normalized.push_back('/');
    return normalized;
}

/// @brief Shared assets are indexed from existing metadata only.
/// @note This pass never repairs or writes files.
void IndexSharedAssetsLocked()
{
    namespace fs = std::filesystem;
    if (s_sharedAssetsRoot.empty()) return;

    const fs::path root = util::FileSystem::PathFromUtf8(s_sharedAssetsRoot);
    std::error_code error;
    if (!fs::is_directory(root, error) || error) {
        FBZZ_LOG_WARN("AssetDatabase: shared Engine assets unavailable [%s]",
                      s_sharedAssetsRoot.c_str());
        return;
    }

    const auto indexFile = [](const std::string& absPath) {
        const std::string filename = util::StringUtils::ToLowerCopy(util::FileSystem::GetFilename(absPath));
        if (!AssetDatabase::ShouldHaveMeta(filename)) return;
        const std::string guid = ReadGuidFromMeta(absPath + ".meta");
        if (!guid.empty()) RegisterSharedLocked(guid, absPath);
    };

    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, error);
    const fs::recursive_directory_iterator last;
    while (!error && it != last) {
        const fs::directory_entry& entry = *it;
        if (entry.is_regular_file(error) && !error) {
            const std::string absPath =
                NormalizePath(util::FileSystem::PathToUtf8(entry.path()));
            if (util::StringUtils::ToLowerCopy(util::FileSystem::GetExtension(absPath)) != ".meta")
                indexFile(absPath);
        } else if (error) {
            break;
        }
        it.increment(error);
    }
    if (error) {
        ClearSharedMappingsLocked();
        FBZZ_LOG_ERROR("AssetDatabase: shared Engine asset scan failed [%s]",
                       error.message().c_str());
        return;
    }

    error.clear();
    fs::recursive_directory_iterator dirIt(
        root, fs::directory_options::skip_permission_denied, error);
    while (!error && dirIt != last) {
        const fs::directory_entry& entry = *dirIt;
        std::error_code directoryError;
        if (entry.is_directory(directoryError) && !directoryError) {
            const std::string name = util::FileSystem::PathToUtf8(entry.path().filename());
            if (!AssetDatabase::ShouldHaveFolderMeta(name)
                || IsGeneratedModelPackageDir(entry.path())) {
                dirIt.disable_recursion_pending();
            } else {
                const std::string absPath =
                    NormalizePath(util::FileSystem::PathToUtf8(entry.path()));
                const std::string guid = ReadGuidFromMeta(absPath + ".meta");
                if (!guid.empty()) RegisterSharedLocked(guid, absPath);
            }
        }
        dirIt.increment(error);
    }
    if (error) {
        ClearSharedMappingsLocked();
        FBZZ_LOG_ERROR("AssetDatabase: shared Engine folder scan failed [%s]",
                       error.message().c_str());
    }
}

}

void AssetDatabase::Init(const std::string& assetsRoot)
{
    Init(assetsRoot, {});
}

void AssetDatabase::Init(const std::string& assetsRoot,
                         const std::string& sharedAssetsRoot)
{
    namespace fs = std::filesystem;
    const fs::path root = util::FileSystem::PathFromUtf8(assetsRoot);
    const auto files = util::FileSystem::ListFilesRecursive(root);
    struct MetaRead {
        std::string path;
        std::string guid;
    };
    std::vector<MetaRead> metaReads;
    metaReads.reserve(files.size());
    for (const auto& file : files) {
        const std::string path = NormalizePath(util::FileSystem::PathToUtf8(file));
        if (ShouldHaveMeta(util::StringUtils::ToLowerCopy(util::FileSystem::GetFilename(path))))
            metaReads.push_back({path, {}});
    }
    /// @note Only independent reads run on workers; repairs and duplicate winners retain traversal order.
    /// @note Finish before taking s_mutex so queued asset tasks cannot deadlock the shared pool.
    const size_t readerCount = metaReads.size() < 64 ? 0
        : static_cast<size_t>((std::min)(8, TaskSystem::WorkerCount()));
    std::vector<std::future<void>> reads;
    for (size_t reader = 0; reader < readerCount; ++reader) {
        reads.push_back(TaskSystem::Submit([&, reader] {
            for (size_t i = reader; i < metaReads.size(); i += readerCount)
                metaReads[i].guid = ReadGuidFromMeta(metaReads[i].path + ".meta");
        }));
    }
    if (readerCount == 0) {
        for (auto& meta : metaReads) meta.guid = ReadGuidFromMeta(meta.path + ".meta");
    }
    for (auto& read : reads) read.get();
    std::lock_guard lock(s_mutex);
    ++s_revision;
    s_guidToPath.clear();
    s_pathToGuid.clear();
    s_sharedGuidToPath.clear();
    s_sharedPathToGuid.clear();
    s_ambiguousSharedGuids.clear();
    s_movedPathAliases.clear();
    s_conflicts.clear();
    s_assetsRoot = NormalizePath(assetsRoot);
    if (!s_assetsRoot.empty() && s_assetsRoot.back() != '/') s_assetsRoot.push_back('/');
    s_sharedAssetsRoot = NormalizeSharedAssetsRoot(sharedAssetsRoot);

    if (!util::FileSystem::Exists(root)) {
        FBZZ_LOG_WARN("AssetDatabase: assets root not found [%s]", assetsRoot.c_str());
        IndexSharedAssetsLocked();
        s_initialized = true;
        return;
    }

    size_t healed  = 0;
    size_t orphans = 0;

    /// @note .meta を持つべき対象を 1 件処理する共通クロージャ (ファイル / フォルダ共通)。
    const auto indexTarget = [&](const std::string& absPath, std::string guid) {
        const std::string metaPath = absPath + ".meta";
        if (guid.empty()) {
            /// @note モデル原本は Import 実行時に GUID を確定する。AssetBrowser を開いただけで
            /// @note 原本の隣に .meta を生成すると、未 Import と Import 済みの境界が崩れる。
            if (asset::IsModelSourcePath(absPath)) return;
            /// @note 自己修復: .meta が無い / guid が無い対象に新規発行する。
            guid = GenerateGuid();
            if (!WriteGuidToMeta(metaPath, guid)) {
                FBZZ_LOG_WARN("AssetDatabase: cannot write meta [%s]", metaPath.c_str());
                return;
            }
            ++healed;
        }
        RegisterLocked(guid, absPath);
    };

    size_t metaIndex = 0;
    for (const fs::path& p : files) {
        const std::string absPath = NormalizePath(util::FileSystem::PathToUtf8(p));
        const std::string ext = util::StringUtils::ToLowerCopy(util::FileSystem::GetExtension(absPath));

        /// @note 孤児 .meta の検出は索引構築と同じ 1 パスで行う (件数はログにだけ出す)。
        if (ext == ".meta") {
            if (IsOrphanMeta(p)) ++orphans;
            continue;
        }

        if (!ShouldHaveMeta(util::StringUtils::ToLowerCopy(util::FileSystem::GetFilename(absPath)))) continue;
        indexTarget(absPath, std::move(metaReads[metaIndex++].guid));
    }

    /// @name フォルダの .meta
    /// @note フォルダも参照される (デフォルト保存先・検索スコープ)。パスで覚えるとリネームや
    /// @note 移動で参照が切れるため、ファイルと同じ guid 方式に揃える。走査はファイルと分けて
    /// @note いる (ListFilesRecursive がファイルのみを返すため)。
    {
        /// @note range-for は begin() が反復子のコピーを返すため disable_recursion_pending が
        /// @note 効かない。除外フォルダの配下を辿らないよう、明示的な while ループで回す。
        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator last;
        while (!ec && it != last) {
            const fs::directory_entry& entry = *it;

            std::error_code dirEc;
            if (entry.is_directory(dirEc) && !dirEc) {
                const fs::path&   dir  = entry.path();
                const std::string name = util::FileSystem::PathToUtf8(dir.filename());
                /// @note 除外フォルダと、モデルインポートの従属フォルダは配下ごと辿らない。
                if (!ShouldHaveFolderMeta(name) || IsGeneratedModelPackageDir(dir))
                    it.disable_recursion_pending();
                else
                    indexTarget(NormalizePath(util::FileSystem::PathToUtf8(dir)),
                        ReadGuidFromMeta(util::FileSystem::PathToUtf8(dir) + ".meta"));
            }

            it.increment(ec);
        }
        if (ec) {
            FBZZ_LOG_WARN("AssetDatabase: folder scan stopped [%s]", ec.message().c_str());
        }
    }

    s_initialized = true;
    FBZZ_LOG_INFO("AssetDatabase: indexed %zu assets (%zu meta healed, %zu orphan meta kept) under [%s]",
                  s_guidToPath.size(), healed, orphans, assetsRoot.c_str());
    /// @note 孤児が多い＝本体がローカルに揃っていない (LFS 未取得 / 素材が git 管理外) 状態。
    /// @note 参照切れの原因がここにあると分かるよう、黙って進めずに一度だけ警告する。
    if (orphans > 0)
        FBZZ_LOG_WARN("AssetDatabase: %zu meta have no owner asset. "
                      "References to them stay unresolved until the assets are restored.",
                      orphans);
    /// @note 重複は 1 件ごとにエラーを出しているが、スキャン中の行は起動ログに流れて気づかれない。
    /// @note 総数だけをもう一度、通知バーと同じ文言で残す。
    if (!s_conflicts.empty())
        FBZZ_LOG_ERROR("AssetDatabase: %zu duplicate guid(s). "
                       "References resolve to one side only \xe2\x80\x94 "
                       "fix them in Tools > Asset Maintenance.",
                       s_conflicts.size());

    /// @name Library/Baked の生成物を導出 GUID で索引する
    /// @note Assets の後に索引する。同じ実体が両方に居る移行期に、Assets 側 (人が編集しうる方)
    /// @note の path→guid を優先させるため。RegisterLocked は先勝ちではないが、guid が違えば
    /// @note 衝突しないので順序で意味を持つのは path→guid だけ。
    if (s_assetsRoot.size() > 7) {
        const std::string projectRoot = s_assetsRoot.substr(0, s_assetsRoot.size() - 7);
        IndexBakedLibraryLocked(projectRoot + "Library/Baked");
    }
    IndexSharedAssetsLocked();
    s_indexDirty = true;
}

void AssetDatabase::Shutdown()
{
    std::lock_guard lock(s_mutex);
    if (s_initialized || !s_guidToPath.empty() || !s_pathToGuid.empty()
        || !s_sharedGuidToPath.empty() || !s_sharedPathToGuid.empty()
        || !s_ambiguousSharedGuids.empty() || !s_movedPathAliases.empty()
        || !s_sharedAssetsRoot.empty())
        ++s_revision;
    s_guidToPath.clear();
    s_pathToGuid.clear();
    s_sharedGuidToPath.clear();
    s_sharedPathToGuid.clear();
    s_ambiguousSharedGuids.clear();
    s_movedPathAliases.clear();
    s_conflicts.clear();
    s_sharedAssetsRoot.clear();
    s_initialized = false;
}

uint64_t AssetDatabase::Revision()
{
    std::lock_guard lock(s_mutex);
    return s_revision;
}

std::string AssetDatabase::PathFromGuid(const std::string& guid)
{
    std::lock_guard lock(s_mutex);
    const std::string key = util::StringUtils::ToLowerCopy(guid);
    if (const auto it = s_guidToPath.find(key); it != s_guidToPath.end())
        return it->second;
    if (s_ambiguousSharedGuids.contains(key)) return {};
    const auto shared = s_sharedGuidToPath.find(key);
    return shared != s_sharedGuidToPath.end() ? shared->second : std::string{};
}

std::string AssetDatabase::GuidFromPath(const std::string& absPath)
{
    const std::string existingGuid = TryGetGuidFromPath(absPath);
    if (!existingGuid.empty()) return existingGuid;

    {
        std::lock_guard lock(s_mutex);
        if (IsUnderSharedAssetsLocked(absPath)) return {};
    }

    /// @note 未登録: インデックス構築後に追加されたアセット。自己修復して登録する。
    /// @note 実在しないパスに .meta を作らないよう必ず存在確認する。
    if (!util::FileSystem::Exists(absPath)) return {};

    if (util::FileSystem::IsDirectory(absPath)) {
        const std::filesystem::path dir = util::FileSystem::PathFromUtf8(absPath);
        if (!ShouldHaveFolderMeta(util::FileSystem::PathToUtf8(dir.filename()))) return {};
        if (IsGeneratedModelPackageDir(dir)) return {};
    } else {
        if (!ShouldHaveMeta(util::StringUtils::ToLowerCopy(util::FileSystem::GetFilename(absPath)))) return {};
    }

    const std::string metaPath = absPath + ".meta";
    std::string guid = ReadGuidFromMeta(metaPath);
    if (guid.empty()) {
        guid = GenerateGuid();
        if (!WriteGuidToMeta(metaPath, guid)) {
            FBZZ_LOG_WARN("AssetDatabase: cannot write meta [%s]", metaPath.c_str());
            return {};
        }
    }
    std::lock_guard lock(s_mutex);
    RegisterLocked(guid, absPath);
    return guid;
}

std::string AssetDatabase::EnsureGuidMetaUnindexed(const std::string& absPath)
{
    {
        std::lock_guard lock(s_mutex);
        if (IsUnderSharedAssetsLocked(absPath))
            return ReadGuidFromMeta(absPath + ".meta");
    }
    if (!util::FileSystem::Exists(absPath)) return {};
    const std::string metaPath = absPath + ".meta";
    std::string guid = ReadGuidFromMeta(metaPath);
    if (!guid.empty()) return guid;
    guid = GenerateGuid();
    return WriteGuidToMeta(metaPath, guid) ? guid : std::string{};
}

std::string AssetDatabase::TryGetGuidFromPath(const std::string& absPath)
{
    {
        std::lock_guard lock(s_mutex);
        const std::string key = PathKey(absPath);
        const auto it = s_pathToGuid.find(key);
        if (it != s_pathToGuid.end()) return it->second;
        const auto shared = s_sharedPathToGuid.find(key);
        if (shared != s_sharedPathToGuid.end()
            && !s_ambiguousSharedGuids.contains(shared->second))
            return shared->second;
        const auto alias = s_movedPathAliases.find(key);
        if (alias != s_movedPathAliases.end()) return alias->second;
    }

    /// @note 参照系では .meta を新規作成しない。既存の .meta だけを読む。
    if (!util::FileSystem::Exists(absPath)) return {};
    const std::string guid = ReadGuidFromMeta(absPath + ".meta");
    if (guid.empty()) return {};

    std::lock_guard lock(s_mutex);
    if (IsUnderSharedAssetsLocked(absPath)) {
        RegisterSharedLocked(guid, absPath);
        return s_ambiguousSharedGuids.contains(guid) ? std::string{} : guid;
    }
    RegisterLocked(guid, absPath);
    return guid;
}

void AssetDatabase::OnAssetMoved(const std::string& oldAbsPath, const std::string& newAbsPath)
{
    std::lock_guard lock(s_mutex);

    /// @note 単一ファイル、または移動元フォルダ自身の GUID を先に付け替える。フォルダにも
    /// @note .meta を発行しているため、フォルダ参照は配下のファイルとは別の GUID を持つ
    /// @note (従来は配下だけを更新し、フォルダ自身の参照が切れていた)。
    const auto it = s_pathToGuid.find(PathKey(oldAbsPath));
    if (it != s_pathToGuid.end()) {
        const std::string guid = it->second;
        SetMappingLocked(s_movedPathAliases, PathKey(oldAbsPath), guid);
        s_pathToGuid.erase(it);
        ++s_revision;
        RegisterLocked(guid, NormalizePath(newAbsPath));
        /// @note ファイルの移動ならここで完了。ディレクトリなら配下も続けて更新する。
        if (!util::FileSystem::IsDirectory(newAbsPath)) return;
    }

    /// @note ディレクトリ移動: 配下に登録された全アセットのパスを一括で付け替える。フォルダ
    /// @note ごと移動しても .meta は中身と一緒に動くため guid は不変で、索引だけ旧プレフィックス
    /// @note から新プレフィックスへ張り替えれば参照は生き続ける。
    const std::string oldPrefixKey = PathKey(oldAbsPath) + "/";
    const std::string newBase      = NormalizePath(newAbsPath) + "/";
    /// @note guid, 新絶対パス
    std::vector<std::pair<std::string, std::string>> moved;
    for (auto iter = s_pathToGuid.begin(); iter != s_pathToGuid.end(); ) {
        if (iter->first.rfind(oldPrefixKey, 0) == 0) {
            /// @note 元の大文字小文字を保った登録パスから相対部分を取り出す (キーと同じ長さ)。
            const auto [guidEntry, inserted] = s_guidToPath.try_emplace(iter->second);
            if (inserted) ++s_revision;
            const std::string& stored = guidEntry->second;
            SetMappingLocked(s_movedPathAliases, iter->first, iter->second);
            moved.emplace_back(iter->second, newBase + stored.substr(oldPrefixKey.size()));
            iter = s_pathToGuid.erase(iter);
            ++s_revision;
        } else {
            ++iter;
        }
    }
    for (const auto& [guid, newPath] : moved)
        RegisterLocked(guid, newPath);
}

void AssetDatabase::OnAssetRemoved(const std::string& absPath)
{
    std::lock_guard lock(s_mutex);
    s_indexDirty = true;
    const std::string key = PathKey(absPath);
    const std::string prefix = key + "/";
    std::vector<std::string> removedGuids;

    /// @note 本体と配下を path 索引から取り除く。
    for (auto iter = s_pathToGuid.begin(); iter != s_pathToGuid.end(); ) {
        if (iter->first == key || iter->first.rfind(prefix, 0) == 0) {
            removedGuids.push_back(iter->second);
            iter = s_pathToGuid.erase(iter);
            ++s_revision;
        } else {
            ++iter;
        }
    }

    /// @note guid → path と、移動直後の旧パス alias も同じ GUID 単位で掃除する。
    for (const std::string& guid : removedGuids) {
        const auto guidIt = s_guidToPath.find(guid);
        if (guidIt != s_guidToPath.end()
            && (PathKey(guidIt->second) == key
                || PathKey(guidIt->second).rfind(prefix, 0) == 0)) {
            s_guidToPath.erase(guidIt);
            ++s_revision;
        }
    }
    for (auto iter = s_movedPathAliases.begin(); iter != s_movedPathAliases.end(); ) {
        const bool sameTree = iter->first == key || iter->first.rfind(prefix, 0) == 0;
        const bool sameGuid = std::find(removedGuids.begin(), removedGuids.end(), iter->second)
                              != removedGuids.end();
        if (sameTree || sameGuid) {
            iter = s_movedPathAliases.erase(iter);
            ++s_revision;
        } else
            ++iter;
    }

    /// @note 消えた実体を巻き込んだ衝突は、もう衝突ではない。
    std::erase_if(s_conflicts, [&](const GuidConflict& c) {
        const std::string dupKey  = PathKey(c.duplicatePath);
        const std::string keptKey = PathKey(c.keptPath);
        return dupKey == key || dupKey.rfind(prefix, 0) == 0
            || keptKey == key || keptKey.rfind(prefix, 0) == 0;
    });
}

std::vector<AssetDatabase::GuidConflict> AssetDatabase::GuidConflicts()
{
    std::lock_guard lock(s_mutex);
    /// @note 記録した後にどちらかが消えた / 振り直された場合を、読み出しのたびに落とす。
    /// @note 衝突が解けるきっかけ (外部ツールでの削除、手での .meta 編集) は索引を通らずに
    /// @note 起きるため、「まだ残っているか」の判定は結局ディスクを読むしかなく、読む場所を
    /// @note ここ 1 つに寄せる。
    std::erase_if(s_conflicts, [](const GuidConflict& c) {
        return ReadGuidFromMeta(c.keptPath + ".meta") != c.guid
            || ReadGuidFromMeta(c.duplicatePath + ".meta") != c.guid;
    });
    return s_conflicts;
}

size_t AssetDatabase::GuidConflictCount()
{
    std::lock_guard lock(s_mutex);
    return s_conflicts.size();
}

namespace {

/// @note import 生成物のコンテナ `Library/Baked/<guid>/` を新しい guid の名前へ移す。
/// @note 生成物が無い (未 import / 生成物を持たない種類) 場合も成功として扱う。
/// @note s_mutex を «取っていない» 状態で呼ぶこと。AssetsRoot() が内部でロックを取る。
bool MoveBakedContainer(const std::string& oldGuid, const std::string& newGuid)
{
    if (oldGuid.empty() || newGuid.empty() || oldGuid == newGuid) return true;

    const std::string assetsRoot = AssetDatabase::AssetsRoot();
    /// @note "Assets/"
    constexpr size_t  kAssetsLen = 7;
    if (assetsRoot.size() <= kAssetsLen) return true;

    const std::string bakedRoot = assetsRoot.substr(0, assetsRoot.size() - kAssetsLen)
                                + "Library/Baked/";
    const std::string src = bakedRoot + oldGuid;
    const std::string dst = bakedRoot + newGuid;
    if (!util::FileSystem::Exists(src)) return true;

    /// @note 新 guid は 128bit 乱数。既に居るのは «移送済み» か本物の衝突かの区別が付かない。
    /// @note どちらにせよ上書きは生成物を壊すので、振り直しごと失敗させる。
    if (util::FileSystem::Exists(dst)) return false;

    return util::FileSystem::Rename(util::FileSystem::PathFromUtf8(src),
                                    util::FileSystem::PathFromUtf8(dst));
}

}

bool AssetDatabase::ReassignGuid(const std::string& absPath, std::string& outNewGuid)
{
    {
        std::lock_guard lock(s_mutex);
        if (IsUnderSharedAssetsLocked(absPath)) {
            FBZZ_LOG_WARN("AssetDatabase: cannot reassign read-only Engine guid [%s]",
                          absPath.c_str());
            return false;
        }
    }
    if (!util::FileSystem::Exists(absPath)) return false;

    const std::string metaPath = absPath + ".meta";
    const std::string oldGuid  = ReadGuidFromMeta(metaPath);
    const std::string newGuid  = GenerateGuid();

    /// @note guid は参照キーであると同時に import 生成物の «置き場所» でもある
    /// @note (`Library/Baked/<guid>/`)。.meta だけ書き換えると生成物が迷子になり、起動の
    /// @note たびに «未 import» と判定されて焼き直しが走り続けるため、先に move し、
    /// @note 失敗したら guid も振り直さない。
    if (!MoveBakedContainer(oldGuid, newGuid)) {
        FBZZ_LOG_ERROR("AssetDatabase: cannot move baked container [%s -> %s]",
                       oldGuid.c_str(), newGuid.c_str());
        return false;
    }

    /// @note .meta の他セクション (importer 設定) は WriteGuidToMeta が残す。
    if (!WriteGuidToMeta(metaPath, newGuid)) {
        FBZZ_LOG_ERROR("AssetDatabase: cannot rewrite meta [%s]", metaPath.c_str());
        /// @note .meta が旧 guid のままなので戻す
        MoveBakedContainer(newGuid, oldGuid);
        return false;
    }

    std::lock_guard lock(s_mutex);
    const std::string key = PathKey(absPath);

    /// @note 旧 guid の guid→path は «先勝ちした別の実体» を指していることがある。
    /// @note 自分が指されている場合だけ外す。
    if (const auto it = s_pathToGuid.find(key); it != s_pathToGuid.end()) {
        if (const auto g = s_guidToPath.find(it->second);
            g != s_guidToPath.end() && PathKey(g->second) == key) {
            s_guidToPath.erase(g);
            ++s_revision;
        }
        s_pathToGuid.erase(it);
        ++s_revision;
    }
    RegisterLocked(newGuid, absPath);

    std::erase_if(s_conflicts, [&](const GuidConflict& c) {
        return PathKey(c.duplicatePath) == key || PathKey(c.keptPath) == key;
    });

    /// @note 自分が先勝ち側だった場合、旧 guid の席が空く。同じ guid で弾かれていた実体を
    /// @note 入れ直さないと、衝突を解いたのに誰も索引に載っていない状態になる。
    if (!oldGuid.empty()) {
        std::vector<std::string> pending;
        for (const GuidConflict& c : s_conflicts) {
            if (c.guid == oldGuid) pending.push_back(c.duplicatePath);
        }
        for (const std::string& path : pending) {
            if (util::FileSystem::Exists(path)) RegisterLocked(oldGuid, path);
        }
    }

    outNewGuid = newGuid;
    return true;
}

AssetDatabase::BakedSweepResult AssetDatabase::SweepOrphanedBaked(bool dryRun)
{
    namespace fs = std::filesystem;
    BakedSweepResult result;

    /// @name 安全弁
    /// @note 索引が «全部読めている» ことだけを条件にする。ここを通れば、載っていない
    /// @note guid は本当に誰も名乗っていない。
    std::unordered_set<std::string> live;
    std::string bakedRoot;
    {
        std::lock_guard lock(s_mutex);
        if (!s_initialized) {
            result.aborted = true;
            result.abortReason = "asset database not initialized";
            return result;
        }
        if (s_pathToGuid.empty()) {
            result.aborted = true;
            result.abortReason = "asset index is empty";
            return result;
        }
        /// @note "Assets/"
        constexpr size_t kAssetsLen = 7;
        if (s_assetsRoot.size() <= kAssetsLen) {
            result.aborted = true;
            result.abortReason = "assets root not resolved";
            return result;
        }
        if (!util::FileSystem::Exists(s_assetsRoot)) {
            result.aborted = true;
            result.abortReason = "assets root missing on disk";
            return result;
        }
        live.reserve(s_pathToGuid.size());
        for (const auto& [path, guid] : s_pathToGuid) live.insert(guid);
        bakedRoot = s_assetsRoot.substr(0, s_assetsRoot.size() - kAssetsLen)
                  + "Library/Baked";
    }

    if (!util::FileSystem::Exists(bakedRoot)) return result;

    std::vector<fs::path> doomed;
    std::error_code ec;
    for (fs::directory_iterator it(bakedRoot, ec), last; !ec && it != last; it.increment(ec)) {
        std::error_code dirEc;
        if (!it->is_directory(dirEc) || dirEc) continue;

        /// @note ディレクトリ名が原本の guid。それ以外の名前には触らない。
        const std::string name =
            util::StringUtils::ToLowerCopy(util::FileSystem::PathToUtf8(it->path().filename()));
        if (name.size() != 32) continue;
        if (!std::all_of(name.begin(), name.end(),
                         [](unsigned char c) { return std::isxdigit(c) != 0; })) continue;

        ++result.scanned;
        if (live.find(name) == live.end()) doomed.push_back(it->path());
    }
    if (ec) {
        /// @note 走査が途中で止まった＝ «誰も名乗っていない» の判定が不完全。消さない。
        result.aborted = true;
        result.abortReason = "baked scan failed: " + ec.message();
        return result;
    }

    for (const fs::path& dir : doomed) {
        uint64_t bytes = 0;
        for (const fs::path& file : util::FileSystem::ListFilesRecursive(dir)) {
            std::error_code sizeEc;
            const uintmax_t size = fs::file_size(file, sizeEc);
            if (!sizeEc) bytes += static_cast<uint64_t>(size);
        }
        if (!dryRun && !util::FileSystem::RemoveAll(dir)) {
            FBZZ_LOG_WARN("AssetDatabase: cannot remove baked container [%s]",
                          util::FileSystem::PathToUtf8(dir).c_str());
            continue;
        }
        ++result.removed;
        result.bytesFreed += bytes;
        result.removedGuids.push_back(
            util::StringUtils::ToLowerCopy(util::FileSystem::PathToUtf8(dir.filename())));
    }

    if (result.removed > 0)
        FBZZ_LOG_INFO("AssetDatabase: %s %zu orphaned baked container(s), %.1f MB (%zu kept)",
                      dryRun ? "would remove" : "removed",
                      result.removed, static_cast<double>(result.bytesFreed) / (1024.0 * 1024.0),
                      result.scanned - result.removed);
    return result;
}

size_t AssetDatabase::Count()
{
    std::lock_guard lock(s_mutex);
    return s_guidToPath.size();
}

std::string AssetDatabase::AssetsRoot()
{
    std::lock_guard lock(s_mutex);
    return s_assetsRoot;
}

namespace {

/// @note s_assetsRoot (`<proj>/Assets/`) から `<proj>/` を作る。呼び出し側がロック済みである前提。
std::string ProjectRootLocked()
{
    /// @note "Assets/"
    constexpr size_t kAssetsLen = 7;
    if (s_assetsRoot.size() > kAssetsLen
        && util::StringUtils::ToLowerCopy(s_assetsRoot.substr(s_assetsRoot.size() - kAssetsLen)) == "assets/")
        return s_assetsRoot.substr(0, s_assetsRoot.size() - kAssetsLen);
    return s_assetsRoot;
}

/// @note TOML の basic string へ落とす。パスは正規化済みで '/' 区切りなので、
/// @note 実質エスケープが要るのは引用符だけだが、規格どおり両方処理する。
std::string TomlQuote(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char c : value) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

}

std::string AssetDatabase::ProjectRoot()
{
    std::lock_guard lock(s_mutex);
    return ProjectRootLocked();
}

std::string AssetDatabase::GuidFromRef(std::string_view ref)
{
    if (!IsGuidRef(ref)) return {};
    ref.remove_prefix(kGuidPrefix.size());

    /// @note パスヒントとサブアセット接尾辞を落とす。guid は 32 桁 hex 固定なので、
    /// @note 「hex が続く限り」で切れば区切り文字の種類に依存せず取り出せる。
    size_t length = 0;
    while (length < ref.size() && length < 32
           && std::isxdigit(static_cast<unsigned char>(ref[length]))) ++length;
    return util::StringUtils::ToLowerCopy(std::string(ref.substr(0, length)));
}

std::string AssetDatabase::HintFromRef(std::string_view ref)
{
    if (!IsGuidRef(ref)) return {};
    const size_t sep = ref.find(kRefHintSeparator);
    if (sep == std::string_view::npos) return {};
    return std::string(ref.substr(sep + 1));
}

void AssetDatabase::SaveIndexFile()
{
    /// @note guid, プロジェクト相対パス
    std::vector<std::pair<std::string, std::string>> entries;
    std::string projectRoot;
    {
        std::lock_guard lock(s_mutex);
        if (!s_initialized) return;
        projectRoot = ProjectRootLocked();
        if (projectRoot.empty()) return;

        const std::string rootKey = util::StringUtils::ToLowerCopy(projectRoot);
        entries.reserve(s_guidToPath.size());
        for (const auto& [guid, absPath] : s_guidToPath) {
            std::string relative = absPath;
            if (relative.size() > rootKey.size()
                && util::StringUtils::ToLowerCopy(relative.substr(0, rootKey.size())) == rootKey)
                relative = relative.substr(rootKey.size());
            entries.emplace_back(guid, std::move(relative));
        }
        s_indexDirty = false;
    }

    /// @note パス順に並べる。関連するアセットが固まって読めるうえ、書き出すたびに
    /// @note 行が入れ替わらないので差分としても意味を持つ。
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    std::ostringstream out;
    out << "# 自動生成 — AssetDatabase が索引を更新するたびに書き直す。手で編集しても読み戻さない。\n"
           "# guid -> プロジェクト相対パス。Library/Baked 配下は import 生成物で、\n"
           "# その guid は原本 GUID + サブキーから DeriveGuid で導出されるためファイル中には存在しない。\n"
           "# ここを引けば、エディターを起動していなくても任意の guid を解決できる。\n\n";
    out << "count = " << entries.size() << "\n\n[guids]\n";
    for (const auto& [guid, relative] : entries)
        out << TomlQuote(guid) << " = " << TomlQuote(relative) << "\n";

    const std::string path = projectRoot + "Library/AssetIndex.toml";
    util::FileSystem::EnsureDirectory(projectRoot + "Library");
    if (!util::FileSystem::WriteText(path, out.str()))
        FBZZ_LOG_WARN("AssetDatabase: index write failed [%s]", path.c_str());
}

void AssetDatabase::FlushIndexFile()
{
    {
        std::lock_guard lock(s_mutex);
        if (!s_indexDirty || !s_initialized) return;
    }
    SaveIndexFile();
}

}
