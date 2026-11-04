/// @file    Reflection.hpp
/// @brief   Component・DataAsset・Script 共通の反射契約とフィールド宣言。
/// @author  Hasegawa Jin
/// @date    2026-06-29
///
/// @note 従来は外部ツール (FBZZ Header Tool / FHT) が FBZZ_FIELD を走査し .generated.hpp に Reflect() を
/// @note 吐き出していた (生成漏れフットガン・1 スクリプト = 3 ファイル・リポジトリ外ツール依存)。
/// @note 本ヘッダは __COUNTER__ ベースのタグディスパッチで、フィールド宣言と同じ場所から Reflect() を生成する。
/// @note 外部ツールも .generated.hpp も不要になり、フィールドを 1 行足すだけで Inspector とシリアライズが
/// @note 自動追従する。
#pragma once
#include <string>
#include <Engine/Api.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/ObjectReference.hpp>
#include <Engine/Scene/PrefabRef.hpp>
#include <Engine/Scene/DataAssetRef.hpp>
#include <Engine/Scene/ScriptAssetRef.hpp>
#include <Engine/Scene/ParticleCurve.hpp>
#include <Engine/Scene/Ref.hpp>
#include <Engine/Input/KeyCode.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>


namespace fbzz::scene::detail {

/// @cond INTERNAL
/// @brief 宣言順を保つためのタグ型。`ReflectTag<N>` は `ReflectTag<N-1>` を継承する。
/// @note `ReflectTag<最大値>` を渡すと最後に宣言したフィールドの関数が選ばれ、1 つ前のタグへ再帰して先頭まで宣言順に連鎖する。
/// @note Doxygen は再帰継承を循環と誤検出するため、条件セクションでリファレンスから外している。
template<int N> struct ReflectTag : ReflectTag<N - 1> {};
template<>      struct ReflectTag<0> {};
/// @endcond

/// @brief camelCase / snake_case のメンバー名を人間可読な Inspector 表示名へ整形する (例: `swingStartTime` → `Swing Start Time`)。
/// @note 空文字を渡すと自動生成し、FBZZ_FIELD の表示名の重複記述を省ける。
inline std::string MakeDisplayName(const char* member)
{
    std::string out;
    bool prevLower = false;
    bool prevDigit = false;
    for (const char* p = member; p && *p; ++p) {
        char c = *p;
        if (c == '_') {
            /// @note スネークケースの区切りは空白へ変換する。
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
            prevLower = false;
            prevDigit = false;
            continue;
        }
        const bool isUpper = (c >= 'A' && c <= 'Z');
        const bool isDigit = (c >= '0' && c <= '9');
        /// @note 小文字→大文字 / 文字⇔数字 の境界で単語を区切る。
        if (!out.empty() && out.back() != ' ' &&
            ((isUpper && prevLower) || (isDigit && !prevDigit) || (!isDigit && prevDigit)))
            out.push_back(' ');
        /// @note 各単語の先頭は大文字化する。
        if (out.empty() || out.back() == ' ') {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        }
        out.push_back(c);
        prevLower = (c >= 'a' && c <= 'z');
        prevDigit = isDigit;
    }
    return out;
}

/// @brief 明示表示名が空 (`""`) なら member 名から自動生成する。非空ならそのまま使う。
inline std::string DisplayOr(const char* explicitName, const char* member)
{
    return (explicitName && explicitName[0] != '\0')
        ? std::string(explicitName)
        : MakeDisplayName(member);
}

} /// @note namespace fbzz::scene::detail

namespace fbzz::scene {
/// @brief `FBZZ_REF(T, ...)` が RefField へ渡す型名を解決する。
/// @param declaredName 登録コンポーネントの型名 (FBZZ_REF に書いた型名そのもの)。GameObject / Script 参照では未使用。
/// @return GameObject なら空文字、Script 派生・インターフェースなら T::TYPE_NAME、それ以外は declaredName。
/// @note Inspector はこの型名でドロップを検証し、フィルタ付きピッカーを出す。
/// @note コンポーネントだけマクロから名前を貰うのは TYPE_NAME を持たないため。名前の正本は ComponentRegistry の serializedName (中身は登録マクロの `#Type`) で、FBZZ_REF が持つ `#Type` から同じ文字列が作れる。名前空間付きで書くと食い違うため素の型名で書くこと。
template<typename T>
constexpr const char* RefTypeNameOf(const char* declaredName)
{
    /// @note GameObject / Script 参照では使わない。
    (void)declaredName;
    if constexpr (std::is_same_v<T, GameObject>)
        return "";
    else if constexpr (requires(const T& value) { value.FbzzAsType(std::string_view{}); })
        return T::TYPE_NAME;
    else
        return declaredName;
}

struct IReflector;
/// @brief Script 内のネスト値型が同じ Reflect 経路へ参加するための最小 interface。
struct IScriptSerializable {
    virtual ~IScriptSerializable() = default;
    virtual void Reflect(IReflector& reflector) = 0;
};

class ScriptSerializableFactory {
public:
    using Factory = std::function<std::unique_ptr<IScriptSerializable>()>;

    FBZZ_ENGINE_API static bool Register(std::string_view typeName, Factory factory);
    FBZZ_ENGINE_API static std::unique_ptr<IScriptSerializable> Create(std::string_view typeName);
    FBZZ_ENGINE_API static std::vector<std::string> RegisteredTypeNames();
    FBZZ_ENGINE_API static void UnregisterAll();
};

struct ScriptSerializedReference {
    std::string type;
    std::unique_ptr<IScriptSerializable> value;
    std::string preservedFieldsToml;

    FBZZ_ENGINE_API bool SetType(std::string_view typeName);
    FBZZ_ENGINE_API void Clear();
};


/// @brief Component・DataAsset・Script の Inspector と保存に共通のビジター。
/// @note Inspector が ImGui でフィールドを表示・編集し、SceneSerializer が JSON との相互変換で実装する。
struct IReflector {
    enum class FieldHint {
        Default,
        Multiline,
        Color,
        Angle,
        LayerMask,
        Tag,
        File,
        AudioBus, ///< @note ミキサーバス名。Inspector は ProjectSettings のバス一覧をドロップダウンで出す。綴りミスは Master へ黙って落ちるため自由入力のまま。
    };

    virtual ~IReflector() = default;

