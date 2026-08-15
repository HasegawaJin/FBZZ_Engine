// FBZZ Engine
// Script.hpp | fbzz::scene
// ユーザースクリプト基底クラスとリフレクション補助
// GameObject にアタッチされる振る舞いの共通ライフサイクルを定義する。
// engine 側の Component とは分け、ScriptComponent が所有する。
#pragma once

#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/PrefabRef.hpp>
#include <Engine/Scene/DataAssetRef.hpp> // DataAsset (純共有 ScriptableObject) 参照スロット
#include <Engine/Scene/ScriptAssetRef.hpp>
#include <Engine/Scene/Reflection.hpp>  // 自己登録リフレクション基盤 (ReflectTag / DisplayOr)
#include <Engine/Scene/Ref.hpp>          // 型安全オブジェクト参照ハンドル Ref<T>
// 全プロキシヘッダーのアンブレラインクルード。新プロキシ追加時はこちらを編集すること。
#include <Engine/Scene/ScriptProxy/AllScriptProxies.hpp>
#include <Engine/Scene/Coroutine.hpp>
#include <Engine/Input/KeyCode.hpp>
// WHY ここで include するか: ユーザースクリプトは Script.hpp しか include しない前提のため、
//     input.GetPadButton(GamepadButton::A) を書くのに必要な列挙をここで供給する。
//     GamepadButton.hpp は Windows.h に依存しない軽量ヘッダなのでコストは小さい。
#include <Engine/Input/GamepadButton.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
// WHY: Script.hpp は全ユーザースクリプトの共通プレリュードを兼ねる。ゲームスクリプトが
//      ほぼ必ず使う標準ヘッダー (clamp/min/max・数学関数・文字列・コンテナ) をここへ集約し、
//      各スクリプトが <algorithm> 等を個別に並べる定型を不要にする。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fbzz::scene {

struct ColliderComponent;
struct Transform;
class GameObject;
class Scene;
struct IReflector;

// Script内のネスト値型が同じReflect経路へ参加するための最小interface。
struct IScriptSerializable {
    virtual ~IScriptSerializable() = default;
    virtual void Reflect(IReflector& reflector) = 0;
};

class ScriptSerializableFactory {
public:
    using Factory = std::function<std::unique_ptr<IScriptSerializable>()>;

    static bool Register(std::string_view typeName, Factory factory);
    static std::unique_ptr<IScriptSerializable> Create(std::string_view typeName);
    static std::vector<std::string> RegisteredTypeNames();
    static void UnregisterAll();
};

struct ScriptSerializedReference {
    std::string type;
    std::unique_ptr<IScriptSerializable> value;
    std::string preservedFieldsToml;

    bool SetType(std::string_view typeName);
    void Clear();
};

} // namespace fbzz::scene

namespace fbzz::physics {
class World;
}

namespace fbzz::renderer {
struct ShaderDescriptor;
struct PostProcessSettings;
}

namespace fbzz::scene {
struct RenderPassContext;
struct UserRenderPassDesc;
class RenderPipeline;

struct CollisionInfo {
    GameObject* self = nullptr;
    GameObject* other = nullptr;
    const ColliderComponent* selfCollider = nullptr;
    const ColliderComponent* otherCollider = nullptr;
    math::Vector3 contactNormal = math::Vector3::UP;
    math::Vector3 contactPoint = math::Vector3::ZERO;
    float contactDepth = 0.0f;
};

// ── IReflector ────────────────────────────────────────────────────────────────
// Script::Reflect() に渡されるビジターインターフェース。
// Inspector が ImGui を介してフィールドを表示・編集し、
// SceneSerializer が JSON にシリアライズ/デシリアライズする際にこれを実装する。
struct IReflector {
    enum class FieldHint {
        Default,
        Multiline,
        Color,
        Angle,
        LayerMask,
        Tag,
        File,
    };

    virtual ~IReflector() = default;

