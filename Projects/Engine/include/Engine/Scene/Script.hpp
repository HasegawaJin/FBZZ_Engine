/// @file    Script.hpp
/// @brief   ユーザースクリプト基底クラスとリフレクション補助。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// @note GameObject にアタッチされる振る舞いの共通ライフサイクルを定義する。
/// @note engine 側の Component とは分け、ScriptComponent が所有する。
#pragma once

#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/PrefabRef.hpp>
/// @note DataAsset (純共有 ScriptableObject) 参照スロット。
#include <Engine/Scene/DataAssetRef.hpp>
#include <Engine/Scene/ScriptAssetRef.hpp>
/// @note 自己登録リフレクション基盤 (ReflectTag / DisplayOr)。
#include <Engine/Scene/Reflection.hpp>
/// @note FBZZ_FIELD_CURVE / FBZZ_FIELD_GRADIENT の値型。
#include <Engine/Scene/ParticleCurve.hpp>
/// @note 型安全オブジェクト参照ハンドル `Ref<T>`。
#include <Engine/Scene/Ref.hpp>
/// @note 全プロキシヘッダーのアンブレラインクルード。新プロキシ追加時はこちらを編集する。
#include <Engine/Scene/ScriptProxy/AllScriptProxies.hpp>
#include <Engine/Scene/Coroutine.hpp>
#include <Engine/Input/KeyCode.hpp>
/// @note ユーザースクリプトは Script.hpp のみ include する前提のため、パッド入力に要る列挙をここで供給する。
/// @note GamepadButton.hpp は Windows.h 非依存の軽量ヘッダなのでコストは小さい。
#include <Engine/Input/GamepadButton.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
/// @note Script.hpp は全ユーザースクリプトの共通プレリュードを兼ねる。ゲームスクリプトがほぼ必ず使う標準ヘッダーをここへ集約し、各スクリプトが個別に include する定型を不要にする。
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

/// @brief Script 内のネスト値型が同じ Reflect 経路へ参加するための最小 interface。
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

} /// @note namespace fbzz::scene

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

    /// @name 衝突の強さ
    /// @note コールバックは速度解決後に呼ばれるため、GetVelocity() は衝突後の値しか返さない。解決前の勢いはここに記録する。
    /// @note OnCollisionEnter でのみ意味を持つ (Stay / Exit では 0)。
    /// @{
    math::Vector3 relativeVelocity = math::Vector3::ZERO; ///< @note 接触点での相対速度 (self から見た other との差、角速度の寄与を含む)。
    float approachSpeed = 0.0f; ///< @note 法線方向の接近速度 [m/s]。正 = 近づいていた強さ。一定速度以上の衝突だけダメージにする判定に使う。
    float impactImpulse = 0.0f; ///< @note 解決で加わった法線インパルス (質量込み)。軽重で手応えを変えたいときは approachSpeed でなくこちらを見る。
    /// @}
};

/// @brief Script::Reflect() に渡されるビジターインターフェース。
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
    virtual void Field(const char* name, EntityRef& v)  { Field(name, v.id); }
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
    /// @note 既定はシリアライズと同じく EntityID を保存する。
    virtual void RefField(const char* name, EntityRef& v, const char* typeName) { Field(name, v.id); }
    /// @brief 直前に描画したフィールドへ説明ツールチップを付ける (Inspector のみ表示、シリアライズ非対象)。
    virtual void Tooltip(const char* text) {}
    virtual void Group(const char* label) {}
    virtual void Space(float height) { (void)height; }
    virtual void Readonly(const char* name, const std::string& v) {}
    virtual void Readonly(const char* name, float v)  {}
    virtual void Readonly(const char* name, int v)    {}
    /// @}

    /// @name 入れ子オブジェクト / 構造体配列
    /// @note 新しい仮想関数は必ずクラス末尾へ追記する。DLL の Reflect() は vtable インデックスで呼ぶため、途中へ挿すと既存関数の番号がずれる。追記したら ScriptDllAbi.hpp の kReflectionAbiVersion を必ずインクリメントする。
    /// @{

    /// @brief BeginObject / EndObject で挟んだ範囲を 1 つの入れ子オブジェクトとして扱う。
    /// @note 既定実装は何もしない (= 親と同じ階層へフラット展開、未対応リフレクタと後方互換)。ObjectField でなくスコープ対なのは継承を要求しないため。ObjectField だと renderer::BloomSettings のような他層の構造体に Scene 層の interface が要り、RenderSettings.hpp が Script.hpp を include して依存方向が逆流する。
    virtual void BeginObject(const char* name) { (void)name; }
    virtual void EndObject() {}

    /// @brief 構造体の配列。現在の要素数を渡し、リフレクタが決めた新しい要素数を返す。
    /// @return 読み込みリフレクタは保存されていた要素数、Inspector は Add / Remove 後の要素数、書き込みリフレクタは受け取った値をそのまま返す。
    /// @note 呼び出し側は戻り値で vector を resize してから、要素ごとに BeginObjectElement / EndObjectElement で挟んで反映する。戻り値で返すのは、コールバックだと DLL 境界を越える std::function が増えるため。
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
};

