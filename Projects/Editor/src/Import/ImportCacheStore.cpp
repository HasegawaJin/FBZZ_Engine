/// @file    ImportCacheStore.cpp
/// @brief   Library/ImportCache.toml の読み書き。
/// @author  Hasegawa Jin
/// @date    2026-09-09
#include <Editor/Import/ImportCacheStore.hpp>

#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace fbzz::editor {
namespace {

// std::map なので書き出しは常に guid 順で安定する。差分としても読める。
std::map<std::string, ImportCacheStore::Entry> s_importCacheEntries;
// 読み込み済みプロジェクトの root。変わっていれば別プロジェクトなので読み直す。
std::string s_importCacheProjectRoot;
bool        s_importCacheLoaded = false;
std::mutex  s_importCacheMutex;

std::string ImportCacheFilePath(const std::string& projectRoot)
{
    return projectRoot + "Library/ImportCache.toml";
}

// s_importCacheMutex 取得済み前提。開いているプロジェクトが変わっていれば読み直す。
// @return 現在のプロジェクト root。AssetDatabase 未初期化なら空文字列。
std::string EnsureImportCacheLoaded()
{
    const std::string projectRoot = asset::AssetDatabase::ProjectRoot();
    if (projectRoot.empty()) return {};
    if (s_importCacheLoaded && projectRoot == s_importCacheProjectRoot) return projectRoot;

    s_importCacheEntries.clear();
    s_importCacheProjectRoot = projectRoot;
    s_importCacheLoaded      = true;

    const std::string path = ImportCacheFilePath(projectRoot);
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) return projectRoot;

    std::istringstream iss(text);
    const auto parsed = toml::parse(iss);
    if (!parsed) {
        // 壊れていても復旧は要らない。空から作り直せば、次の走査で焼き直されるだけ。
        FBZZ_LOG_WARN("ImportCacheStore: parse failed, starting empty [%s]", path.c_str());
        return projectRoot;
    }

    const toml::table* hashes = parsed.table()["hashes"].as_table();
    if (!hashes) return projectRoot;
    for (const auto& [guid, value] : *hashes) {
        const toml::table* entry = value.as_table();
        if (!entry) continue;
        ImportCacheStore::Entry loaded;
        loaded.contentHash  = (*entry)["content"].value_or(std::string{});
        loaded.settingsHash = (*entry)["settings"].value_or(std::string{});
        // TOML の整数は int64_t。負値は «壊れた記録» なので目印を捨てる (0 = 未記録)。
        const int64_t rawSize = (*entry)["size"].value_or(int64_t{0});
        loaded.size  = rawSize > 0 ? static_cast<uint64_t>(rawSize) : 0;
        loaded.mtime = (*entry)["mtime"].value_or(int64_t{0});
        // 旧形式は原本の «中身» ではなくパスと更新時刻から作られていた。値としては
        // 使えないので、移行判定用の別枠で持つ。
        if (loaded.contentHash.empty())
            loaded.legacyStamp = (*entry)["source"].value_or(std::string{});
        if (loaded.Empty()) continue;
        s_importCacheEntries.emplace(std::string(guid.str()), std::move(loaded));
    }
    return projectRoot;
}

// s_importCacheMutex 取得済み前提。
bool WriteImportCacheFile(const std::string& projectRoot)
{
    std::ostringstream out;
    out << "# 自動生成 — import が成功するたびに書き直す。手で編集しても読み戻さない。\n"
           "# アセット GUID -> 前回 import 時の fingerprint。生成物の実体は\n"
           "# Library/Baked/<guid>/ にあり、content / settings が現在と食い違ったら焼き直す。\n"
           "#\n"
           "# content = 原本の «中身» のハッシュ。size / mtime は «中身を読まずに\n"
           "#           変わっていないと言い切る» ための目印で、判定の材料ではない。\n"
           "#           どちらかが動いていたら中身を読み直し、content で最終判断する。\n"
           "#\n"
           "# WHY .meta ではなくここか: size / mtime は人・マシン・clone ごとに違う。\n"
           "#     git 追跡下の .meta へ書くと、同じプロジェクトを触る全員の手元が常に差分になる。\n\n";
    out << "count = " << s_importCacheEntries.size() << "\n\n[hashes]\n";
    // guid も hash も hex に限定済みなので、リテラル文字列でエスケープが要らない。
    for (const auto& [guid, entry] : s_importCacheEntries) {
        out << '\'' << guid << "' = { content = '" << entry.contentHash
            << "', settings = '" << entry.settingsHash
            << "', size = " << entry.size
            << ", mtime = " << entry.mtime << " }\n";
    }

    util::FileSystem::EnsureDirectory(projectRoot + "Library");
    const std::string path = ImportCacheFilePath(projectRoot);
    if (util::FileSystem::WriteText(path, out.str())) return true;
    FBZZ_LOG_WARN("ImportCacheStore: write failed [%s]", path.c_str());
    return false;
}

