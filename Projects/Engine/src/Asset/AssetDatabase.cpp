/// @file    AssetDatabase.cpp
/// @brief   GUID ⇄ アセットパスの双方向インデックス実装。
/// @author  Hasegawa Jin
/// @date    2026-07-08
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
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

// 双方向インデックス。キーは正規化 (小文字ドライブ + '/' 区切り) した絶対パス。
std::unordered_map<std::string, std::string> s_guidToPath;
std::unordered_map<std::string, std::string> s_pathToGuid;
// リネーム直後に開いている Scene が旧パスを保持していても、保存時に GUID 化できるよう
// 旧パスを一時的に GUID へ解決する。新しいアセットが同じ場所へ作られたら消費する。
std::unordered_map<std::string, std::string> s_movedPathAliases;
std::mutex  s_mutex;
bool        s_initialized = false;
std::string s_assetsRoot;  // Init に渡された Assets ルート (末尾 '/' 付き正規化)
// 索引が最後の書き出し以降に変わったか。FlushIndexFile が見る。
bool        s_indexDirty = false;
// 先勝ちで弾いた重複の記録。修復ツールと通知バーが読む。
std::vector<AssetDatabase::GuidConflict> s_conflicts;

std::string NormalizePath(std::string p)
{
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

std::string LowerCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// パス索引キー。Windows はパス大文字小文字非区別のため小文字化して同一視する。
std::string PathKey(const std::string& absPath)
{
    return LowerCopy(NormalizePath(absPath));
}

// "<asset>.meta" から guid を読む。無ければ空文字列。
std::string ReadGuidFromMeta(const std::string& metaPath)
{
    std::string text;
    if (!util::FileSystem::ReadText(metaPath, text)) return {};
    std::istringstream iss(text);
    const auto parsed = toml::parse(iss);
    if (!parsed) return {};
    if (auto v = parsed.table()["meta"]["guid"].value<std::string>())
        return LowerCopy(*v);
    return {};
}

// .meta に [meta] guid を書き込む。既存の他セクション ([texture] 等) は保持する。
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
    return util::FileSystem::WriteText(metaPath, ss.str());
}

// "Foo.fbx" のインポートで生成される従属フォルダ "Foo/" か判定する。
// WHY: 従属フォルダは原本モデルから再生成されるため、独立した guid を持たせない。
//      持たせると再インポートのたびに孤児 .meta が残り、索引が汚れる。
bool IsGeneratedModelPackageDir(const std::filesystem::path& dir)
{
    namespace fs = std::filesystem;
    const std::string stem = util::FileSystem::PathToUtf8(dir.filename());
    if (stem.empty()) return false;

    // 兄弟に同名のモデル原本があれば、このフォルダはその出力先。
    static constexpr const char* kModelExts[] = { ".fbx", ".obj", ".gltf", ".glb" };
    for (const char* ext : kModelExts) {
        if (util::FileSystem::Exists(dir.parent_path() / (stem + ext))) return true;
    }
    // 原本が消えていても、内部コンテナが残っていれば生成物と分かる。
    std::error_code ec;
    return fs::exists(dir / (stem + ".fzasset"), ec);
}

// 本体が存在しない孤児 .meta か。索引には載せず、削除もしない。
//
// WHY 消さないか: 大容量のバイナリ素材 (png / fbx / wav) を .gitignore しつつ .meta だけを
//     追跡するリポジトリでは、clone 直後に本体不在の .meta が大量に孤児として見える。
//     ここで消すと .scene / .mat の guid 参照が一斉に切れるうえ、後から素材を入れ直しても
//     GuidFromPath が新しい乱数 guid を振るため二度と元へ戻らない。残しておけば、
//     本体が戻った時点で同じ guid のまま索引へ復帰する。
//     エディター内の削除は本体と .meta を対で .fbzz/Trash へ移すので、ここへ来るのは
//     エディター外 (git / エクスプローラー) で消された場合だけ。
//
// 代償: 同じパスへ «別の» アセットを作り直すと、残った .meta の guid を引き継ぐ。
//       ファイルを消して入れ替えただけの場合は望ましい挙動なので、これは受け入れる。
bool IsOrphanMeta(const std::filesystem::path& metaPath)
{
    const std::string metaUtf8 = util::FileSystem::PathToUtf8(metaPath);
    // "Foo.png.meta" → "Foo.png" / "Textures.meta" → "Textures" (ディレクトリ)
    return !util::FileSystem::Exists(metaUtf8.substr(0, metaUtf8.size() - 5));
}

