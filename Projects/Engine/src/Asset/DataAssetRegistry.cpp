/// @file    DataAssetRegistry.cpp
/// @brief   DataAsset の「パス → 共有 1 実体」キャッシュと .fzdata (TOML) 入出力の実装。
/// @author  Hasegawa Jin
/// @date    2026-07-01
///
/// @note 汎用の値型変換は Engine/Scene/TomlReflector.hpp に委譲する。
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Asset/DataAsset.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/RenderPipelineAsset.hpp>
#include "RenderPipelineAssetCodec.hpp"
/// @note scene::IReflector / DataAssetRef を使う。
#include <Engine/Scene/Script.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Scene/TomlReflector.hpp>
#include <Engine/Core/Logger.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Math/Quaternion.hpp>
#include <toml++/toml.hpp>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fbzz::asset {

namespace {

/// @name パスキー正規化 (区切りを / に統一)
std::string NormalizeKey(const std::string& path)
{
    std::string key = path;
    for (char& c : key)
        if (c == '\\') c = '/';
    return key;
}

/// @note DataAssetRef は IReflector の既定実装で path 文字列として往復する。
using util::TomlReadReflector;
using util::TomlWriteReflector;

/// @name キャッシュ
struct CacheEntry {
    std::unique_ptr<DataAsset> asset;
    std::string typeName;

    /// @note 失敗したロードの型登録世代。同世代では同じ失敗を再試行しない。
    std::uint64_t factoryEpoch = 0;
};

std::unordered_map<std::string, CacheEntry>& Cache()
{
    static std::unordered_map<std::string, CacheEntry> cache;
    return cache;
}

/// @return 読取・パース・型生成・typed 検証の失敗時は nullptr のエントリ。
CacheEntry LoadFromDisk(const std::string& path)
{
    /// @note 失敗にも世代を刻み、型登録変更時だけ Resolve が再試行する。
    CacheEntry failed{ nullptr, {}, DataAssetFactory::RegistrationEpoch() };

    const std::string absPath = AssetManager::ResolveAssetPath(path);
    std::string text;
    if (!util::FileSystem::ReadText(absPath, text)) {
        FBZZ_LOG_WARN("DataAssetRegistry: file not found -> %s", path.c_str());
        return failed;
    }

    /// @note toml++ は例外無効ビルド (TOML_EXCEPTIONS=0) のため parse_result を真偽で判定する。
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

    std::unique_ptr<DataAsset> asset = DataAssetFactory::Create(typeName);
    if (!asset) {
        FBZZ_LOG_WARN("DataAssetRegistry: type '%s' not registered -> %s", typeName.c_str(), path.c_str());
        return failed;
    }

    if (typeName == RenderPipelineAsset::TYPE_NAME) {
        if (!RenderPipelineAssetCodec::Load(table, static_cast<RenderPipelineAsset&>(*asset))) {
            FBZZ_LOG_WARN("DataAssetRegistry: invalid RenderPipelineAsset -> %s", path.c_str());
            return failed;
        }
    } else {
        TomlReadReflector reader(table);
        asset->Reflect(reader);
    }
    return { std::move(asset), typeName, DataAssetFactory::RegistrationEpoch() };
}

/// @return typed アセットの編集内容が不正なら false。
bool BuildTable(DataAsset& asset, toml::table& table)
{
    if (std::string_view(asset.GetTypeName()) == RenderPipelineAsset::TYPE_NAME)
        return RenderPipelineAssetCodec::Save(static_cast<const RenderPipelineAsset&>(asset), table);
    table.insert_or_assign("type", std::string(asset.GetTypeName()));
    TomlWriteReflector writer(table);
    asset.Reflect(writer);
    return true;
}

bool WriteTableToDisk(const std::string& path, const toml::table& table)
{
    std::ostringstream oss;
    oss << table;

    /// @note GUID の索引が引けない保存は空の絶対パスになるため、黙って書き損じず報告する。
    const std::string absPath = AssetManager::ResolveAssetPath(path);
    if (absPath.empty()) {
        FBZZ_LOG_ERROR("DataAssetRegistry: cannot resolve save path -> %s", path.c_str());
        return false;
    }

    /// @note 自動保存中の中途半端なファイルを原本にしないよう、旧版か新版へアトミックに置き換える。
    if (!util::FileSystem::WriteTextAtomic(absPath, oss.str())) {
        FBZZ_LOG_ERROR("DataAssetRegistry: save failed -> %s", absPath.c_str());
        return false;
    }
    return true;
}

} /// @note namespace

DataAsset* DataAssetRegistry::Resolve(const std::string& path)
{
    if (path.empty()) return nullptr;
    const std::string key = NormalizeKey(path);

    auto& cache = Cache();
    if (auto it = cache.find(key); it != cache.end()) {
        if (it->second.asset) return it->second.asset.get();

        /// @note 失敗もキャッシュし、型登録が変わったときだけ再試行してログ連打と永久失敗を防ぐ。
        if (it->second.factoryEpoch == DataAssetFactory::RegistrationEpoch())
            return nullptr;

        it->second = LoadFromDisk(key);
        return it->second.asset.get();
    }

    CacheEntry entry = LoadFromDisk(key);
    DataAsset* ptr = entry.asset.get();
    cache.emplace(key, std::move(entry));
    return ptr;
}

