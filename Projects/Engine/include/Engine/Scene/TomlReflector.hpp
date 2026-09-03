/// @file    TomlReflector.hpp
/// @brief   IReflector を TOML テーブルへ読み書きする共通リフレクタ。
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// 設計意図 (WHY):
///   同じ「IReflector → toml::table」の変換が SceneSerializer / DataAssetRegistry /
///   SaveStore の 3 か所で必要になる。別々に書くと ListField に型を 1 つ足すたびに
///   3 か所を直すことになり、片方だけ対応が漏れた型は *エラーも警告もなく*
///   保存されなくなる。値型の変換とスコープ管理はここへ一本化し、
///   参照型 (EntityID / アセット GUID) のように文脈が要るものだけを派生で足す。
///
///   実体は scene::IReflector の実装なので Util ではなく Scene に置く。
///   名前空間は呼び出し側を巻き込まないよう fbzz::util のまま据え置いている。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <toml++/toml.hpp>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::util {

/// @name 値型 ⇔ TOML 配列
/// 欠損・要素数不足は既定値をそのまま返す。読み込み側が「無ければ既定値」を
/// 毎回書かずに済むようにするため。
///@{
[[nodiscard]] toml::array Vec2ToArr(const math::Vector2& v);
[[nodiscard]] toml::array Vec3ToArr(const math::Vector3& v);
[[nodiscard]] toml::array Vec4ToArr(const math::Vector4& v);
[[nodiscard]] toml::array QuatToArr(const math::Quaternion& q);

[[nodiscard]] math::Vector2 ArrToVec2(const toml::array* a, math::Vector2 def = {});
[[nodiscard]] math::Vector3 ArrToVec3(const toml::array* a, math::Vector3 def = {});
[[nodiscard]] math::Vector4 ArrToVec4(const toml::array* a, math::Vector4 def = {});
[[nodiscard]] math::Quaternion ArrToQuat(const toml::array* a,
                                         math::Quaternion def = { 0.0f, 0.0f, 0.0f, 1.0f });
///@}

/// 値型フィールドと入れ子スコープを toml::table へ書き出す。
///
/// 書き込み先はスタックで持つ。BeginObject / BeginObjectElement が「現在の書き込み先」を
/// 子テーブルへ差し替えるため、任意の深さの復帰先を LIFO で覚える必要がある。
class TomlWriteReflector : public scene::IReflector {
public:
    /// @param overwriteDuplicates 同じキーへ 2 度書いたときに後勝ちで上書きするか。
    ///   false なら先勝ち (toml++ の insert 既定) で、後から来た値は黙って捨てられる。
    ///   WHY 選べるようにするか: Scene の直列化は先勝ちを前提に組まれている。
    ///   どちらも BeginField の戻し忘れを救えないが、既存の保存結果を変えないため
    ///   呼び出し側に選ばせる。新規の書き出し先は後勝ちでよい。
    explicit TomlWriteReflector(toml::table& table, bool overwriteDuplicates = true)
        : m_overwrite(overwriteDuplicates)
    {
        m_stack.push_back(&table);
    }

    void Field(const char* name, float& v) override { Put(name, static_cast<double>(v)); }
    void Field(const char* name, int& v) override { Put(name, static_cast<int64_t>(v)); }
    void Field(const char* name, bool& v) override { Put(name, v); }
    void Field(const char* name, std::string& v) override { Put(name, v); }
    void Field(const char* name, math::Vector2& v) override { Put(name, Vec2ToArr(v)); }
    void Field(const char* name, math::Vector3& v) override { Put(name, Vec3ToArr(v)); }
    void Field(const char* name, math::Vector4& v) override { Put(name, Vec4ToArr(v)); }
    void Field(const char* name, math::Quaternion& v) override { Put(name, QuatToArr(v)); }
    void Field(const char* name, scene::ParticleCurve& v) override;
    void Field(const char* name, scene::ParticleGradient& v) override;

    void ListField(const char* name, std::vector<float>& values) override;
    void ListField(const char* name, std::vector<int>& values) override;
    void ListField(const char* name, std::vector<bool>& values) override;
    void ListField(const char* name, std::vector<std::string>& values) override;
    void ListField(const char* name, std::vector<math::Vector2>& values) override;
    void ListField(const char* name, std::vector<math::Vector3>& values) override;
    void ListField(const char* name, std::vector<math::Vector4>& values) override;

    void BeginObject(const char* name) override;
    void EndObject() override;

    [[nodiscard]] std::size_t BeginObjectList(const char* name, std::size_t count) override;
    void BeginObjectElement(std::size_t index) override;
    void EndObjectElement() override { EndObject(); }
    [[nodiscard]] std::size_t EndObjectList() override;

protected:
    [[nodiscard]] toml::table& Current() { return *m_stack.back(); }

    /// 挿入ポリシー (先勝ち / 後勝ち) を一箇所に閉じる。
    template<typename T>
    void Put(const char* name, T&& value)
    {
        if (m_overwrite)
            Current().insert_or_assign(PersistentKey(name), std::forward<T>(value));
        else
            Current().insert(PersistentKey(name), std::forward<T>(value));
    }

private:
    bool m_overwrite = true;
    std::vector<toml::table*> m_stack;
    std::vector<toml::array*> m_listStack;
};

/// toml::table から値型フィールドを読み戻す。書き込み側と対称。
///
/// 対応するテーブルが無い入れ子は「欠損スコープ」として nullptr を積み、
/// 中のフィールドは呼び出し側の既定値のまま残す (古いファイルでも壊れない)。
class TomlReadReflector : public scene::IReflector {
public:
    explicit TomlReadReflector(const toml::table& table) { m_stack.push_back(&table); }

    void Field(const char* name, float& v) override;
    void Field(const char* name, int& v) override;
    void Field(const char* name, bool& v) override;
    void Field(const char* name, std::string& v) override;
    void Field(const char* name, math::Vector2& v) override { v = ArrToVec2(FindArray(name), v); }
    void Field(const char* name, math::Vector3& v) override { v = ArrToVec3(FindArray(name), v); }
    void Field(const char* name, math::Vector4& v) override { v = ArrToVec4(FindArray(name), v); }
    void Field(const char* name, math::Quaternion& v) override { v = ArrToQuat(FindArray(name), v); }
    void Field(const char* name, scene::ParticleCurve& v) override;
    void Field(const char* name, scene::ParticleGradient& v) override;

    void ListField(const char* name, std::vector<float>& values) override;
    void ListField(const char* name, std::vector<int>& values) override;
    void ListField(const char* name, std::vector<bool>& values) override;
    void ListField(const char* name, std::vector<std::string>& values) override;
    void ListField(const char* name, std::vector<math::Vector2>& values) override;
    void ListField(const char* name, std::vector<math::Vector3>& values) override;
    void ListField(const char* name, std::vector<math::Vector4>& values) override;

    void BeginObject(const char* name) override;
    void EndObject() override;

    [[nodiscard]] std::size_t BeginObjectList(const char* name, std::size_t count) override;
    void BeginObjectElement(std::size_t index) override;
    void EndObjectElement() override { EndObject(); }
    [[nodiscard]] std::size_t EndObjectList() override;

protected:
    [[nodiscard]] const toml::table* Current() const { return m_stack.back(); }
    [[nodiscard]] const toml::node*  FindNode(const char* fallback) const;
    [[nodiscard]] const toml::array* FindArray(const char* fallback) const;

private:
    std::vector<const toml::table*> m_stack;
    std::vector<const toml::array*> m_listStack;
};

} // namespace fbzz::util
