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
        ".mat", ".anim", ".animcontroller", ".animctrl",
        ".scene", ".terrain", ".fzdata", ".fnt", ".ibl",
        // シェーダーソース (.mat から参照される)
        ".hlsl",
        // オーディオ
        ".mp3", ".wav", ".ogg",
    };
    for (const auto e : kExts)
        if (lowerExt == e) return true;
    return false;
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

    size_t healed = 0;
    for (const fs::path& p : util::FileSystem::ListFilesRecursive(root)) {
        const std::string absPath = NormalizePath(util::FileSystem::PathToUtf8(p));
        const std::string ext = LowerCopy(util::FileSystem::GetExtension(absPath));
        if (!ShouldHaveMeta(ext)) continue;

        const std::string metaPath = absPath + ".meta";
        std::string guid = ReadGuidFromMeta(metaPath);
        if (guid.empty()) {
            // 自己修復: .meta が無い / guid が無いアセットに新規発行する。
            guid = GenerateGuid();
            if (!WriteGuidToMeta(metaPath, guid)) {
                FBZZ_LOG_WARN("AssetDatabase: cannot write meta [%s]", metaPath.c_str());
                continue;
            }
            ++healed;
        }
        RegisterLocked(guid, absPath);
    }

    s_initialized = true;
    FBZZ_LOG_INFO("AssetDatabase: indexed %zu assets (%zu meta healed) under [%s]",
                  s_guidToPath.size(), healed, assetsRoot.c_str());
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
    const std::string ext = LowerCopy(util::FileSystem::GetExtension(absPath));
    if (!ShouldHaveMeta(ext)) return {};
    if (!util::FileSystem::Exists(absPath)) return {};

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