// ロック取得済み前提。同じ (guid, 弾かれたパス) を二重に積まない。
// WHY: 索引の再構築や watcher の再通知で同じ衝突が何度も通る。件数がそのたびに
//      増えると、通知バーの「N 件」が実際の重複数と合わなくなる。
void RecordConflictLocked(const std::string& guid, const std::string& keptPath,
                          const std::string& duplicatePath)
{
    const std::string dupKey = PathKey(duplicatePath);
    for (const AssetDatabase::GuidConflict& c : s_conflicts) {
        if (c.guid == guid && PathKey(c.duplicatePath) == dupKey) return;
    }
    s_conflicts.push_back({ guid, NormalizePath(keptPath), NormalizePath(duplicatePath) });
}

// "…/Assets/" 以降を返す。含まなければ空。
// WHY 先頭の "Assets/" も見るか: 索引には絶対パスが載るが、参照解決は
//     "Assets/Models/Foo.fbx" のような論理パスのまま入ってくることがある。
//     区切りだけを探すと、先頭に来た "Assets/" を取りこぼして «別物» と判定する。
std::string RelativeToAnyAssetsRoot(const std::string& absPath)
{
    static constexpr std::string_view kLeading = "assets/";
    static constexpr std::string_view kEmbedded = "/assets/";

    const std::string lower = LowerCopy(NormalizePath(absPath));
    if (lower.rfind(kLeading, 0) == 0) return lower.substr(kLeading.size());
    const size_t pos = lower.rfind(kEmbedded);
    if (pos == std::string::npos) return {};
    return lower.substr(pos + kEmbedded.size());
}

// 同じ guid を名乗る 2 つのパスが、同じアセットの «コピー» か。
//
// WHY 要るか: エンジンは共通シェーダーをプロジェクトの Assets へ配る。コピーは .meta ごと
//     複製されるので guid も同一になるが、これは «同じアセットが 2 箇所にある» のであって、
//     参照が別物へ吸われる本物の衝突ではない。区別せずに重複として鳴らすと、起動のたびに
//     エラーが出るうえ Asset Maintenance の一覧にも並び、«直す» と guid が振り直されて
//     どちらか一方の参照が本当に壊れる。
bool IsSameAssetCopyLocked(const std::string& a, const std::string& b)
{
    const std::string relA = RelativeToAnyAssetsRoot(a);
    return !relA.empty() && relA == RelativeToAnyAssetsRoot(b);
}

// absPath が «開いているプロジェクトの» Assets 配下か。
bool IsUnderProjectAssetsLocked(const std::string& absPath)
{
    if (s_assetsRoot.empty()) return false;
    const std::string key = PathKey(absPath);
    return key.rfind(LowerCopy(s_assetsRoot), 0) == 0;
}