bool DataAssetRegistry::Save(const std::string& path)
{
    const std::string key = NormalizeKey(path);
    auto& cache = Cache();
    auto it = cache.find(key);
    if (it == cache.end() || !it->second.asset) return false;

    if (it->second.typeName == RenderPipelineAsset::TYPE_NAME) {
        /// @note Last-good reload values must not overwrite an incompatible or malformed disk revision.
        std::string text;
        if (!util::FileSystem::ReadText(AssetManager::ResolveAssetPath(key), text)) return false;
        auto parsed = toml::parse(text);
        if (!parsed) return false;
        RenderPipelineAsset disk;
        if (!RenderPipelineAssetCodec::Load(parsed.table(), disk)) return false;
    }

    toml::table table;
    if (!BuildTable(*it->second.asset, table)) return false;
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

    std::unique_ptr<DataAsset> asset = DataAssetFactory::Create(typeName);
    if (!asset) {
        FBZZ_LOG_ERROR("DataAssetRegistry: cannot create unregistered type '%s'", typeName.c_str());
        return false;
    }

    toml::table table;
    if (!BuildTable(*asset, table)) return false;
    if (!WriteTableToDisk(key, table)) return false;

    /// @note 生成直後の実体をそのままキャッシュへ載せる (次の Resolve でディスク再読込しない)。
    Cache().insert_or_assign(
        key, CacheEntry{ std::move(asset), typeName, DataAssetFactory::RegistrationEpoch() });
    return true;
}

std::string DataAssetRegistry::Snapshot(const std::string& path)
{
    const std::string key = NormalizeKey(path);
    auto& cache = Cache();
    auto it = cache.find(key);
    if (it == cache.end() || !it->second.asset) return {};

    std::ostringstream oss;
    toml::table table;
    if (!BuildTable(*it->second.asset, table)) return {};
    oss << table;
    return oss.str();
}

bool DataAssetRegistry::RestoreSnapshot(const std::string& path, const std::string& snapshot)
{
    if (snapshot.empty()) return false;

    const std::string key = NormalizeKey(path);
    auto& cache = Cache();
    auto it = cache.find(key);
    if (it == cache.end() || !it->second.asset) return false;

    auto result = toml::parse(snapshot);
    if (!result) {
        FBZZ_LOG_ERROR("DataAssetRegistry: snapshot parse failed -> %s", path.c_str());
        return false;
    }

    /// @note 現在の実体のアドレスを保持して復元し、Undo で型を差し替えない。配列は保存時の長さへ戻す。
    if (it->second.typeName == RenderPipelineAsset::TYPE_NAME)
        return RenderPipelineAssetCodec::Load(result.table(), static_cast<RenderPipelineAsset&>(*it->second.asset));
    TomlReadReflector reader(result.table());
    it->second.asset->Reflect(reader);
    return true;
}

int DataAssetRegistry::ReloadFile(const std::string& absPath)
{
    if (absPath.empty()) return 0;

    /// @note 相対/GUID キャッシュキーを解決し、監視イベントの絶対パスと区切り・大小を無視して比較する。
    const auto samePath = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        const auto fold = [](char c) {
            if (c == '\\') return '/';
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        };
        for (size_t i = 0; i < a.size(); ++i)
            if (fold(a[i]) != fold(b[i])) return false;
        return true;
    };

    int reloaded = 0;
    for (auto& [key, entry] : Cache()) {
        const std::string resolved = AssetManager::ResolveAssetPath(key);
        if (!samePath(resolved, absPath)) continue;

        std::string text;
        if (!util::FileSystem::ReadText(resolved, text)) continue;

        auto parsed = toml::parse(text);
        if (!parsed) {
            /// @note 書き込み途中を掴んだ可能性がある。動いている値は残す。
            FBZZ_LOG_WARN("DataAssetRegistry: reload failed, keeping previous -> %s", key.c_str());
            continue;
        }
        const toml::table& table = parsed.table();
        const std::string typeName = table["type"].value_or(std::string{});

        /// @note 型が同じなら実体は作り直さず、フィールドだけ上書きする。
        if (entry.asset && !typeName.empty() && entry.typeName == typeName) {
            if (typeName == RenderPipelineAsset::TYPE_NAME) {
                if (!RenderPipelineAssetCodec::Load(table, static_cast<RenderPipelineAsset&>(*entry.asset))) {
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

        /// @note 型が変わった / 前回のロードに失敗していた場合だけ実体を差し替える。
        CacheEntry fresh = LoadFromDisk(key);
        if (!fresh.asset) continue;
        entry = std::move(fresh);
        ++reloaded;
    }
    return reloaded;
}

std::string DataAssetRegistry::TypeOf(const std::string& path)
{
    const std::string key = NormalizeKey(path);
    auto& cache = Cache();
    if (auto it = cache.find(key); it != cache.end())
        return it->second.typeName;
    return {};
}

void DataAssetRegistry::ClearCache()
{
    Cache().clear();
}

} /// @note namespace fbzz::asset