    /// @brief Reflect 宣言の永続キーと Inspector 表示名を分離するため、各 Field 直前に呼ばれる。
    /// @note 表示名を変更しても Scene / Prefab の保存キーが変わらないようにするため。
    void BeginField(const char* persistentKey, const char* displayName)
    {
        m_persistentKey = persistentKey ? persistentKey : "";
        m_displayName = displayName ? displayName : m_persistentKey;
        m_fieldVisible = true;
        m_fieldEnabled = true;
        m_fieldReadOnly = false;
        m_fieldHidden = false;
        m_fieldHint = FieldHint::Default;
        m_fieldMin = 0.0f;
        m_hasFieldMin = false;
        m_fieldMax = 0.0f;
        m_hasFieldMax = false;
        m_fieldStep = 0.0f;
        m_fileExtensions.clear();
        m_fixedList = false;
        m_previousFieldKeys.clear();
    }

    [[nodiscard]] const char* PersistentKey(const char* fallback) const
    {
        return m_persistentKey.empty() ? fallback : m_persistentKey.c_str();
    }

    [[nodiscard]] const char* DisplayName(const char* fallback) const
    {
        return m_displayName.empty() ? fallback : m_displayName.c_str();
    }

    void SetFieldVisible(bool visible) { m_fieldVisible = visible; }
    void SetFieldEnabled(bool enabled) { m_fieldEnabled = enabled; }
    void SetFieldReadOnly(bool readOnly) { m_fieldReadOnly = readOnly; }
    void SetFieldHidden(bool hidden) { m_fieldHidden = hidden; }
    void SetFieldHint(FieldHint hint) { m_fieldHint = hint; }
    void SetFieldMin(float minimum) { m_fieldMin = minimum; m_hasFieldMin = true; }
    /// @brief 表示レンジの上限を設定する (数値の clamp ではない)。
    /// @note カーブの縦軸表示に使う。
    void SetFieldMax(float maximum) { m_fieldMax = maximum; m_hasFieldMax = true; }
    void SetFieldStep(float step) { m_fieldStep = step; }
    void SetFileExtensions(std::string_view extensions)
    {
        m_fileExtensions = extensions;
        m_fieldHint = FieldHint::File;
    }
    void SetFixedList(bool fixed) { m_fixedList = fixed; }
    void SetPreviousFieldKeys(const std::vector<std::string>& keys) { m_previousFieldKeys = keys; }
    /// @note 読み込みの最終参照パスだけ true にし、同一 GUID の未解決 wrapper を明示的に再接続する。
    void SetResolveObjectReferences(bool resolve) { m_resolveObjectReferences = resolve; }
    [[nodiscard]] bool ResolveObjectReferences() const { return m_resolveObjectReferences; }
    [[nodiscard]] const std::vector<std::string>& PreviousFieldKeys() const { return m_previousFieldKeys; }

    [[nodiscard]] bool FieldVisible() const { return m_fieldVisible && !m_fieldHidden; }
    [[nodiscard]] bool FieldEnabled() const { return m_fieldEnabled && !m_fieldReadOnly; }
    [[nodiscard]] bool FieldReadOnly() const { return m_fieldReadOnly; }
    [[nodiscard]] FieldHint CurrentFieldHint() const { return m_fieldHint; }
    [[nodiscard]] float FieldMin() const { return m_fieldMin; }
    [[nodiscard]] bool HasFieldMin() const { return m_hasFieldMin; }
    [[nodiscard]] float FieldMax() const { return m_fieldMax; }
    [[nodiscard]] bool HasFieldMax() const { return m_hasFieldMax; }
    [[nodiscard]] float FieldStep() const { return m_fieldStep; }
    [[nodiscard]] const std::string& FileExtensions() const { return m_fileExtensions; }
    [[nodiscard]] bool FixedList() const { return m_fixedList; }

    virtual void Field(const char* name, float& v) = 0;
    virtual void Field(const char* name, int& v) = 0;
    virtual void Field(const char* name, bool& v) = 0;
    virtual void Field(const char* name, math::Vector2& v) = 0;
    virtual void Field(const char* name, math::Vector3& v) = 0;
    virtual void Field(const char* name, math::Vector4& v) = 0;
    virtual void Field(const char* name, std::string& v) = 0;
    virtual void Field(const char* name, math::Quaternion& v) = 0;

    /// @note 参照型。デフォルト実装は非対応 Reflector へのフォールバック。
    virtual void Field(const char* name, EntityID& v) {}
    virtual void Field(const char* name, EntityRef& v)
    {
        if (v.guid.empty()) { Field(name, v.id); return; }
        ObjectReference value{v.guid, v.id, v.sceneGeneration};
        ObjectReferenceField(name, value);
        v.guid = std::move(value.guid); v.id = value.id; v.sceneGeneration = value.sceneGeneration;
    }
    virtual void Field(const char* name, PrefabRef& v)  { Field(name, v.path); }
    /// @note DataAsset (純共有 ScriptableObject) 参照。既定は path 文字列をそのまま保存/復元するため TOML リフレクタは無変更でよい。
    /// @note Inspector の ImGuiReflector だけがアセットスロット UI を上書きする。
    virtual void Field(const char* name, DataAssetRef& v) { Field(name, v.path); }
    /// @note キー名ドロップダウン。
    virtual void Field(const char* name, input::KeyCode& v) {}
    /// @brief Script 用型付き Asset 参照。Serializer は GUID と path、Inspector は型フィルター付き slot を扱う。
    virtual void AssetField(const char* name,
                            ScriptAssetReference& v,
                            ScriptAssetType type)
    {
        (void)type;
        Field(name, v.path);
    }
    virtual void ListField(const char* name, std::vector<float>& values) { (void)name; (void)values; }
    virtual void ListField(const char* name, std::vector<int>& values) { (void)name; (void)values; }
    virtual void ListField(const char* name, std::vector<bool>& values) { (void)name; (void)values; }
    virtual void ListField(const char* name, std::vector<std::string>& values) { (void)name; (void)values; }
    virtual void ListField(const char* name, std::vector<math::Vector2>& values) { (void)name; (void)values; }
    virtual void ListField(const char* name, std::vector<math::Vector3>& values) { (void)name; (void)values; }
    virtual void ListField(const char* name, std::vector<math::Vector4>& values) { (void)name; (void)values; }
    virtual void ListField(const char* name, std::vector<EntityRef>& values) { (void)name; (void)values; }
    /// @brief 型付きオブジェクト参照のリスト (`FBZZ_REF_LIST_FIELD` 用)。
    /// @note 既定は型情報を落として通常のリストとして扱うため TOML 側は無変更で済む。Inspector だけが typeName でドロップ可否を判定する。
    virtual void RefListField(const char* name, std::vector<EntityRef>& values, const char* typeName)
    {
        (void)typeName;
        ListField(name, values);
    }
    virtual void AssetListField(const char* name,
                                std::vector<ScriptAssetReference>& values,
                                ScriptAssetType type)
    {
        (void)name;
        (void)values;
        (void)type;
    }
    virtual void ObjectField(const char* name, IScriptSerializable& value)
    {
        /// @note BeginObject / EndObject へ委譲する。BeginObject を実装したリフレクタは入れ子化も自動的に手に入る。
        BeginObject(name);
        value.Reflect(*this);
        EndObject();
    }
    virtual void ReferenceField(const char* name, ScriptSerializedReference& value)
    {
        (void)name;
        if (value.value)
            value.value->Reflect(*this);
    }

