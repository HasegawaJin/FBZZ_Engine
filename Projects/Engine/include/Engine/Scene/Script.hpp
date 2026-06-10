// FBZZ Engine
// Script.hpp | fbzz::scene
// ユーザースクリプト基底クラスとリフレクション補助
// GameObject にアタッチされる振る舞いの共通ライフサイクルを定義する。
// engine 側の Component とは分け、ScriptComponent が所有する。
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/PrefabRef.hpp>
#include <Engine/Scene/ScriptProxy/ScriptAnimatorProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptAudioProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptCameraProxy.hpp>
#include <Engine/Scene/ScriptProxy/GizmoProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptDebugProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptInputProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptLightProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptMaterialProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptMeshTrailProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptParticleProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptPhysicsProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptPostProcessProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptSceneProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptTransformProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptTrailProxy.hpp>
#include <Engine/Input/KeyCode.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
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
class RenderGraph;
struct ShaderDescriptor;
struct PostProcessSettings;
}

namespace fbzz::scene {
struct RenderPassContext;
struct UserRenderPassDesc;

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
    virtual void Field(const char* name, input::KeyCode& v) {}  // キー名ドロップダウン

    // 付加情報付き (デフォルトは Field へフォールバック)
    virtual void FloatRange(const char* name, float& v, float min, float max)  { Field(name, v); }
    virtual void Enum(const char* name, int& v, std::span<const char* const> labels) { Field(name, v); }
    virtual void Group(const char* label) {}
    virtual void Readonly(const char* name, const std::string& v) {}
    virtual void Readonly(const char* name, float v)  {}
    virtual void Readonly(const char* name, int v)    {}
};

// ── InvokeHandle ─────────────────────────────────────────────────────────────
// Invoke / InvokeRepeating が返す軽量値型。CancelInvoke(handle) で個別キャンセル。
struct InvokeHandle {
    uint32_t id = 0;
    bool IsValid() const { return id != 0; }
};

// ── FBZZ マクロ ───────────────────────────────────────────────────────────────

// FBZZ_GENERATED_BODY のフォールバック — .generated.hpp がインクルードされるまで空の Reflect を展開する。
#ifndef FBZZ_GENERATED_BODY
#define FBZZ_GENERATED_BODY() void Reflect(IReflector&) override {}
#endif

// FBZZ_SCRIPT — TYPE_NAME + GetTypeName + Reflect 宣言をまとめる。
// FHT が .generated.hpp を生成すると FBZZ_GENERATED_BODY() が Reflect 実装に差し替わる。
#define FBZZ_SCRIPT(T)                                                         \
    static constexpr const char* TYPE_NAME = #T;                               \
    const char* GetTypeName() const override { return TYPE_NAME; }             \
    FBZZ_GENERATED_BODY()

// FHT がパースして .generated.hpp 内の Reflect() を生成するマクロ群。
// コンパイラには普通のフィールド宣言として見える。
#define FBZZ_FIELD(Type, Name, Default, DisplayName)              Type Name = Default
#define FBZZ_FIELD_RANGE(Type, Name, Default, DisplayName, Min, Max) Type Name = Default
#define FBZZ_FIELD_ENUM(Type, Name, Default, DisplayName, ...)    Type Name = Default
#define FBZZ_FIELD_READONLY(Type, Name, DisplayName)              Type Name = {}
#define FBZZ_GROUP(Label)  // Inspector グループ見出し (FHT が読む)

// ── Script 基底クラス ─────────────────────────────────────────────────────────
class Script {
public:
    virtual ~Script();

    virtual void OnAwake() {}
    virtual void OnStart() {}
    virtual void OnEnable() {}
    virtual void OnDisable() {}
    virtual void OnUpdate(float) {}
    virtual void OnLateUpdate(float) {}
    virtual void OnDestroy() {}
    virtual void OnDrawGizmos() {}
    virtual void OnSetupRenderPasses(renderer::RenderGraph&, RenderPassContext&) {}
    virtual void OnCollisionEnter(const CollisionInfo&) {}
    virtual void OnCollisionStay(const CollisionInfo&) {}
    virtual void OnCollisionExit(const CollisionInfo&) {}
    virtual void OnTriggerEnter(const CollisionInfo&) {}
    virtual void OnTriggerStay(const CollisionInfo&) {}
    virtual void OnTriggerExit(const CollisionInfo&) {}
    virtual void Reflect(IReflector&) {}
    virtual const char* GetTypeName() const { return "Script"; }

    bool enabled = true;

    // タイミング情報 (ScriptSystem が毎フレーム注入)
    float    deltaTime         = 0.0f;
    float    time              = 0.0f;
    float    unscaledDeltaTime = 0.0f;
    uint64_t frameCount        = 0;

    // 後方互換 alias — 既存コードがコンパイルを通すための橋渡し
    [[deprecated("Use deltaTime")]]         float& m_deltaTime         = deltaTime;
    [[deprecated("Use time")]]              float& m_time              = time;
    [[deprecated("Use unscaledDeltaTime")]] float& m_unscaledDeltaTime = unscaledDeltaTime;
    [[deprecated("Use frameCount")]]        uint64_t& m_frameCount     = frameCount;

    // Proxy — カテゴリごとに責務を分け、Script.hpp の肥大化を避ける。
    ScriptTransformProxy  transform   { this };
    ScriptInputProxy      input       { this };
    ScriptPhysicsProxy    physics     { this };
    ScriptAudioProxy      audio       { this };
    ScriptLightProxy      light       { this };
    ScriptCameraProxy     camera      { this };
    ScriptMaterialProxy   material    { this };
    ScriptParticleProxy   particle    { this };
    ScriptTrailProxy      trail       { this };
    ScriptMeshTrailProxy  meshTrail   { this };
    ScriptSceneProxy      scene       { this };
    ScriptAnimatorProxy   animator    { this };
    ScriptDebugProxy      debug       { this };
    GizmoProxy            gizmo;
    ScriptPostProcessProxy postprocess{ this };

    template<typename T>
    T* GetComponent() const;

    // Invoke / タイマー
    [[nodiscard]] InvokeHandle Invoke(std::function<void()> fn, float delay);
    [[nodiscard]] InvokeHandle InvokeRepeating(std::function<void()> fn, float delay, float interval);
    void FrameDelay(uint32_t n, std::function<void()> fn);
    void CancelInvoke();
    void CancelInvoke(InvokeHandle handle);

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
    void SetDeltaTime(float dt);
    void TickInvokes(float dt);
    void TickFrameDelays();
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
    friend struct ScriptPhysicsProxy;
    friend struct ScriptAudioProxy;
    friend struct ScriptLightProxy;
    friend struct ScriptCameraProxy;
    friend struct ScriptMaterialProxy;
    friend struct ScriptParticleProxy;
    friend struct ScriptTrailProxy;
    friend struct ScriptMeshTrailProxy;
    friend struct ScriptSceneProxy;
    friend struct ScriptAnimatorProxy;
    friend struct ScriptDebugProxy;
    friend struct ScriptPostProcessProxy;

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
    bool m_enableStateInitialized = false;
    bool m_lastEnabled = true;
    bool m_isTickingInvokes = false;
    static physics::World*     s_physicsWorld;
    static PrefabInstantiateFn s_instantiateFn;
};

} // namespace fbzz::scene