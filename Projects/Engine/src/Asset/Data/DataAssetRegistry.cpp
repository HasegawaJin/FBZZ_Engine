/// @file    DataAssetRegistry.cpp
/// @brief   DataAsset の「パス → 共有 1 実体」キャッシュと .fzdata (TOML) 入出力の実装。
/// @author  Hasegawa Jin
/// @date    2026-07-01
///
/// @note 汎用の値型変換は Engine/Scene/Serialization/TomlReflector.hpp に委譲する。
#include <Engine/Asset/Data/DataAssetRegistry.hpp>
#include <Engine/Asset/Data/DataAsset.hpp>
#include <Engine/Asset/Data/DataAssetFactory.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/RenderSettings/RenderPipelineAsset.hpp>
#include "../RenderSettings/RenderPipelineAssetCodec.hpp"
/// @note scene::IReflector / DataAssetRef を使う。
#include <Engine/Reflection/Reflection.hpp>
#include <Engine/Util/Files/FileSystem.hpp>
#include <Engine/Scene/Serialization/TomlReflector.hpp>
#include <Engine/Core/Diagnostics/Logger.hpp>
#include <Math/LinearAlgebra/Vector2.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <Math/LinearAlgebra/Vector4.hpp>
#include <Math/LinearAlgebra/Quaternion.hpp>
#include <toml++/toml.hpp>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace fbzz::asset {