    /// @name 非仮想ヘルパー
    /// @note BeginField → SetFieldHint 等 → Field → EndField の手順を 1 呼び出しにまとめる。ヒントは BeginField でリセットされるため、手で書くと 1 箇所抜けただけで数値入力へ戻る。
    /// @note 保存キーは name のままなので r.Field から差し替えてもシーンのデータは変わらない。非仮想なのは vtable の並びを崩さないため。
    /// @{

    /// @brief BeginField で立てた保存キー・表示条件・ヒントを既定へ戻す。
    /// @note 状態は次の BeginField まで残る。戻し忘れると後続の Field が前のキーを引き継ぎ、TOML が同じキーへの二重挿入を黙って捨てるためエラーも警告もなく保存されなくなる。BeginField を直に呼んだら必ず呼ぶこと。
    void EndField() { BeginField(nullptr, nullptr); }

    void ColorField(const char* name, math::Vector3& v)
    {
        BeginField(name, name);
        SetFieldHint(FieldHint::Color);
        Field(name, v);
        EndField();
    }
    void ColorField(const char* name, math::Vector4& v)
    {
        BeginField(name, name);
        SetFieldHint(FieldHint::Color);
        Field(name, v);
        EndField();
    }

    /// @brief 条件を満たすときだけ Inspector に出すフィールド。保存は条件によらず常に行う。
    /// @note 表示条件は「今この値が効くか」でしかなく、条件付き保存にするとモードを戻すだけで設定が消える。tooltip を受け取るのは Tooltip() が直前に描いた項目へ付くため。
    template<typename T>
    void FieldIf(const char* name, T& v, bool visible, const char* tooltip = nullptr)
    {
        BeginField(name, name);
        SetFieldVisible(visible);
        Field(name, v);
        if (visible && tooltip) Tooltip(tooltip);
        EndField();
    }

    /// @brief アセットパス欄。Inspector は extensions で絞ったピッカーとドロップ先を出す。
    /// @note 名前規約 (xxxPath) に頼らない理由: 同じ種類のアセットを 1 コンポーネントが複数持つ場合 (ボタンの状態別スプライト等)、規約に合う名前は 1 つしか作れない。
    void FileField(const char* name, std::string& v, const char* extensions,
                   const char* tooltip = nullptr)
    {
        BeginField(name, name);
        SetFileExtensions(extensions);
        Field(name, v);
        if (tooltip) Tooltip(tooltip);
        EndField();
    }
    /// @}

    /// @name 付加情報付き
    /// @note デフォルトは Field へフォールバックする。
    /// @{
    virtual void FloatRange(const char* name, float& v, float min, float max)  { Field(name, v); }
    virtual void IntRange(const char* name, int& v, int min, int max)          { Field(name, v); }
    virtual void Enum(const char* name, int& v, std::span<const char* const> labels) { Field(name, v); }
    virtual void Flags(const char* name, int& v, std::span<const char* const> labels) { Enum(name, v, labels); }
    /// @brief 型付きオブジェクト参照スロット。typeName が非空ならその Script 型を持つ GameObject だけを受け付ける (Inspector のドロップ型チェック用)。
    /// @note 型指定はUIだけに使い、永続GUIDは通常参照と同じ経路で保持する。
    virtual void RefField(const char* name, EntityRef& v, const char* typeName) { (void)typeName; Field(name, v); }
    /// @brief 直前に描画したフィールドへ説明ツールチップを付ける (Inspector のみ表示、シリアライズ非対象)。
    virtual void Tooltip(const char* text) {}
    virtual void Group(const char* label) {}
    virtual void Space(float height) { (void)height; }
    virtual void Readonly(const char* name, const std::string& v) {}
    virtual void Readonly(const char* name, float v)  {}
    virtual void Readonly(const char* name, int v)    {}
    /// @}

    /// @name 入れ子オブジェクト / 構造体配列
    /// @note ManagedScriptAdapter が Reflect() を override し、Inspector と保存 codec へ managed field を投影する。
    /// @{

    /// @brief BeginObject / EndObject で挟んだ範囲を 1 つの入れ子オブジェクトとして扱う。
    /// @note 既定実装は何もしない (= 親と同じ階層へフラット展開、未対応リフレクタと後方互換)。ObjectField でなくスコープ対なのは継承を要求しないため。ObjectField だと renderer::BloomSettings のような他層の構造体に Scene 層の interface が要り、RenderSettings.hpp が Script.hpp を include して依存方向が逆流する。
    virtual void BeginObject(const char* name) { (void)name; }
    virtual void EndObject() {}

    /// @brief 構造体の配列。現在の要素数を渡し、リフレクタが決めた新しい要素数を返す。
    /// @return 読み込みリフレクタは保存されていた要素数、Inspector は Add / Remove 後の要素数、書き込みリフレクタは受け取った値をそのまま返す。
    /// @note 呼び出し側は戻り値で vector を resize してから、要素ごとに BeginObjectElement / EndObjectElement で挟んで反映する。
    [[nodiscard]] virtual std::size_t BeginObjectList(const char* name, std::size_t count)
    {
        (void)name;
        return count;
    }
    virtual void BeginObjectElement(std::size_t index) { (void)index; }
    virtual void EndObjectElement() {}

