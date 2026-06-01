// FBZZ Engine
// Script.hpp | fbzz::scene
// ユーザースクリプト基底クラスとリフレクション補助
// GameObject にアタッチされる振る舞いの共通ライフサイクルを定義する。
// engine 側の Component とは分け、ScriptComponent が所有する。
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/ScriptProxy/ScriptAnimatorProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptAudioProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptCameraProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptDebugProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptInputProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptLightProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptMaterialProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptMemoryProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptParticleProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptPhysicsProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptPostProcessProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptSceneProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptTransformProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptUIProxy.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <cstdint>
#include <functional>
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
    // WHAT: self から見た接触法線。地面判定など、衝突相手がどちら側にあるかを Script で判断するために渡す。
    math::Vector3 contactNormal = math::Vector3::UP;
    math::Vector3 contactPoint = math::Vector3::ZERO;
    float contactDepth = 0.0f;
};

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
};

class Script {
public:
    virtual ~Script();

    virtual void OnAwake() {}                              // AddComponent / シーンロード直後に 1 度だけ呼ばれる
    virtual void OnStart() {}                              // 初回 Update 直前に 1 度だけ呼ばれる
    virtual void OnEnable() {}                             // enabled が false → true へ変化した直後に呼ばれる
    virtual void OnDisable() {}                            // enabled が true → false へ変化した直後に呼ばれる
    virtual void OnUpdate(float) {}                        // 毎フレーム呼ばれる (PhysicsSystem 前)
    virtual void OnFixedUpdate(float) {}                   // 固定物理ステップで呼ばれる想定のフック
    virtual void OnLateUpdate(float) {}                   // 毎フレーム呼ばれる (PhysicsSystem 後)
    virtual void OnPreRender() {}                          // カメラ描画直前に呼ぶための予約フック
    virtual void OnPostRender() {}                         // カメラ描画直後に呼ぶための予約フック
    virtual void OnCollisionEnter(const CollisionInfo&) {}
    virtual void OnCollisionStay(const CollisionInfo&) {}
    virtual void OnCollisionExit(const CollisionInfo&) {}
    virtual void OnTriggerEnter(const CollisionInfo&) {}
    virtual void OnTriggerStay(const CollisionInfo&) {}
    virtual void OnTriggerExit(const CollisionInfo&) {}
    virtual void OnDestroy() {}                            // GameObject 破棄時に呼ばれる
    // OnSetupRenderPasses — RenderSystem が RenderGraph 登録中に呼ぶ、Script 側の描画パス注入フック。
    virtual void OnSetupRenderPasses(renderer::RenderGraph&, RenderPassContext&) {}
    virtual void Reflect(IReflector&) {}                   // Inspector / Serializer からフィールドを列挙
    // GetScript<T>() の型判別に使う。派生クラスは TYPE_NAME static constexpr も定義する
    virtual const char* GetTypeName() const { return "Script"; }

    bool enabled = true;
    // ScriptSystem が毎フレーム注入する直近フレームの delta time。
    // WHY: OnLateUpdate や Invoke コールバックなど、OnUpdate(float) の引数を直接受け取れない処理でも同じ dt を参照できるようにする。
    float m_deltaTime = 0.0f;
    float m_time = 0.0f;
    uint64_t m_frameCount = 0;
    float m_unscaledDeltaTime = 0.0f;

    // ── Unity 風ショートハンド ──────────────────────────────────────────────
    // WHY: MonoBehaviour 相当の利便性を提供し、スクリプト記述量を削減する。
    //      m_gameObject / m_scene への直接参照は引き続き使用可能。