/// @brief AnimationClip の Event Track から Script へ渡す DLL 安全な値型。
struct AnimationEventInfo {
    const char* name = "";
    int32_t intParam = 0;
    float floatParam = 0.0f;
    float clipTime = 0.0f;
};

/// @brief `.sequence` の EventTrack から Script へ渡す DLL 安全な値型。
/// @note AnimationEventInfo と同じ形にするのは、受け手が覚えることを増やさないため。どちらも時間軸上の点で飛んでくる名前付きの合図で扱いは変わらない。
struct SequenceEventInfo {
    const char* name         = "";
    int32_t     intParam     = 0;
    float       floatParam   = 0.0f;
    float       sequenceTime = 0.0f;
    const char* sequenceName = "";
};

/// @brief binding を持たない EventTrack が ScriptEventBus へ流す合図。
/// @note 「ボス自身に効く合図」はターゲットへ直接届けたいが、「盤面全体に効く合図」の受け手は演出の当事者ではない。binding で縛ると演出のたびに全シーケンスへバインドして回る。
struct SequenceEvent {
    FBZZ_EVENT(SequenceEvent);
    const char* name         = "";
    int32_t     intParam     = 0;
    float       floatParam   = 0.0f;
    float       sequenceTime = 0.0f;
    const char* sequenceName = "";
};

/// @brief OnAnimatorMove へ渡すルートモーション 1 フレーム分の移動量。
/// @note AnimatorSystem は Phase::LateUpdate なので、ポーリングだと Phase::Script は常に 1 フレーム遅れた delta を読む。抽出直後に同期コールバックを飛ばす。
struct RootMotionInfo {
    math::Vector3    deltaPosition = math::Vector3::ZERO; ///< @note Animator 所有 GameObject のローカル空間 (親回転を掛ける前) での移動量。
    math::Quaternion deltaRotation = math::Quaternion::Identity();
    math::Vector3    worldDeltaPosition = math::Vector3::ZERO; ///< @note deltaPosition をワールド空間へ変換した値。速度制御へそのまま渡せる。
    math::Vector3    worldVelocity = math::Vector3::ZERO; ///< @note worldDeltaPosition / deltaTime。dt が 0 のフレームではゼロ。
    float            deltaTime = 0.0f; ///< @note この delta を生成したフレーム時間。Time::deltaTime では Animator が実際に進めた時間とずれるため明示的に渡す。
    bool             appliedByEngine = false; ///< @note エンジンが既に Transform / RigidBody へ適用済みなら true。ExtractOnly のときだけ false になり、移動の適用は Script の責任になる。
};

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

/// @brief Invoke / InvokeRepeating が返す軽量値型。CancelInvoke(handle) で個別キャンセルする。
struct InvokeHandle {
    uint32_t id = 0;
    bool IsValid() const { return id != 0; }
};

/// @brief FBZZ リフレクションマクロ群 (自己登録方式)。
/// @note `__COUNTER__` で採番した `detail::ReflectTag<N>` を宣言順に連鎖させ Reflect() を生成する。外部ツールも `.generated.hpp` も要らない。
/// @note 表示名は "" なら変数名から自動生成 (DisplayOr)。FBZZ_SCRIPT が Reflect() を宣言し、FBZZ_REFLECT が定義する。

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