    // Reflect宣言の永続キーとInspector表示名を分離するため、各Field直前に呼ばれる。
    // WHY: 表示名を変更してもScene / Prefabの保存キーが変わらないようにする。
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
        m_fieldStep = 0.0f;
        m_fileExtensions.clear();
        m_fixedList = false;
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
    void SetFieldStep(float step) { m_fieldStep = step; }
    void SetFileExtensions(std::string_view extensions)
    {
        m_fileExtensions = extensions;
        m_fieldHint = FieldHint::File;
    }
    void SetFixedList(bool fixed) { m_fixedList = fixed; }

    [[nodiscard]] bool FieldVisible() const { return m_fieldVisible && !m_fieldHidden; }
    [[nodiscard]] bool FieldEnabled() const { return m_fieldEnabled && !m_fieldReadOnly; }
    [[nodiscard]] bool FieldReadOnly() const { return m_fieldReadOnly; }
    [[nodiscard]] FieldHint CurrentFieldHint() const { return m_fieldHint; }
    [[nodiscard]] float FieldMin() const { return m_fieldMin; }
    [[nodiscard]] bool HasFieldMin() const { return m_hasFieldMin; }
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

    // 参照型 (デフォルト実装あり — 非対応 Reflector はフォールバックする)
    virtual void Field(const char* name, EntityID& v) {}
    virtual void Field(const char* name, EntityRef& v)  { Field(name, v.id); }
    virtual void Field(const char* name, PrefabRef& v)  { Field(name, v.path); }
    // DataAsset (純共有 ScriptableObject) 参照。既定は path 文字列をそのまま保存/復元するため、
    // TOML リフレクタは何も変更不要。Inspector の ImGuiReflector だけがアセットスロット UI を上書きする。
    virtual void Field(const char* name, DataAssetRef& v) { Field(name, v.path); }
    virtual void Field(const char* name, input::KeyCode& v) {}  // キー名ドロップダウン
    // Script用型付きAsset参照。SerializerはGUIDとpath、Inspectorは型フィルター付きslotを扱う。
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
    // 型付きオブジェクト参照のリスト (FBZZ_REF_LIST_FIELD 用)。
    // 既定は型情報を落として通常のリストとして扱うため、TOML 側は無変更で済む。
    // Inspector だけが typeName を使ってドロップ可否を判定する。
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
        // BeginObject / EndObject へ委譲する。これにより BeginObject を実装した
        // リフレクタは ObjectField の入れ子化も自動的に手に入る。
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

    // 付加情報付き (デフォルトは Field へフォールバック)
    virtual void FloatRange(const char* name, float& v, float min, float max)  { Field(name, v); }
    virtual void IntRange(const char* name, int& v, int min, int max)          { Field(name, v); }
    virtual void Enum(const char* name, int& v, std::span<const char* const> labels) { Field(name, v); }
    virtual void Flags(const char* name, int& v, std::span<const char* const> labels) { Enum(name, v, labels); }
    // 型付きオブジェクト参照スロット。typeName が非空ならその Script 型を持つ GameObject だけを
    // 受け付ける (Inspector のドロップ型チェック用)。既定はシリアライズと同じく EntityID を保存する。
    virtual void RefField(const char* name, EntityRef& v, const char* typeName) { Field(name, v.id); }
    // 直前に描画したフィールドへ説明ツールチップを付ける (Inspector のみ表示、シリアライズ非対象)。
    virtual void Tooltip(const char* text) {}
    virtual void Group(const char* label) {}
    virtual void Space(float height) { (void)height; }
    virtual void Readonly(const char* name, const std::string& v) {}
    virtual void Readonly(const char* name, float v)  {}
    virtual void Readonly(const char* name, int v)    {}

    // ── 入れ子オブジェクト / 構造体配列 ─────────────────────────────────────
    // WHY 新しい仮想関数を必ずクラス末尾へ追記するか:
    //     スクリプト DLL の Reflect() は vtable インデックスで仮想呼び出しする。
    //     途中に挿入すると既存関数のインデックスまでずれ、ABI チェックを
    //     すり抜けた場合の被害が大きくなる。追記した際は ScriptDllAbi.hpp の
    //     kReflectionAbiVersion を必ずインクリメントすること。

