/// @file    ScriptSnapshot.cpp
/// @brief   Script 1 個の Reflect フィールド ↔ TOML テキスト。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <Editor/Util/ScriptSnapshot.hpp>
#include <Engine/Asset/ParticleEmitterAssetCodec.hpp>
#include <Engine/Scene/Script.hpp>
#include <toml++/toml.hpp>

#include <cstddef>
#include <sstream>
#include <vector>

namespace fbzz::editor {

namespace {

toml::array Vec2ToArr(const math::Vector2& v)
{
    toml::array a; a.push_back((double)v.x); a.push_back((double)v.y); return a;
}
toml::array Vec3ToArr(const math::Vector3& v)
{
    toml::array a; a.push_back((double)v.x); a.push_back((double)v.y); a.push_back((double)v.z); return a;
}
toml::array Vec4ToArr(const math::Vector4& v)
{
    toml::array a;
    a.push_back((double)v.x); a.push_back((double)v.y);
    a.push_back((double)v.z); a.push_back((double)v.w);
    return a;
}
toml::array QuatToArr(const math::Quaternion& q)
{
    toml::array a;
    a.push_back((double)q.x); a.push_back((double)q.y);
    a.push_back((double)q.z); a.push_back((double)q.w);
    return a;
}

// 配列が欠けている / 要素数が足りない場合は現在値を保つ。
// WHY: スナップショットを取った後にスクリプトへフィールドを足す場面があり、
//      その差で値がゼロクリアされると Undo が「壊す操作」になってしまう。
float ArrAt(const toml::array* arr, std::size_t i, float fallback)
{
    if (!arr || i >= arr->size()) return fallback;
    return static_cast<float>((*arr)[i].value_or(static_cast<double>(fallback)));
}

// ── 書き出し ─────────────────────────────────────────────────────────────────
class SnapshotWriter final : public scene::IReflector {
public:
    explicit SnapshotWriter(toml::table& table) { m_stack.push_back(&table); }

    void Field(const char* name, float& v) override         { Current().insert_or_assign(PersistentKey(name), (double)v); }
    void Field(const char* name, int& v) override           { Current().insert_or_assign(PersistentKey(name), (int64_t)v); }
    void Field(const char* name, bool& v) override          { Current().insert_or_assign(PersistentKey(name), v); }
    void Field(const char* name, math::Vector2& v) override { Current().insert_or_assign(PersistentKey(name), Vec2ToArr(v)); }
    void Field(const char* name, math::Vector3& v) override { Current().insert_or_assign(PersistentKey(name), Vec3ToArr(v)); }
    void Field(const char* name, math::Vector4& v) override { Current().insert_or_assign(PersistentKey(name), Vec4ToArr(v)); }
    void Field(const char* name, std::string& v) override   { Current().insert_or_assign(PersistentKey(name), v); }
    void Field(const char* name, math::Quaternion& v) override { Current().insert_or_assign(PersistentKey(name), QuatToArr(v)); }

    // WHY 対応が必須か: Undo はこのスナップショットの差分で戻す。落とすとカーブだけが
    //     「編集はできるが元に戻せない」フィールドになる。
    void Field(const char* name, scene::ParticleCurve& v) override
    {
        Current().insert_or_assign(PersistentKey(name), asset::SerializeParticleCurve(v));
    }
    void Field(const char* name, scene::ParticleGradient& v) override
    {
        Current().insert_or_assign(PersistentKey(name), asset::SerializeParticleGradient(v));
    }