#define FBZZ_SCRIPT(T)                                                          \
    FBZZ_REFLECT_CORE_(T)                                                       \
    using FbzzBase = ::fbzz::scene::Script;                                     \
    void* FbzzAsType(::std::string_view n_) const override                      \
        { return n_ == TYPE_NAME ? const_cast<T*>(this)                         \
                                 : ::fbzz::scene::Script::FbzzAsType(n_); }     \
    void _fbzz_reflect(::fbzz::scene::detail::ReflectTag<0>,                    \
                       ::fbzz::scene::IReflector&) {}

/// @note `__VA_ARGS__` の先頭だけを取り出す。MSVC の伝統的プリプロセッサでも展開されるよう EXPAND を 1 枚挟む (`/Zc:preprocessor` の有無に依存しないため)。
#define FBZZ_EXPAND_(x) x
#define FBZZ_FIRST_IMPL_(First, ...) First
#define FBZZ_FIRST_(...) FBZZ_EXPAND_(FBZZ_FIRST_IMPL_(__VA_ARGS__, ))

/// @brief 第 2 引数以降に基底を並べてスクリプトを継承する (`FBZZ_SCRIPT_DERIVED(T, Base1, ...)`)。
/// @note Script 派生の鎖は 1 本に保つこと。2 つ以上継承すると Script 部分オブジェクトが 2 個になり、`unique_ptr<Script>` も proxy も m_gameObject も曖昧になる。横断的な能力は Script を継承しないインターフェース (FBZZ_SCRIPT_INTERFACE) で足す。
/// @note 基底の Reflect は tag 0 で呼ぶ。`__COUNTER__ - _fbzz_base` はクラスごとに 1 から並ぶので、終端の tag 0 を差し替えるだけで基底が先・派生が後になる。引き継ぐのは先頭の基底だけ。
/// @note FBZZ_REQUIRE_COMPONENT / FBZZ_OPTIONAL_COMPONENT は仮想オーバーライドなので、派生で再宣言すると基底の宣言を上書きする。基底が要求するぶんも並べること。
#define FBZZ_SCRIPT_DERIVED(T, ...)                                             \
    FBZZ_REFLECT_CORE_(T)                                                       \
    using FbzzBase = FBZZ_FIRST_(__VA_ARGS__);                                  \
    void* FbzzAsType(::std::string_view n_) const override                      \
        { return n_ == TYPE_NAME                                                \
            ? const_cast<T*>(this)                                              \
            : ::fbzz::scene::detail::TryBases<__VA_ARGS__>(this, n_); }         \
    void _fbzz_reflect(::fbzz::scene::detail::ReflectTag<0>,                    \
                       ::fbzz::scene::IReflector& r_)                           \
        { FBZZ_FIRST_(__VA_ARGS__)::Reflect(r_); }

/// @brief 共有基底。中身は FBZZ_SCRIPT_DERIVED と同じだが、ScriptCodeGen が ScriptList.inl へ登録しないため GameObject へ直接アタッチできない。
/// @note 登録簿のファクトリは `make_unique<T>()` を作る。純粋仮想を持つ基底はそこでコンパイルが通らず、通っても「基底そのもの」を Add Component の一覧に出す意味がない。
#define FBZZ_SCRIPT_BASE(T, ...) FBZZ_SCRIPT_DERIVED(T, __VA_ARGS__)

/// @brief Script を継承しない横断インターフェース。能力だけを表す (`FBZZ_SCRIPT_INTERFACE(I)`)。
/// @note 継承させないのは、継承すると Script 部分オブジェクトが複数になり `unique_ptr<Script>` が曖昧になるため (FBZZ_SCRIPT_DERIVED 参照)。能力側が状態を持たなければこの制約は要らない。
/// @note FbzzAsType を純粋仮想にしないのは、TryBases が `Bases::FbzzAsType(n)` を修飾呼び出しで叩くため。純粋仮想を修飾呼び出しすると実体が無くリンクできない。ここで自分の分だけ答える実装を置く。
#define FBZZ_SCRIPT_INTERFACE(I)                                                \
    public:                                                                     \
    static constexpr const char* TYPE_NAME = #I;                               \
    virtual ~I() = default;                                                     \
    virtual void* FbzzAsType(::std::string_view n_) const                       \
        { return n_ == TYPE_NAME ? const_cast<I*>(this) : nullptr; }

