/// @file    Script.hpp
/// @brief   ユーザースクリプト基底クラスとリフレクション補助。
/// @author  Hasegawa Jin
/// @date    2026-05-21

/// @note GameObject にアタッチされる振る舞いの共通ライフサイクルを定義する。
/// @note engine 側の Component とは分け、ScriptComponent が所有する。
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/ObjectReference.hpp>
#include <Engine/Scene/PrefabRef.hpp>
/// @note DataAsset (純共有 ScriptableObject) 参照スロット。
#include <Engine/Scene/DataAssetRef.hpp>
#include <Engine/Scene/ScriptAssetRef.hpp>
#include <Engine/Scene/ScriptCallbackKind.hpp>
/// @note 自己登録リフレクション基盤 (ReflectTag / DisplayOr)。
#include <Engine/Scene/Reflection.hpp>
#include <Engine/Scene/CollisionInfo.hpp>
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

class ScriptProfiler;

struct ColliderComponent;
struct Transform;
class GameObject;
class Scene;
struct IReflector;

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

/// @brief Invoke / InvokeRepeating が返す軽量値型。CancelInvoke(handle) で個別キャンセルする。
struct InvokeHandle {
    uint32_t id = 0;
    bool IsValid() const { return id != 0; }
};

/// @brief オブジェクトの生存期間だけ有効な観測 ID。コピー先には新しい ID を発行する。
struct ScriptInspectionIdentity {
    FBZZ_ENGINE_API ScriptInspectionIdentity();
    FBZZ_ENGINE_API ScriptInspectionIdentity(const ScriptInspectionIdentity&);
    ScriptInspectionIdentity& operator=(const ScriptInspectionIdentity&) { return *this; }
    std::string value;
};

class Script {
public:
    [[nodiscard]] const std::string& InspectionId() const { return m_inspectionIdentity.value; }
    [[nodiscard]] bool IsRuntimeFaulted() const { return m_runtimeFaulted; }
    FBZZ_ENGINE_API virtual ~Script();

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
    /// @note 対象型が型名と基底ポインタ補正を明示できる場合だけ照合する。
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
    /// @note ManagedScriptAdapter が managed `RequireComponentAttribute` の型名を返す。
    /// @note 返す名前は ComponentRegistry の serializedName (= 型名そのもの) と同じ綴り。

    virtual std::span<const std::string> RequiredComponents() const { return {}; }
    virtual std::span<const std::string> OptionalComponents() const { return {}; }

    /// @note true を返すと Play 中でなくても ScriptSystem が回す。ManagedScriptAdapter が managed `ExecuteAlwaysAttribute` を反映する。
    /// @note 既定は false。編集中の実行はシーンの中身を書き換えるので、ゲームロジックが走ると
    /// @note 保存したシーンに遊んだ後の状態が入る。見た目を組み立てる用途だけ opt-in する。
    virtual bool ExecuteInEditMode() const { return false; }
    /// @}

    /// @name .sequence のコールバック
    /// @{
    /// @note EventTrack のキーを跨いだフレームで、時刻昇順に呼ばれる。逆再生と
    /// @note エディタのスクラブでは発火しない (演出の合図は巻き戻せないため)。

    virtual void OnSequenceEvent(const SequenceEventInfo&) {}
    /// @note wrapMode が Once のシーケンスが終端へ達し、復帰まで終えた直後に呼ばれる。
    virtual void OnSequenceFinished(const char*) {}

    /// @brief エディターで自分 (または祖先) が選択されているときだけ、OnDrawGizmos の後に呼ばれる。
    /// @note 描くのは gizmo 経由。Scene View の «Script Gizmos» が点いているときだけ呼ばれる。
    virtual void OnDrawGizmosSelected() {}

    virtual std::span<const std::string> RequiredScripts() const { return {}; }

    /// @note 小さい値を先に配送し、同値は従来の個体順を保つ。teardown や物理通知の順序は変えない。
    virtual int ExecutionOrder() const { return 0; }

    bool enabled = true;

    /// @note Proxy はカテゴリごとに責務を分け、Script.hpp の肥大化を避ける。新プロキシを追加する際は ScriptProxyMembers.inl を編集すること (このファイルは触らなくてよい)。
#define FBZZ_PROXY_MEMBER(Type, Name)     Type Name { this };
#define FBZZ_PROXY_STANDALONE(Type, Name) Type Name;
#include <Engine/Scene/ScriptProxy/ScriptProxyMembers.inl>
#undef FBZZ_PROXY_MEMBER
    #undef FBZZ_PROXY_STANDALONE