    /// @brief 削除要求のインデックスを返す。要素数未満なら呼び出し側がその要素を erase する。
    /// @return 削除要求が無い場合は NO_REMOVE。
    /// @note 要素数の増加は BeginObjectList の戻り値 + resize で表せるが、途中の要素を消す操作は resize では表せない (必ず末尾が落ちる) ため、削除だけ別経路で伝える。
    static constexpr std::size_t NO_REMOVE = static_cast<std::size_t>(-1);
    [[nodiscard]] virtual std::size_t EndObjectList() { return NO_REMOVE; }

    /// @brief 時間 → 値 / 色のカーブ。Inspector は専用のキャンバスエディタを出す。
    /// @note 縦軸の最大値は SetFieldMax で伝える (未指定なら 1.0)。既定が no-op なのは未対応リフレクタでクラッシュさせないため。保存を伴うリフレクタ (TomlWrite/Read・Snapshot) は必ず override すること (落とすと Play/Stop で既定へ戻る)。
    virtual void Field(const char* name, ParticleCurve& v) { (void)name; (void)v; }
    virtual void Field(const char* name, ParticleGradient& v) { (void)name; (void)v; }

    /// @brief Inspector のアクションボタン。押されたら action(userData) を 1 回だけ呼ぶ。
    /// @note 関数ポインタ + void* なのは、std::function が実装定義のレイアウトを DLL 境界へ晒すため。キャプチャ無しラムダなら関数ポインタへ落ち、境界を安全に越える。
    using ActionCallback = void (*)(void* userData);
    virtual void Button(const char* label, ActionCallback action, void* userData)
    {
        (void)label;
        (void)action;
        (void)userData;
    }