    // BeginObject / EndObject で挟んだ範囲を 1 つの入れ子オブジェクトとして扱う。
    // 既定実装は何もしない = 従来どおり親と同じ階層へフラット展開される。
    // これにより未対応のリフレクタでも挙動が変わらない (後方互換)。
    //
    // WHY ObjectField ではなくスコープ対を用意するか:
    //     ObjectField は入れ子の型が IScriptSerializable を継承していることを要求する。
    //     しかし反映したい構造体 (renderer::BloomSettings 等) はレンダラー層に住み、
    //     Scene 層のインターフェースを継承させると RenderSettings.hpp が
    //     Script.hpp を include することになり、依存方向が逆流する。
    //     スコープ対なら継承を要求せず、自由関数のヘルパーで任意の構造体を反映できる。
    virtual void BeginObject(const char* name) { (void)name; }
    virtual void EndObject() {}

    // 構造体の配列。現在の要素数を渡し、リフレクタが決めた新しい要素数を返す。
    //   - 読み込みリフレクタ: 保存されていた要素数を返す
    //   - Inspector:          ユーザーが Add / Remove した後の要素数を返す
    //   - 書き込みリフレクタ: 受け取った値をそのまま返す
    //
    // 呼び出し側は戻り値で vector を resize してから、要素ごとに
    // BeginObjectElement / EndObjectElement で挟んで反映する。
    //
    // WHY 戻り値で要素数を返す形にするか:
    //     読み込み・UI 編集・書き込みの 3 方向すべてで要素数の変更が起こりうる。
    //     コールバックを渡す設計にすると DLL 境界を越える std::function が増え、
    //     ScriptDllAbi の互換管理が複雑になる。戻り値なら vtable への追加で済む。
    [[nodiscard]] virtual std::size_t BeginObjectList(const char* name, std::size_t count)
    {
        (void)name;
        return count;
    }
    virtual void BeginObjectElement(std::size_t index) { (void)index; }
    virtual void EndObjectElement() {}

