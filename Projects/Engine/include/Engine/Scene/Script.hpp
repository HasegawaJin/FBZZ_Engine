// FBZZ Engine
// Script.hpp | fbzz::scene
// ユーザースクリプト基底クラスとリフレクション補助
// GameObject にアタッチされる振る舞いの共通ライフサイクルを定義する。
// engine 側の Component とは分け、ScriptComponent が所有する。
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <string>
#include <string_view>

namespace fbzz::scene {

struct ColliderComponent;
struct Transform;
class GameObject;
class Scene;

} // namespace fbzz::scene

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
    virtual ~Script() = default;

    virtual void OnStart() {}                              // 初回 Update 直前に 1 度だけ呼ばれる
    virtual void OnUpdate(float) {}                        // 毎フレーム呼ばれる (PhysicsSystem 前)
    virtual void OnLateUpdate(float) {}                   // 毎フレーム呼ばれる (PhysicsSystem 後)
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

    // ── Unity 風ショートハンド ──────────────────────────────────────────────
    // WHY: MonoBehaviour 相当の利便性を提供し、スクリプト記述量を削減する。
    //      m_gameObject / m_scene への直接参照は引き続き使用可能。

    // Unity: transform (自 GameObject の Transform への直接ポインタ)
    // SetContext で設定される。OnStart より前は nullptr の可能性がある。
    Transform* transform = nullptr;

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

protected:
    renderer::PostProcessSettings& GetRuntimePostProcessSettings();
    const renderer::PostProcessSettings* TryGetRuntimePostProcessSettings() const;
    void SetRuntimePostProcessSettings(const renderer::PostProcessSettings& settings);
    void ClearRuntimePostProcessSettings();

    Scene*      m_scene      = nullptr; // 非所有参照
    GameObject* m_gameObject = nullptr; // 非所有参照
};

} // namespace fbzz::scene