    /// @note Invoke / タイマー
    [[nodiscard]] FBZZ_ENGINE_API InvokeHandle Invoke(std::function<void()> fn, float delay);
    [[nodiscard]] FBZZ_ENGINE_API InvokeHandle InvokeRepeating(std::function<void()> fn, float delay, float interval);
    FBZZ_ENGINE_API void FrameDelay(uint32_t n, std::function<void()> fn);
    FBZZ_ENGINE_API void CancelInvoke();
    FBZZ_ENGINE_API void CancelInvoke(InvokeHandle handle);
    FBZZ_ENGINE_API void CancelEventSubscriptions();

    /// @note コルーチン (Coroutine.hpp)。WaitForSeconds 等を co_await して時間軸処理を直線的に書く。
    FBZZ_ENGINE_API void StartCoroutine(Coroutine co);
    /// @note コルーチン内から呼んでもよい。その場合は実行中のハンドルを自己破棄しないよう、
    /// @note ティックを抜けてから実際に畳む。
    FBZZ_ENGINE_API void StopAllCoroutines();

    /// @note QueueRenderPass / GetShaderDescriptor
    FBZZ_ENGINE_API void QueueRenderPass(UserRenderPassDesc desc) const;
    FBZZ_ENGINE_API const renderer::ShaderDescriptor* GetShaderDescriptor(std::string_view shaderPath) const;

    /// @note Engine の実行時コールバック共通経路
    FBZZ_ENGINE_API void SetContext(Scene* scene, GameObject* gameObject);
    /// @brief GameObject の階層有効状態も含めた実効 enabled を更新し、OnEnable / OnDisable を通知する。
    /// @note Script 自身の enabled だけを見ると、GameObject を無効化してもコールバックが動き続ける。
    FBZZ_ENGINE_API void SynchronizeEnabledState(bool gameObjectActive = true);
    /// @return Script 自体と Engine の要件検証で有効な場合は true。
    [[nodiscard]] bool IsContextEnabled() const { return enabled && !m_requirementsBlocked; }
    /// @brief Script 内の空参照によるアクセス違反を Editor プロセスへ伝播させない共通入口。
    /// @note C++ の nullptr 参照は例外ではなく通常の try/catch では保護できない。すべての実行時コールバックをここへ通し、問題の Script だけを停止する。
    FBZZ_ENGINE_API bool ExecuteCallback(void (Script::*callback)(), const char* callbackName);
    FBZZ_ENGINE_API bool ExecuteCallback(void (Script::*callback)(const CollisionInfo&),
                         const CollisionInfo& info,
                         const char* callbackName);
    FBZZ_ENGINE_API bool ExecuteCallback(void (Script::*callback)(const AnimationEventInfo&),
                         const AnimationEventInfo& info);
    FBZZ_ENGINE_API bool ExecuteCallback(void (Script::*callback)(const RootMotionInfo&),
                         const RootMotionInfo& info);
    FBZZ_ENGINE_API bool ExecuteCallback(void (Script::*callback)(const SequenceEventInfo&),
                         const SequenceEventInfo& info);
    FBZZ_ENGINE_API bool ExecuteCallback(void (Script::*callback)(const char*),
                         const char* argument,
                         const char* callbackName);
    FBZZ_ENGINE_API bool ExecuteCallback(void (Script::*callback)(RenderPipeline&, RenderPassContext&),
                         RenderPipeline& pipeline,
                         RenderPassContext& context);
    FBZZ_ENGINE_API bool ExecuteCallback(const std::function<void()>& function, const char* callbackName);
    /// @note 既知 callback は明示 kind を渡す。互換入口は UNKNOWN として記録する。
    /// @see https://eel.is/c++draft/expr.eq#5.3 仮想メンバー関数ポインターの等値比較は unspecified。
    FBZZ_ENGINE_API bool ExecuteProfiledCallback(void (Script::*callback)(), ScriptCallbackKind kind, const char* callbackName);
    FBZZ_ENGINE_API bool ExecuteProfiledCallback(void (Script::*callback)(const CollisionInfo&), const CollisionInfo& info, ScriptCallbackKind kind, const char* callbackName);
    FBZZ_ENGINE_API bool ExecuteProfiledCallback(void (Script::*callback)(const AnimationEventInfo&), const AnimationEventInfo& info, ScriptCallbackKind kind, const char* callbackName);
    FBZZ_ENGINE_API bool ExecuteProfiledCallback(void (Script::*callback)(const SequenceEventInfo&), const SequenceEventInfo& info, ScriptCallbackKind kind, const char* callbackName);
    FBZZ_ENGINE_API bool ExecuteProfiledCallback(void (Script::*callback)(const char*), const char* argument, ScriptCallbackKind kind, const char* callbackName);
    FBZZ_ENGINE_API bool ExecuteProfiledCallback(void (Script::*callback)(const RootMotionInfo&), const RootMotionInfo& info, ScriptCallbackKind kind, const char* callbackName);
    FBZZ_ENGINE_API bool ExecuteProfiledCallback(void (Script::*callback)(RenderPipeline&, RenderPassContext&), RenderPipeline& pipeline, RenderPassContext& context, ScriptCallbackKind kind, const char* callbackName);
    /// @note 明示した種別を一度だけ計測し、障害を受信側 Script に帰属させる。
    FBZZ_ENGINE_API bool ExecuteProfiledCallback(const std::function<void()>& function, ScriptCallbackKind kind, const char* callbackName);
    FBZZ_ENGINE_API bool ResumeCoroutine(Coroutine& coroutine);
    FBZZ_ENGINE_API void UpdateInvocations(float dt);
    FBZZ_ENGINE_API void UpdateFrameDelays();
    FBZZ_ENGINE_API void UpdateCoroutines();
    /// @brief OnAwake 前と同じ状態へ戻す。Play の開始・停止をまたぐときに ScriptSystem が呼ぶ。
    /// @note 編集中に積んだ Invoke / Coroutine / OnEnable 済みフラグを Play へ持ち越すと、Play 開始直後に「前のセッションの続き」が発火する。
    FBZZ_ENGINE_API void ResetLifecycleState();
    /// @note 必須設定不足の停止は enabled を戻すだけでは解除しない。再生し直して検証する。
    void BlockForMissingRequirements() { m_requirementsBlocked = true; enabled = false; }
    [[nodiscard]] bool RequirementsBlocked() const { return m_requirementsBlocked; }
    FBZZ_ENGINE_API static void SetPhysicsWorld(physics::World* world);
    /// @note 実行主体が設定した非所有参照。Play の物理 World が未接続なら nullptr。
    [[nodiscard]] FBZZ_ENGINE_API static physics::World* GetPhysicsWorld();
    /// @brief 今のスクリプト実行が Play セッション中か。`app.IsPlaying()` の実体。
    /// @note 静的なのは、編集中も走るスクリプトは自分がどちらのモードに居るか知る必要があるが Script は SceneManager を知らないため。実行主体である ScriptSystem が毎フレーム書く。
    FBZZ_ENGINE_API static void SetInPlayMode(bool inPlayMode);
    [[nodiscard]] FBZZ_ENGINE_API static bool IsInPlayMode();

