/// @file    TomlReflector.cpp
/// @brief   TOML リフレクタの値型変換とスコープ管理。
/// @author  Hasegawa Jin
/// @date    2026-08-23
#include <Engine/Scene/TomlReflector.hpp>
#include <Engine/Asset/ParticleEmitterAssetCodec.hpp>

/// @note カーブ / グラデーションの TOML 表現は .vfx と .scene で既に 1 つに統一されているため、
///       ここで別形式を作ると同じ型なのに出どころによって読めるファイルと読めないファイルが生まれる。
namespace fbzz::util {

namespace {

/// 数値ノードを double として読む。TOML は 1.0 を整数 1 として書き戻すことがあり、
/// double だけを見ると SetFloat(1.0f) → GetFloat() が既定値へ落ちる。
[[nodiscard]] bool ReadNumber(const toml::node& node, double& out)
{
    if (const auto asDouble = node.value<double>()) { out = *asDouble; return true; }
    if (const auto asInt = node.value<int64_t>())   { out = static_cast<double>(*asInt); return true; }
    return false;
}

[[nodiscard]] float ElementAsFloat(const toml::node& node, float fallback)
{
    double value = 0.0;
    return ReadNumber(node, value) ? static_cast<float>(value) : fallback;
}

/// 配列から先頭 N 要素を float として取り出す。要素数が足りなければ既定値を返す。
template<std::size_t N>
[[nodiscard]] bool ReadFloats(const toml::array* array, float (&out)[N])
{
    if (!array || array->size() < N) return false;
    for (std::size_t i = 0; i < N; ++i)
        out[i] = ElementAsFloat(array->at(i), out[i]);
    return true;
}

template<typename Element, typename Convert>
[[nodiscard]] toml::array MakeArray(const std::vector<Element>& values, Convert&& convert)
{
    toml::array array;
    for (const auto& value : values) array.push_back(convert(value));
    return array;
}

} // namespace

/// @name 値型 ⇔ TOML 配列

toml::array Vec2ToArr(const math::Vector2& v)
{
    return toml::array{ static_cast<double>(v.x), static_cast<double>(v.y) };
}

toml::array Vec3ToArr(const math::Vector3& v)
{
    return toml::array{ static_cast<double>(v.x), static_cast<double>(v.y),
                        static_cast<double>(v.z) };
}

toml::array Vec4ToArr(const math::Vector4& v)
{
    return toml::array{ static_cast<double>(v.x), static_cast<double>(v.y),
                        static_cast<double>(v.z), static_cast<double>(v.w) };
}

toml::array QuatToArr(const math::Quaternion& q)
{
    return toml::array{ static_cast<double>(q.x), static_cast<double>(q.y),
                        static_cast<double>(q.z), static_cast<double>(q.w) };
}

math::Vector2 ArrToVec2(const toml::array* a, math::Vector2 def)
{
    float values[2] = { def.x, def.y };
    if (!ReadFloats(a, values)) return def;
    return { values[0], values[1] };
}

math::Vector3 ArrToVec3(const toml::array* a, math::Vector3 def)
{
    float values[3] = { def.x, def.y, def.z };
    if (!ReadFloats(a, values)) return def;
    return { values[0], values[1], values[2] };
}

math::Vector4 ArrToVec4(const toml::array* a, math::Vector4 def)
{
    float values[4] = { def.x, def.y, def.z, def.w };
    if (!ReadFloats(a, values)) return def;
    return { values[0], values[1], values[2], values[3] };
}

math::Quaternion ArrToQuat(const toml::array* a, math::Quaternion def)
{
    float values[4] = { def.x, def.y, def.z, def.w };
    if (!ReadFloats(a, values)) return def;
    return { values[0], values[1], values[2], values[3] };
}

/// @name TomlWriteReflector

void TomlWriteReflector::Field(const char* name, scene::ParticleCurve& v)
{
    Put(name, asset::SerializeParticleCurve(v));
}

void TomlWriteReflector::Field(const char* name, scene::ParticleGradient& v)
{
    Put(name, asset::SerializeParticleGradient(v));
}

void TomlWriteReflector::ListField(const char* name, std::vector<float>& values)
{
    Put(name, MakeArray(values, [](float v) { return static_cast<double>(v); }));
}

void TomlWriteReflector::ListField(const char* name, std::vector<int>& values)
{
    Put(name, MakeArray(values, [](int v) { return static_cast<int64_t>(v); }));
}

void TomlWriteReflector::ListField(const char* name, std::vector<bool>& values)
{
    /// @note `vector<bool>` だけ MakeArray を通さない。プロキシ参照を返す特殊化のため、
    ///       const auto& で受けた要素をそのまま push_back できない。
    toml::array array;
    for (const bool value : values) array.push_back(value);
    Put(name, std::move(array));
}

void TomlWriteReflector::ListField(const char* name, std::vector<std::string>& values)
{
    Put(name, MakeArray(values, [](const std::string& v) { return v; }));
}

void TomlWriteReflector::ListField(const char* name, std::vector<math::Vector2>& values)
{
    Put(name, MakeArray(values, [](const math::Vector2& v) { return Vec2ToArr(v); }));
}

void TomlWriteReflector::ListField(const char* name, std::vector<math::Vector3>& values)
{
    Put(name, MakeArray(values, [](const math::Vector3& v) { return Vec3ToArr(v); }));
}

void TomlWriteReflector::ListField(const char* name, std::vector<math::Vector4>& values)
{
    Put(name, MakeArray(values, [](const math::Vector4& v) { return Vec4ToArr(v); }));
}

void TomlWriteReflector::BeginObject(const char* name)
{
    /// @note 親へ空テーブルを先に挿入し、その実体を書き込み先として積む。構築し終えてから move で
    ///       挿入する方式だと、構築中に子のアドレスを保持できずスタックに積めない。
    auto [iterator, inserted] =
        Current().insert_or_assign(PersistentKey(name), toml::table{});
    toml::table* child = iterator->second.as_table();
    /// @note 挿入直後なので as_table() は必ず成功する。防御的に親を積み直して破綻を避ける。
    m_stack.push_back(child ? child : &Current());
}

void TomlWriteReflector::EndObject()
{
    /// @note ルート (最初の 1 枚) は決して pop しない。
    if (m_stack.size() > 1) m_stack.pop_back();
}

std::size_t TomlWriteReflector::BeginObjectList(const char* name, std::size_t count)
{
    auto [iterator, inserted] =
        Current().insert_or_assign(PersistentKey(name), toml::array{});
    m_listStack.push_back(iterator->second.as_array());
    /// @note 書き込みは要素数を変えない
    return count;
}

void TomlWriteReflector::BeginObjectElement(std::size_t index)
{
    (void)index;
    toml::array* array = m_listStack.empty() ? nullptr : m_listStack.back();
    if (!array) { m_stack.push_back(&Current()); return; }

    array->push_back(toml::table{});
    toml::table* element = array->back().as_table();
    m_stack.push_back(element ? element : &Current());
}

std::size_t TomlWriteReflector::EndObjectList()
{
    if (!m_listStack.empty()) m_listStack.pop_back();
    /// @note 永続化は要素を削除しない
    return NO_REMOVE;
}

/// @name TomlReadReflector

const toml::node* TomlReadReflector::FindNode(const char* fallback) const
{
    const toml::table* table = Current();
    /// @note 欠損スコープの内側
    if (!table) return nullptr;
    return table->get(PersistentKey(fallback));
}

const toml::array* TomlReadReflector::FindArray(const char* fallback) const
{
    const toml::node* node = FindNode(fallback);
    return node ? node->as_array() : nullptr;
}

void TomlReadReflector::Field(const char* name, float& v)
{
    const toml::node* node = FindNode(name);
    double value = 0.0;
    if (node && ReadNumber(*node, value)) v = static_cast<float>(value);
}

void TomlReadReflector::Field(const char* name, int& v)
{
    if (const toml::node* node = FindNode(name))
        v = static_cast<int>(node->value_or(static_cast<int64_t>(v)));
}

void TomlReadReflector::Field(const char* name, bool& v)
{
    if (const toml::node* node = FindNode(name)) v = node->value_or(v);
}

/// codec 側は「キーが無ければ何もしない」ため、欠損スコープでも既定値が残る。
void TomlReadReflector::Field(const char* name, scene::ParticleCurve& v)
{
    if (const toml::table* table = Current())
        asset::DeserializeParticleCurve(*table, PersistentKey(name), v);
}

void TomlReadReflector::Field(const char* name, scene::ParticleGradient& v)
{
    if (const toml::table* table = Current())
        asset::DeserializeParticleGradient(*table, PersistentKey(name), v);
}

void TomlReadReflector::Field(const char* name, std::string& v)
{
    if (const toml::node* node = FindNode(name)) v = node->value_or(v);
}

void TomlReadReflector::ListField(const char* name, std::vector<float>& values)
{
    const toml::array* array = FindArray(name);
    if (!array) return;
    values.clear();
    values.reserve(array->size());
    for (const auto& node : *array) values.push_back(ElementAsFloat(node, 0.0f));
}

void TomlReadReflector::ListField(const char* name, std::vector<int>& values)
{
    const toml::array* array = FindArray(name);
    if (!array) return;
    values.clear();
    values.reserve(array->size());
    for (const auto& node : *array)
        values.push_back(static_cast<int>(node.value_or(int64_t{ 0 })));
}

void TomlReadReflector::ListField(const char* name, std::vector<bool>& values)
{
    const toml::array* array = FindArray(name);
    if (!array) return;
    values.clear();
    values.reserve(array->size());
    for (const auto& node : *array) values.push_back(node.value_or(false));
}

void TomlReadReflector::ListField(const char* name, std::vector<std::string>& values)
{
    const toml::array* array = FindArray(name);
    if (!array) return;
    values.clear();
    values.reserve(array->size());
    for (const auto& node : *array) values.push_back(node.value_or(std::string{}));
}

void TomlReadReflector::ListField(const char* name, std::vector<math::Vector2>& values)
{
    const toml::array* array = FindArray(name);
    if (!array) return;
    values.clear();
    values.reserve(array->size());
    for (const auto& node : *array) values.push_back(ArrToVec2(node.as_array()));
}

void TomlReadReflector::ListField(const char* name, std::vector<math::Vector3>& values)
{
    const toml::array* array = FindArray(name);
    if (!array) return;
    values.clear();
    values.reserve(array->size());
    for (const auto& node : *array) values.push_back(ArrToVec3(node.as_array()));
}

void TomlReadReflector::ListField(const char* name, std::vector<math::Vector4>& values)
{
    const toml::array* array = FindArray(name);
    if (!array) return;
    values.clear();
    values.reserve(array->size());
    for (const auto& node : *array) values.push_back(ArrToVec4(node.as_array()));
}

void TomlReadReflector::BeginObject(const char* name)
{
    const toml::node* node = FindNode(name);
    /// @note 見つからなければ nullptr を積む。以降の Field は読み込み元が無いため何もせず、
    ///       呼び出し側の既定値がそのまま残る (欠損スコープ)。スコープ対は必ず EndObject と
    ///       釣り合う必要があるため、積まずに抜けると EndObject でスタックが破綻する。
    m_stack.push_back(node ? node->as_table() : nullptr);
}

void TomlReadReflector::EndObject()
{
    if (m_stack.size() > 1) m_stack.pop_back();
}

std::size_t TomlReadReflector::BeginObjectList(const char* name, std::size_t count)
{
    (void)count;
    const toml::array* array = FindArray(name);
    m_listStack.push_back(array);
    /// @note 保存されていた要素数を返す。呼び出し側はこの値で vector を resize する。
    ///       配列が無い場合は 0 を返し、既存要素を消す (ファイルの内容を正とする)。
    return array ? array->size() : 0u;
}

void TomlReadReflector::BeginObjectElement(std::size_t index)
{
    const toml::array* array = m_listStack.empty() ? nullptr : m_listStack.back();
    if (!array || index >= array->size()) { m_stack.push_back(nullptr); return; }
    m_stack.push_back(array->at(index).as_table());
}

std::size_t TomlReadReflector::EndObjectList()
{
    if (!m_listStack.empty()) m_listStack.pop_back();
    return NO_REMOVE;
}

} // namespace fbzz::util