// ロック取得済み前提でインデックスに登録する。guid 重複はエラーログを出し先勝ち。
// guid → path の登録。
//
// primary=false のときは「別名」として扱い、path → guid の逆引きは上書きしない。
// WHY 別名が要るか: 移行期には 1 つの実体を 2 つの GUID が指しうる
//     (旧: .meta に書かれた乱数 / 新: 原本から導出)。参照側は旧 GUID で書かれているので
//     どちらからも引けなければならないが、これから保存する値は新 GUID に一本化したい。
//     guid→path は多対一を許し、path→guid は primary だけが書く。
void RegisterLocked(const std::string& guid, const std::string& absPath, bool primary = true)
{
    const std::string key = PathKey(absPath);
    s_movedPathAliases.erase(key);
    s_indexDirty = true;
    const auto it = s_guidToPath.find(guid);
    if (it != s_guidToPath.end() && PathKey(it->second) != key) {
        // 外部ツールの移動で Renamed 通知が欠落した場合、古い実体が消えていれば
        // 新しいパスを同じアセットとして受け入れる。両方が存在する場合は本当の
        // GUID 重複なので、先勝ちを維持して誤った参照の乗っ取りを防ぐ。
        if (!util::FileSystem::Exists(it->second)) {
            s_pathToGuid.erase(PathKey(it->second));
            s_guidToPath[guid] = NormalizePath(absPath);
            if (primary) s_pathToGuid[key] = guid;
            else         s_pathToGuid.try_emplace(key, guid);
            return;
        }
        // 同じ実体を 2 通りのパス表記で登録しただけなら、そもそも衝突ではない。
        //
        // WHY 文字列比較で足りないか: 相対と絶対、ドライブ文字や中間ディレクトリの
        //     大文字小文字、ジャンクション経由 — どれも «違う文字列で同じファイル» を作る。
        //     PathKey の正規化は小文字化と区切りの統一までで、ここを吸収できない。
        //     ファイルシステムに «同じものか» を聞けば、経路によらず一度で判定できる。
        if (std::error_code ec; std::filesystem::equivalent(
                util::FileSystem::PathFromUtf8(it->second),
                util::FileSystem::PathFromUtf8(absPath), ec) && !ec) {
            // 実体は 1 つ。guid→path は先に入った表記を保ち、逆引きだけ増やす。
            if (primary) s_pathToGuid[key] = guid;
            else         s_pathToGuid.try_emplace(key, guid);
            return;
        }
        // エンジン内蔵 Assets とプロジェクト Assets に配られた同じアセット。
        // 衝突ではないので記録も警告もせず、プロジェクト側を «実体» として採用する。
        if (IsSameAssetCopyLocked(it->second, absPath)) {
            if (IsUnderProjectAssetsLocked(absPath)
                && !IsUnderProjectAssetsLocked(it->second))
                s_guidToPath[guid] = NormalizePath(absPath);
            // 逆引きはどちらのパスからも引けてよい。参照はパスで来ることがある。
            if (primary) s_pathToGuid[key] = guid;
            else         s_pathToGuid.try_emplace(key, guid);
            return;
        }
        FBZZ_LOG_ERROR("AssetDatabase: duplicate guid [%s]\n  kept: %s\n  dup : %s",
                       guid.c_str(), it->second.c_str(), absPath.c_str());
        RecordConflictLocked(guid, it->second, NormalizePath(absPath));
        return;
    }
    s_guidToPath[guid] = NormalizePath(absPath);
    if (primary) s_pathToGuid[key] = guid;
    else         s_pathToGuid.try_emplace(key, guid);
}

} // namespace

bool AssetDatabase::ShouldHaveMeta(std::string_view lowerFileName)
{
    // 拡張子を持たないものは対象外。".gitignore" のような管理ファイルと
    // "LICENSE" のような無拡張ファイルがここに落ちる。
    // WHY 先頭ドットだけを弾くか: ".playmode_snapshot.scene" のようにドット始まりでも
    //     拡張子を持つ実アセットがあるため、「ドット始まりは全部除外」にはできない。
    const size_t dot = lowerFileName.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return false;
    const std::string_view ext = lowerFileName.substr(dot);

    // 拡張子として妥当な形だけを通す。
    // WHY: EncodeGuidRefs はドキュメント中の全文字列にこの判定を掛けるため、
    //      "Player.001" や "Score: 0.5" のような値までアセット候補として
    //      ディスクを叩きに行かないよう、英字を含む短い英数字列に限る。
    if (ext.size() < 2 || ext.size() > 17) return false;
    bool hasAlpha = false;
    for (const char c : ext.substr(1)) {
        const auto uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc)) return false;
        if (std::isalpha(uc)) hasAlpha = true;
    }
    if (!hasAlpha) return false;

    // WHY 許可リストではなく除外リストか: 「Asset Browser には出るし Inspector でも
    //     選べるのに ShouldHaveMeta に無く、guid が振られないので参照が壊れる」不具合を
    //     拡張子を足すたびに繰り返してきた (Docs/design/audio-system.md の .flac)。
    //     .md / .txt / .pdf のような資料も含め、原本は等しく GUID を持たせ、
    //     再生成できる派生物だけを名指しで除外する。
    static constexpr std::string_view kExcludedExts[] = {
        ".meta",
        // import / shader compile の生成物。GUID は原本の GUID から導出される (DeriveGuid)。
        ".fzasset", ".mesh", ".skel", ".cso",
        // ビルド生成物と作業用の一時ファイル。
        ".dll", ".lib", ".pdb", ".exp", ".ilk", ".obj", ".exe", ".tmp", ".bak",
    };
    for (const auto e : kExcludedExts)
        if (ext == e) return false;

    // ScriptCodeGen の出力。原本の .hpp から再生成されるので独立した GUID を持たない。
    static constexpr std::string_view kExcludedSuffixes[] = { ".generated.hpp" };
    for (const auto s : kExcludedSuffixes)
        if (lowerFileName.size() >= s.size()
            && lowerFileName.substr(lowerFileName.size() - s.size()) == s) return false;

    // 原子的な保存の途中経過 ("Stage_01.scene.tmp.<pid>.<hash>")。
    // WHY 末尾の ".tmp" 判定では足りないか: 一時名は .tmp の «後ろ» に pid とハッシュを
    //     足すため、拡張子として見えるのは ".<hash>" になる。英字を含む短い英数字列なので
    //     上の書式検査を通ってしまい、rename で消える相手に .meta を発行して孤児が残る。
    if (lowerFileName.find(".tmp.") != std::string_view::npos) return false;

    return true;
}