namespace detail {

/// @brief T が FbzzAsType で引ける型か。Script 派生と `FBZZ_SCRIPT_INTERFACE` を持つ横断インターフェースの両方で true になる。
/// @note インターフェースは Script を継承しないため `is_base_of<Script, T>` では拾えず、`FindObjectsOfType<T>()` が ECS 側の分岐へ落ちてしまう。
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

/// @note "RigidBodyComponent, AnimatorComponent" → { "RigidBodyComponent", "AnimatorComponent" }
/// @note 名前空間修飾は落とす。短縮名でも fbzz::scene:: 付きでも ComponentRegistry の
/// @note serializedName (= #Type の短縮名) と突き合わせられるよう、最後の "::" より後だけを採る。
inline std::vector<std::string> SplitComponentNames(const char* list)
{
    std::vector<std::string> names;
    if (!list) return names;

    const std::string_view all(list);
    std::size_t begin = 0;
    while (begin <= all.size()) {
        const std::size_t comma = all.find(',', begin);
        std::string_view token =
            all.substr(begin, comma == std::string_view::npos ? all.size() - begin : comma - begin);

        /// @note 前後の空白を落としてから名前空間修飾を剥がす。
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
            token.remove_prefix(1);
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
            token.remove_suffix(1);
        if (const std::size_t scope = token.rfind("::"); scope != std::string_view::npos)
            token.remove_prefix(scope + 2);

        if (!token.empty()) names.emplace_back(token);
        if (comma == std::string_view::npos) break;
        begin = comma + 1;
    }
    return names;
}

/// @brief 要求に並べた型が「完全型か」を静的に確かめるだけの補助。
/// @note 名前は `#__VA_ARGS__` の文字列から採るため型そのものは使われず、綴り違いや include 漏れが実行時まで露見しない。sizeof で完全型を強制し宣言した場所でコンパイルエラーにする。
template<typename... Ts>
struct ComponentCompleteness {
    static constexpr std::size_t value = (sizeof(Ts) + ... + 0);
};

template<typename... Ts>
std::span<const std::string> RequiredScriptNames()
{
    static const std::array<std::string, sizeof...(Ts)> names{Ts::TYPE_NAME...};
    return names;
}

} /// @note namespace detail