    // 削除要求のインデックスを返す。要素数未満なら呼び出し側がその要素を erase する。
    // 削除要求が無い場合は NO_REMOVE を返す。
    //
    // WHY 戻り値で削除を伝えるか: 配列の実体を所有しているのは呼び出し側であり、
    //     リフレクタは触れない。要素数の増加は BeginObjectList の戻り値 + resize で
    //     表現できるが、「途中の要素を消す」は resize では表現できない
    //     (resize は必ず末尾を落とすため、消したい要素と実際に消える要素がずれる)。
    //     削除だけは別の経路で伝える必要がある。
    static constexpr std::size_t NO_REMOVE = static_cast<std::size_t>(-1);
    [[nodiscard]] virtual std::size_t EndObjectList() { return NO_REMOVE; }

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
    float m_fieldStep = 0.0f;
    std::string m_fileExtensions;
    bool m_fixedList = false;
};

// AnimationClip の Event Track から Script へ渡す DLL 安全な値型。
struct AnimationEventInfo {
    const char* name = "";
    int32_t intParam = 0;
    float floatParam = 0.0f;
    float clipTime = 0.0f;
};

// OnAnimatorMove へ渡すルートモーション 1 フレーム分の移動量。
//
// WHY: 以前は Script が ScriptAnimatorProxy 経由で「前フレームの値」をポーリングするしか
//      なかった。AnimatorSystem は Phase::LateUpdate に居るため、Phase::Script の OnUpdate は
//      常に 1 フレーム遅れた delta を読むことになる。抽出直後に同期コールバックを飛ばすことで、
//      移動の権威を Script / CharacterController 側へ渡せるようにする。
struct RootMotionInfo {
    // Animator 所有 GameObject のローカル空間 (親回転を掛ける前) での移動量。
    math::Vector3    deltaPosition = math::Vector3::ZERO;
    math::Quaternion deltaRotation = math::Quaternion::Identity();
    // deltaPosition をワールド空間へ変換した値。速度制御へそのまま渡せる。
    math::Vector3    worldDeltaPosition = math::Vector3::ZERO;
    // worldDeltaPosition / deltaTime。dt が 0 のフレームではゼロ。
    math::Vector3    worldVelocity = math::Vector3::ZERO;
    // この delta を生成したフレーム時間。Script 側で Time::deltaTime を使うと
    // Animator が実際に進めた時間とずれることがあるため、明示的に渡す。
    float            deltaTime = 0.0f;
    // エンジンが既に Transform / RigidBody へ適用済みなら true。
    // ExtractOnly のときだけ false になり、移動の適用は Script の責任になる。
    bool             appliedByEngine = false;
};

// FBZZ_REF(T, ...) が RefField へ渡す型名を解決する。
// GameObject 参照は「任意の GameObject 可」を意味する空文字、Script 派生参照は T::TYPE_NAME を返す。
// WHY: Inspector はこの型名でドロップを検証し、フィルタ付きピッカーを出す (型不一致アサインを防ぐ)。
template<typename T>
constexpr const char* RefTypeNameOf()
{
    if constexpr (std::is_same_v<T, GameObject>)
        return "";
    else
        return T::TYPE_NAME;
}

// ── InvokeHandle ─────────────────────────────────────────────────────────────
// Invoke / InvokeRepeating が返す軽量値型。CancelInvoke(handle) で個別キャンセル。
struct InvokeHandle {
    uint32_t id = 0;
    bool IsValid() const { return id != 0; }
};

// ── FBZZ リフレクションマクロ (自己登録方式) ──────────────────────────────────
//
// 設計 (WHY):
//   FBZZ_FIELD 等は「メンバー宣言」と「Reflect() への登録」を同じ 1 行で行う。
//   __COUNTER__ で採番したタグ型 detail::ReflectTag<N> のオーバーロードを宣言順に
//   連鎖させ、外部ツール (旧 FHT) も .generated.hpp も使わずに Reflect() を生成する。
//   フィールドを 1 行足すだけで Inspector / シリアライズが自動追従する (単一の真実)。
//
//   表示名は "" を渡すと変数名から自動生成される (DisplayOr)。冗長な再掲が不要。
//
// 使い方:
//   class Foo : public Script {
//       FBZZ_SCRIPT(Foo)                       // クラス先頭
//   public:
//       FBZZ_FIELD(float, speed, 1.0f, "")     // 表示名は "Speed" に自動生成
//       FBZZ_REF(GameObject, target, "Target") // ドラッグ&ドロップ参照
//       void OnUpdate() override;
//   };
//   FBZZ_REFLECT(Foo)                          // クラス直後・同 namespace 内に置く
//
// 役割分担: FBZZ_SCRIPT が Reflect() を「宣言」し、FBZZ_REFLECT が「定義」する。
//          外部コード生成ツール (旧 FHT) と .generated.hpp は全廃済み。

#define FBZZ_SCRIPT(T)                                                          \
    public:                                                                     \
    using FbzzSelf = T;                                                         \
    static constexpr const char* TYPE_NAME = #T;                               \
    const char* GetTypeName() const override { return TYPE_NAME; }             \
    void Reflect(::fbzz::scene::IReflector& r_) override;                       \
    static constexpr int _fbzz_base = __COUNTER__;                              \
    void _fbzz_reflect(::fbzz::scene::detail::ReflectTag<0>,                    \
                       ::fbzz::scene::IReflector&) {}

// Script以外のネスト値型へ同じ宣言式Reflectを与える。
#define FBZZ_SERIALIZABLE(T)                                                    \
    public:                                                                     \
    using FbzzSelf = T;                                                         \
    static constexpr const char* TYPE_NAME = #T;                               \
    void Reflect(::fbzz::scene::IReflector& r_) override;                       \
    static constexpr int _fbzz_base = __COUNTER__;                              \
    void _fbzz_reflect(::fbzz::scene::detail::ReflectTag<0>,                    \
                       ::fbzz::scene::IReflector&) {}

// 1 エントリ分の登録。直前タグ (= 1 つ前のフィールド/グループ) を先に処理してから
// 自分を反映することで宣言順を保つ。UniqueTok はメンバー名や行番号で一意化する。
// WHY (可変長): リフレクション文に含まれるトップレベルのカンマ (FBZZ_FIELD_ENUM の
//              ラベル配列など、() で保護されないもの) を __VA_ARGS__ で吸収するため。
#define FBZZ_REFLECT_ENTRY_(UniqueTok, ...)                                     \
    enum { _fbzz_idx_##UniqueTok = __COUNTER__ - _fbzz_base };                  \
    void _fbzz_reflect(                                                         \
        ::fbzz::scene::detail::ReflectTag<_fbzz_idx_##UniqueTok>,               \
        ::fbzz::scene::IReflector& r_) {                                        \
        _fbzz_reflect(::fbzz::scene::detail::ReflectTag<                        \
                          (_fbzz_idx_##UniqueTok - 1)>{}, r_);                  \
        __VA_ARGS__;                                                            \
    }

// 表示名解決 (空なら変数名から自動生成)。const char* を期待する Field 等へ渡す。
#define FBZZ_DISP_(Display, Name)                                               \
    ::fbzz::scene::detail::DisplayOr(Display, #Name).c_str()

#define FBZZ_FIELD(Type, Name, Default, Display)                                \
    Type Name = Default;                                                        \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.Field(FBZZ_DISP_(Display, Name), Name);                              \
    })

// 旧保存キーを読み込み、新しいメンバー名で保存し直すフィールド。
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

// 構造体の配列。要素型は IScriptSerializable を実装していること。
// 要素数の増減と途中要素の削除をリフレクタから受け取り、vector へ反映する。
//
// WHY 削除を戻り値で受けるか: resize は必ず末尾を落とすため、
//     「途中の要素を消す」を要素数の変更だけでは表現できない。
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

// int 用レンジフィールド (スライダー)。FBZZ_FIELD_RANGE の int 版。
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

// 参照型のデフォルト値省略版 — PrefabRef / EntityRef 等のデフォルトが {} のフィールドに使う。
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

// GUID付き型安全Asset参照フィールド。
// TypeにはMaterialRef / TextureRef / SpriteRef / VFXRef等を指定する。
#define FBZZ_ASSET_FIELD(Type, Name, Display)                                   \
    Type Name = {};                                                             \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.AssetField(FBZZ_DISP_(Display, Name), Name.reference,                \
            Type::ASSET_TYPE);                                                  \
    })

// Inspector 表示のみ・Serializer 非保存の計算値ラベル。
#define FBZZ_COMPUTED(Type, Name, Display)                                      \
    Type Name = {};                                                            \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.Readonly(FBZZ_DISP_(Display, Name), Name);                           \
    })