bool AssetDatabase::ShouldHaveFolderMeta(std::string_view folderName)
{
    if (folderName.empty()) return false;

    // ドット始まりは VCS / エディター内部の管理ディレクトリ (.git / .import_presets 等)。
    if (folderName.front() == '.') return false;

    // 再生成される中間・出力物。guid を振っても参照先として意味を持たない。
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
    // 128bit 乱数 → 32 桁 hex (Unity と同形式)。暗号強度は不要なので mt19937_64 で足りる。
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

    // FNV-1a を 2 系統 (offset basis 違い) で回して 128bit を作る。
    // WHY 既存と同じ FNV か: GuidRefCodec / FbxMetaSerializer が既に FNV-1a を使っている。
    //     新しいハッシュを持ち込まずに済み、実装の妥当性を読み手が確かめやすい。
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

// s_mutex を呼び出し側が保持している前提の実体。
// WHY 分けるか: Init は本体の全体でロックを持つため、そこから公開 API を呼ぶと
//     std::mutex は再帰不可なのでデッドロックする。
static void IndexBakedLibraryLocked(const std::string& libraryBakedRoot)
{
    namespace fs = std::filesystem;
    if (libraryBakedRoot.empty()) return;
    const fs::path root = util::FileSystem::PathFromUtf8(libraryBakedRoot);
    if (!util::FileSystem::Exists(root)) return;

    size_t indexed = 0;

    // Library/Baked/<sourceGuid>/<subKey...>
    // ディレクトリ名がそのまま原本の GUID。.meta は読まない (存在しない)。
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), last; !ec && it != last; it.increment(ec)) {
        std::error_code dirEc;
        if (!it->is_directory(dirEc) || dirEc) continue;

        const std::string sourceGuid =
            LowerCopy(util::FileSystem::PathToUtf8(it->path().filename()));
        if (sourceGuid.size() != 32) continue;   // guid ディレクトリ以外は無視

        for (const fs::path& file : util::FileSystem::ListFilesRecursive(it->path())) {
            const std::string absPath = NormalizePath(util::FileSystem::PathToUtf8(file));
            const std::string ext = LowerCopy(util::FileSystem::GetExtension(absPath));
            // 参照されうるものだけ索引する。.fzasset/.mesh/.skel は原本の GUID から
            // 引かれるので個別の GUID を持たない。
            // 画像は .mat が guid: で参照するため索引が要る。
            static constexpr std::string_view kIndexed[] = {
                ".anim", ".mat",
                ".png", ".jpg", ".jpeg", ".tga", ".dds", ".bmp", ".hdr", ".exr",
            };
            bool indexable = false;
            for (const auto candidate : kIndexed)
                if (ext == candidate) { indexable = true; break; }
            if (!indexable) continue;

            // WHY 専用の error_code を使うか: 外側のイテレータ用 ec を使い回すと、
            //     relative の失敗でループ条件 (!ec) が偽になり走査が途中で止まる。
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

void AssetDatabase::Init(const std::string& assetsRoot)
{
    namespace fs = std::filesystem;
    std::lock_guard lock(s_mutex);
    s_guidToPath.clear();
    s_pathToGuid.clear();
    s_movedPathAliases.clear();
    s_conflicts.clear();
    s_assetsRoot = NormalizePath(assetsRoot);
    if (!s_assetsRoot.empty() && s_assetsRoot.back() != '/') s_assetsRoot.push_back('/');

    const fs::path root = util::FileSystem::PathFromUtf8(assetsRoot);
    if (!util::FileSystem::Exists(root)) {
        FBZZ_LOG_WARN("AssetDatabase: assets root not found [%s]", assetsRoot.c_str());
        s_initialized = true;
        return;
    }

    size_t healed  = 0;
    size_t orphans = 0;

    // .meta を持つべき対象を 1 件処理する共通クロージャ (ファイル / フォルダ共通)。
    const auto indexTarget = [&](const std::string& absPath) {
        const std::string metaPath = absPath + ".meta";
        std::string guid = ReadGuidFromMeta(metaPath);
        if (guid.empty()) {
            // FBX は Import 実行時に GUID を確定する。AssetBrowser を開いただけで
            // 原本の隣に .meta を生成すると、未 Import と Import 済みの境界が崩れる。
            if (LowerCopy(util::FileSystem::GetExtension(absPath)) == ".fbx") return;
            // 自己修復: .meta が無い / guid が無い対象に新規発行する。
            guid = GenerateGuid();
            if (!WriteGuidToMeta(metaPath, guid)) {
                FBZZ_LOG_WARN("AssetDatabase: cannot write meta [%s]", metaPath.c_str());
                return;
            }
            ++healed;
        }
        RegisterLocked(guid, absPath);
    };

    for (const fs::path& p : util::FileSystem::ListFilesRecursive(root)) {
        const std::string absPath = NormalizePath(util::FileSystem::PathToUtf8(p));
        const std::string ext = LowerCopy(util::FileSystem::GetExtension(absPath));

        // 孤児 .meta の検出は索引構築と同じ 1 パスで行う (件数はログにだけ出す)。
        if (ext == ".meta") {
            if (IsOrphanMeta(p)) ++orphans;
            continue;
        }

        if (!ShouldHaveMeta(LowerCopy(util::FileSystem::GetFilename(absPath)))) continue;
        indexTarget(absPath);
    }

    // ── フォルダの .meta ──────────────────────────────────────────────────
    // WHY: フォルダも参照される (デフォルト保存先・検索スコープ)。パスで覚えると
    //      リネームや移動で参照が切れるため、ファイルと同じ guid 方式に揃える。
    //      走査はファイルと分けている。ListFilesRecursive がファイルのみを返すため。
    {
        // NOTE: range-for は begin() が反復子のコピーを返すため disable_recursion_pending が
        //       効かない。除外フォルダの配下を辿らないよう、明示的な while ループで回す。
        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator last;
        while (!ec && it != last) {
            const fs::directory_entry& entry = *it;

            std::error_code dirEc;
            if (entry.is_directory(dirEc) && !dirEc) {
                const fs::path&   dir  = entry.path();
                const std::string name = util::FileSystem::PathToUtf8(dir.filename());
                // 除外フォルダと、モデルインポートの従属フォルダは配下ごと辿らない。
                if (!ShouldHaveFolderMeta(name) || IsGeneratedModelPackageDir(dir))
                    it.disable_recursion_pending();
                else
                    indexTarget(NormalizePath(util::FileSystem::PathToUtf8(dir)));
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
    // 孤児が多い＝本体がローカルに揃っていない (LFS 未取得 / 素材が git 管理外) 状態。
    // 参照切れの原因がここにあると分かるよう、黙って進めずに一度だけ警告する。
    if (orphans > 0)
        FBZZ_LOG_WARN("AssetDatabase: %zu meta have no owner asset. "
                      "References to them stay unresolved until the assets are restored.",
                      orphans);
    // 重複は 1 件ごとにエラーを出しているが、スキャン中の行は起動ログに流れて気づかれない。
    // 総数だけをもう一度、通知バーと同じ文言で残す。
    if (!s_conflicts.empty())
        FBZZ_LOG_ERROR("AssetDatabase: %zu duplicate guid(s). "
                       "References resolve to one side only \xe2\x80\x94 "
                       "fix them in Tools > Asset Maintenance.",
                       s_conflicts.size());

    // ── Library/Baked の生成物を導出 GUID で索引する ──────────────────────
    // WHY Assets の後か: 同じ実体が両方に居る移行期に、Assets 側 (人が編集しうる方) の
    //     path→guid を優先させるため。RegisterLocked は先勝ちではないが、
    //     guid が違えば衝突しないので順序で意味を持つのは path→guid だけ。
    if (s_assetsRoot.size() > 7) {
        const std::string projectRoot = s_assetsRoot.substr(0, s_assetsRoot.size() - 7);
        IndexBakedLibraryLocked(projectRoot + "Library/Baked");
    }
    s_indexDirty = true;
}

void AssetDatabase::Shutdown()
{
    std::lock_guard lock(s_mutex);
    s_guidToPath.clear();
    s_pathToGuid.clear();
    s_movedPathAliases.clear();
    s_conflicts.clear();
    s_initialized = false;
}

std::string AssetDatabase::PathFromGuid(const std::string& guid)
{
    std::lock_guard lock(s_mutex);
    const auto it = s_guidToPath.find(LowerCopy(guid));
    return it != s_guidToPath.end() ? it->second : std::string{};
}

std::string AssetDatabase::GuidFromPath(const std::string& absPath)
{
    const std::string existingGuid = TryGetGuidFromPath(absPath);
    if (!existingGuid.empty()) return existingGuid;

    // 未登録: インデックス構築後に追加されたアセット。自己修復して登録する。
    // 実在しないパスに .meta を作らないよう必ず存在確認する。
    if (!util::FileSystem::Exists(absPath)) return {};

    if (util::FileSystem::IsDirectory(absPath)) {
        const std::filesystem::path dir = util::FileSystem::PathFromUtf8(absPath);
        if (!ShouldHaveFolderMeta(util::FileSystem::PathToUtf8(dir.filename()))) return {};
        if (IsGeneratedModelPackageDir(dir)) return {};
    } else {
        if (!ShouldHaveMeta(LowerCopy(util::FileSystem::GetFilename(absPath)))) return {};
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

std::string AssetDatabase::TryGetGuidFromPath(const std::string& absPath)
{
    {
        std::lock_guard lock(s_mutex);
        const auto it = s_pathToGuid.find(PathKey(absPath));
        if (it != s_pathToGuid.end()) return it->second;
        const auto alias = s_movedPathAliases.find(PathKey(absPath));
        if (alias != s_movedPathAliases.end()) return alias->second;
    }

    // 参照系では .meta を新規作成しない。既存の .meta だけを読む。
    if (!util::FileSystem::Exists(absPath)) return {};
    const std::string guid = ReadGuidFromMeta(absPath + ".meta");
    if (guid.empty()) return {};

    std::lock_guard lock(s_mutex);
    RegisterLocked(guid, absPath);
    return guid;
}

void AssetDatabase::OnAssetMoved(const std::string& oldAbsPath, const std::string& newAbsPath)
{
    std::lock_guard lock(s_mutex);

    // 単一ファイル、または移動元フォルダ自身の GUID を先に付け替える。
    // WHY: フォルダにも .meta を発行しているため、フォルダ参照は配下のファイルとは
    //      別の GUID を持つ。従来は配下だけを更新し、フォルダ自身の参照が切れていた。
    const auto it = s_pathToGuid.find(PathKey(oldAbsPath));
    if (it != s_pathToGuid.end()) {
        const std::string guid = it->second;
        s_movedPathAliases[PathKey(oldAbsPath)] = guid;
        s_pathToGuid.erase(it);
        RegisterLocked(guid, NormalizePath(newAbsPath));
        // ファイルの移動ならここで完了。ディレクトリなら配下も続けて更新する。
        if (!util::FileSystem::IsDirectory(newAbsPath)) return;
    }

    // ディレクトリ移動: 配下に登録された全アセットのパスを一括で付け替える。
    // WHY: フォルダごと移動しても .meta は中身と一緒に動くため guid は不変。
    //      索引だけ旧プレフィックスから新プレフィックスへ張り替えれば参照は生き続ける。
    const std::string oldPrefixKey = PathKey(oldAbsPath) + "/";
    const std::string newBase      = NormalizePath(newAbsPath) + "/";
    std::vector<std::pair<std::string, std::string>> moved; // guid, 新絶対パス
    for (auto iter = s_pathToGuid.begin(); iter != s_pathToGuid.end(); ) {
        if (iter->first.rfind(oldPrefixKey, 0) == 0) {
            // 元の大文字小文字を保った登録パスから相対部分を取り出す (キーと同じ長さ)。
            const std::string& stored = s_guidToPath[iter->second];
            s_movedPathAliases[iter->first] = iter->second;
            moved.emplace_back(iter->second, newBase + stored.substr(oldPrefixKey.size()));
            iter = s_pathToGuid.erase(iter);
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

    // 本体と配下を path 索引から取り除く。
    for (auto iter = s_pathToGuid.begin(); iter != s_pathToGuid.end(); ) {
        if (iter->first == key || iter->first.rfind(prefix, 0) == 0) {
            removedGuids.push_back(iter->second);
            iter = s_pathToGuid.erase(iter);
        } else {
            ++iter;
        }
    }

    // guid → path と、移動直後の旧パス alias も同じ GUID 単位で掃除する。
    for (const std::string& guid : removedGuids) {
        const auto guidIt = s_guidToPath.find(guid);
        if (guidIt != s_guidToPath.end()
            && (PathKey(guidIt->second) == key
                || PathKey(guidIt->second).rfind(prefix, 0) == 0))
            s_guidToPath.erase(guidIt);
    }
    for (auto iter = s_movedPathAliases.begin(); iter != s_movedPathAliases.end(); ) {
        const bool sameTree = iter->first == key || iter->first.rfind(prefix, 0) == 0;
        const bool sameGuid = std::find(removedGuids.begin(), removedGuids.end(), iter->second)
                              != removedGuids.end();
        if (sameTree || sameGuid)
            iter = s_movedPathAliases.erase(iter);
        else
            ++iter;
    }

    // 消えた実体を巻き込んだ衝突は、もう衝突ではない。
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
    // 記録した後にどちらかが消えた / 振り直された場合を、読み出しのたびに落とす。
    // WHY ここで捨てるか: 衝突が解けるきっかけ (外部ツールでの削除、手での .meta 編集) は
    //     索引を通らずに起きる。通知を出す側から見て「まだ残っているか」の判定は、
    //     結局ディスクを読むしかない。読む場所をここ 1 つに寄せる。
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

// import 生成物のコンテナ Library/Baked/<guid>/ を新しい guid の名前へ移す。
// 生成物が無い (未 import / 生成物を持たない種類) 場合も成功として扱う。
// s_mutex を «取っていない» 状態で呼ぶこと。AssetsRoot() が内部でロックを取る。
bool MoveBakedContainer(const std::string& oldGuid, const std::string& newGuid)
{
    if (oldGuid.empty() || newGuid.empty() || oldGuid == newGuid) return true;

    const std::string assetsRoot = AssetDatabase::AssetsRoot();
    constexpr size_t  kAssetsLen = 7;  // "Assets/"
    if (assetsRoot.size() <= kAssetsLen) return true;

    const std::string bakedRoot = assetsRoot.substr(0, assetsRoot.size() - kAssetsLen)
                                + "Library/Baked/";
    const std::string src = bakedRoot + oldGuid;
    const std::string dst = bakedRoot + newGuid;
    if (!util::FileSystem::Exists(src)) return true;

    // 新 guid は 128bit 乱数。既に居るのは «移送済み» か本物の衝突かの区別が付かない。
    // どちらにせよ上書きは生成物を壊すので、振り直しごと失敗させる。
    if (util::FileSystem::Exists(dst)) return false;

    return util::FileSystem::Rename(util::FileSystem::PathFromUtf8(src),
                                    util::FileSystem::PathFromUtf8(dst));
}

} // namespace

bool AssetDatabase::ReassignGuid(const std::string& absPath, std::string& outNewGuid)
{
    if (!util::FileSystem::Exists(absPath)) return false;

    const std::string metaPath = absPath + ".meta";
    const std::string oldGuid  = ReadGuidFromMeta(metaPath);
    const std::string newGuid  = GenerateGuid();

    // guid は参照キーであると同時に import 生成物の «置き場所» でもある
    // (Library/Baked/<guid>/)。.meta だけ書き換えると生成物が迷子になり、
    // 起動のたびに «未 import» と判定されて焼き直しが走り続ける。
    // 先に move し、失敗したら guid も振り直さない。
    if (!MoveBakedContainer(oldGuid, newGuid)) {
        FBZZ_LOG_ERROR("AssetDatabase: cannot move baked container [%s -> %s]",
                       oldGuid.c_str(), newGuid.c_str());
        return false;
    }

    // .meta の他セクション (importer 設定) は WriteGuidToMeta が残す。
    if (!WriteGuidToMeta(metaPath, newGuid)) {
        FBZZ_LOG_ERROR("AssetDatabase: cannot rewrite meta [%s]", metaPath.c_str());
        MoveBakedContainer(newGuid, oldGuid);  // .meta が旧 guid のままなので戻す
        return false;
    }

    std::lock_guard lock(s_mutex);
    const std::string key = PathKey(absPath);

    // 旧 guid の guid→path は «先勝ちした別の実体» を指していることがある。
    // 自分が指されている場合だけ外す。
    if (const auto it = s_pathToGuid.find(key); it != s_pathToGuid.end()) {
        if (const auto g = s_guidToPath.find(it->second);
            g != s_guidToPath.end() && PathKey(g->second) == key)
            s_guidToPath.erase(g);
        s_pathToGuid.erase(it);
    }
    RegisterLocked(newGuid, absPath);

    std::erase_if(s_conflicts, [&](const GuidConflict& c) {
        return PathKey(c.duplicatePath) == key || PathKey(c.keptPath) == key;
    });

    // 自分が先勝ち側だった場合、旧 guid の席が空く。同じ guid で弾かれていた実体を
    // 入れ直さないと、衝突を解いたのに誰も索引に載っていない状態になる。
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

    // ── 安全弁 ───────────────────────────────────────────────────────────
    // 索引が «全部読めている» ことだけを条件にする。ここを通れば、載っていない
    // guid は本当に誰も名乗っていない。
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
        constexpr size_t kAssetsLen = 7;  // "Assets/"
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

        // ディレクトリ名が原本の guid。それ以外の名前には触らない。
        const std::string name =
            LowerCopy(util::FileSystem::PathToUtf8(it->path().filename()));
        if (name.size() != 32) continue;
        if (!std::all_of(name.begin(), name.end(),
                         [](unsigned char c) { return std::isxdigit(c) != 0; })) continue;

        ++result.scanned;
        if (live.find(name) == live.end()) doomed.push_back(it->path());
    }
    if (ec) {
        // 走査が途中で止まった＝ «誰も名乗っていない» の判定が不完全。消さない。
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
            LowerCopy(util::FileSystem::PathToUtf8(dir.filename())));
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

// s_assetsRoot ("<proj>/Assets/") から "<proj>/" を作る。呼び出し側がロック済みである前提。
std::string ProjectRootLocked()
{
    constexpr size_t kAssetsLen = 7;  // "Assets/"
    if (s_assetsRoot.size() > kAssetsLen
        && LowerCopy(s_assetsRoot.substr(s_assetsRoot.size() - kAssetsLen)) == "assets/")
        return s_assetsRoot.substr(0, s_assetsRoot.size() - kAssetsLen);
    return s_assetsRoot;
}

// TOML の basic string へ落とす。パスは正規化済みで '/' 区切りなので、
// 実質エスケープが要るのは引用符だけだが、規格どおり両方処理する。
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

} // namespace

std::string AssetDatabase::ProjectRoot()
{
    std::lock_guard lock(s_mutex);
    return ProjectRootLocked();
}

std::string AssetDatabase::GuidFromRef(std::string_view ref)
{
    if (!IsGuidRef(ref)) return {};
    ref.remove_prefix(kGuidPrefix.size());

    // パスヒントとサブアセット接尾辞を落とす。guid は 32 桁 hex 固定なので、
    // 「hex が続く限り」で切れば区切り文字の種類に依存せず取り出せる。
    size_t length = 0;
    while (length < ref.size() && length < 32
           && std::isxdigit(static_cast<unsigned char>(ref[length]))) ++length;
    return LowerCopy(std::string(ref.substr(0, length)));
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
    std::vector<std::pair<std::string, std::string>> entries;  // guid, プロジェクト相対パス
    std::string projectRoot;
    {
        std::lock_guard lock(s_mutex);
        if (!s_initialized) return;
        projectRoot = ProjectRootLocked();
        if (projectRoot.empty()) return;

        const std::string rootKey = LowerCopy(projectRoot);
        entries.reserve(s_guidToPath.size());
        for (const auto& [guid, absPath] : s_guidToPath) {
            std::string relative = absPath;
            if (relative.size() > rootKey.size()
                && LowerCopy(relative.substr(0, rootKey.size())) == rootKey)
                relative = relative.substr(rootKey.size());
            entries.emplace_back(guid, std::move(relative));
        }
        s_indexDirty = false;
    }

    // パス順に並べる。関連するアセットが固まって読めるうえ、書き出すたびに
    // 行が入れ替わらないので差分としても意味を持つ。
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

} // namespace fbzz::asset
