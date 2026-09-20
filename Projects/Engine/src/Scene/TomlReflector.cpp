/// @file    TomlReflector.cpp
/// @brief   TOML リフレクタの値型変換とスコープ管理。
/// @author  Hasegawa Jin
/// @date    2026-08-23
#include <Engine/Scene/TomlReflector.hpp>
#include <Engine/Core/Logger.hpp>
#include <sstream>
#include <Engine/Asset/ParticleEmitterAssetCodec.hpp>

/// @note カーブ / グラデーションの TOML 表現は .vfx と .scene で既に 1 つに統一されているため、
/// @note ここで別形式を作ると同じ型なのに出どころによって読めるファイルと読めないファイルが生まれる。
namespace fbzz::util {

namespace {
static const char* KeyCodeToString(input::KeyCode k)
{
    using KC = input::KeyCode;
    switch (k) {
    case KC::A: return "A"; case KC::B: return "B"; case KC::C: return "C";
    case KC::D: return "D"; case KC::E: return "E"; case KC::F: return "F";
    case KC::G: return "G"; case KC::H: return "H"; case KC::I: return "I";
    case KC::J: return "J"; case KC::K: return "K"; case KC::L: return "L";
    case KC::M: return "M"; case KC::N: return "N"; case KC::O: return "O";
    case KC::P: return "P"; case KC::Q: return "Q"; case KC::R: return "R";
    case KC::S: return "S"; case KC::T: return "T"; case KC::U: return "U";
    case KC::V: return "V"; case KC::W: return "W"; case KC::X: return "X";
    case KC::Y: return "Y"; case KC::Z: return "Z";
    case KC::KEY_0: return "0"; case KC::KEY_1: return "1"; case KC::KEY_2: return "2";
    case KC::KEY_3: return "3"; case KC::KEY_4: return "4"; case KC::KEY_5: return "5";
    case KC::KEY_6: return "6"; case KC::KEY_7: return "7"; case KC::KEY_8: return "8";
    case KC::KEY_9: return "9";
    case KC::ESCAPE:    return "Escape";
    case KC::TAB: return "Tab";
    case KC::SPACE:     return "Space";
    case KC::ENTER:     return "Enter";
    case KC::BACKSPACE: return "Backspace";
    case KC::SHIFT:     return "Shift";
    case KC::CTRL:      return "Ctrl";
    case KC::ALT:       return "Alt";
    case KC::LEFT:      return "Left";
    case KC::RIGHT:     return "Right";
    case KC::UP:        return "Up";
    case KC::DOWN:      return "Down";
    case KC::F1:  return "F1";  case KC::F2:  return "F2";  case KC::F3:  return "F3";
    case KC::F4:  return "F4";  case KC::F5:  return "F5";  case KC::F6:  return "F6";
    case KC::F7:  return "F7";  case KC::F8:  return "F8";  case KC::F9:  return "F9";
    case KC::F10: return "F10"; case KC::F11: return "F11"; case KC::F12: return "F12";
    case KC::MouseLeft:   return "MouseLeft";
    case KC::MouseRight:  return "MouseRight";
    case KC::MouseMiddle: return "MouseMiddle";
    default: return "Unknown";
    }
}

static input::KeyCode KeyCodeFromString(const std::string& s)
{
    using KC = input::KeyCode;
    if (s == "A") return KC::A; if (s == "B") return KC::B; if (s == "C") return KC::C;
    if (s == "D") return KC::D; if (s == "E") return KC::E; if (s == "F") return KC::F;
    if (s == "G") return KC::G; if (s == "H") return KC::H; if (s == "I") return KC::I;
    if (s == "J") return KC::J; if (s == "K") return KC::K; if (s == "L") return KC::L;
    if (s == "M") return KC::M; if (s == "N") return KC::N; if (s == "O") return KC::O;
    if (s == "P") return KC::P; if (s == "Q") return KC::Q; if (s == "R") return KC::R;
    if (s == "S") return KC::S; if (s == "T") return KC::T; if (s == "U") return KC::U;
    if (s == "V") return KC::V; if (s == "W") return KC::W; if (s == "X") return KC::X;
    if (s == "Y") return KC::Y; if (s == "Z") return KC::Z;
    if (s == "0") return KC::KEY_0; if (s == "1") return KC::KEY_1;
    if (s == "2") return KC::KEY_2; if (s == "3") return KC::KEY_3;
    if (s == "4") return KC::KEY_4; if (s == "5") return KC::KEY_5;
    if (s == "6") return KC::KEY_6; if (s == "7") return KC::KEY_7;
    if (s == "8") return KC::KEY_8; if (s == "9") return KC::KEY_9;
    if (s == "Escape")     return KC::ESCAPE;
    if (s == "Tab") return KC::TAB;
    if (s == "Space")      return KC::SPACE;
    if (s == "Enter")      return KC::ENTER;
    if (s == "Backspace")  return KC::BACKSPACE;
    if (s == "Shift")      return KC::SHIFT;
    if (s == "Ctrl")       return KC::CTRL;
    if (s == "Alt")        return KC::ALT;
    if (s == "Left")       return KC::LEFT;
    if (s == "Right")      return KC::RIGHT;
    if (s == "Up")         return KC::UP;
    if (s == "Down")       return KC::DOWN;
    if (s == "F1")  return KC::F1;  if (s == "F2")  return KC::F2;
    if (s == "F3")  return KC::F3;  if (s == "F4")  return KC::F4;
    if (s == "F5")  return KC::F5;  if (s == "F6")  return KC::F6;
    if (s == "F7")  return KC::F7;  if (s == "F8")  return KC::F8;
    if (s == "F9")  return KC::F9;  if (s == "F10") return KC::F10;
    if (s == "F11") return KC::F11; if (s == "F12") return KC::F12;
    if (s == "MouseLeft")   return KC::MouseLeft;
    if (s == "MouseRight")  return KC::MouseRight;
    if (s == "MouseMiddle") return KC::MouseMiddle;
    return KC::SPACE;
}



/// @note 数値ノードを double として読む。TOML は 1.0 を整数 1 として書き戻すことがあり、
/// @note double だけを見ると SetFloat(1.0f) → GetFloat() が既定値へ落ちる。
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

/// @note 配列から先頭 N 要素を float として取り出す。要素数が足りなければ既定値を返す。
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


void TomlWriteReflector::Field(const char* name, input::KeyCode& value)
{
    Put(name, std::string(KeyCodeToString(value)));
}

void TomlReadReflector::Field(const char* name, input::KeyCode& value)
{
    if (const auto* node = FindNode(name)) {
        if (const auto text = node->value<std::string>()) value = KeyCodeFromString(*text);
        else if (const auto number = node->value<int64_t>(); number && *number >= 0 && *number <= 255)
            value = static_cast<input::KeyCode>(*number);
    }
}

void TomlWriteReflector::AssetField(const char* name, scene::ScriptAssetReference& value, scene::ScriptAssetType)
{
    if (value.guid.empty() && !value.path.empty()) value.SetPath(value.path);
    Put(name, toml::table{{"guid", value.guid}, {"path", value.path}});
}

void TomlReadReflector::AssetField(const char* name, scene::ScriptAssetReference& value, scene::ScriptAssetType)
{
    const auto* node = FindNode(name);
    if (!node) return;
    if (const auto* table = node->as_table()) {
        value.guid = (*table)["guid"].value_or(std::string{});
        value.path = (*table)["path"].value_or(std::string{});
    } else if (const auto path = node->value<std::string>()) {
        /// @note 以前の DataAsset は path 文字列だけを保存していた。
        value.Clear();
        value.SetPath(*path);
    }
}

void TomlWriteReflector::AssetListField(const char* name, std::vector<scene::ScriptAssetReference>& values, scene::ScriptAssetType)
{
    toml::array array;
    for (auto& value : values) {
        if (value.guid.empty() && !value.path.empty()) value.SetPath(value.path);
        array.push_back(toml::table{{"guid", value.guid}, {"path", value.path}});
    }
    Put(name, std::move(array));
}

void TomlReadReflector::AssetListField(const char* name, std::vector<scene::ScriptAssetReference>& values, scene::ScriptAssetType)
{
    const auto* array = FindArray(name);
    if (!array) return;
    std::vector<scene::ScriptAssetReference> result;
    for (const auto& node : *array) {
        scene::ScriptAssetReference value;
        if (const auto* table = node.as_table()) {
            value.guid = (*table)["guid"].value_or(std::string{});
            value.path = (*table)["path"].value_or(std::string{});
        } else if (const auto path = node.value<std::string>()) value.SetPath(*path);
        else { FBZZ_LOG_WARN("Invalid asset reference list: %s", name); return; }
        result.push_back(std::move(value));
    }
    values = std::move(result);
}

void TomlWriteReflector::ReferenceField(const char* name, scene::ScriptSerializedReference& value)
{
    toml::table fields;
    if (value.value) {
        TomlWriteReflector child(fields);
        value.value->Reflect(child);
        std::ostringstream text;
        text << fields;
        value.preservedFieldsToml = text.str();
    } else if (!value.preservedFieldsToml.empty()) {
        auto parsed = toml::parse(value.preservedFieldsToml);
        if (parsed) fields = std::move(parsed.table());
    }
    Put(name, toml::table{{"type", value.type}, {"fields", std::move(fields)}});
}

void TomlReadReflector::ReferenceField(const char* name, scene::ScriptSerializedReference& value)
{
    const auto* node = FindNode(name);
    const auto* reference = node ? node->as_table() : nullptr;
    if (!reference) return;
    value.type = (*reference)["type"].value_or(std::string{});
    const auto* fields = (*reference)["fields"].as_table();
    std::ostringstream text;
    if (fields) text << *fields;
    value.preservedFieldsToml = text.str();
    value.value = scene::ScriptSerializableFactory::Create(value.type);
    if (value.value && fields) {
        TomlReadReflector child(*fields);
        value.value->Reflect(child);
    }
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
    /// @note const auto& で受けた要素をそのまま push_back できない。
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
    /// @note 挿入する方式だと、構築中に子のアドレスを保持できずスタックに積めない。
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

/// @note codec 側は「キーが無ければ何もしない」ため、欠損スコープでも既定値が残る。
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
    /// @note 呼び出し側の既定値がそのまま残る (欠損スコープ)。スコープ対は必ず EndObject と
    /// @note 釣り合う必要があるため、積まずに抜けると EndObject でスタックが破綻する。
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
    /// @note 配列が無い場合は 0 を返し、既存要素を消す (ファイルの内容を正とする)。
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
