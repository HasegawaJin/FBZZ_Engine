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
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <vector>

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
//
// 入れ子オブジェクトと構造体配列に対応するため、書き込み先をスタックで管理する。
// WHY スタックが要るか: BeginObject / BeginObjectElement は「現在の書き込み先」を
//      一時的に子テーブルへ差し替える。ネストは任意の深さになりうるため、
//      復帰先を LIFO で覚えておく必要がある。
class TomlWriteReflector : public scene::IReflector {
public:
    explicit TomlWriteReflector(toml::table& table) { m_stack.push_back(&table); }

    void Field(const char* name, float& v) override { Current().insert_or_assign(PersistentKey(name), (double)v); }
    void Field(const char* name, int& v) override { Current().insert_or_assign(PersistentKey(name), (int64_t)v); }
    void Field(const char* name, bool& v) override { Current().insert_or_assign(PersistentKey(name), v); }
    void Field(const char* name, std::string& v) override { Current().insert_or_assign(PersistentKey(name), v); }
    void Field(const char* name, math::Vector2& v) override { Current().insert_or_assign(PersistentKey(name), Vec2ToArr(v)); }
    void Field(const char* name, math::Vector3& v) override { Current().insert_or_assign(PersistentKey(name), Vec3ToArr(v)); }
    void Field(const char* name, math::Vector4& v) override { Current().insert_or_assign(PersistentKey(name), Vec4ToArr(v)); }
    void Field(const char* name, math::Quaternion& v) override { Current().insert_or_assign(PersistentKey(name), QuatToArr(v)); }

    // ── 入れ子オブジェクト ───────────────────────────────────────────────────
    void BeginObject(const char* name) override
    {
        // 親へ空テーブルを挿入し、その実体を書き込み先に積む。
        // WHY 先に挿入するか: toml::table を後から move で挿入すると、
        //      構築中に取得したポインタが無効化される。先に置いて実体の
        //      アドレスを確定させてから書き込む方が安全。
        auto [iterator, inserted] =
            Current().insert_or_assign(PersistentKey(name), toml::table{});
        toml::table* child = iterator->second.as_table();
        // 挿入直後なので as_table() は必ず成功する。防御的に親を積み直して破綻を避ける。
        m_stack.push_back(child ? child : &Current());
    }

    void EndObject() override
    {
        // ルート (最初の 1 枚) は決して pop しない。
        if (m_stack.size() > 1) m_stack.pop_back();
    }

    // ── 構造体配列 ───────────────────────────────────────────────────────────
    std::size_t BeginObjectList(const char* name, std::size_t count) override
    {
        auto [iterator, inserted] =
            Current().insert_or_assign(PersistentKey(name), toml::array{});
        m_listStack.push_back(iterator->second.as_array());
        return count;   // 書き込みは要素数を変えない
    }

    void BeginObjectElement(std::size_t index) override
    {
        (void)index;
        toml::array* array = m_listStack.empty() ? nullptr : m_listStack.back();
        if (!array) { m_stack.push_back(&Current()); return; }

        array->push_back(toml::table{});
        toml::table* element = array->back().as_table();
        m_stack.push_back(element ? element : &Current());
    }

    void EndObjectElement() override { EndObject(); }

    std::size_t EndObjectList() override
    {
        if (!m_listStack.empty()) m_listStack.pop_back();
        return NO_REMOVE;   // 永続化は要素を削除しない
    }

private:
    toml::table& Current() { return *m_stack.back(); }

    std::vector<toml::table*> m_stack;
    std::vector<toml::array*> m_listStack;
};

// ── 読み込みリフレクタ (toml::table → DataAsset) ─────────────────────────────
//
// 書き込み側と同じくスタックで読み込み元を管理する。
// 対応するテーブルが存在しない入れ子は「欠損スコープ」として積み、
// 中のフィールドはすべて既定値のまま残す (部分的に古いファイルでも壊れない)。
class TomlReadReflector : public scene::IReflector {
public:
    explicit TomlReadReflector(const toml::table& table) { m_stack.push_back(&table); }

    void Field(const char* name, float& v) override { if (const auto* n = Find(name)) v = (float)n->value_or((double)v); }
    void Field(const char* name, int& v) override { if (const auto* n = Find(name)) v = (int)n->value_or((int64_t)v); }
    void Field(const char* name, bool& v) override { if (const auto* n = Find(name)) v = n->value_or(v); }
    void Field(const char* name, std::string& v) override { if (const auto* n = Find(name)) v = n->value_or(v); }
    void Field(const char* name, math::Vector2& v) override { const auto* n = Find(name); v = ArrToVec2(n ? n->as_array() : nullptr, v); }
    void Field(const char* name, math::Vector3& v) override { const auto* n = Find(name); v = ArrToVec3(n ? n->as_array() : nullptr, v); }
    void Field(const char* name, math::Vector4& v) override { const auto* n = Find(name); v = ArrToVec4(n ? n->as_array() : nullptr, v); }
    void Field(const char* name, math::Quaternion& v) override { const auto* n = Find(name); v = ArrToQuat(n ? n->as_array() : nullptr, v); }

    // ── 入れ子オブジェクト ───────────────────────────────────────────────────
    void BeginObject(const char* name) override
    {
        const toml::node* node = Find(name);
        // 見つからなければ nullptr を積む。以降の Field は Current() が null なので
        // 何も読まず、呼び出し側の既定値がそのまま残る。
        m_stack.push_back(node ? node->as_table() : nullptr);
    }

