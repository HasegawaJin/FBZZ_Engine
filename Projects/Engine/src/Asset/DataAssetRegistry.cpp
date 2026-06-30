// FBZZ Engine
// DataAssetRegistry.cpp | fbzz::asset
// DataAsset の「パス → 共有 1 実体」キャッシュと .fzdata (TOML) 入出力の実装。
//
// 値型の TOML 変換はここに閉じた小さなリフレクタで行う。
// WHY: SceneSerializer.cpp の TomlWrite/ReadReflector は Scene 直列化に深く結合しており、
//      共有化リファクタはシーン保存を不安定化させるリスクがある。DataAsset が必要とするのは
//      値型 + 参照パス文字列のみなので、独立した最小リフレクタを置く (将来 SceneSerializer と統一可)。
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Asset/DataAsset.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Scene/Script.hpp>           // scene::IReflector / DataAssetRef
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Core/Logger.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Math/Quaternion.hpp>
#include <toml++/toml.hpp>
#include <memory>
#include <sstream>
#include <unordered_map>

namespace fbzz::asset {

namespace {

// ── パスキー正規化 (区切りを / に統一) ──────────────────────────────────────
std::string NormalizeKey(const std::string& path)
{
    std::string key = path;
    for (char& c : key)
        if (c == '\\') c = '/';
    return key;
}

// ── 値型 ⇔ TOML 配列ヘルパー ────────────────────────────────────────────────
toml::array Vec2ToArr(const math::Vector2& v) { return toml::array{ (double)v.x, (double)v.y }; }
toml::array Vec3ToArr(const math::Vector3& v) { return toml::array{ (double)v.x, (double)v.y, (double)v.z }; }
toml::array Vec4ToArr(const math::Vector4& v) { return toml::array{ (double)v.x, (double)v.y, (double)v.z, (double)v.w }; }
toml::array QuatToArr(const math::Quaternion& q) { return toml::array{ (double)q.x, (double)q.y, (double)q.z, (double)q.w }; }

math::Vector2 ArrToVec2(const toml::array* a, math::Vector2 def)
{
    if (!a || a->size() < 2) return def;
    return { (float)a->at(0).value_or((double)def.x), (float)a->at(1).value_or((double)def.y) };
}
math::Vector3 ArrToVec3(const toml::array* a, math::Vector3 def)
{
    if (!a || a->size() < 3) return def;
    return { (float)a->at(0).value_or((double)def.x), (float)a->at(1).value_or((double)def.y),
             (float)a->at(2).value_or((double)def.z) };
}
math::Vector4 ArrToVec4(const toml::array* a, math::Vector4 def)
{
    if (!a || a->size() < 4) return def;
    return { (float)a->at(0).value_or((double)def.x), (float)a->at(1).value_or((double)def.y),
             (float)a->at(2).value_or((double)def.z), (float)a->at(3).value_or((double)def.w) };
}
math::Quaternion ArrToQuat(const toml::array* a, math::Quaternion def)
{
    if (!a || a->size() < 4) return def;
    return { (float)a->at(0).value_or((double)def.x), (float)a->at(1).value_or((double)def.y),
             (float)a->at(2).value_or((double)def.z), (float)a->at(3).value_or((double)def.w) };
}

// ── 書き込みリフレクタ (DataAsset → toml::table) ─────────────────────────────
class TomlWriteReflector : public scene::IReflector {
public:
    explicit TomlWriteReflector(toml::table& table) : m_table(table) {}

    void Field(const char* name, float& v) override { m_table.insert_or_assign(name, (double)v); }
    void Field(const char* name, int& v) override { m_table.insert_or_assign(name, (int64_t)v); }
    void Field(const char* name, bool& v) override { m_table.insert_or_assign(name, v); }
    void Field(const char* name, std::string& v) override { m_table.insert_or_assign(name, v); }
    void Field(const char* name, math::Vector2& v) override { m_table.insert_or_assign(name, Vec2ToArr(v)); }
    void Field(const char* name, math::Vector3& v) override { m_table.insert_or_assign(name, Vec3ToArr(v)); }
    void Field(const char* name, math::Vector4& v) override { m_table.insert_or_assign(name, Vec4ToArr(v)); }
    void Field(const char* name, math::Quaternion& v) override { m_table.insert_or_assign(name, QuatToArr(v)); }

private:
    toml::table& m_table;
};

// ── 読み込みリフレクタ (toml::table → DataAsset) ─────────────────────────────
class TomlReadReflector : public scene::IReflector {
public:
    explicit TomlReadReflector(const toml::table& table) : m_table(table) {}