    /// @brief 構造体配列の並び替え要求。EndObjectList の直後に呼ばれる。
    /// @note 戻り値 1 つでは「どこからどこへ」を運べないので削除とは別の経路にする。同じフレームで削除と並び替えが重なったら削除だけを通すこと (index がずれる)。
    [[nodiscard]] virtual bool ObjectListMove(std::size_t& from, std::size_t& to)
    {
        (void)from;
        (void)to;
        return false;
    }
    /// @brief 観測値の型を通知する。true の利用者だけが値の評価を要求する。
    /// @note 保存・復元・Undo は既定の false を使い、getter を一切呼ばない。
    virtual bool BeginObservation(const char* name, const char* type)
    {
        (void)name;
        (void)type;
        return false;
    }
    /// @brief 必須設定のメタデータ。保存・通常の Inspector 描画では何もしない。
    virtual void RequireAsset(const char*, const DataAssetRef&) {}
    virtual void RequireReference(const char*, const EntityRef&, const char*) {}
    /// @note 保存は GUID のみ。未対応 Reflector は文字列として扱い、Scene/Inspector は一時 EntityID を補う。
    virtual void ObjectReferenceField(const char* name, ScriptObjectReference& value)
    {
        const std::string before = value.guid;
        Field(name, value.guid);
        if (value.guid != before) {
            value.id = EntityID::INVALID;
            value.sceneGeneration = 0;
        }
    }
    /// @brief 保存可能な GameObject 参照を Inspector の型制約付きで描画する。
    /// @note 保存・再対応は ObjectReferenceField へ委譲し、未対応 Reflector との互換性を保つ。
    virtual void TypedObjectReferenceField(const char* name, ObjectReference& value, const char* typeName)
    {
        (void)typeName;
        ObjectReferenceField(name, value);
    }
    /// @note 順序・重複・空 GUID の null スロット・欠損 GUID を保持する。
    virtual void ObjectReferenceListField(const char* name, std::vector<ScriptObjectReference>& values)
    {
        std::vector<std::string> guids;
        guids.reserve(values.size());
        for (const auto& value : values) guids.push_back(value.guid);
        ListField(name, guids);
        values.resize(guids.size());
        for (size_t index = 0; index < guids.size(); ++index) {
            if (values[index].guid != guids[index]) {
                values[index].id = EntityID::INVALID;
                values[index].sceneGeneration = 0;
            }
            values[index].guid = std::move(guids[index]);
        }
    }
    /// @}

private:
    std::string m_persistentKey;
    std::string m_displayName;
    bool m_fieldVisible = true;
    bool m_fieldEnabled = true;
    bool m_fieldReadOnly = false;
    bool m_fieldHidden = false;
    FieldHint m_fieldHint = FieldHint::Default;
    float m_fieldMin = 0.0f;
    bool m_hasFieldMin = false;
    float m_fieldMax = 0.0f;
    bool m_hasFieldMax = false;
    float m_fieldStep = 0.0f;
    std::string m_fileExtensions;
    bool m_fixedList = false;
    bool m_resolveObjectReferences = false;
    std::vector<std::string> m_previousFieldKeys;
};


/// @brief C++ のデータ型に反射フィールド連鎖を生成するマクロ。
/// @note `__COUNTER__` で採番した `detail::ReflectTag<N>` を宣言順に連鎖させ Reflect() を生成する。

/// @brief 型名と Reflect 連鎖の土台。`GetTypeName() const` / `Reflect(IReflector&)` を持つ基底であれば Script でなくても使える (DataAsset がこれを流用する)。
/// @note 終端 (ReflectTag<0>) は含めない。中身が用途ごとに違い、固定で置くと派生でフィールドの引き継ぎができなくなる。
/// @note `_fbzz_base` を採ったあとに `__COUNTER__` を消費しないこと (フィールドの採番がずれる)。
#define FBZZ_REFLECT_CORE_(T)                                                   \
    public:                                                                     \
    using FbzzSelf = T;                                                         \
    static constexpr const char* TYPE_NAME = #T;                               \
    const char* GetTypeName() const override { return TYPE_NAME; }             \
    void Reflect(::fbzz::scene::IReflector& r_) override;                       \
    static constexpr int _fbzz_base = __COUNTER__;

namespace detail {

/// @brief T が `FbzzAsType` による Script 型検索へ参加するか。
/// @note managed Script adapter とその C++ 基底型の検索経路を選ぶ。
template<typename T, typename = void>
inline constexpr bool kIsScriptQueryable = false;
template<typename T>
inline constexpr bool kIsScriptQueryable<
    T, std::void_t<decltype(std::declval<const T&>().FbzzAsType(std::string_view{}))>> = true;

/// @brief 基底を順に当たり、最初に見つかった部分オブジェクトのポインタを返す。
/// @note 修飾呼び出し (`Bases::FbzzAsType`) にするのは、仮想呼び出しだと派生の実装へ戻り無限再帰になるため。修飾すればその基底の実装が直接呼ばれ、self は static_cast でその基底の部分オブジェクトへずらされた後なので番地も自動的に正しくなる。
/// @note 同じ型名が 2 つの経路から見える場合 (ダイヤモンド) は先に書いた基底が勝つ。
/// @note MSVC で副作用付き fold 式の結果が失われるため、1 基底ずつ結果を返す。
template<typename First, typename... Rest, typename Self>
[[nodiscard]] inline void* TryBases(const Self* self, std::string_view typeName)
{
    if (void* found = static_cast<const First*>(self)->First::FbzzAsType(typeName))
        return found;
    if constexpr (sizeof...(Rest) > 0)
        return TryBases<Rest...>(self, typeName);
    return nullptr;
}

/// @note 1 要素を from から to へ運ぶ。間の要素の相対順序は保たれる。
/// @note FBZZ_OBJECT_LIST_FIELD が IReflector::ObjectListMove の結果を反映するのに使う。
template<typename T>
inline void MoveListElement(std::vector<T>& values, std::size_t from, std::size_t to)
{
    if (from >= values.size() || to >= values.size() || from == to) return;
    const auto begin = values.begin();
    const auto source = begin + static_cast<std::ptrdiff_t>(from);
    const auto target = begin + static_cast<std::ptrdiff_t>(to);
    if (from < to) std::rotate(source, source + 1, target + 1);
    else           std::rotate(target, source, source + 1);
}

} /// @note namespace detail

/// @brief Script 以外のネスト値型へ同じ宣言式 Reflect を与える。
#define FBZZ_SERIALIZABLE(T)                                                    \
    public:                                                                     \
    using FbzzSelf = T;                                                         \
    static constexpr const char* TYPE_NAME = #T;                               \
    void Reflect(::fbzz::scene::IReflector& r_) override;                       \
    static constexpr int _fbzz_base = __COUNTER__;                              \
    void _fbzz_reflect(::fbzz::scene::detail::ReflectTag<0>,                    \
                       ::fbzz::scene::IReflector&) {}

namespace detail {
/// @brief 観測型を JSON / Inspector 共通の名前へ変換する。未対応型は宣言時に拒否する。
template<typename T>
constexpr const char* ObservationTypeName()
{
    static_assert(std::is_same_v<T, bool> || std::is_same_v<T, int> ||
        std::is_same_v<T, float> || std::is_same_v<T, std::string> ||
        std::is_same_v<T, math::Vector2> || std::is_same_v<T, math::Vector3> ||
        std::is_same_v<T, math::Vector4> || std::is_same_v<T, math::Quaternion>,
        "FBZZ_OBSERVE supports bool, int, float, string, vectors and quaternion");
    if constexpr (std::is_same_v<T, bool>) return "bool";
    if constexpr (std::is_same_v<T, int>) return "int";
    if constexpr (std::is_same_v<T, float>) return "float";
    if constexpr (std::is_same_v<T, std::string>) return "string";
    if constexpr (std::is_same_v<T, math::Vector2>) return "vector2";
    if constexpr (std::is_same_v<T, math::Vector3>) return "vector3";
    if constexpr (std::is_same_v<T, math::Vector4>) return "vector4";
    return "quaternion";
}
} /// @note namespace detail

/// @brief 1 エントリ分の登録。直前タグ (1 つ前のフィールド/グループ) を先に処理してから自分を反映することで宣言順を保つ。UniqueTok はメンバー名や行番号で一意化する。
/// @note 可変長引数なのは、リフレクション文に含まれるトップレベルのカンマ (FBZZ_FIELD_ENUM のラベル配列など `()` で保護されないもの) を `__VA_ARGS__` で吸収するため。
#define FBZZ_REFLECT_ENTRY_(UniqueTok, ...)                                     \
    enum { _fbzz_idx_##UniqueTok = __COUNTER__ - _fbzz_base };                  \
    void _fbzz_reflect(                                                         \
        ::fbzz::scene::detail::ReflectTag<_fbzz_idx_##UniqueTok>,               \
        ::fbzz::scene::IReflector& r_) {                                        \
        _fbzz_reflect(::fbzz::scene::detail::ReflectTag<                        \
                          (_fbzz_idx_##UniqueTok - 1)>{}, r_);                  \
        __VA_ARGS__;                                                            \
    }

/// @brief 表示名解決 (空なら変数名から自動生成)。`const char*` を期待する Field 等へ渡す。
#define FBZZ_DISP_(Display, Name)                                               \
    ::fbzz::scene::detail::DisplayOr(Display, #Name).c_str()

#define FBZZ_FIELD(Type, Name, Default, Display)                                \
    Type Name = Default;                                                        \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.Field(FBZZ_DISP_(Display, Name), Name);                              \
    })

/// @brief 旧保存キーを読み込み、新しいメンバー名で保存し直すフィールド。
#define FBZZ_FIELD_MIN(Type, Name, Default, Display, Min)                       \
    Type Name = Default;                                                        \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.SetFieldMin(static_cast<float>(Min));                                \
        r_.Field(FBZZ_DISP_(Display, Name), Name);                              \
    })

#define FBZZ_FIELD_STEP(Type, Name, Default, Display, Step)                     \
    Type Name = Default;                                                        \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.SetFieldStep(static_cast<float>(Step));                              \
        r_.Field(FBZZ_DISP_(Display, Name), Name);                              \
    })

#define FBZZ_FIELD_MULTILINE(Name, Default, Display)                            \
    std::string Name = Default;                                                 \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.SetFieldHint(::fbzz::scene::IReflector::FieldHint::Multiline);       \
        r_.Field(FBZZ_DISP_(Display, Name), Name);                              \
    })

#define FBZZ_FIELD_COLOR(Name, Default, Display)                                \
    ::fbzz::math::Vector4 Name = Default;                                       \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.SetFieldHint(::fbzz::scene::IReflector::FieldHint::Color);           \
        r_.Field(FBZZ_DISP_(Display, Name), Name);                              \
    })

#define FBZZ_FIELD_ANGLE(Name, Default, Display)                                \
    float Name = Default;                                                       \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.SetFieldHint(::fbzz::scene::IReflector::FieldHint::Angle);           \
        r_.Field(FBZZ_DISP_(Display, Name), Name);                              \
    })