    // Proxy はカテゴリごとに責務を分け、Script.hpp の肥大化を避ける。
    // WHY: 旧 transform ポインタ互換は ScriptTransformProxy::operator-> で維持する。
    ScriptTransformProxy transform{ this };
    ScriptInputProxy input{ this };
    ScriptPhysicsProxy physics{ this };
    ScriptAudioProxy audio{ this };
    ScriptLightProxy light{ this };
    ScriptCameraProxy camera{ this };
    ScriptMaterialProxy material{ this };
    ScriptParticleProxy particle{ this };
    ScriptUIProxy ui{ this };
    ScriptSceneProxy scene{ this };
    ScriptAnimatorProxy animator{ this };
    ScriptDebugProxy debug{ this };
    ScriptPostProcessProxy postprocess{ this };
    ScriptMemoryProxy memory{ this };

    // Unity: GetComponent<T>()
    // WHY: template 定義は Scene.hpp 末尾で行う (GameObject が完全型である必要があるため)
    template<typename T>
    T* GetComponent() const;

    // Unity: GameObject.Find / FindWithTag
    GameObject* Find(const std::string& name)       const;
    GameObject* FindWithTag(const std::string& tag) const;
    // EntityID から GameObject を引く (ScriptComponent 間の相互参照に使う)
    GameObject* GetGameObject(EntityID id)          const;

    // Unity: Instantiate に相当する GO 生成
    GameObject& CreateGameObject(const std::string& name = "GameObject") const;

    // シーン内のメインカメラ GameObject を返す。見つからなければ nullptr
    GameObject* GetMainCameraObject() const;

    // Unity: Destroy(gameObject)
    static void Destroy(GameObject& go, float delay = 0.0f);

    // Invoke — コルーチンを使わずに「delay 秒後に 1 回だけ実行する」処理を予約する。
    // WHY: Step 1〜5 のシングルスレッド制約を保ちつつ、スクリプト側にタイマー用フラグを量産させないための最小 API。
    void Invoke(std::function<void()> fn, float delay);
    // InvokeRepeating — delay 秒後から interval 秒ごとに同じ処理を繰り返す。
    // interval <= 0 の場合は 1 回だけの Invoke と同じ扱いにして、無限ループを避ける。
    void InvokeRepeating(std::function<void()> fn, float delay, float interval);
    // FrameDelay — 秒ではなくフレーム数で遅延させる軽量タイマー。
    // WHY: コルーチンを導入せず、UI 演出や 1 フレーム待ちの用途を明示的に扱う。
    void FrameDelay(uint32_t n, std::function<void()> fn);
    // CancelInvoke — この Script が予約した Invoke / InvokeRepeating をすべて破棄する。
    void CancelInvoke();

    template<typename T>
    void Emit(const T& data) const;

    template<typename T>
    void On(std::function<void(const T&)> callback);

    // ── Animator ショートハンド ──────────────────────────────────────────────
    // 自 GameObject の AnimatorComponent に転送する。Animator が無ければ何もしない。
    void SetAnimatorFloat(std::string_view name, float v)   const;
    void SetAnimatorInt  (std::string_view name, int v)     const;
    void SetAnimatorBool (std::string_view name, bool v)    const;
    void SetAnimatorTrigger(std::string_view name)          const;
    bool IsAnimatorInState(std::string_view name)           const;

    // QueueRenderPass — OnSetupRenderPasses 内から RenderGraph 注入パスを登録する。
    // WHY: Script が RenderGraph の登録順や RenderSystem.cpp の内部構造を知らなくても、
    //      VFX / 水面 / カスタム描画を Scene に閉じて拡張できるようにする。
    void QueueRenderPass(UserRenderPassDesc desc) const;

    // GetShaderDescriptor — MaterialComponent の名前引き SetParam に渡す ShaderDescriptor を取得する。
    // WHY: MaterialComponent から ResourceManager::Active() 依存を除去し、リソース解決の責務を Script 側へ移す。
    const renderer::ShaderDescriptor* GetShaderDescriptor(std::string_view shaderPath) const;

    // ScriptSystem が各ライフサイクル呼び出しの前に設定する。
    // 派生クラスは m_scene / m_gameObject を介して Scene / GameObject にアクセスする。
    void SetContext(Scene* scene, GameObject* gameObject);