    // 参照は index / generation の組で持つ。
    // WHY: このスナップショットは「同一セッション内の Undo」専用で、シーンの
    //      作り直しを挟まない。EntityID はその間ずっと有効なので GUID 解決は要らない。
    void Field(const char* name, scene::EntityID& v) override
    {
        toml::array a;
        a.push_back((int64_t)v.index);
        a.push_back((int64_t)v.generation);
        Current().insert_or_assign(PersistentKey(name), std::move(a));
    }
    void Field(const char* name, input::KeyCode& v) override
    {
        Current().insert_or_assign(PersistentKey(name), (int64_t)v);
    }
    void AssetField(const char* name,
                    scene::ScriptAssetReference& v,
                    scene::ScriptAssetType) override
    {
        toml::table assetRef;
        assetRef.insert("guid", v.guid);
        assetRef.insert("path", v.path);
        Current().insert_or_assign(PersistentKey(name), std::move(assetRef));
    }
    void ListField(const char* name, std::vector<float>& values) override
    {
        toml::array array;
        for (float value : values) array.push_back(static_cast<double>(value));
        Current().insert_or_assign(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<int>& values) override
    {
        toml::array array;
        for (int value : values) array.push_back(static_cast<int64_t>(value));
        Current().insert_or_assign(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<bool>& values) override
    {
        toml::array array;
        for (bool value : values) array.push_back(value);
        Current().insert_or_assign(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<std::string>& values) override
    {
        toml::array array;
        for (const auto& value : values) array.push_back(value);
        Current().insert_or_assign(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<math::Vector2>& values) override
    {
        toml::array array;
        for (const auto& value : values) array.push_back(Vec2ToArr(value));
        Current().insert_or_assign(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<math::Vector3>& values) override
    {
        toml::array array;
        for (const auto& value : values) array.push_back(Vec3ToArr(value));
        Current().insert_or_assign(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<math::Vector4>& values) override
    {
        toml::array array;
        for (const auto& value : values) array.push_back(Vec4ToArr(value));
        Current().insert_or_assign(PersistentKey(name), std::move(array));
    }
    void ListField(const char* name, std::vector<scene::EntityRef>& values) override
    {
        toml::array array;
        for (const auto& value : values) {
            toml::array entity;
            entity.push_back(static_cast<int64_t>(value.id.index));
            entity.push_back(static_cast<int64_t>(value.id.generation));
            array.push_back(std::move(entity));
        }
        Current().insert_or_assign(PersistentKey(name), std::move(array));
    }
    void AssetListField(const char* name,
                        std::vector<scene::ScriptAssetReference>& values,
                        scene::ScriptAssetType) override
    {
        toml::array array;
        for (const auto& value : values) {
            toml::table assetRef;
            assetRef.insert("guid", value.guid);
            assetRef.insert("path", value.path);
            array.push_back(std::move(assetRef));
        }
        Current().insert_or_assign(PersistentKey(name), std::move(array));
    }
    // ObjectField は基底の既定実装 (BeginObject → Reflect → EndObject) に委ねる。

    // ── 入れ子オブジェクト / 構造体配列 ─────────────────────────────────────
    // WHY 対応が必須か: 未対応だと入れ子フィールドが親と同じ階層へフラット展開され、
    //     同名フィールド同士が衝突する。スクリプトのホットリロードでは
    //     このスナップショットから状態を復元するため、衝突すると値が失われる。
    void BeginObject(const char* name) override
    {
        auto [iterator, inserted] =
            Current().insert_or_assign(PersistentKey(name), toml::table{});
        toml::table* child = iterator->second.as_table();
        m_stack.push_back(child ? child : &Current());
    }

    void EndObject() override
    {
        if (m_stack.size() > 1) m_stack.pop_back();
    }

    std::size_t BeginObjectList(const char* name, std::size_t count) override
    {
        auto [iterator, inserted] =
            Current().insert_or_assign(PersistentKey(name), toml::array{});
        m_listStack.push_back(iterator->second.as_array());
        return count;
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
        return NO_REMOVE;
    }

    void ReferenceField(const char* name, scene::ScriptSerializedReference& value) override
    {
        toml::table reference;
        reference.insert("type", value.type);
        toml::table fields;
        if (value.value) {
            SnapshotWriter child(fields);
            value.value->Reflect(child);
        } else if (!value.preservedFieldsToml.empty()) {
            toml::parse_result parsed = toml::parse(value.preservedFieldsToml);
            if (parsed) fields = std::move(parsed.table());
        }
        reference.insert("fields", std::move(fields));
        Current().insert_or_assign(PersistentKey(name), std::move(reference));
    }

private:
    toml::table& Current() { return *m_stack.back(); }

    std::vector<toml::table*> m_stack;
    std::vector<toml::array*> m_listStack;
};

// ── 読み戻し ─────────────────────────────────────────────────────────────────
class SnapshotReader final : public scene::IReflector {
public:
    explicit SnapshotReader(const toml::table& table) { m_stack.push_back(&table); }

    void Field(const char* name, float& v) override
    {
        if (const toml::node* node = FindNode(name))
            v = static_cast<float>(node->value_or(static_cast<double>(v)));
    }
    void Field(const char* name, int& v) override
    {
        if (const toml::node* node = FindNode(name))
            v = static_cast<int>(node->value_or(static_cast<int64_t>(v)));
    }
    void Field(const char* name, bool& v) override
    {
        if (const toml::node* node = FindNode(name))
            v = node->value_or(v);
    }
    void Field(const char* name, math::Vector2& v) override
    {
        const auto* a = FindArray(name);
        v = { ArrAt(a, 0, v.x), ArrAt(a, 1, v.y) };
    }
    void Field(const char* name, math::Vector3& v) override
    {
        const auto* a = FindArray(name);
        v = { ArrAt(a, 0, v.x), ArrAt(a, 1, v.y), ArrAt(a, 2, v.z) };
    }
    void Field(const char* name, math::Vector4& v) override
    {
        const auto* a = FindArray(name);
        v = { ArrAt(a, 0, v.x), ArrAt(a, 1, v.y), ArrAt(a, 2, v.z), ArrAt(a, 3, v.w) };
    }
    void Field(const char* name, std::string& v) override
    {
        if (const toml::node* node = FindNode(name))
            v = node->value_or(v);
    }
    void Field(const char* name, math::Quaternion& v) override
    {
        const auto* a = FindArray(name);
        v = { ArrAt(a, 0, v.x), ArrAt(a, 1, v.y), ArrAt(a, 2, v.z), ArrAt(a, 3, v.w) };
    }
    void Field(const char* name, scene::ParticleCurve& v) override
    {
        if (const toml::table* table = Current())
            asset::DeserializeParticleCurve(*table, PersistentKey(name), v);
    }
    void Field(const char* name, scene::ParticleGradient& v) override
    {
        if (const toml::table* table = Current())
            asset::DeserializeParticleGradient(*table, PersistentKey(name), v);
    }
    void Field(const char* name, scene::EntityID& v) override
    {
        const auto* a = FindArray(name);
        if (!a || a->size() < 2) return;
        v.index      = static_cast<decltype(v.index)>((*a)[0].value_or<int64_t>(v.index));
        v.generation = static_cast<decltype(v.generation)>((*a)[1].value_or<int64_t>(v.generation));
    }
    void Field(const char* name, input::KeyCode& v) override
    {
        if (const toml::node* node = FindNode(name))
            v = static_cast<input::KeyCode>(node->value_or(static_cast<int64_t>(v)));
    }
    void AssetField(const char* name,
                    scene::ScriptAssetReference& v,
                    scene::ScriptAssetType) override
    {
        const toml::node* node = FindNode(name);
        if (!node) return;
        if (const toml::table* assetRef = node->as_table()) {
            v.guid = (*assetRef)["guid"].value_or(std::string{});
            v.path = (*assetRef)["path"].value_or(std::string{});
        }
    }
    void ListField(const char* name, std::vector<float>& values) override
    {
        if (const auto* array = FindArray(name)) {
            values.clear();
            for (const auto& node : *array)
                values.push_back(static_cast<float>(node.value_or(0.0)));
        }
    }
    void ListField(const char* name, std::vector<int>& values) override
    {
        if (const auto* array = FindArray(name)) {
            values.clear();
            for (const auto& node : *array)
                values.push_back(static_cast<int>(node.value_or(int64_t{0})));
        }
    }
    void ListField(const char* name, std::vector<bool>& values) override
    {
        if (const auto* array = FindArray(name)) {
            values.clear();
            for (const auto& node : *array)
                values.push_back(node.value_or(false));
        }
    }
    void ListField(const char* name, std::vector<std::string>& values) override
    {
        if (const auto* array = FindArray(name)) {
            values.clear();
            for (const auto& node : *array)
                values.push_back(node.value_or(std::string{}));
        }
    }
    void ListField(const char* name, std::vector<math::Vector2>& values) override
    {
        if (const auto* array = FindArray(name)) {
            values.clear();
            for (const auto& node : *array) {
                const auto* value = node.as_array();
                values.push_back({ ArrAt(value, 0, 0.0f), ArrAt(value, 1, 0.0f) });
            }
        }
    }
    void ListField(const char* name, std::vector<math::Vector3>& values) override
    {
        if (const auto* array = FindArray(name)) {
            values.clear();
            for (const auto& node : *array) {
                const auto* value = node.as_array();
                values.push_back({ ArrAt(value, 0, 0.0f), ArrAt(value, 1, 0.0f),
                                   ArrAt(value, 2, 0.0f) });
            }
        }
    }
    void ListField(const char* name, std::vector<math::Vector4>& values) override
    {
        if (const auto* array = FindArray(name)) {
            values.clear();
            for (const auto& node : *array) {
                const auto* value = node.as_array();
                values.push_back({ ArrAt(value, 0, 0.0f), ArrAt(value, 1, 0.0f),
                                   ArrAt(value, 2, 0.0f), ArrAt(value, 3, 0.0f) });
            }
        }
    }
    void ListField(const char* name, std::vector<scene::EntityRef>& values) override
    {
        const toml::array* array = FindArray(name);
        if (!array) return;
        values.clear();
        for (const auto& node : *array) {
            scene::EntityRef value;
            if (const toml::array* entity = node.as_array(); entity && entity->size() >= 2) {
                value.id.index = static_cast<uint32_t>(
                    entity->at(0).value_or(static_cast<int64_t>(
                        scene::EntityID::INVALID_INDEX)));
                value.id.generation = static_cast<uint32_t>(
                    entity->at(1).value_or(int64_t{0}));
            }
            values.push_back(value);
        }
    }
    void AssetListField(const char* name,
                        std::vector<scene::ScriptAssetReference>& values,
                        scene::ScriptAssetType) override
    {
        const toml::array* array = FindArray(name);
        if (!array) return;
        values.clear();
        for (const auto& node : *array) {
            scene::ScriptAssetReference value;
            if (const toml::table* assetRef = node.as_table()) {
                value.guid = (*assetRef)["guid"].value_or(std::string{});
                value.path = (*assetRef)["path"].value_or(std::string{});
            }
            values.push_back(std::move(value));
        }
    }
    // ObjectField は基底の既定実装 (BeginObject → Reflect → EndObject) に委ねる。

    // ── 入れ子オブジェクト / 構造体配列 ─────────────────────────────────────
    void BeginObject(const char* name) override
    {
        const toml::node* node = FindNode(name);
        // 見つからなければ nullptr を積む (欠損スコープ)。
        // スコープ対は必ず EndObject と釣り合わせる必要があるため、早期 return しない。
        m_stack.push_back(node ? node->as_table() : nullptr);
    }

    void EndObject() override
    {
        if (m_stack.size() > 1) m_stack.pop_back();
    }

    std::size_t BeginObjectList(const char* name, std::size_t count) override
    {
        (void)count;
        const toml::array* array = FindArray(name);
        m_listStack.push_back(array);
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

    void ReferenceField(const char* name, scene::ScriptSerializedReference& value) override
    {
        const toml::node* node = FindNode(name);
        const toml::table* reference = node ? node->as_table() : nullptr;
        if (!reference) return;
        const std::string type = (*reference)["type"].value_or(std::string{});
        const toml::table* fields = (*reference)["fields"].as_table();
        if (!value.SetType(type)) {
            value.type = type;
            value.value.reset();
            if (fields) {
                std::ostringstream stream;
                stream << *fields;
                value.preservedFieldsToml = stream.str();
            }
            return;
        }
        if (fields) {
            SnapshotReader child(*fields);
            value.value->Reflect(child);
        }
    }

private:
    [[nodiscard]] const toml::table* Current() const { return m_stack.back(); }

    [[nodiscard]] const toml::node* FindNode(const char* fallback) const
    {
        const toml::table* table = Current();
        if (!table) return nullptr;   // 欠損スコープの内側

        if (const toml::node* node = table->get(PersistentKey(fallback)))
            return node;
        return nullptr;
    }

    [[nodiscard]] const toml::array* FindArray(const char* fallback) const
    {
        const toml::node* node = FindNode(fallback);
        return node ? node->as_array() : nullptr;
    }

    std::vector<const toml::table*> m_stack;
    std::vector<const toml::array*> m_listStack;
};

} // namespace

std::string CaptureScriptSnapshot(scene::Script& script)
{
    toml::table table;
    SnapshotWriter writer(table);
    script.Reflect(writer);

    std::ostringstream ss;
    ss << table;
    return ss.str();
}

bool ApplyScriptSnapshot(scene::Script& script, const std::string& snapshot)
{
    // 空スナップショットは「フィールドを持たないスクリプト」なので成功扱い。
    if (snapshot.empty()) return true;

    toml::parse_result parsed = toml::parse(snapshot);
    if (!parsed) return false;

    SnapshotReader reader(parsed.table());
    script.Reflect(reader);
    return true;
}

} // namespace fbzz::editor