#define FBZZ_FIELD_LAYER_MASK(Name, Default, Display)                           \
    int Name = Default;                                                         \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.SetFieldHint(::fbzz::scene::IReflector::FieldHint::LayerMask);       \
        r_.Field(FBZZ_DISP_(Display, Name), Name);                              \
    })

#define FBZZ_FIELD_TAG(Name, Default, Display)                                  \
    std::string Name = Default;                                                 \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.SetFieldHint(::fbzz::scene::IReflector::FieldHint::Tag);             \
        r_.Field(FBZZ_DISP_(Display, Name), Name);                              \
    })

#define FBZZ_FIELD_FILE(Name, Default, Display, Extensions)                     \
    std::string Name = Default;                                                 \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.SetFileExtensions(Extensions);                                       \
        r_.Field(FBZZ_DISP_(Display, Name), Name);                              \
    })

/// @brief 音声クリップのパス欄。拡張子は kAudioClipExtensions で一元管理する。
/// @note 専用マクロにするのは、FBZZ_FIELD_FILE に `.wav,.ogg` と手書きすると対応形式を増やしても既存フィールドが取り残されるため (実際 `.mp3`/`.synth` がどのスロットにも入らない状態になっていた)。
#define FBZZ_FIELD_AUDIO(Name, Default, Display)                                \
    std::string Name = Default;                                                 \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.SetFileExtensions(::fbzz::scene::kAudioClipExtensions);              \
        r_.Field(FBZZ_DISP_(Display, Name), Name);                              \
    })

/// @brief ミキサーバス名の欄。Inspector は ProjectSettings のバス一覧から選ばせる。
#define FBZZ_FIELD_AUDIO_BUS(Name, Default, Display)                            \
    std::string Name = Default;                                                 \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.SetFieldHint(::fbzz::scene::IReflector::FieldHint::AudioBus);        \
        r_.Field(FBZZ_DISP_(Display, Name), Name);                              \
    })

#define FBZZ_FIELD_READ_ONLY(Type, Name, Default, Display)                      \
    Type Name = Default;                                                        \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.SetFieldReadOnly(true);                                              \
        r_.Field(FBZZ_DISP_(Display, Name), Name);                              \
    })

#define FBZZ_FIELD_HIDDEN(Type, Name, Default)                                  \
    Type Name = Default;                                                        \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, #Name);                                            \
        r_.SetFieldHidden(true);                                                \
        r_.Field(#Name, Name);                                                  \
    })

#define FBZZ_FIELD_SHOW_IF(Type, Name, Default, Display, Condition)             \
    Type Name = Default;                                                        \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.SetFieldVisible(static_cast<bool>(Condition));                       \
        r_.Field(FBZZ_DISP_(Display, Name), Name);                              \
    })

#define FBZZ_FIELD_ENABLE_IF(Type, Name, Default, Display, Condition)           \
    Type Name = Default;                                                        \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.SetFieldEnabled(static_cast<bool>(Condition));                       \
        r_.Field(FBZZ_DISP_(Display, Name), Name);                              \
    })

#define FBZZ_LIST_FIELD(Type, Name, Display)                                    \
    std::vector<Type> Name;                                                     \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.ListField(FBZZ_DISP_(Display, Name), Name);                          \
    })

#define FBZZ_LIST_FIELD_DEFAULT(Type, Name, Default, Display)                   \
    std::vector<Type> Name = Default;                                           \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.ListField(FBZZ_DISP_(Display, Name), Name);                          \
    })

#define FBZZ_FIXED_ARRAY_FIELD(Type, Name, Count, Default, Display)             \
    std::array<Type, Count> Name = Default;                                     \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.SetFixedList(true);                                                  \
        std::vector<Type> _fbzz_values(Name.begin(), Name.end());               \
        r_.ListField(FBZZ_DISP_(Display, Name), _fbzz_values);                  \
        const std::size_t _fbzz_count = (std::min)(Name.size(), _fbzz_values.size());\
        for (std::size_t _fbzz_i = 0; _fbzz_i < _fbzz_count; ++_fbzz_i)        \
            Name[_fbzz_i] = _fbzz_values[_fbzz_i];                             \
    })

#define FBZZ_OBJECT_FIELD(Type, Name, Display)                                  \
    Type Name = {};                                                             \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.ObjectField(FBZZ_DISP_(Display, Name), Name);                        \
    })

/// @brief 構造体の配列。要素型は IScriptSerializable を実装していること。
/// @note 要素数の増減・途中要素の削除・並び替えをリフレクタから受け取り vector へ反映する。削除を戻り値で受けるのは resize が必ず末尾を落とすため。
#define FBZZ_OBJECT_LIST_FIELD(Type, Name, Display)                             \
    std::vector<Type> Name;                                                     \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        const std::size_t _fbzz_n =                                             \
            r_.BeginObjectList(FBZZ_DISP_(Display, Name), Name.size());         \
        Name.resize(_fbzz_n);                                                   \
        for (std::size_t _fbzz_i = 0; _fbzz_i < _fbzz_n; ++_fbzz_i) {          \
            r_.BeginObjectElement(_fbzz_i);                                     \
            Name[_fbzz_i].Reflect(r_);                                          \
            r_.EndObjectElement();                                              \
        }                                                                       \
        const std::size_t _fbzz_rm = r_.EndObjectList();                        \
        if (_fbzz_rm < Name.size())                                             \
            Name.erase(Name.begin() + static_cast<std::ptrdiff_t>(_fbzz_rm));   \
        std::size_t _fbzz_from = 0;                                             \
        std::size_t _fbzz_to = 0;                                               \
        if (r_.ObjectListMove(_fbzz_from, _fbzz_to))                            \
            ::fbzz::scene::detail::MoveListElement(Name, _fbzz_from, _fbzz_to); \
    })

#define FBZZ_ASSET_LIST_FIELD(Type, Name, Display)                              \
    std::vector<Type> Name;                                                     \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        std::vector<::fbzz::scene::ScriptAssetReference> _fbzz_values;          \
        _fbzz_values.reserve(Name.size());                                      \
        for (const auto& _fbzz_value : Name)                                   \
            _fbzz_values.push_back(_fbzz_value.reference);                     \
        r_.AssetListField(FBZZ_DISP_(Display, Name), _fbzz_values,              \
                          Type::ASSET_TYPE);                                    \
        Name.resize(_fbzz_values.size());                                       \
        for (std::size_t _fbzz_i = 0; _fbzz_i < Name.size(); ++_fbzz_i)        \
            Name[_fbzz_i].reference = _fbzz_values[_fbzz_i];                   \
    })

