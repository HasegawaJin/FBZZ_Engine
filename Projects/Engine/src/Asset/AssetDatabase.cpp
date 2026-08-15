// FBZZ Engine
// AssetDatabase.cpp | fbzz::asset
// GUID ⇄ アセットパスの双方向インデックス実装
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
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbzz::asset {

namespace {

// 双方向インデックス。キーは正規化 (小文字ドライブ + '/' 区切り) した絶対パス。
std::unordered_map<std::string, std::string> s_guidToPath;
std::unordered_map<std::string, std::string> s_pathToGuid;
std::mutex  s_mutex;
bool        s_initialized = false;
std::string s_assetsRoot;  // Init に渡された Assets ルート (末尾 '/' 付き正規化)

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

// 本体が存在しない孤児 .meta を削除する。
// WHY: エディター外 (エクスプローラー / git) でアセットを消すと .meta だけが残る。
//      放置すると Asset Browser には出ないのにファイルだけ増え続け、
//      同名アセットを作り直したときに古い guid を拾って参照が入れ替わる。
bool RemoveIfOrphanMeta(const std::filesystem::path& metaPath)
{
    const std::string metaUtf8 = util::FileSystem::PathToUtf8(metaPath);
    // "Foo.png.meta" → "Foo.png" / "Textures.meta" → "Textures" (ディレクトリ)
    const std::string ownerUtf8 = metaUtf8.substr(0, metaUtf8.size() - 5);
    if (util::FileSystem::Exists(ownerUtf8)) return false;
    if (!util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(metaUtf8))) return false;
    FBZZ_LOG_INFO("AssetDatabase: removed orphan meta [%s]", metaUtf8.c_str());
    return true;
}

// ロック取得済み前提でインデックスに登録する。guid 重複はエラーログを出し先勝ち。
void RegisterLocked(const std::string& guid, const std::string& absPath)
{
    const std::string key = PathKey(absPath);
    const auto it = s_guidToPath.find(guid);
    if (it != s_guidToPath.end() && PathKey(it->second) != key) {
        FBZZ_LOG_ERROR("AssetDatabase: duplicate guid [%s]\n  kept: %s\n  dup : %s",
                       guid.c_str(), it->second.c_str(), absPath.c_str());
        return;
    }
    s_guidToPath[guid] = NormalizePath(absPath);
    s_pathToGuid[key]  = guid;
}

} // namespace

bool AssetDatabase::ShouldHaveMeta(std::string_view lowerExt)
{
    // 参照されうる原本アセットのみ。baked 生成物 (.fzasset/.mesh/.skel/.cso) は
    // ソースから再生成されるため .meta を持たない (Library 隔離後は guid キーで引く)。
    static constexpr std::string_view kExts[] = {
        // 画像ソース
        ".png", ".jpg", ".jpeg", ".tga", ".dds", ".hdr", ".exr", ".bmp",
        // モデルソース
        ".fbx",
        // native アセット (エディターで作る著作物)
        ".mat", ".anim", ".animcontroller", ".animctrl", ".mask",
        ".scene", ".terrain", ".fzdata", ".fnt", ".ibl",
        // 物理マテリアル。ColliderComponent がパスで参照するため GUID が要る
        // (リネーム・移動しても参照が切れないように)。
        ".physmat",
        // Behavior Tree。BehaviorTreeComponent がパスで参照するため GUID が要る。
        // NOTE: .vfx はこのリストに入っておらず GUID を持たない (既存の穴)。
        //       .behaviortree では同じ轍を踏まない。
        ".behaviortree",
        // シェーダーソース (.mat から参照される)
        ".hlsl",
        // オーディオ
        ".mp3", ".wav", ".ogg",
    };
    for (const auto e : kExts)
        if (lowerExt == e) return true;
    return false;
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

void AssetDatabase::Init(const std::string& assetsRoot)
{
    namespace fs = std::filesystem;
    std::lock_guard lock(s_mutex);
    s_guidToPath.clear();
    s_pathToGuid.clear();
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

        // 孤児 .meta の掃除は索引構築と同じ 1 パスで行う。
        if (ext == ".meta") {
            if (RemoveIfOrphanMeta(p)) ++orphans;
            continue;
        }

        if (!ShouldHaveMeta(ext)) continue;
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
    FBZZ_LOG_INFO("AssetDatabase: indexed %zu assets (%zu meta healed, %zu orphans removed) under [%s]",
                  s_guidToPath.size(), healed, orphans, assetsRoot.c_str());
}

void AssetDatabase::Shutdown()
{
    std::lock_guard lock(s_mutex);
    s_guidToPath.clear();
    s_pathToGuid.clear();
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
    {
        std::lock_guard lock(s_mutex);
        const auto it = s_pathToGuid.find(PathKey(absPath));
        if (it != s_pathToGuid.end()) return it->second;
    }

    // 未登録: インデックス構築後に追加されたアセット。自己修復して登録する。
    // 実在しないパスに .meta を作らないよう必ず存在確認する。
    if (!util::FileSystem::Exists(absPath)) return {};

    if (util::FileSystem::IsDirectory(absPath)) {
        const std::filesystem::path dir = util::FileSystem::PathFromUtf8(absPath);
        if (!ShouldHaveFolderMeta(util::FileSystem::PathToUtf8(dir.filename()))) return {};
        if (IsGeneratedModelPackageDir(dir)) return {};
    } else {
        const std::string ext = LowerCopy(util::FileSystem::GetExtension(absPath));
        if (!ShouldHaveMeta(ext)) return {};
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

void AssetDatabase::OnAssetMoved(const std::string& oldAbsPath, const std::string& newAbsPath)
{
    std::lock_guard lock(s_mutex);

    // 単一ファイルの移動 / リネーム。
    const auto it = s_pathToGuid.find(PathKey(oldAbsPath));
    if (it != s_pathToGuid.end()) {
        const std::string guid = it->second;
        s_pathToGuid.erase(it);
        RegisterLocked(guid, NormalizePath(newAbsPath));
        return;
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
            moved.emplace_back(iter->second, newBase + stored.substr(oldPrefixKey.size()));
            iter = s_pathToGuid.erase(iter);
        } else {
            ++iter;
        }
    }
    for (const auto& [guid, newPath] : moved)
        RegisterLocked(guid, newPath);
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

} // namespace fbzz::asset