/// @brief GameObject に要る/望ましいコンポーネントを宣言する。無いと成立しないものは `FBZZ_REQUIRE_COMPONENT`、無くても縮退動作するものは `FBZZ_OPTIONAL_COMPONENT`。
/// @note `GetComponent<T>()` が null なら黙って早期 return するため、付け忘れはエラーにならず動かない理由がどこにも出ない。宣言しておけば Inspector (赤帯 + Fix ボタン)・Play 開始時 (Console へ一括検証)・ScriptSystem (実行時に一度だけ警告、Standalone でも出る) の 3 箇所が同じ情報で名指しする。
/// @note 文字列でなく型で書かせるのは、綴り違いを文字列だとコンパイルが通ってしまい「宣言したのに検証されない」壊れ方をするため。名前は `#__VA_ARGS__` から採るので型名の綴りがそのまま検証キーになる。
#define FBZZ_REQUIRE_COMPONENT(...)                                             \
    ::std::span<const ::std::string> RequiredComponents() const override {      \
        static_assert(                                                          \
            ::fbzz::scene::detail::ComponentCompleteness<__VA_ARGS__>::value > 0,\
            "FBZZ_REQUIRE_COMPONENT: 未定義の型です (include 漏れ / 綴り違い)"); \
        static const ::std::vector<::std::string> names_ =                      \
            ::fbzz::scene::detail::SplitComponentNames(#__VA_ARGS__);           \
        return names_;                                                          \
    }

/// @brief 同一 GameObject の必須 Script を宣言する。基底・インターフェースも指定できる。
/// @note 不足時は Start と更新を止める。派生で再宣言すると基底の要求を上書きする。
#define FBZZ_REQUIRE_SCRIPT(...)                                                \
    ::std::span<const ::std::string> RequiredScripts() const override {        \
        return ::fbzz::scene::detail::RequiredScriptNames<__VA_ARGS__>();       \
    }

#define FBZZ_OPTIONAL_COMPONENT(...)                                            \
    ::std::span<const ::std::string> OptionalComponents() const override {      \
        static_assert(                                                          \
            ::fbzz::scene::detail::ComponentCompleteness<__VA_ARGS__>::value > 0,\
            "FBZZ_OPTIONAL_COMPONENT: 未定義の型です (include 漏れ / 綴り違い)");\
        static const ::std::vector<::std::string> names_ =                      \
            ::fbzz::scene::detail::SplitComponentNames(#__VA_ARGS__);           \
        return names_;                                                          \
    }

/// @brief Play 中だけでなく編集中もこのスクリプトを実行する (Unity の `[ExecuteAlways]` 相当)。
/// @note 編集中に呼ばれるのは OnAwake/OnStart/OnEnable/OnDisable/OnUpdate/OnLateUpdate/OnDestroy と Invoke・Coroutine まで。OnFixedUpdate と衝突系は呼ばれない (PhysicsSystem が停止しており積分する相手が居ないため)。
/// @note ここでシーンへ書いた値はそのまま保存対象になる。体力・スコア・座標のようなゲーム進行状態を触るスクリプトには向かない。
/// @note Play の開始・停止でライフサイクルは張り直され (OnDisable→OnDestroy→OnAwake→OnStart)、編集中の状態は持ち越さない。編集中は入力・物理・音が動いていないため `app.IsPlaying()` で分岐すること。
#define FBZZ_EXECUTE_ALWAYS()                                                   \
    bool ExecuteInEditMode() const override { return true; }

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

/// @brief オブジェクトの生存期間だけ有効な観測 ID。コピー先には新しい ID を発行する。
struct ScriptInspectionIdentity {
    ScriptInspectionIdentity();
    ScriptInspectionIdentity(const ScriptInspectionIdentity&);
    ScriptInspectionIdentity& operator=(const ScriptInspectionIdentity&) { return *this; }
    std::string value;
};

class Script {
public:
    [[nodiscard]] const std::string& InspectionId() const { return m_inspectionIdentity.value; }
    [[nodiscard]] bool IsRuntimeFaulted() const { return m_runtimeFaulted; }
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
    virtual void OnNavMeshDestinationReached() {} ///< @note NavMeshAgentComponent が目的地に到達したとき
    virtual void OnNavMeshPathFailed() {}          ///< @note 目的地までのパスが見つからなかったとき
    virtual void OnNavMeshTargetSpotted() {}       ///< @note NavMeshSensorComponent が対象を視界内に検知したとき
    virtual void OnNavMeshTargetLost() {}          ///< @note 検知していた対象を見失ったとき
    virtual void OnAnimationEvent(const AnimationEventInfo&) {}
    /// @note AnimatorComponent がルートモーションを抽出した直後に、同じフレーム内で呼ばれる。
    /// @note rootMotion.mode が None 以外なら毎フレーム発火する (delta がゼロでも呼ばれる)。
    /// @note ExtractOnly のときはエンジンが何も動かさないため、ここで移動を適用する。
    virtual void OnAnimatorMove(const RootMotionInfo&) {}
    /// @note Editor authoring / serialization lifecycle。
    virtual void Reset() {}
    virtual void OnValidate() {}
    virtual void OnBeforeSerialize() {}
    virtual void OnAfterDeserialize() {}
    virtual void Reflect(IReflector&) {}
    virtual const char* GetTypeName() const { return "Script"; }

    /// @note 型名から、その型の部分オブジェクトを指すポインタを返す。無ければ nullptr。
    /// @note GetScript<T>() / FindObjectsOfType<T>() が基底型やインターフェースで引けるのはこれによる。
    /// @note bool ではなく void* を返す。多重継承では派生の先頭番地と基底の番地が一致せず、
    /// @note 補正できるのは自分の型を知っている派生側だけなので、調整済みのポインタを返させる。
    /// @note dynamic_cast は使えない ─ スクリプトはホットリロードされる DLL 側に居るため、
    /// @note EXE と DLL で同じクラスの type_info が別実体になり得る。
    virtual void* FbzzAsType(std::string_view typeName) const
    {
        return typeName == "Script" ? const_cast<Script*>(this) : nullptr;
    }

    /// @note 型名による is-a 判定。継承鎖とインターフェースを辿り、1 つでも一致すれば true。
    [[nodiscard]] bool IsA(std::string_view typeName) const
    {
        return FbzzAsType(typeName) != nullptr;
    }

    /// @name 固定ステップ更新
    /// @{
    /// @note Phase::Physics と同じ固定タイムステップ (既定 60Hz) で、物理の直前に呼ばれる。
    /// @note フレームレートに関わらず 1 秒あたりの呼び出し回数が一定なので、力の加算や
    /// @note 移動量の積分はここへ置くと PC 性能で挙動が変わらない。
    /// @note 経過時間は time.DeltaTime() ではなく time.FixedDeltaTime() を使うこと。
    /// @note OnUpdate は描画フレームごと (可変 dt) なので、物理に効く処理を書くと環境で結果がずれる。
    virtual void OnFixedUpdate() {}
    /// @}

    /// @name オブジェクトプール
    /// @{
    /// @note scene.Spawn() で貸し出された / scene.Despawn() で返却された瞬間に呼ばれる。
    /// @note 再利用インスタンスでは OnAwake / OnStart は再発火しないため、
    /// @note 「毎回リセットしたい状態」(HP・経過時間・軌跡バッファ) はここで初期化する。
    virtual void OnSpawn() {}
    virtual void OnDespawn() {}

    /// @note このスクリプトが成立するために同じ GameObject へ必要なコンポーネント型名。
    /// @note FBZZ_REQUIRE_COMPONENT / FBZZ_OPTIONAL_COMPONENT が override する。
    /// @note 返す名前は ComponentRegistry の serializedName (= 型名そのもの) と同じ綴り。
    ///
    /// @note 仮想関数の追加は必ず仮想関数列の末尾に行い、ScriptDllAbi.hpp の
    /// @note kScriptVtableAbiVersion をインクリメントすること (途中へ挿すと番号がずれる)。
    virtual std::span<const std::string> RequiredComponents() const { return {}; }
    virtual std::span<const std::string> OptionalComponents() const { return {}; }

    /// @note true を返すと Play 中でなくても ScriptSystem が回す。FBZZ_EXECUTE_ALWAYS() が override する。
    /// @note 既定は false。編集中の実行はシーンの中身を書き換えるので、ゲームロジックが走ると
    /// @note 保存したシーンに遊んだ後の状態が入る。見た目を組み立てる用途だけ opt-in する。
    /// @note 追加位置は仮想関数列の末尾 (kScriptVtableAbiVersion も併せて上げる)。
    virtual bool ExecuteInEditMode() const { return false; }
    /// @}

    /// @name .sequence のコールバック
    /// @{
    /// @note EventTrack のキーを跨いだフレームで、時刻昇順に呼ばれる。逆再生と
    /// @note エディタのスクラブでは発火しない (演出の合図は巻き戻せないため)。
    ///
    /// @note 追加位置について: 仮想関数列の末尾に置くこと。ScriptDllAbi.hpp の
    /// @note kScriptVtableAbiVersion も併せて上げる。
    virtual void OnSequenceEvent(const SequenceEventInfo&) {}
    /// @note wrapMode が Once のシーケンスが終端へ達し、復帰まで終えた直後に呼ばれる。
    virtual void OnSequenceFinished(const char* /*sequenceName*/) {}

    /// @brief エディターで自分 (または祖先) が選択されているときだけ、OnDrawGizmos の後に呼ばれる。
    /// @note 描くのは gizmo 経由。Scene View の «Script Gizmos» が点いているときだけ呼ばれる。
    /// @note 追加位置は仮想関数列の末尾 (kScriptVtableAbiVersion 7)。
    virtual void OnDrawGizmosSelected() {}

    virtual std::span<const std::string> RequiredScripts() const { return {}; }

    bool enabled = true;

    /// @note Proxy はカテゴリごとに責務を分け、Script.hpp の肥大化を避ける。新プロキシを追加する際は ScriptProxyMembers.inl を編集すること (このファイルは触らなくてよい)。
#define FBZZ_PROXY_MEMBER(Type, Name)     Type Name { this };
#define FBZZ_PROXY_STANDALONE(Type, Name) Type Name;
#include <Engine/Scene/ScriptProxy/ScriptProxyMembers.inl>
#undef FBZZ_PROXY_MEMBER
    #undef FBZZ_PROXY_STANDALONE


    /// @note Invoke / タイマー
    [[nodiscard]] InvokeHandle Invoke(std::function<void()> fn, float delay);
    [[nodiscard]] InvokeHandle InvokeRepeating(std::function<void()> fn, float delay, float interval);
    void FrameDelay(uint32_t n, std::function<void()> fn);
    void CancelInvoke();
    void CancelInvoke(InvokeHandle handle);
    void CancelEventSubscriptions();

    /// @note コルーチン (Coroutine.hpp)。WaitForSeconds 等を co_await して時間軸処理を直線的に書く。
    void StartCoroutine(Coroutine co);
    /// @note コルーチン内から呼んでもよい。その場合は実行中のハンドルを自己破棄しないよう、
    /// @note ティックを抜けてから実際に畳む。
    void StopAllCoroutines();

    /// @note QueueRenderPass / GetShaderDescriptor
    void QueueRenderPass(UserRenderPassDesc desc) const;
    const renderer::ShaderDescriptor* GetShaderDescriptor(std::string_view shaderPath) const;

    /// @note Engine の実行時コールバック共通経路
    void SetContext(Scene* scene, GameObject* gameObject);
    /// @brief 複合 Script が内部モジュールへ同じ Scene / GameObject コンテキストを渡す。
    /// @note 公開するのは、PlayerComponent のような 1 コンポーネント構成でも責務別クラスをファイル分割したまま既存の Script proxy と EntityRef を再利用できるようにするため。
    void AdoptContext(const Script& owner)
    {
        SetContext(owner.m_scene, owner.m_gameObject);
        m_contextOwner = &owner;
    }
    /// @brief 内部モジュールは親 Script の無効化も実効状態へ反映する。
    /// @pre AdoptContext で渡した親は自分より長く生存し、所有関係は循環しない。
    [[nodiscard]] bool IsContextEnabled() const
    {
        return enabled && !m_requirementsBlocked && (!m_contextOwner || m_contextOwner->IsContextEnabled());
    }
    /// @brief GameObject の階層有効状態も含めた実効 enabled を更新し、OnEnable / OnDisable を通知する。
    /// @note Script 自身の enabled だけを見ると、GameObject を無効化してもコールバックが動き続ける。
    void SynchronizeEnabledState(bool gameObjectActive = true);
    /// @brief Script 内の空参照によるアクセス違反を Editor プロセスへ伝播させない共通入口。
    /// @note C++ の nullptr 参照は例外ではなく通常の try/catch では保護できない。すべての実行時コールバックをここへ通し、問題の Script だけを停止する。
    bool ExecuteCallback(void (Script::*callback)(), const char* callbackName);
    bool ExecuteCallback(void (Script::*callback)(const CollisionInfo&),
                         const CollisionInfo& info,
                         const char* callbackName);
    bool ExecuteCallback(void (Script::*callback)(const AnimationEventInfo&),
                         const AnimationEventInfo& info);
    bool ExecuteCallback(void (Script::*callback)(const RootMotionInfo&),
                         const RootMotionInfo& info);
    bool ExecuteCallback(void (Script::*callback)(const SequenceEventInfo&),
                         const SequenceEventInfo& info);
    bool ExecuteCallback(void (Script::*callback)(const char*),
                         const char* argument,
                         const char* callbackName);
    bool ExecuteCallback(void (Script::*callback)(RenderPipeline&, RenderPassContext&),
                         RenderPipeline& pipeline,
                         RenderPassContext& context);
    bool ExecuteCallback(const std::function<void()>& function, const char* callbackName);
    bool ResumeCoroutine(Coroutine& coroutine);
    void UpdateInvocations(float dt);
    void UpdateFrameDelays();
    void UpdateCoroutines();
    /// @brief OnAwake 前と同じ状態へ戻す。Play の開始・停止をまたぐときに ScriptSystem が呼ぶ。
    /// @note 編集中に積んだ Invoke / Coroutine / OnEnable 済みフラグを Play へ持ち越すと、Play 開始直後に「前のセッションの続き」が発火する。
    void ResetLifecycleState();
    /// @note 必須設定不足の停止は enabled を戻すだけでは解除しない。再生し直して検証する。
    void BlockForMissingRequirements() { m_requirementsBlocked = true; enabled = false; }
    [[nodiscard]] bool RequirementsBlocked() const { return m_requirementsBlocked; }
    static void SetPhysicsWorld(physics::World* world);
    /// @brief 今のスクリプト実行が Play セッション中か。`app.IsPlaying()` の実体。
    /// @note 静的なのは、編集中も走るスクリプトは自分がどちらのモードに居るか知る必要があるが Script は SceneManager を知らないため。実行主体である ScriptSystem が毎フレーム書く。
    static void SetInPlayMode(bool inPlayMode);
    [[nodiscard]] static bool IsInPlayMode();

    using PrefabInstantiateFn = std::function<bool(Scene&, const std::string&, std::vector<EntityID>&)>;
    static void SetPrefabInstantiationCallback(PrefabInstantiateFn fn);
    static bool InstantiatePrefab(Scene& scene, const std::string& path, std::vector<EntityID>& roots);
    /// @}

protected:
    renderer::PostProcessSettings& GetRuntimePostProcessSettings();
    const renderer::PostProcessSettings* TryGetRuntimePostProcessSettings() const;
    void SetRuntimePostProcessSettings(const renderer::PostProcessSettings& settings);
    void ClearRuntimePostProcessSettings();

    Scene*      m_scene      = nullptr;
    const Script* m_contextOwner = nullptr;
    GameObject* m_gameObject = nullptr;
    /// @note アクセス違反後は同じ Script を毎フレーム呼ばず、Play を継続できるようにする。
    bool        m_runtimeFaulted = false;

private:
    void ReleaseOwnedAudioLoops();
    std::vector<std::weak_ptr<audio::VoiceLifetime>> m_ownedAudioLoops;
    ScriptInspectionIdentity m_inspectionIdentity;
    bool m_requirementsBlocked = false;
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
    friend struct ScriptFlowFieldProxy;
    friend struct ScriptCloudProxy;
    friend struct ScriptSunMoonProxy;
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
    friend struct ScriptEnvironmentProxy;
    friend struct ScriptDecalProxy;
    friend struct ScriptVolumeProxy;
    friend struct ScriptReflectionProbeProxy;
    friend struct ScriptMotionWarpProxy;
    friend struct ScriptLifetimeProxy;
    friend struct ScriptSaveProxy;
    friend struct ScriptEventProxy;
    friend struct ScriptRandomProxy;
    friend struct ScriptTweenProxy;
    friend struct ScriptSequenceProxy;
    friend struct ScriptObjectMaskProxy;
    friend struct ScriptSpringBoneProxy;
    friend struct ScriptJointProxy;

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
    /// @note Tick 中に開始されたコルーチンは m_coroutines を再確保し実行中ハンドルを移動させてしまうため、ティック中は m_pendingCoroutines に積み、ループ後に取り込む。
    std::vector<Coroutine> m_coroutines;
    std::vector<Coroutine> m_pendingCoroutines;
    bool m_isTickingCoroutines = false;
    bool m_stopAllCoroutinesRequested = false;
    bool m_enableStateInitialized = false;
    bool m_lastEnabled = true;
    bool m_isTickingInvokes = false;
    static physics::World*     s_physicsWorld;
    static bool                s_inPlayMode;
    static PrefabInstantiateFn s_instantiateFn;
};

/// @brief Script / ScriptSceneProxy が完全型になったここで定義する。
/// @note `object()` は `owner->scene` (Script のメンバー)、`Get()` は `GetScript<T>` のテンプレート定義 (Scene.hpp 末尾) を必要とするため、宣言と分けてここへ置く。
template<typename T>
inline GameObject* Ref<T>::object() const
{
    if (!owner) return nullptr;
    /// @note EntityRef::Resolve(const ScriptSceneProxy&)
    return ref.Resolve(owner->scene);
}

template<typename T>
inline T* Ref<T>::Get() const
{
    GameObject* go = object();
    if (!go) return nullptr;
    if constexpr (std::is_same_v<T, GameObject>)
        return go;
    else if constexpr (detail::kIsScriptQueryable<T>)
        /// @note Script 派生と横断インターフェースは同じ取得経路を使う。
        return owner->scene.template GetScript<T>(go);
    else
        /// @note それ以外はコンポーネント。GameObject を完全型にしないで済むよう
        /// @note        プロキシ経由で引く (Script.hpp は GameObject.hpp を include していない)。
        return owner->scene.template GetComponent<T>(go);
}

} /// @note namespace fbzz::scene