#define FBZZ_SERIALIZE_REFERENCE(Name, Display)                                 \
    ::fbzz::scene::ScriptSerializedReference Name;                              \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.ReferenceField(FBZZ_DISP_(Display, Name), Name);                     \
    })

#define FBZZ_FIELD_RANGE(Type, Name, Default, Display, Min, Max)                \
    Type Name = Default;                                                        \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.FloatRange(FBZZ_DISP_(Display, Name), Name, Min, Max);               \
    })

/// @brief int 用レンジフィールド (スライダー)。FBZZ_FIELD_RANGE の int 版。
#define FBZZ_FIELD_RANGE_INT(Type, Name, Default, Display, Min, Max)            \
    Type Name = Default;                                                        \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.IntRange(FBZZ_DISP_(Display, Name), Name, Min, Max);                 \
    })

#define FBZZ_FIELD_ENUM(Type, Name, Default, Display, ...)                      \
    Type Name = Default;                                                        \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        static const char* const _fbzz_labels[] = { __VA_ARGS__ };             \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        int _fbzz_value = static_cast<int>(Name);                               \
        r_.Enum(FBZZ_DISP_(Display, Name),                                      \
                _fbzz_value,                                                    \
                ::std::span<const char* const>(_fbzz_labels));                  \
        Name = static_cast<Type>(_fbzz_value);                                  \
    })

/// @brief 参照型のデフォルト値省略版。PrefabRef / EntityRef 等のデフォルトが `{}` のフィールドに使う。
#define FBZZ_FIELD_REF(Type, Name, Display)                                     \
    Type Name = {};                                                            \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.Field(FBZZ_DISP_(Display, Name), Name);                              \
    })

#define FBZZ_FIELD_FLAGS(Type, Name, Default, Display, ...)                     \
    Type Name = Default;                                                        \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        static const char* const _fbzz_labels[] = { __VA_ARGS__ };             \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        int _fbzz_value = static_cast<int>(Name);                               \
        r_.Flags(FBZZ_DISP_(Display, Name),                                     \
                 _fbzz_value,                                                   \
                 ::std::span<const char* const>(_fbzz_labels));                 \
        Name = static_cast<Type>(_fbzz_value);                                  \
    })

/// @brief GUID 付き型安全 Asset 参照フィールド。
/// @note Type には MaterialRef / TextureRef / SpriteRef / VFXRef 等を指定する。
#define FBZZ_ASSET_FIELD(Type, Name, Display)                                   \
    Type Name = {};                                                             \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.AssetField(FBZZ_DISP_(Display, Name), Name.reference,                \
            Type::ASSET_TYPE);                                                  \
    })

/// @brief 時間 (0..1) に対する値のカーブ。Inspector はキーをドラッグできるキャンバスを出す。実体は ParticleCurve。
/// @note Max は縦軸の最大値 (減衰率なら 1、速度倍率なら 10 など)。値は `curve.Evaluate(t)` で読む。
#define FBZZ_FIELD_CURVE(Name, Display, Max)                                    \
    ::fbzz::scene::ParticleCurve Name;                                          \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.SetFieldMax(static_cast<float>(Max));                                \
        r_.Field(FBZZ_DISP_(Display, Name), Name);                              \
    })

/// @brief 時間 (0..1) に対する色のグラデーション。
/// @note `gradient.Evaluate(t)` が sRGB、`EvaluateLinear(t)` がシェーダーへ渡すリニア色を返す。
#define FBZZ_FIELD_GRADIENT(Name, Display)                                      \
    ::fbzz::scene::ParticleGradient Name;                                       \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.Field(FBZZ_DISP_(Display, Name), Name);                              \
    })

/// @brief Inspector のアクションボタン。押すと引数なしのメンバー関数 `Method()` を 1 回呼ぶ。
/// @note メンバー関数の宣言は受け持たない (in-class の再宣言はエラーになり定義をクラス外へ追い出すことになる)。ここは結線だけを担う。
/// @note 編集中 (Play していない) でも押せる。押した結果はそのまま保存対象になる。
#define FBZZ_BUTTON(Method, Display)                                            \
    FBZZ_REFLECT_ENTRY_(Method, {                                               \
        r_.BeginField(#Method, FBZZ_DISP_(Display, Method));                    \
        r_.Button(FBZZ_DISP_(Display, Method),                                  \
                  [](void* self_) {                                             \
                      static_cast<FbzzSelf*>(self_)->Method();                  \
                  },                                                            \
                  this);                                                        \
        r_.EndField();                                                          \
    })

/// @brief 保存しない読み取り専用値。Expression は const 文脈で評価し、複製メンバーを作らない。
/// @note getter は副作用を持たず、OnAwake 前・無効状態でも安全に読めること。
#define FBZZ_OBSERVE(Type, Name, Expression, Display)                           \
    Type _fbzz_observe_##Name() const { return (Expression); }                 \
    FBZZ_REFLECT_ENTRY_(Name, {                                                \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.SetFieldReadOnly(true);                                             \
        if (r_.BeginObservation(FBZZ_DISP_(Display, Name),                     \
                ::fbzz::scene::detail::ObservationTypeName<Type>())) {         \
            Type observedValue = _fbzz_observe_##Name();                       \
            r_.Field(FBZZ_DISP_(Display, Name), observedValue);                 \
        }                                                                     \
        r_.EndField();                                                        \
    })

/// @brief Inspector 表示のみ・Serializer 非保存の計算値ラベル。
#define FBZZ_COMPUTED(Type, Name, Display)                                      \
    Type Name = {};                                                            \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.Readonly(FBZZ_DISP_(Display, Name), Name);                           \
    })
/// @brief 型安全オブジェクト参照フィールド。Inspector にドラッグ&ドロップスロットを出す。
/// @note `Ref<T>` は `{ this }` で所有 Script を受け取り、`Name.Get()` / `if (Name)` で解決する。シリアライズは内包する EntityRef (= EntityID) を対象にする。
/// @note T に取れるのは GameObject (任意)、Script 派生 (その型またはその基底型のスクリプトを持つ GameObject)、登録コンポーネント (そのコンポーネントを持つ GameObject) のいずれか。
#define FBZZ_REF(Type, Name, Display)                                           \
    ::fbzz::scene::Ref<Type> Name { this };                                     \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.RefField(FBZZ_DISP_(Display, Name), Name.ref,                        \
            ::fbzz::scene::RefTypeNameOf<Type>(#Type));                         \
    })