    void Field(const char* name, float& v) override { v = (float)m_table[name].value_or((double)v); }
    void Field(const char* name, int& v) override { v = (int)m_table[name].value_or((int64_t)v); }
    void Field(const char* name, bool& v) override { v = m_table[name].value_or(v); }
    void Field(const char* name, std::string& v) override { v = m_table[name].value_or(v); }
    void Field(const char* name, math::Vector2& v) override { v = ArrToVec2(m_table[name].as_array(), v); }
    void Field(const char* name, math::Vector3& v) override { v = ArrToVec3(m_table[name].as_array(), v); }
    void Field(const char* name, math::Vector4& v) override { v = ArrToVec4(m_table[name].as_array(), v); }
    void Field(const char* name, math::Quaternion& v) override { v = ArrToQuat(m_table[name].as_array(), v); }

private:
    const toml::table& m_table;
};

// ── キャッシュ ───────────────────────────────────────────────────────────────
struct CacheEntry {
    std::unique_ptr<DataAsset> asset;
    std::string typeName;
};

std::unordered_map<std::string, CacheEntry>& Cache()
{
    static std::unordered_map<std::string, CacheEntry> cache;
    return cache;
}

// .fzdata をパースして型生成 + フィールド読み込みを行う。失敗時 nullptr。
CacheEntry LoadFromDisk(const std::string& path)
{
    const std::string absPath = AssetManager::ResolveAssetPath(path);
    std::string text;
    if (!util::FileSystem::ReadText(absPath, text)) {
        FBZZ_LOG_WARN("DataAssetRegistry: file not found -> %s", path.c_str());
        return {};
    }

    // toml++ は例外無効ビルド (TOML_EXCEPTIONS=0) のため parse_result を真偽で判定する。
    auto result = toml::parse(text);
    if (!result) {
        FBZZ_LOG_ERROR("DataAssetRegistry: TOML parse failed -> %s", path.c_str());
        return {};
    }
    const toml::table& table = result.table();

    const std::string typeName = table["type"].value_or(std::string{});
    if (typeName.empty()) {
        FBZZ_LOG_ERROR("DataAssetRegistry: missing 'type' key -> %s", path.c_str());
        return {};
    }

    std::unique_ptr<DataAsset> asset = DataAssetFactory::Create(typeName);
    if (!asset) {
        FBZZ_LOG_WARN("DataAssetRegistry: type '%s' not registered -> %s", typeName.c_str(), path.c_str());
        return {};
    }

    TomlReadReflector reader(table);
    asset->Reflect(reader);
    return { std::move(asset), typeName };
}

// DataAsset を toml::table へ書き出し (type キー + 全フィールド)。
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
    const std::string absPath = AssetManager::ResolveAssetPath(path);
    return util::FileSystem::WriteText(absPath, oss.str());
}

} // namespace

DataAsset* DataAssetRegistry::Resolve(const std::string& path)
{
    if (path.empty()) return nullptr;
    const std::string key = NormalizeKey(path);

    auto& cache = Cache();
    if (auto it = cache.find(key); it != cache.end())
        return it->second.asset.get();

    CacheEntry entry = LoadFromDisk(key);
    DataAsset* ptr = entry.asset.get();
    // 失敗 (nullptr) もキャッシュして毎フレームのディスクアクセス・ログ連打を防ぐ。
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

    // 生成直後の実体をそのままキャッシュへ載せる (次の Resolve でディスク再読込しない)。
    Cache().insert_or_assign(key, CacheEntry{ std::move(asset), typeName });
    return true;
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
