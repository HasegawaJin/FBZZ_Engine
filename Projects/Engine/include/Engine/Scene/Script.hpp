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
#include <Engine/Scene/Reflection.hpp>  // 自己登録リフレクション基盤 (ReflectTag / DisplayOr)
#include <Engine/Scene/Ref.hpp>          // 型安全オブジェクト参照ハンドル Ref<T>
// 全プロキシヘッダーのアンブレラインクルード。新プロキシ追加時はこちらを編集すること。
#include <Engine/Scene/ScriptProxy/AllScriptProxies.hpp>
#include <Engine/Scene/Coroutine.hpp>
#include <Engine/Input/KeyCode.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
// WHY: Script.hpp は全ユーザースクリプトの共通プレリュードを兼ねる。ゲームスクリプトが
//      ほぼ必ず使う標準ヘッダー (clamp/min/max・数学関数・文字列・コンテナ) をここへ集約し、
//      各スクリプトが <algorithm> 等を個別に並べる定型を不要にする。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::scene {

struct ColliderComponent;
struct Transform;
class GameObject;
class Scene;

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
    virtual ~IReflector() = default;

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

    // 付加情報付き (デフォルトは Field へフォールバック)
    virtual void FloatRange(const char* name, float& v, float min, float max)  { Field(name, v); }
    virtual void IntRange(const char* name, int& v, int min, int max)          { Field(name, v); }
    virtual void Enum(const char* name, int& v, std::span<const char* const> labels) { Field(name, v); }
    // 型付きオブジェクト参照スロット。typeName が非空ならその Script 型を持つ GameObject だけを
    // 受け付ける (Inspector のドロップ型チェック用)。既定はシリアライズと同じく EntityID を保存する。
    virtual void RefField(const char* name, EntityRef& v, const char* typeName) { Field(name, v.id); }
    // 直前に描画したフィールドへ説明ツールチップを付ける (Inspector のみ表示、シリアライズ非対象)。
    virtual void Tooltip(const char* text) {}
    virtual void Group(const char* label) {}
    virtual void Readonly(const char* name, const std::string& v) {}
    virtual void Readonly(const char* name, float v)  {}
    virtual void Readonly(const char* name, int v)    {}
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
    FBZZ_REFLECT_ENTRY_(Name, r_.Field(FBZZ_DISP_(Display, Name), Name))

#define FBZZ_FIELD_RANGE(Type, Name, Default, Display, Min, Max)                \
    Type Name = Default;                                                        \
    FBZZ_REFLECT_ENTRY_(Name,                                                   \
        r_.FloatRange(FBZZ_DISP_(Display, Name), Name, Min, Max))

// int 用レンジフィールド (スライダー)。FBZZ_FIELD_RANGE の int 版。
#define FBZZ_FIELD_RANGE_INT(Type, Name, Default, Display, Min, Max)            \
    Type Name = Default;                                                        \
    FBZZ_REFLECT_ENTRY_(Name,                                                   \
        r_.IntRange(FBZZ_DISP_(Display, Name), Name, Min, Max))

#define FBZZ_FIELD_ENUM(Type, Name, Default, Display, ...)                      \
    Type Name = Default;                                                        \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        static const char* const _fbzz_labels[] = { __VA_ARGS__ };             \
        r_.Enum(FBZZ_DISP_(Display, Name),                                      \
                reinterpret_cast<int&>(Name),                                   \
                ::std::span<const char* const>(_fbzz_labels));                  \
    })

// 参照型のデフォルト値省略版 — PrefabRef / EntityRef 等のデフォルトが {} のフィールドに使う。
#define FBZZ_FIELD_REF(Type, Name, Display)                                     \
    Type Name = {};                                                            \
    FBZZ_REFLECT_ENTRY_(Name, r_.Field(FBZZ_DISP_(Display, Name), Name))

// Inspector 表示のみ・Serializer 非保存の計算値ラベル。
#define FBZZ_COMPUTED(Type, Name, Display)                                      \
    Type Name = {};                                                            \
    FBZZ_REFLECT_ENTRY_(Name, r_.Readonly(FBZZ_DISP_(Display, Name), Name))
// 後方互換 alias — 新規コードでは FBZZ_COMPUTED を使うこと。
#define FBZZ_FIELD_READONLY(Type, Name, Display) FBZZ_COMPUTED(Type, Name, Display)

// 型安全オブジェクト参照フィールド。Inspector にドラッグ&ドロップスロットを出す。
// Ref<T> は { this } で所有 Script を受け取り、Name.Get() / if (Name) で解決する。
// シリアライズは内包する EntityRef (= EntityID) を対象にする。
#define FBZZ_REF(Type, Name, Display)                                           \
    ::fbzz::scene::Ref<Type> Name { this };                                     \
    FBZZ_REFLECT_ENTRY_(Name, r_.RefField(FBZZ_DISP_(Display, Name), Name.ref,  \
        ::fbzz::scene::RefTypeNameOf<Type>()))

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

// ── Script 基底クラス ─────────────────────────────────────────────────────────
class Script {
public:
    virtual ~Script();

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
    virtual void Reflect(IReflector&) {}
    virtual const char* GetTypeName() const { return "Script"; }

    bool enabled = true;

    // Proxy — カテゴリごとに責務を分け、Script.hpp の肥大化を避ける。
    // 新プロキシを追加する際は ScriptProxyMembers.inl を編集すること (このファイルは触らなくてよい)。
#define FBZZ_PROXY_MEMBER(Type, Name)     Type Name { this };
#define FBZZ_PROXY_STANDALONE(Type, Name) Type Name;
#include <Engine/Scene/ScriptProxy/ScriptProxyMembers.inl>
#undef FBZZ_PROXY_MEMBER
#undef FBZZ_PROXY_STANDALONE

    template<typename T>
    [[deprecated("Use scene.GetComponent<T>()")]]
    T* GetComponent() const;

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

    // deprecated 委譲メソッド — scene.* を使うこと
    [[deprecated("Use scene.Find()")]]
    GameObject* Find(const std::string& name) const;
    [[deprecated("Use scene.FindWithTag()")]]
    GameObject* FindWithTag(const std::string& tag) const;
    [[deprecated("Use scene.GetGameObject()")]]
    GameObject* GetGameObject(EntityID id) const;
    [[deprecated("Use scene.Create()")]]
    GameObject& CreateGameObject(const std::string& name = "GameObject") const;
    [[deprecated("Use scene.GetMainCameraObject()")]]
    GameObject* GetMainCameraObject() const;
    static void Destroy(GameObject& go, float delay = 0.0f);

    // deprecated animator 委譲メソッド — animator.* を使うこと
    [[deprecated("Use animator.SetFloat()")]]  void SetAnimatorFloat(std::string_view n, float v) const;
    [[deprecated("Use animator.SetInt()")]]    void SetAnimatorInt  (std::string_view n, int v)   const;
    [[deprecated("Use animator.SetBool()")]]   void SetAnimatorBool (std::string_view n, bool v)  const;
    [[deprecated("Use animator.SetTrigger()")]]void SetAnimatorTrigger(std::string_view n)        const;
    [[deprecated("Use animator.IsInState()")]] bool IsAnimatorInState(std::string_view n)         const;

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