/// @brief 未設定・破棄済み・型不一致なら開始を止める型付き参照。
#define FBZZ_REQUIRED_REF(Type, Name, Display)                                   \
    ::fbzz::scene::Ref<Type> Name { this };                                     \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.RequireReference(FBZZ_DISP_(Display, Name), Name.ref,               \
            ::fbzz::scene::RefTypeNameOf<Type>(#Type));                         \
        r_.RefField(FBZZ_DISP_(Display, Name), Name.ref,                        \
            ::fbzz::scene::RefTypeNameOf<Type>(#Type));                         \
        r_.EndField();                                                        \
    })

/// @brief 型安全オブジェクト参照の可変長リスト。ウェイポイント列・砲塔の候補ターゲット・スポーン地点の集合など「同じ型の参照を N 個並べる」用途に使う。
/// @note EntityRef 列へ詰め替えてから渡す。シリアライズの実体は EntityID だけで足り、リフレクタ実装 3 種が `Ref<T>` を知る必要はない。
/// @note 詰め替え後は owner を貼り直すこと。増えた要素の `Ref<T>` は owner=nullptr のままだと解決できない。
#define FBZZ_REF_LIST_FIELD(Type, Name, Display)                                \
    ::std::vector<::fbzz::scene::Ref<Type>> Name;                               \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        ::std::vector<::fbzz::scene::EntityRef> _fbzz_refIds;                   \
        _fbzz_refIds.reserve(Name.size());                                      \
        for (const auto& _fbzz_item : Name)                                     \
            _fbzz_refIds.push_back(_fbzz_item.ref);                             \
        r_.RefListField(FBZZ_DISP_(Display, Name), _fbzz_refIds,                \
            ::fbzz::scene::RefTypeNameOf<Type>(#Type));                         \
        Name.resize(_fbzz_refIds.size());                                       \
        for (::std::size_t _fbzz_i = 0; _fbzz_i < _fbzz_refIds.size(); ++_fbzz_i) { \
            Name[_fbzz_i].ref   = _fbzz_refIds[_fbzz_i];                        \
            Name[_fbzz_i].owner = this;                                         \
        }                                                                       \
    })

/// @brief Inspector グループ見出し。順序保持のためタグを 1 つ消費する。
#define FBZZ_GROUP(Label)     FBZZ_GROUP_(Label, __LINE__)
#define FBZZ_GROUP_(Label, L) FBZZ_GROUP__(Label, L)
#define FBZZ_GROUP__(Label, L)                                                  \
    enum { _fbzz_grp_##L = __COUNTER__ - _fbzz_base };                          \
    void _fbzz_reflect(::fbzz::scene::detail::ReflectTag<_fbzz_grp_##L>,        \
                       ::fbzz::scene::IReflector& r_) {                         \
        _fbzz_reflect(::fbzz::scene::detail::ReflectTag<(_fbzz_grp_##L - 1)>{}, r_); \
        r_.Group(Label);                                                        \
    }

#define FBZZ_SPACE(Height)     FBZZ_SPACE_(Height, __LINE__)
#define FBZZ_SPACE_(Height, L) FBZZ_SPACE__(Height, L)
#define FBZZ_SPACE__(Height, L)                                                 \
    enum { _fbzz_space_##L = __COUNTER__ - _fbzz_base };                        \
    void _fbzz_reflect(::fbzz::scene::detail::ReflectTag<_fbzz_space_##L>,      \
                       ::fbzz::scene::IReflector& r_) {                         \
        _fbzz_reflect(::fbzz::scene::detail::ReflectTag<(_fbzz_space_##L - 1)>{}, r_); \
        r_.Space(static_cast<float>(Height));                                   \
    }

/// @brief 直前のフィールドへ Inspector ツールチップを付ける (設計意図コメントを UI に出す用)。
/// @note 対象フィールドの「次の行」に置く。順序保持のためタグを 1 つ消費する (FBZZ_GROUP と同方式)。
#define FBZZ_TOOLTIP(Text)     FBZZ_TOOLTIP_(Text, __LINE__)
#define FBZZ_TOOLTIP_(Text, L) FBZZ_TOOLTIP__(Text, L)
#define FBZZ_TOOLTIP__(Text, L)                                                 \
    enum { _fbzz_tip_##L = __COUNTER__ - _fbzz_base };                          \
    void _fbzz_reflect(::fbzz::scene::detail::ReflectTag<_fbzz_tip_##L>,        \
                       ::fbzz::scene::IReflector& r_) {                         \
        _fbzz_reflect(::fbzz::scene::detail::ReflectTag<(_fbzz_tip_##L - 1)>{}, r_); \
        r_.Tooltip(Text);                                                       \
    }

/// @brief クラス直後 (同 namespace 内) に置き、Reflect() 本体を生成する。
/// @note 最後に採番されたタグから連鎖を起動することで、全フィールドを宣言順に反映する。
#define FBZZ_REFLECT(T)                                                         \
    inline void T::Reflect(::fbzz::scene::IReflector& r_) {                     \
        _fbzz_reflect(::fbzz::scene::detail::ReflectTag<                        \
                          (__COUNTER__ - T::_fbzz_base - 1)>{}, r_);            \
    }

#define FBZZ_SERIALIZABLE_CONCAT_INNER_(A, B) A##B
#define FBZZ_SERIALIZABLE_CONCAT_(A, B) FBZZ_SERIALIZABLE_CONCAT_INNER_(A, B)
#define FBZZ_REGISTER_SERIALIZABLE(T) FBZZ_REGISTER_SERIALIZABLE_(T, __COUNTER__)
#define FBZZ_REGISTER_SERIALIZABLE_(T, N)                                       \
    namespace {                                                                 \
        [[maybe_unused]] const bool FBZZ_SERIALIZABLE_CONCAT_(                   \
            s_fbzzSerializableRegistered_, N) =                                 \
            ::fbzz::scene::ScriptSerializableFactory::Register(                 \
                T::TYPE_NAME, []() { return std::make_unique<T>(); });          \
    }


} /// @note namespace fbzz::scene
