/// @file    DataAssetRegistry.cpp
/// @brief   DataAsset の「パス → 共有 1 実体」キャッシュと .fzdata (TOML) 入出力の実装。
/// @author  Hasegawa Jin
/// @date    2026-07-01
///
/// 値型の TOML 変換は util::TomlWrite/ReadReflector (Engine/Scene/TomlReflector.hpp) を使う。
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Asset/DataAsset.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>
#include <Engine/Asset/AssetManager.hpp>
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

/// 値型と入れ子スコープの TOML 変換は util の共通リフレクタへ委譲する。
/// DataAsset が扱うのは値型 + 参照パス文字列だけなので、派生は要らない
/// (DataAssetRef は IReflector の既定実装で path 文字列として往復する)。
using util::TomlReadReflector;
using util::TomlWriteReflector;

/// @name キャッシュ
struct CacheEntry {
    std::unique_ptr<DataAsset> asset;
    std::string typeName;

    /// ロードを試みた時点の型登録の世代。asset == nullptr のときだけ意味を持ち、
    /// 「同じ登録状態なら結果も変わらない」判定に使う (Resolve の再試行条件)。
    std::uint64_t factoryEpoch = 0;
};

std::unordered_map<std::string, CacheEntry>& Cache()
{
    static std::unordered_map<std::string, CacheEntry> cache;
    return cache;
}

/// .fzdata をパースして型生成 + フィールド読み込みを行う。失敗時 nullptr。
CacheEntry LoadFromDisk(const std::string& path)
{
    /// @note 失敗して返すエントリにも世代を刻む。Resolve はこの値を見て
    ///       「型登録が変わったのでもう一度試す価値がある」かを判断する。
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

    TomlReadReflector reader(table);
    asset->Reflect(reader);
    return { std::move(asset), typeName, DataAssetFactory::RegistrationEpoch() };
}

/// DataAsset を toml::table へ書き出し (type キー + 全フィールド)。
toml::table BuildTable(DataAsset& asset)
{
    toml::table table;
    table.insert_or_assign("type", std::string(asset.GetTypeName()));
    TomlWriteReflector writer(table);
    asset.Reflect(writer);
    return table;
}

bool WriteTableToDisk(const std::string& path, const toml::table& table)
{
    std::ostringstream oss;
    oss << table;

    /// @note 参照が guid 形式のままここへ来て索引が引けないと、絶対パスが空になる。
    ///       黙って書き損じると「編集したのに保存されていない」に化けるので必ず報告する。
    const std::string absPath = AssetManager::ResolveAssetPath(path);
    if (absPath.empty()) {
        FBZZ_LOG_ERROR("DataAssetRegistry: cannot resolve save path -> %s", path.c_str());
        return false;
    }

    /// @note .fzdata は Inspector のウィジェットを離すたびに自動保存される。通常の上書きだと
    ///       切り詰め済みの状態が一瞬でも露出し、そこで落ちる・掴まれると壊れたファイルが
    ///       原本として残る。アトミック置き換えなら旧版か新版のどちらかになる。
    if (!util::FileSystem::WriteTextAtomic(absPath, oss.str())) {
        FBZZ_LOG_ERROR("DataAssetRegistry: save failed -> %s", absPath.c_str());
        return false;
    }
    return true;
}

} // namespace

DataAsset* DataAssetRegistry::Resolve(const std::string& path)
{
    if (path.empty()) return nullptr;
    const std::string key = NormalizeKey(path);

    auto& cache = Cache();
    if (auto it = cache.find(key); it != cache.end()) {
        if (it->second.asset) return it->second.asset.get();

        /// @note 失敗 (nullptr) もキャッシュして毎フレームのディスクアクセス・ログ連打を防ぐ。
        ///       型登録が変わっていれば結果が変わりうるのでそのときだけ引き直す。型が登録される
        ///       前に一度 Resolve されただけで参照が永久に死ぬのを防ぐ (DLL ロード順やホット
        ///       リロードの過渡状態で普通に起こる)。
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

    const toml::table table = BuildTable(*it->second.asset);
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

    const toml::table table = BuildTable(*asset);
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
    oss << BuildTable(*it->second.asset);
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

    /// @note "type" キーは読み飛ばす。復元先は常に「今キャッシュされている実体」であり、
    ///       スナップショットで型を差し替えることはしない (型が変わる操作は Undo 対象外)。
    ///       構造体配列は BeginObjectList が保存時の要素数を返し、呼び出し側がその値で
    ///       resize するため、スナップショットより要素が増えている状態からでも正しく縮む。
    TomlReadReflector reader(result.table());
    it->second.asset->Reflect(reader);
    return true;
}

int DataAssetRegistry::ReloadFile(const std::string& absPath)
{
    if (absPath.empty()) return 0;

    /// @note キャッシュキーは "Assets/..." 相対と guid 参照が混在する。監視イベントは絶対パス
    ///       なので、キーを解決してから区切り文字と大小を無視して突き合わせる。
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
            TomlReadReflector reader(table);
            entry.asset->Reflect(reader);
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

} // namespace fbzz::asset