    using PrefabInstantiateFn = std::function<bool(Scene&, const std::string&, std::vector<EntityID>&)>;
    FBZZ_ENGINE_API static void SetPrefabInstantiationCallback(PrefabInstantiateFn fn);
    /// @note 注入経路の任意の Scene 変更を、連続生成の索引再利用から保護するための判定。
    [[nodiscard]] FBZZ_ENGINE_API static bool HasPrefabInstantiationCallback();
    FBZZ_ENGINE_API static bool InstantiatePrefab(Scene& scene, const std::string& path, std::vector<EntityID>& roots);
    /// @}

protected:
    FBZZ_ENGINE_API renderer::PostProcessSettings& GetRuntimePostProcessSettings();
    FBZZ_ENGINE_API const renderer::PostProcessSettings* TryGetRuntimePostProcessSettings() const;
    FBZZ_ENGINE_API void SetRuntimePostProcessSettings(const renderer::PostProcessSettings& settings);
    FBZZ_ENGINE_API void ClearRuntimePostProcessSettings();

    Scene*      m_scene      = nullptr;
    GameObject* m_gameObject = nullptr;
    /// @note アクセス違反後は同じ Script を毎フレーム呼ばず、Play を継続できるようにする。
    bool        m_runtimeFaulted = false;

private:
    FBZZ_ENGINE_API void ReleaseOwnedAudioLoops();
    /// @note 引数型ごとの入口を保ち、SEH の後に managed 障害ラッチを確認して一度だけ計測を確定する。
    template<typename Argument, typename Callback>
    bool ExecuteProfiledCallbackWithArgument(Callback callback, const Argument& argument,
                                             ScriptCallbackKind kind, const char* callbackName);
    std::vector<std::weak_ptr<audio::VoiceLifetime>> m_ownedAudioLoops;
    ScriptInspectionIdentity m_inspectionIdentity;
    bool m_requirementsBlocked = false;
    friend struct ScriptTransformProxy;
    friend class ScriptProfiler;
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
    FBZZ_ENGINE_API static bool                s_inPlayMode;
    FBZZ_ENGINE_API static PrefabInstantiateFn s_instantiateFn;
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