// 型安全オブジェクト参照フィールド。Inspector にドラッグ&ドロップスロットを出す。
// Ref<T> は { this } で所有 Script を受け取り、Name.Get() / if (Name) で解決する。
// シリアライズは内包する EntityRef (= EntityID) を対象にする。
#define FBZZ_REF(Type, Name, Display)                                           \
    ::fbzz::scene::Ref<Type> Name { this };                                     \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.RefField(FBZZ_DISP_(Display, Name), Name.ref,                        \
            ::fbzz::scene::RefTypeNameOf<Type>());                              \
    })

// 型安全オブジェクト参照の可変長リスト。ウェイポイント列・砲塔の候補ターゲット・
// スポーン地点の集合など「同じ型の参照を N 個並べる」用途に使う。
//
// WHY std::vector<Ref<T>> を直接リフレクタへ渡さないか:
//   シリアライズの実体は EntityRef (= EntityID) だけで足り、リフレクタ実装 3 種
//   (Inspector / TOML 読み / TOML 書き) が Ref<T> というテンプレートを知る必要はない。
//   ここで EntityRef 列へ詰め替えることで、リフレクタ側は 1 つの非テンプレート
//   オーバーロードだけを実装すればよくなる。
//   詰め替え後は owner を貼り直す — 要素が増えたときの Ref<T> は既定構築 (owner=nullptr)
//   で、そのままでは Get() が解決できないため。
#define FBZZ_REF_LIST_FIELD(Type, Name, Display)                                \
    ::std::vector<::fbzz::scene::Ref<Type>> Name;                               \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        ::std::vector<::fbzz::scene::EntityRef> _fbzz_refIds;                   \
        _fbzz_refIds.reserve(Name.size());                                      \
        for (const auto& _fbzz_item : Name)                                     \
            _fbzz_refIds.push_back(_fbzz_item.ref);                             \
        r_.RefListField(FBZZ_DISP_(Display, Name), _fbzz_refIds,                \
            ::fbzz::scene::RefTypeNameOf<Type>());                              \
        Name.resize(_fbzz_refIds.size());                                       \
        for (::std::size_t _fbzz_i = 0; _fbzz_i < _fbzz_refIds.size(); ++_fbzz_i) { \
            Name[_fbzz_i].ref   = _fbzz_refIds[_fbzz_i];                        \
            Name[_fbzz_i].owner = this;                                         \
        }                                                                       \
    })