    // ScriptSystem 専用: enabled の差分を検出して OnEnable / OnDisable を呼ぶ。
    // WHY: enabled は Inspector や Script から直接書き換えられる public フラグのため、setter ではなく System 側の同期点で検知する。
    void SyncEnabledState();
    // ScriptSystem 専用: m_deltaTime を注入し、この Script が持つ Invoke タイマーを進める。
    void SetDeltaTime(float dt);
    void TickInvokes(float dt);
    void TickFrameDelays();
    // PhysicsSystem 専用: ScriptPhysicsProxy が参照する直近の physics::World を共有する。
    // WHY: ScriptSystem は World を引数に持たないため、物理同期の責務を持つ System から注入する。
    static void SetPhysicsWorld(physics::World* world);

protected:
    renderer::PostProcessSettings& GetRuntimePostProcessSettings();
    const renderer::PostProcessSettings* TryGetRuntimePostProcessSettings() const;
    void SetRuntimePostProcessSettings(const renderer::PostProcessSettings& settings);
    void ClearRuntimePostProcessSettings();

    Scene*      m_scene      = nullptr; // 非所有参照
    GameObject* m_gameObject = nullptr; // 非所有参照

private:
    friend struct ScriptTransformProxy;
    friend struct ScriptInputProxy;
    friend struct ScriptPhysicsProxy;
    friend struct ScriptAudioProxy;
    friend struct ScriptLightProxy;
    friend struct ScriptCameraProxy;
    friend struct ScriptMaterialProxy;
    friend struct ScriptParticleProxy;
    friend struct ScriptUIProxy;
    friend struct ScriptSceneProxy;
    friend struct ScriptAnimatorProxy;
    friend struct ScriptDebugProxy;
    friend struct ScriptPostProcessProxy;
    friend struct ScriptMemoryProxy;

    struct InvokeEntry {
        std::function<void()> fn;
        float remaining = 0.0f;
        float interval = 0.0f;
        bool repeating = false;
        bool canceled = false;
    };

    struct FrameDelayEntry {
        std::function<void()> fn;
        uint32_t remainingFrames = 0;
        bool canceled = false;
    };

    template<typename T>
    struct EventSubscription {
        Script* owner = nullptr;
        Scene* scene = nullptr;
        std::function<void(const T&)> callback;
    };

    template<typename T>
    static std::vector<EventSubscription<T>>& EventSubscriptions()
    {
        static std::vector<EventSubscription<T>> subscriptions;
        return subscriptions;
    }

    void CancelEventSubscriptions();
    std::vector<InvokeEntry> m_invokes;
    std::vector<FrameDelayEntry> m_frameDelays;
    std::vector<std::function<void(Script*)>> m_eventUnsubscribers;
    bool m_enableStateInitialized = false;
    bool m_lastEnabled = true;
    bool m_isTickingInvokes = false;
    static physics::World* s_physicsWorld;
};

template<typename T>
void Script::Emit(const T& data) const
{
    // WHAT: 型ごとに分離した Scene スコープの購読リストへ同期配信する。
    // WHY: Step 1〜5 のシングルスレッド前提では、キューイングより即時 dispatch の方が挙動を読みやすい。
    auto& subscriptions = EventSubscriptions<T>();
    for (auto& sub : subscriptions) {
        if (sub.owner && sub.scene == m_scene && sub.callback)
            sub.callback(data);
    }
}

template<typename T>
void Script::On(std::function<void(const T&)> callback)
{
    if (!callback) return;
    auto& subscriptions = EventSubscriptions<T>();
    subscriptions.push_back({ this, m_scene, std::move(callback) });
    m_eventUnsubscribers.push_back([](Script* owner) {
        auto& entries = EventSubscriptions<T>();
        for (auto& entry : entries) {
            if (entry.owner == owner)
                entry.owner = nullptr;
        }
    });
}

} // namespace fbzz::scene