    void EndObject() override
    {
        if (m_stack.size() > 1) m_stack.pop_back();
    }

    // ── 構造体配列 ───────────────────────────────────────────────────────────
    std::size_t BeginObjectList(const char* name, std::size_t count) override
    {
        const toml::node* node  = Find(name);
        const toml::array* array = node ? node->as_array() : nullptr;
        m_listStack.push_back(array);
        // 保存されていた要素数を返す。呼び出し側はこの値で vector を resize する。
        // 配列が無い場合は 0 を返し、既存要素を消す (ファイルの内容を正とする)。
        return array ? array->size() : 0u;
    }

    void BeginObjectElement(std::size_t index) override
    {
        const toml::array* array = m_listStack.empty() ? nullptr : m_listStack.back();
        if (!array || index >= array->size()) { m_stack.push_back(nullptr); return; }
        m_stack.push_back(array->at(index).as_table());
    }

    void EndObjectElement() override { EndObject(); }

    std::size_t EndObjectList() override
    {
        if (!m_listStack.empty()) m_listStack.pop_back();
        return NO_REMOVE;
    }

private:
    const toml::table* Current() const { return m_stack.back(); }

    const toml::node* Find(const char* fallback) const
    {
        const toml::table* table = Current();
        if (!table) return nullptr;   // 欠損スコープの内側

        if (const toml::node* node = table->get(PersistentKey(fallback))) return node;
        return nullptr;
    }

    std::vector<const toml::table*> m_stack;
    std::vector<const toml::array*> m_listStack;
};

// ── キャッシュ ───────────────────────────────────────────────────────────────
struct CacheEntry {
    std::unique_ptr<DataAsset> asset;
    std::string typeName;

    // ロードを試みた時点の型登録の世代。asset == nullptr のときだけ意味を持ち、
    // 「同じ登録状態なら結果も変わらない」判定に使う (Resolve の再試行条件)。
    std::uint64_t factoryEpoch = 0;
};

std::unordered_map<std::string, CacheEntry>& Cache()
{
    static std::unordered_map<std::string, CacheEntry> cache;
    return cache;
}

// .fzdata をパースして型生成 + フィールド読み込みを行う。失敗時 nullptr。
CacheEntry LoadFromDisk(const std::string& path)
{
    // 失敗して返すエントリにも世代を刻む。Resolve はこの値を見て
    // 「型登録が変わったのでもう一度試す価値がある」かを判断する。
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

    std::unique_ptr<DataAsset> asset = DataAssetFactory::Create(typeName);
    if (!asset) {
        FBZZ_LOG_WARN("DataAssetRegistry: type '%s' not registered -> %s", typeName.c_str(), path.c_str());
        return failed;
    }

    TomlReadReflector reader(table);
    asset->Reflect(reader);
    return { std::move(asset), typeName, DataAssetFactory::RegistrationEpoch() };
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

    // 参照が guid 形式のままここへ来て索引が引けないと、絶対パスが空になる。
    // 黙って書き損じると「編集したのに保存されていない」に化けるので必ず報告する。
    const std::string absPath = AssetManager::ResolveAssetPath(path);
    if (absPath.empty()) {
        FBZZ_LOG_ERROR("DataAssetRegistry: cannot resolve save path -> %s", path.c_str());
        return false;
    }

    // WHY アトミック版か: .fzdata は Inspector のウィジェットを離すたびに自動保存される。
    //      通常の上書きだと切り詰め済みの状態が一瞬でも露出し、そこで落ちる・掴まれると
    //      壊れたファイルが原本として残る。置き換え方式なら旧版か新版のどちらかになる。
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

        // 失敗 (nullptr) もキャッシュして毎フレームのディスクアクセス・ログ連打を防ぐ。
        // ただし型登録が変わっていれば結果が変わりうるので、そのときだけ引き直す。
        // WHY: 型が登録される前に一度 Resolve されただけで参照が永久に死ぬのを防ぐ。
        //      DLL ロード順やホットリロードの過渡状態で普通に起こる。
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

    // 生成直後の実体をそのままキャッシュへ載せる (次の Resolve でディスク再読込しない)。
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

    // "type" キーは読み飛ばす。復元先は常に「今キャッシュされている実体」であり、
    // スナップショットで型を差し替えることはしない (型が変わる操作は Undo 対象外)。
    // 構造体配列は BeginObjectList が保存時の要素数を返し、呼び出し側がその値で
    // resize するため、スナップショットより要素が増えている状態からでも正しく縮む。
    TomlReadReflector reader(result.table());
    it->second.asset->Reflect(reader);
    return true;
}

int DataAssetRegistry::ReloadFile(const std::string& absPath)
{
    if (absPath.empty()) return 0;

    // キャッシュキーは "Assets/..." 相対と guid 参照が混在する。監視イベントは絶対パス
    // なので、キーを解決してから区切り文字と大小を無視して突き合わせる。
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
            // 書き込み途中を掴んだ可能性がある。動いている値は残す。
            FBZZ_LOG_WARN("DataAssetRegistry: reload failed, keeping previous -> %s", key.c_str());
            continue;
        }
        const toml::table& table = parsed.table();
        const std::string typeName = table["type"].value_or(std::string{});

        // 型が同じなら実体は作り直さず、フィールドだけ上書きする。
        if (entry.asset && !typeName.empty() && entry.typeName == typeName) {
            TomlReadReflector reader(table);
            entry.asset->Reflect(reader);
            ++reloaded;
            continue;
        }

        // 型が変わった / 前回のロードに失敗していた場合だけ実体を差し替える。
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