// 32 桁 hex か。引用符やセクション記号が混ざった guid をそのまま書くと
// ファイルごと壊れて、他の全アセットの fingerprint まで道連れになる。
bool IsWellFormedGuid(const std::string& guid)
{
    return guid.size() == 32
        && std::all_of(guid.begin(), guid.end(),
                       [](unsigned char c) { return std::isxdigit(c) != 0; });
}

} // namespace

ImportCacheStore::Entry ImportCacheStore::Load(const std::string& assetGuid)
{
    if (assetGuid.empty()) return {};

    std::lock_guard lock(s_importCacheMutex);
    if (EnsureImportCacheLoaded().empty()) return {};

    const auto it = s_importCacheEntries.find(assetGuid);
    return it != s_importCacheEntries.end() ? it->second : Entry{};
}

bool ImportCacheStore::Save(const std::string& assetGuid, const Entry& entry)
{
    if (entry.Empty() || !IsWellFormedGuid(assetGuid)) return false;

    std::lock_guard lock(s_importCacheMutex);
    const std::string projectRoot = EnsureImportCacheLoaded();
    if (projectRoot.empty()) return false;

    const auto it = s_importCacheEntries.find(assetGuid);
    if (it != s_importCacheEntries.end()
        && it->second.contentHash  == entry.contentHash
        && it->second.settingsHash == entry.settingsHash
        && it->second.size         == entry.size
        && it->second.mtime        == entry.mtime)
        return true;

    s_importCacheEntries[assetGuid] = entry;
    return WriteImportCacheFile(projectRoot);
}

bool ImportCacheStore::RefreshStamp(const std::string& assetGuid, uint64_t size, int64_t mtime)
{
    if (!IsWellFormedGuid(assetGuid)) return false;

    std::lock_guard lock(s_importCacheMutex);
    const std::string projectRoot = EnsureImportCacheLoaded();
    if (projectRoot.empty()) return false;

    const auto it = s_importCacheEntries.find(assetGuid);
    if (it == s_importCacheEntries.end()) return false;
    if (it->second.size == size && it->second.mtime == mtime) return true;

    it->second.size  = size;
    it->second.mtime = mtime;
    return WriteImportCacheFile(projectRoot);
}

size_t ImportCacheStore::Forget(const std::vector<std::string>& guids)
{
    if (guids.empty()) return 0;

    std::lock_guard lock(s_importCacheMutex);
    const std::string projectRoot = EnsureImportCacheLoaded();
    if (projectRoot.empty()) return 0;

    size_t dropped = 0;
    for (const std::string& guid : guids)
        dropped += s_importCacheEntries.erase(guid);

    if (dropped > 0) WriteImportCacheFile(projectRoot);
    return dropped;
}

bool ImportCacheStore::Rekey(const std::string& oldGuid, const std::string& newGuid)
{
    if (!IsWellFormedGuid(oldGuid) || !IsWellFormedGuid(newGuid)) return false;
    if (oldGuid == newGuid) return true;

    std::lock_guard lock(s_importCacheMutex);
    const std::string projectRoot = EnsureImportCacheLoaded();
    if (projectRoot.empty()) return false;

    const auto it = s_importCacheEntries.find(oldGuid);
    if (it == s_importCacheEntries.end()) return true;  // 未記録なら移すものが無い

    s_importCacheEntries[newGuid] = it->second;
    s_importCacheEntries.erase(it);
    return WriteImportCacheFile(projectRoot);
}

} // namespace fbzz::editor