// Inspector グループ見出し。順序保持のためタグを 1 つ消費する。
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

// 直前のフィールドへ Inspector ツールチップを付ける (設計意図コメントを UI に出す用)。
// 対象フィールドの「次の行」に置く。順序保持のためタグを 1 つ消費する (FBZZ_GROUP と同方式)。
//   FBZZ_FIELD_RANGE(float, reach, 1.1f, "Reach", 0, 5)
//   FBZZ_TOOLTIP("キャラ原点から前方への判定球オフセット")
#define FBZZ_TOOLTIP(Text)     FBZZ_TOOLTIP_(Text, __LINE__)
#define FBZZ_TOOLTIP_(Text, L) FBZZ_TOOLTIP__(Text, L)
#define FBZZ_TOOLTIP__(Text, L)                                                 \
    enum { _fbzz_tip_##L = __COUNTER__ - _fbzz_base };                          \
    void _fbzz_reflect(::fbzz::scene::detail::ReflectTag<_fbzz_tip_##L>,        \
                       ::fbzz::scene::IReflector& r_) {                         \
        _fbzz_reflect(::fbzz::scene::detail::ReflectTag<(_fbzz_tip_##L - 1)>{}, r_); \
        r_.Tooltip(Text);                                                       \
    }

// クラス直後 (同 namespace 内) に置き、Reflect() 本体を生成する。
// 最後に採番されたタグから連鎖を起動することで、全フィールドを宣言順に反映する。
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

// ── Script 基底クラス ─────────────────────────────────────────────────────────
class Script {
public:
    virtual ~Script();

    template<typename T>
    T* GetComponent() const;

    virtual void OnAwake() {}
    virtual void OnStart() {}
    virtual void OnEnable() {}
    virtual void OnDisable() {}
    virtual void OnUpdate() {}
    virtual void OnLateUpdate() {}
    virtual void OnDestroy() {}
    virtual void OnDrawGizmos() {}
    virtual void OnPreRender()  {}
    virtual void OnPostRender() {}
    virtual void OnSetupRenderPasses(RenderPipeline&, RenderPassContext&) {}
    virtual void OnCollisionEnter(const CollisionInfo&) {}
    virtual void OnCollisionStay(const CollisionInfo&) {}
    virtual void OnCollisionExit(const CollisionInfo&) {}
    virtual void OnTriggerEnter(const CollisionInfo&) {}
    virtual void OnTriggerStay(const CollisionInfo&) {}
    virtual void OnTriggerExit(const CollisionInfo&) {}
    virtual void OnNavMeshDestinationReached() {} // NavMeshAgentComponent が目的地に到達したとき
    virtual void OnNavMeshPathFailed() {}          // 目的地までのパスが見つからなかったとき
    virtual void OnNavMeshTargetSpotted() {}       // NavMeshSensorComponent が対象を視界内に検知したとき
    virtual void OnNavMeshTargetLost() {}          // 検知していた対象を見失ったとき
    virtual void OnAnimationEvent(const AnimationEventInfo&) {}
    // AnimatorComponent がルートモーションを抽出した直後に、同じフレーム内で呼ばれる。
    // rootMotion.mode が None 以外なら毎フレーム発火する (delta がゼロでも呼ばれる)。
    // ExtractOnly のときはエンジンが何も動かさないため、ここで移動を適用する。
    virtual void OnAnimatorMove(const RootMotionInfo&) {}
    // Editor authoring / serialization lifecycle。
    virtual void Reset() {}
    virtual void OnValidate() {}
    virtual void OnBeforeSerialize() {}
    virtual void OnAfterDeserialize() {}
    virtual void Reflect(IReflector&) {}
    virtual const char* GetTypeName() const { return "Script"; }

    // ── 固定ステップ更新 ────────────────────────────────────────────────────
    // Phase::Physics と同じ固定タイムステップ (既定 60Hz) で、物理の直前に呼ばれる。
    // フレームレートに関わらず 1 秒あたりの呼び出し回数が一定なので、力の加算や
    // 移動量の積分はここへ置くと PC 性能で挙動が変わらない。
    // 経過時間は time.DeltaTime() ではなく time.FixedDeltaTime() を使うこと。
    //
    // WHY OnUpdate と分けるか: OnUpdate は描画フレームごと (可変 dt) に 1 回で、
    //     入力の取りこぼしを避けたい処理や見た目の更新に向く。両者は呼ばれる回数が
    //     違うため、物理に効く処理を OnUpdate に書くと重い/軽い環境で結果がずれる。
    virtual void OnFixedUpdate() {}

    // ── オブジェクトプール ──────────────────────────────────────────────────
    // scene.Spawn() で貸し出された / scene.Despawn() で返却された瞬間に呼ばれる。
    // 再利用インスタンスでは OnAwake / OnStart は再発火しないため、
    // 「毎回リセットしたい状態」(HP・経過時間・軌跡バッファ) はここで初期化する。
    virtual void OnSpawn() {}
    virtual void OnDespawn() {}

    bool enabled = true;

    // Proxy — カテゴリごとに責務を分け、Script.hpp の肥大化を避ける。
    // 新プロキシを追加する際は ScriptProxyMembers.inl を編集すること (このファイルは触らなくてよい)。
#define FBZZ_PROXY_MEMBER(Type, Name)     Type Name { this };
#define FBZZ_PROXY_STANDALONE(Type, Name) Type Name;
#include <Engine/Scene/ScriptProxy/ScriptProxyMembers.inl>
#undef FBZZ_PROXY_MEMBER
    #undef FBZZ_PROXY_STANDALONE


    // Invoke / タイマー
    [[nodiscard]] InvokeHandle Invoke(std::function<void()> fn, float delay);
    [[nodiscard]] InvokeHandle InvokeRepeating(std::function<void()> fn, float delay, float interval);
    void FrameDelay(uint32_t n, std::function<void()> fn);
    void CancelInvoke();
    void CancelInvoke(InvokeHandle handle);
    void CancelEventSubscriptions();

    // コルーチン (Coroutine.hpp)。WaitForSeconds 等を co_await して時間軸処理を直線的に書く。
    // 注意: コルーチン内から StopAllCoroutines() を呼ばないこと (実行中ハンドルの自己破棄になる)。
    void StartCoroutine(Coroutine co);
    void StopAllCoroutines();

    // QueueRenderPass / GetShaderDescriptor
    void QueueRenderPass(UserRenderPassDesc desc) const;
    const renderer::ShaderDescriptor* GetShaderDescriptor(std::string_view shaderPath) const;

    // ScriptSystem 専用
    void SetContext(Scene* scene, GameObject* gameObject);
    void SyncEnabledState();
    void TickInvokes(float dt);
    void TickFrameDelays();
    void TickCoroutines();
    static void SetPhysicsWorld(physics::World* world);

    using PrefabInstantiateFn = std::function<bool(Scene&, const std::string&, std::vector<EntityID>&)>;
    static void SetInstantiateFn(PrefabInstantiateFn fn);
    static bool InvokePrefabInstantiate(Scene& scene, const std::string& path, std::vector<EntityID>& roots);

protected:
    renderer::PostProcessSettings& GetRuntimePostProcessSettings();
    const renderer::PostProcessSettings* TryGetRuntimePostProcessSettings() const;
    void SetRuntimePostProcessSettings(const renderer::PostProcessSettings& settings);
    void ClearRuntimePostProcessSettings();

    Scene*      m_scene      = nullptr;
    GameObject* m_gameObject = nullptr;

private:
    friend struct ScriptTransformProxy;
    friend struct ScriptInputProxy;
    friend struct ScriptCursorProxy;
    friend struct ScriptApplicationProxy;
    friend struct ScriptTimeProxy;
    friend struct ScriptPhysicsProxy;
    friend struct ScriptColliderProxy;
    friend struct ScriptAudioProxy;
    friend struct ScriptLightProxy;
    friend struct ScriptCameraProxy;
    friend struct ScriptMaterialProxy;
    friend class MaterialInstance;
    friend struct ScriptParticleProxy;
    friend struct ScriptParticleForceFieldProxy;
    friend struct ScriptCloudProxy;
    friend struct ScriptSunMoonProxy;
    friend struct ScriptTerrainDetailProxy;
    friend struct ScriptPatrolProxy;
    friend struct ScriptWindProxy;
    friend struct ScriptTrailProxy;
    friend struct ScriptMeshTrailProxy;
    friend struct ScriptSceneProxy;
    friend struct ScriptAnimatorProxy;
    friend struct ScriptDebugProxy;
    friend struct ScriptPostProcessProxy;
    friend struct ScriptUIProxy;
    friend struct ScriptUIAnimatorProxy;
    friend struct ScriptNavigationProxy;
    friend struct ScriptCharacterProxy;
    friend struct ScriptMeshProxy;
    friend struct ScriptIKProxy;
    friend struct ScriptWaterProxy;
    friend struct ScriptTerrainProxy;
    friend struct ScriptFoliageProxy;
    friend struct ScriptEnvironmentProxy;
    friend struct ScriptDecalProxy;
    friend struct ScriptVolumeProxy;
    friend struct ScriptReflectionProbeProxy;
    friend struct ScriptLifetimeProxy;
    friend struct ScriptSaveProxy;
    friend struct ScriptEventProxy;
    friend struct ScriptRandomProxy;
    friend struct ScriptTweenProxy;

    struct InvokeEntry {
        std::function<void()> fn;
        float remaining = 0.0f;
        float interval  = 0.0f;
        bool repeating  = false;
        bool canceled   = false;
        uint32_t id     = 0;
    };

    struct FrameDelayEntry {
        std::function<void()> fn;
        uint32_t remainingFrames = 0;
        bool canceled = false;
    };

    uint32_t m_nextInvokeId = 1;
    std::vector<InvokeEntry>     m_invokes;
    std::vector<FrameDelayEntry> m_frameDelays;
    std::vector<std::function<void(Script*)>> m_eventUnsubscribers;
    // WHY: Tick 中に開始されたコルーチンは m_coroutines を再確保し、実行中ハンドルを移動させて
    //      しまうため、ティック中は m_pendingCoroutines に積み、ループ後に取り込む。
    std::vector<Coroutine> m_coroutines;
    std::vector<Coroutine> m_pendingCoroutines;
    bool m_isTickingCoroutines = false;
    bool m_enableStateInitialized = false;
    bool m_lastEnabled = true;
    bool m_isTickingInvokes = false;
    static physics::World*     s_physicsWorld;
    static PrefabInstantiateFn s_instantiateFn;
};

// ── Ref<T> の実体 ─────────────────────────────────────────────────────────────
// Script / ScriptSceneProxy が完全型になったここで定義する。
// WHY: object() は owner->scene (Script のメンバー) を、Get() は GetScript<T> の
//      テンプレート定義 (Scene.hpp 末尾) を必要とするため、宣言と分けてここへ置く。
template<typename T>
inline GameObject* Ref<T>::object() const
{
    if (!owner) return nullptr;
    return ref.Resolve(owner->scene);  // EntityRef::Resolve(const ScriptSceneProxy&)
}

template<typename T>
inline T* Ref<T>::Get() const
{
    GameObject* go = object();
    if (!go) return nullptr;
    if constexpr (std::is_same_v<T, GameObject>)
        return go;
    else
        // T が Script 派生のとき、その GameObject 上の T スクリプトを取得する。
        return owner->scene.template GetScript<T>(go);
}

} // namespace fbzz::scene
