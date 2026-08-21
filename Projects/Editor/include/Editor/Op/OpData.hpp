// FBZZ Engine
// OpData.hpp | fbzz::editor
// WHAT: Operator が「返す」構造化データ。数値・文字列・配列・オブジェクトの入れ子を表す。
// WHY:  OpKind::Query は最初から型にあったのに、**実際に登録された Query 操作は 1 つも無かった**。
//       OpResult が持てるのは ok / errorCode / message だけで、読み取り結果を返す場所が
//       どこにも無かったためで、「読む機能」は Operator にできず 10,000 行の
//       EditorBusDispatcher 側へ書き続けるしかなかった。つまり登録簿は
//       「書く操作の目録」に留まり、AI から見た Editor は
//       「op.list に出る操作」と「dispatcher にしか無い照会」の 2 系統に割れていた。
//       返り値の器を 1 つ足すだけで Query が成立し、以後は読み書きの両方が
//       同じ登録簿へ載る (Docs/design/editor-operator-model.md §7)。
//
// WHY JsonValue を使わないか: Op 層が AI 層 (Editor/Ai/Json.hpp) を知ると、
//       AI を外した構成でエディターが組めなくなる。OpArgs が JSON に依存していないのと
//       同じ理由で、返り値側も Op 層だけで完結する型を持つ。JSON への変換は
//       境界にいる OperatorBridge が 1 箇所で行う。
//
// 表現方法は JsonValue と同じ「タグ + 全フィールド」方式にそろえてある。
// recursive-variant を避けると自己参照 (Array/Object が OpData を含む) を素直に書ける。
#pragma once

#include <Math/Vector3.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::editor {

class OpData {
public:
    enum class Type { Null, Bool, Int, Float, String, Vec3, Array, Object };

    // 挿入順を保つため Object は map ではなく pair の vector で持つ。
    // WHY: 応答の並びが呼び出しごとに変わると、AI が前回の応答と差分を取れない。
    using Array  = std::vector<OpData>;
    using Member = std::pair<std::string, OpData>;
    using Object = std::vector<Member>;

    OpData() = default;
    OpData(bool value)          : m_type(Type::Bool),   m_bool(value) {}
    OpData(int value)           : m_type(Type::Int),    m_int(value) {}
    OpData(float value)         : m_type(Type::Float),  m_float(value) {}
    OpData(double value)        : m_type(Type::Float),  m_float(static_cast<float>(value)) {}
    OpData(const char* value)   : m_type(Type::String), m_string(value != nullptr ? value : "") {}
    OpData(std::string value)   : m_type(Type::String), m_string(std::move(value)) {}
    OpData(math::Vector3 value) : m_type(Type::Vec3),   m_vec3(value) {}

    static OpData MakeArray()  { OpData d; d.m_type = Type::Array;  return d; }
    static OpData MakeObject() { OpData d; d.m_type = Type::Object; return d; }

    [[nodiscard]] Type GetType() const { return m_type; }
    [[nodiscard]] bool IsNull()   const { return m_type == Type::Null; }
    [[nodiscard]] bool IsBool()   const { return m_type == Type::Bool; }
    [[nodiscard]] bool IsInt()    const { return m_type == Type::Int; }
    [[nodiscard]] bool IsFloat()  const { return m_type == Type::Float; }
    [[nodiscard]] bool IsString() const { return m_type == Type::String; }
    [[nodiscard]] bool IsVec3()   const { return m_type == Type::Vec3; }
    [[nodiscard]] bool IsArray()  const { return m_type == Type::Array; }
    [[nodiscard]] bool IsObject() const { return m_type == Type::Object; }

    // 型が一致しない場合は既定値を返す (throw しない — プロジェクト規約)。
    [[nodiscard]] bool               AsBool  (bool fallback = false) const { return m_type == Type::Bool  ? m_bool  : fallback; }
    [[nodiscard]] int                AsInt   (int fallback = 0) const      { return m_type == Type::Int   ? m_int   : fallback; }
    [[nodiscard]] float              AsFloat (float fallback = 0.0f) const { return m_type == Type::Float ? m_float : fallback; }
    [[nodiscard]] const std::string& AsString() const { return m_string; }
    [[nodiscard]] math::Vector3      AsVec3() const { return m_vec3; }

    [[nodiscard]] const Array&  AsArray()  const { return m_array; }
    [[nodiscard]] const Object& AsObject() const { return m_object; }

    // 配列構築ヘルパ。
    void Push(OpData value)
    {
        m_type = Type::Array;
        m_array.push_back(std::move(value));
    }

    // オブジェクト構築ヘルパ (同名キーは追記せず更新する)。
    void Set(std::string key, OpData value)
    {
        m_type = Type::Object;
        for (Member& member : m_object) {
            if (member.first == key) { member.second = std::move(value); return; }
        }
        m_object.emplace_back(std::move(key), std::move(value));
    }

    [[nodiscard]] const OpData* Find(std::string_view key) const
    {
        if (m_type != Type::Object) return nullptr;
        for (const Member& member : m_object)
            if (member.first == key) return &member.second;
        return nullptr;
    }

private:
    Type          m_type = Type::Null;
    bool          m_bool = false;
    int           m_int = 0;
    float         m_float = 0.0f;
    std::string   m_string;
    math::Vector3 m_vec3{};
    Array         m_array;
    Object        m_object;
};

} // namespace fbzz::editor