namespace {

/// @note 保持するのは絶対パスの表記だけ。現行の GUID の解決、ディスク確認、ロケールに応じた正規化は検索ごとに行う。
std::unordered_map<std::string, std::string>& AbsolutePathKeys()
{
    static std::unordered_map<std::string, std::string> keys;
    return keys;
}

/// @note 相対パス・絶対パス・GUID 参照は同じ共有実体と Undo を使う。
std::string NormalizeKey(const std::string& path)
{
    if (path.empty()) {
        return {};
    }
    const std::string resolved = AssetManager::ResolveAssetPath(path);
    if (resolved.empty()) {
        return {};
    }
    auto& absoluteKeys = AbsolutePathKeys();
    std::string key;
    if (const auto found = absoluteKeys.find(resolved); found != absoluteKeys.end()) {
        key = found->second;
    }
    else {
        const auto native = util::FileSystem::PathFromUtf8(resolved);
        std::error_code error;
        const auto absolute = std::filesystem::absolute(native, error);
        if (error) {
            return {};
        }
        key = util::FileSystem::PathToUtf8(absolute.lexically_normal());
        // 相対参照の表記はカレントディレクトリに依存する。失敗や相対入力はパス文字列だけを扱う有限のキャッシュに入れない。
        if (native.is_absolute() && absoluteKeys.size() < 4096) {
            absoluteKeys.emplace(resolved, key);
        }
    }
    for (char& c : key) {
        if (c == '\\') {
            c = '/';
        }
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return key;
}

/// @note DataAssetRef は IReflector の既定実装で path 文字列として往復する。
using util::TomlReadReflector;
using util::TomlWriteReflector;

/// @name キャッシュ
struct CacheEntry {
    std::unique_ptr<DataAsset> asset;
    std::string typeName;

    std::uint64_t factoryEpoch = 0; ///< 失敗したロードの型登録世代。同世代では同じ失敗を再試行しない。
    detail::NativeDataAssetType nativeType;
};

std::unordered_map<std::string, CacheEntry>& Cache()
{
    static std::unordered_map<std::string, CacheEntry> cache;
    return cache;
}

/// @note 保持するのは正規キーで、アセットへのポインターは保持しない。索引を退役させた後は元の解決経路を使い、再読込や取り消しでは現在の登録内容を読む。
struct LoadedGuidKeys {
    uint64_t revision = UINT64_MAX;
    std::unordered_map<std::string, std::string> keys;
    void Refresh(uint64_t current)
    {
        if (revision == current) {
            return;
        }
        keys.clear(); revision = current;
    }
};
LoadedGuidKeys& GuidKeys()
{
    static LoadedGuidKeys keys;
    return keys;
}

/// @return 読取・パース・型生成・typed 検証の失敗時は nullptr のエントリ。
CacheEntry LoadFromDisk(const std::string& path)
{
    // 失敗にも世代を刻み、型登録変更時だけ Resolve が再試行する。
    CacheEntry failed{ nullptr, {}, DataAssetFactory::RegistrationEpoch() };

    const std::string absPath = AssetManager::ResolveAssetPath(path);
    std::string text;
    if (!util::FileSystem::ReadText(absPath, text)) {
        FBZZ_LOG_WARN("DataAssetRegistry: file not found -> %s", path.c_str());
        return failed;
    }

    // toml++ は例外無効ビルド (TOML_EXCEPTIONS=0) のため parse_result を真偽で判定する。
    auto result = toml::parse(text);
    if (!result) {
        FBZZ_LOG_ERROR("DataAssetRegistry: TOML parse failed -> %s", path.c_str());
        return failed;
    }
    const toml::table& table = result.table();

    const std::string typeName = table["type"].value_or(std::string{});
    if (typeName.empty()) {
        FBZZ_LOG_ERROR("DataAssetRegistry: missing 'type' key -> %s", path.c_str());
        return failed;
    }

    auto [asset, nativeType] = detail::CreateDataAsset(typeName);
    if (!asset) {
        FBZZ_LOG_WARN("DataAssetRegistry: type '%s' not registered -> %s", typeName.c_str(), path.c_str());
        return failed;
    }

    if (typeName == RenderPipelineAsset::TYPE_NAME) {
        auto* pipeline = TryAsExactDataAsset<RenderPipelineAsset>(asset.get());
        if (!pipeline || !RenderPipelineAssetCodec::Load(table, *pipeline)) {
            FBZZ_LOG_WARN("DataAssetRegistry: invalid RenderPipelineAsset -> %s", path.c_str());
            return failed;
        }
    } else {
        TomlReadReflector reader(table);
        asset->Reflect(reader);
    }
    return { std::move(asset), typeName, DataAssetFactory::RegistrationEpoch(), nativeType };
}

/// @return typed アセットの編集内容が不正なら false。
bool BuildTable(DataAsset& asset, toml::table& table)
{
    if (std::string_view(asset.GetTypeName()) == RenderPipelineAsset::TYPE_NAME) {
        auto* pipeline = TryAsExactDataAsset<RenderPipelineAsset>(&asset);
        return pipeline && RenderPipelineAssetCodec::Save(*pipeline, table);
    }
    table.insert_or_assign("type", std::string(asset.GetTypeName()));
    TomlWriteReflector writer(table);
    asset.Reflect(writer);
    return true;
}

bool WriteTableToDisk(const std::string& path, const toml::table& table)
{
    std::ostringstream oss;
    oss << table;

    // GUID の索引が引けない保存は空の絶対パスになるため、黙って書き損じず報告する。
    const std::string absPath = AssetManager::ResolveAssetPath(path);
    if (absPath.empty()) {
        FBZZ_LOG_ERROR("DataAssetRegistry: cannot resolve save path -> %s", path.c_str());
        return false;
    }

    // 自動保存中の中途半端なファイルを原本にしないよう、旧版か新版へアトミックに置き換える。
    if (!util::FileSystem::WriteTextAtomic(absPath, oss.str())) {
        FBZZ_LOG_ERROR("DataAssetRegistry: save failed -> %s", absPath.c_str());
        return false;
    }
    return true;
}

CacheEntry* ResolveEntry(const std::string& path)
{
    if (path.empty()) {
        return nullptr;
    }
    const bool guid = AssetDatabase::IsGuidRef(path);
    const uint64_t revision = guid ? AssetDatabase::Revision() : 0;
    auto& aliases = GuidKeys();
    auto& cache = Cache();
    if (guid) {
        aliases.Refresh(revision);
        if (const auto key = aliases.keys.find(path); key != aliases.keys.end()) {
            const auto found = cache.find(key->second);
            if (found != cache.end() && found->second.asset) {
                return &found->second;
            }
        }
    }
    const std::string key = NormalizeKey(path);
    if (key.empty()) {
        return nullptr;
    }
    const auto remember = [&](CacheEntry& entry) -> CacheEntry* {
        // 読込済みの GUID アセットは描画フレームごとにディスクを確認する必要がない。欠落や失敗時は従来の再試行・診断経路を使う。
        if (!entry.asset) {
            return nullptr;
        }
        if (guid && AssetDatabase::Revision() == revision && aliases.keys.size() < 4096) {
            aliases.keys.insert_or_assign(path, key);
        }
        return &entry;
    };
    if (auto it = cache.find(key); it != cache.end()) {
        if (it->second.asset) {
            return remember(it->second);
        }

        // 失敗もキャッシュし、型登録が変わったときだけ再試行してログ連打と永久失敗を防ぐ。
        if (it->second.factoryEpoch == DataAssetFactory::RegistrationEpoch()) {
            return nullptr;
        }

        it->second = LoadFromDisk(key);
        return remember(it->second);
    }

    CacheEntry entry = LoadFromDisk(key);
    return remember(cache.emplace(key, std::move(entry)).first->second);
}

}

DataAsset* DataAssetRegistry::Resolve(const std::string& path)
{
    const auto* entry = ResolveEntry(path);
    return entry ? entry->asset.get() : nullptr;
}

void* DataAssetRegistry::ResolveNative(const std::string& path, const std::type_info& requestedType, DataAsset*& legacy)
{
    legacy = nullptr;
    const auto* entry = ResolveEntry(path);
    if (!entry) {
        return nullptr;
    }
    if (entry->nativeType.query) {
        return entry->nativeType.query(entry->nativeType.instance, requestedType);
    }
    legacy = entry->asset.get();
    return nullptr;
}

bool DataAssetRegistry::Save(const std::string& path)
{
    const std::string key = NormalizeKey(path);
    auto& cache = Cache();
    auto it = cache.find(key);
    if (it == cache.end() || !it->second.asset) {
        return false;
    }

    if (it->second.typeName == RenderPipelineAsset::TYPE_NAME) {
        // ディスク上の版が非互換または不正なら、最後に正常に読み込んだ値でディスクを上書きしない。
        std::string text;
        if (!util::FileSystem::ReadText(AssetManager::ResolveAssetPath(key), text)) {
            return false;
        }
        auto parsed = toml::parse(text);
        if (!parsed) {
            return false;
        }
        RenderPipelineAsset disk;
        if (!RenderPipelineAssetCodec::Load(parsed.table(), disk)) {
            return false;
        }
    }

    toml::table table;
    if (!BuildTable(*it->second.asset, table)) {
        return false;
    }
    return WriteTableToDisk(key, table);
}

bool DataAssetRegistry::Create(const std::string& path, const std::string& typeName)
{
    const std::string key = NormalizeKey(path);
    const std::string absPath = AssetManager::ResolveAssetPath(key);
    if (util::FileSystem::Exists(util::FileSystem::PathFromUtf8(absPath))) {
        FBZZ_LOG_WARN("DataAssetRegistry: already exists -> %s", path.c_str());
        return false;
    }

    auto [asset, nativeType] = detail::CreateDataAsset(typeName);
    if (!asset) {
        FBZZ_LOG_ERROR("DataAssetRegistry: cannot create unregistered type '%s'", typeName.c_str());
        return false;
    }

    if (typeName == RenderPipelineAsset::TYPE_NAME && !TryAsExactDataAsset<RenderPipelineAsset>(asset.get())) {
        return false;
    }
    toml::table table;
    if (!BuildTable(*asset, table)) {
        return false;
    }
    if (!WriteTableToDisk(key, table)) {
        return false;
    }

    // 生成直後の実体をそのままキャッシュへ載せる (次の Resolve でディスク再読込しない)。
    Cache().insert_or_assign(
        key, CacheEntry{ std::move(asset), typeName, DataAssetFactory::RegistrationEpoch(), nativeType });
    return true;
}

std::string DataAssetRegistry::Snapshot(const std::string& path)
{
    const std::string key = NormalizeKey(path);
    auto& cache = Cache();
    auto it = cache.find(key);
    if (it == cache.end() || !it->second.asset) {
        return {};
    }

    std::ostringstream oss;
    toml::table table;
    if (!BuildTable(*it->second.asset, table)) {
        return {};
    }
    oss << table;
    return oss.str();
}

bool DataAssetRegistry::RestoreSnapshot(const std::string& path, const std::string& snapshot)
{
    if (snapshot.empty()) {
        return false;
    }

    const std::string key = NormalizeKey(path);
    auto& cache = Cache();
    auto it = cache.find(key);
    if (it == cache.end() || !it->second.asset) {
        return false;
    }

    auto result = toml::parse(snapshot);
    if (!result) {
        FBZZ_LOG_ERROR("DataAssetRegistry: snapshot parse failed -> %s", path.c_str());
        return false;
    }

    // 現在の実体のアドレスを保持して復元し、Undo で型を差し替えない。配列は保存時の長さへ戻す。
    if (it->second.typeName == RenderPipelineAsset::TYPE_NAME) {
        auto* pipeline = TryAsExactDataAsset<RenderPipelineAsset>(it->second.asset.get());
        return pipeline && RenderPipelineAssetCodec::Load(result.table(), *pipeline);
    }
    TomlReadReflector reader(result.table());
    it->second.asset->Reflect(reader);
    return true;
}

int DataAssetRegistry::ReloadFile(const std::string& absPath)
{
    if (absPath.empty()) {
        return 0;
    }

    // 相対/GUID キャッシュキーを解決し、監視イベントの絶対パスと区切り・大小を無視して比較する。
    const auto samePath = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) {
            return false;
        }
        const auto fold = [](char c) {
            if (c == '\\') {
                return '/';
            }
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        };
        for (size_t i = 0; i < a.size(); ++i)
            if (fold(a[i]) != fold(b[i])) {
                return false;
            }
        return true;
    };

    int reloaded = 0;
    for (auto& [key, entry] : Cache()) {
        const std::string resolved = AssetManager::ResolveAssetPath(key);
        if (!samePath(resolved, absPath)) {
            continue;
        }

        std::string text;
        if (!util::FileSystem::ReadText(resolved, text)) {
            continue;
        }

        auto parsed = toml::parse(text);
        if (!parsed) {
            // 書き込み途中を掴んだ可能性がある。動いている値は残す。
            FBZZ_LOG_WARN("DataAssetRegistry: reload failed, keeping previous -> %s", key.c_str());
            continue;
        }
        const toml::table& table = parsed.table();
        const std::string typeName = table["type"].value_or(std::string{});

        // 型が同じなら実体は作り直さず、フィールドだけ上書きする。
        if (entry.asset && !typeName.empty() && entry.typeName == typeName) {
            if (typeName == RenderPipelineAsset::TYPE_NAME) {
                auto* pipeline = TryAsExactDataAsset<RenderPipelineAsset>(entry.asset.get());
                if (!pipeline || !RenderPipelineAssetCodec::Load(table, *pipeline)) {
                    FBZZ_LOG_WARN("DataAssetRegistry: invalid pipeline reload, keeping previous -> %s", key.c_str());
                    continue;
                }
            } else {
                TomlReadReflector reader(table);
                entry.asset->Reflect(reader);
            }
            ++reloaded;
            continue;
        }

        // 型が変わった / 前回のロードに失敗していた場合だけ実体を差し替える。
        CacheEntry fresh = LoadFromDisk(key);
        if (!fresh.asset) {
            continue;
        }
        entry = std::move(fresh);
        ++reloaded;
    }
    return reloaded;
}

std::string DataAssetRegistry::TypeOf(const std::string& path)
{
    const std::string key = NormalizeKey(path);
    auto& cache = Cache();
    if (auto it = cache.find(key); it != cache.end()) {
        return it->second.typeName;
    }
    return {};
}

void DataAssetRegistry::ClearCache()
{
    Cache().clear();
    AbsolutePathKeys().clear();
    GuidKeys().keys.clear();
}

}
