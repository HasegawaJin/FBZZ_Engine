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
        loaded.sourceHash   = (*entry)["source"].value_or(std::string{});
        loaded.settingsHash = (*entry)["settings"].value_or(std::string{});
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
           "# Library/Baked/<guid>/ にあり、この 2 値が現在の原本・設定と食い違ったら焼き直す。\n"
           "#\n"
           "# WHY .meta ではなくここか: source は原本の絶対パスと更新時刻から作るため、\n"
           "#     人・マシン・clone ごとに必ず違う。git 追跡下の .meta へ書くと\n"
           "#     同じプロジェクトを触る全員の手元が常に差分になる。\n\n";
    out << "count = " << s_importCacheEntries.size() << "\n\n[hashes]\n";
    // guid も hash も hex に限定済みなので、リテラル文字列でエスケープが要らない。
    for (const auto& [guid, entry] : s_importCacheEntries) {
        out << '\'' << guid << "' = { source = '" << entry.sourceHash
            << "', settings = '" << entry.settingsHash << "' }\n";
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
        && it->second.sourceHash   == entry.sourceHash
        && it->second.settingsHash == entry.settingsHash)
        return true;

    s_importCacheEntries[assetGuid] = entry;
    return WriteImportCacheFile(projectRoot);
}

} // namespace fbzz::editor
