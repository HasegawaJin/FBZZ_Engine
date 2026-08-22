// FBZZ Engine
// ScriptProxies.cpp | fbzz::scene
// Script Proxy 群の転送処理
// Script 本体を肥大化させず、Component / System ごとの便利 API をここで具体化する。
#include <Engine/Scene/Script.hpp>
#include <limits>

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/PostProcessProfile.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Cursor.hpp>
#include <Engine/Scene/ScriptRuntime.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Input/Gamepad.hpp>
#include <Engine/Input/InputActionMap.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Renderer/Gizmo.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/PrefabPool.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneManager.hpp>
#include <Engine/Scene/ScriptEvent.hpp>
#include <Engine/Util/Easing.hpp>
#include <Engine/Util/Random.hpp>
#include <Engine/Util/SaveData.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/FoliageComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/Components/NavMeshSensorComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
#include <Engine/Scene/Components/ParticleForceField.hpp>
#include <Engine/Scene/Components/VolumetricCloudComponent.hpp>
#include <Engine/Scene/Components/SunMoonRenderer.hpp>
#include <Engine/Scene/Components/TerrainDetailComponent.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/WindZoneComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/UIAnimator.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/AtmosphericScatteringComponent.hpp>
#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/EnvironmentLightComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/LifetimeComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/ReflectionProbeComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <Engine/Scene/Components/VolumeComponent.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/CameraRigComponents.hpp>
#include <Engine/Scene/Components/UIControls.hpp>
#include <Engine/Scene/Components/AudioSpatialComponents.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/World.hpp>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <string>
#include <utility>

namespace fbzz::scene {
namespace {

constexpr float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;

template<typename T>
T* SelfComponent(const Script* script)
{
    return script ? script->GetComponent<T>() : nullptr;
}

template<typename T>
T* ObjectComponent(GameObject* go)
{
    return go && go->IsValid() ? go->GetComponent<T>() : nullptr;
}

physics::RigidBody* SelfRigidBody(const Script* script)
{
    auto* component = SelfComponent<RigidBodyComponent>(script);
    return component && component->enabled ? component->rigidBody.get() : nullptr;
}

physics::RigidBody* ObjectRigidBody(GameObject* go)
{
    auto* component = ObjectComponent<RigidBodyComponent>(go);
    return component && component->enabled ? component->rigidBody.get() : nullptr;
}

renderer::Camera BuildCameraFromComponent(const GameObject* gameObject, const CameraComponent* component)
{
    renderer::Camera camera;
    if (!gameObject || !component) {
        return camera;
    }

    camera.m_position = gameObject->transform.worldPosition;
    camera.m_rotation = gameObject->transform.worldRotation;
    camera.m_fovY     = component->fovY;
    camera.m_aspect   = component->aspectRatio;
    camera.m_near     = component->nearZ;
    camera.m_far      = component->farZ;
    return camera;
}

uint32_t ParseTextureSlot(std::string_view slot)
{
    if (slot.size() < 2 || slot[0] != 't') return UINT32_MAX;

    uint32_t value = 0;
    for (size_t i = 1; i < slot.size(); ++i) {
        if (slot[i] < '0' || slot[i] > '9') return UINT32_MAX;
        value = value * 10u + static_cast<uint32_t>(slot[i] - '0');
    }
    return value < 16u ? value : UINT32_MAX;
}

std::string_view CanonicalTextureProperty(std::string_view property)
{
    static constexpr std::string_view slots[] = {
        "albedo", "normal", "metallic", "emissive", "ao"
    };
    const uint32_t slot = ParseTextureSlot(property);
    return slot < std::size(slots) ? slots[slot] : property;
}

GameObject* FindGameObjectByCollider(Scene* scene, const physics::Collider* collider)
{
    if (!scene || !collider) return nullptr;

    auto matches = [collider](const ColliderComponent* component) {
        return component && component->collider.get() == collider;
    };

    for (auto& go : scene->GameObjects()) {
        if (matches(go.GetComponent<AabbColliderComponent>())) return &go;
        if (matches(go.GetComponent<BoxColliderComponent>())) return &go;
        if (matches(go.GetComponent<SphereColliderComponent>())) return &go;
        if (matches(go.GetComponent<CapsuleColliderComponent>())) return &go;
        if (matches(go.GetComponent<CylinderColliderComponent>())) return &go;
        if (matches(go.GetComponent<MeshColliderComponent>())) return &go;
        if (matches(go.GetComponent<ConvexHullColliderComponent>())) return &go;
    }
    return nullptr;
}

RaycastHit ToScriptHit(Scene* scene, const physics::World::RaycastHit& worldHit)
{
    RaycastHit hit{};
    hit.gameObject = FindGameObjectByCollider(scene, worldHit.collider);
    hit.collider = worldHit.collider;
    hit.point = worldHit.point;
    hit.normal = worldHit.normal;
    hit.distance = worldHit.distance;
    return hit;
}

} // namespace

bool ScriptMemoryProxy::InitializeFrame(std::size_t capacity) const
{
    if (capacity == 0) {
        return false;
    }

    // WHY: 既存領域を保持したまま容量だけ変えると旧ポインタの寿命が曖昧になるため、明示的に作り直す。
    if (m_frameAllocator.IsInitialized()) {
        m_frameAllocator.Shutdown();
    }

    return m_frameAllocator.Initialize(capacity);
}

void* ScriptMemoryProxy::AllocateFrame(std::size_t size, std::size_t alignment) const
{
    if (size == 0) {
        return nullptr;
    }

    // WHAT: 小規模 Script が OnAwake で明示初期化しなくても使えるよう、初回確保時に既定容量を用意する。
    if (!m_frameAllocator.IsInitialized() && !m_frameAllocator.Initialize(DEFAULT_FRAME_CAPACITY)) {
        return nullptr;
    }

    return m_frameAllocator.Allocate(size, alignment);
}

void ScriptMemoryProxy::ResetFrame() const
{
    if (!m_frameAllocator.IsInitialized()) {
        return;
    }

    m_frameAllocator.Reset();
}

void ScriptMemoryProxy::ShutdownFrame() const
{
    if (!m_frameAllocator.IsInitialized()) {
        return;
    }

    m_frameAllocator.Shutdown();
}

bool ScriptMemoryProxy::InitializePool(std::size_t blockSize,
                                       std::size_t blockCount,
                                       std::size_t alignment) const
{
    if (blockSize == 0 || blockCount == 0) {
        return false;
    }

    // WHY: PoolAllocator は固定 stride のため、用途変更時は古いブロックを破棄してから作り直す。
    if (m_poolAllocator.IsInitialized()) {
        m_poolAllocator.Shutdown();
    }

    return m_poolAllocator.Initialize(blockSize, blockCount, alignment);
}

void* ScriptMemoryProxy::AllocatePool(std::size_t size, std::size_t alignment) const
{
    if (size == 0 || !m_poolAllocator.IsInitialized()) {
        return nullptr;
    }

    return m_poolAllocator.Allocate(size, alignment);
}

void ScriptMemoryProxy::FreePool(void* ptr) const
{
    if (ptr == nullptr || !m_poolAllocator.IsInitialized()) {
        return;
    }

    m_poolAllocator.Free(ptr);
}

void ScriptMemoryProxy::ResetPool() const
{
    if (!m_poolAllocator.IsInitialized()) {
        return;
    }

    m_poolAllocator.Reset();
}

void ScriptMemoryProxy::ShutdownPool() const
{
    if (!m_poolAllocator.IsInitialized()) {
        return;
    }

    m_poolAllocator.Shutdown();
}

bool ScriptMemoryProxy::IsFrameInitialized() const
{
    return m_frameAllocator.IsInitialized();
}

bool ScriptMemoryProxy::IsPoolInitialized() const
{
    return m_poolAllocator.IsInitialized();
}

core::MemoryStats ScriptMemoryProxy::GetFrameStats() const
{
    if (!m_frameAllocator.IsInitialized()) {
        return {};
    }

    return m_frameAllocator.GetStats();
}

core::MemoryStats ScriptMemoryProxy::GetPoolStats() const
{
    if (!m_poolAllocator.IsInitialized()) {
        return {};
    }

    return m_poolAllocator.GetStats();
}

void ScriptMemoryProxy::BeginFrame() const
{
    if (!m_frameAllocator.IsInitialized()) {
        return;
    }

    m_frameAllocator.BeginFrame();
}

Transform* ScriptTransformProxy::Get() const
{
    return script && script->m_gameObject ? &script->m_gameObject->transform : nullptr;
}

// ── ローカル空間 property getter / setter ──────────────────────────────────
math::Vector3 ScriptTransformProxy::_GetPos() const
{
    auto* t = Get(); return t ? t->position : math::Vector3::ZERO;
}
void ScriptTransformProxy::_SetPos(const math::Vector3& v)
{
    if (auto* t = Get()) t->position = v;
}
math::Quaternion ScriptTransformProxy::_GetRot() const
{
    auto* t = Get(); return t ? t->rotation : math::Quaternion::Identity();
}
void ScriptTransformProxy::_SetRot(const math::Quaternion& v)
{
    if (auto* t = Get()) t->rotation = v;
}
math::Vector3 ScriptTransformProxy::_GetScl() const
{
    auto* t = Get(); return t ? t->scale : math::Vector3::ONE;
}
void ScriptTransformProxy::_SetScl(const math::Vector3& v)
{
    if (auto* t = Get()) t->scale = v;
}

// ── ワールド空間 property getter / setter ──────────────────────────────────
math::Vector3 ScriptTransformProxy::_GetWPos() const
{
    auto* t = Get(); return t ? t->worldPosition : math::Vector3::ZERO;
}
void ScriptTransformProxy::_SetWPos(const math::Vector3& v)
{
    if (auto* t = Get()) t->worldPosition = v;
}
math::Quaternion ScriptTransformProxy::_GetWRot() const
{
    auto* t = Get(); return t ? t->worldRotation : math::Quaternion::Identity();
}

// ── 算出値 ─────────────────────────────────────────────────────────────────
math::Vector3 ScriptTransformProxy::_GetFwd()   const { auto* t = Get(); return t ? t->Forward() : math::Vector3::FORWARD; }
math::Vector3 ScriptTransformProxy::_GetUp()    const { auto* t = Get(); return t ? t->Up()      : math::Vector3::UP; }
math::Vector3 ScriptTransformProxy::_GetRight() const { auto* t = Get(); return t ? t->Right()   : math::Vector3::RIGHT; }

// ── メソッド ───────────────────────────────────────────────────────────────
void ScriptTransformProxy::Translate(const math::Vector3& v) const
{
    if (auto* t = Get()) t->Translate(v);
}

void ScriptTransformProxy::Rotate(const math::Vector3& axis, float deg) const
{
    if (auto* t = Get()) {
        const auto delta = math::Quaternion::FromAxisAngle(axis.Normalized(), deg * DEG_TO_RAD);
        t->rotation = (delta * t->rotation).Normalized();
    }
}

void ScriptTransformProxy::LookAt(const math::Vector3& target) const
{
    if (auto* t = Get()) t->LookAt(target);
}

float ScriptTransformProxy::DistanceTo(const GameObject& other) const
{
    const auto* t = Get();
    return t ? (other.transform.worldPosition - t->worldPosition).Length() : 0.0f;
}

math::Vector3 ScriptTransformProxy::DirectionTo(const GameObject& other) const
{
    const auto* t = Get();
    return t ? (other.transform.worldPosition - t->worldPosition).Normalized() : math::Vector3::ZERO;
}

bool ScriptInputProxy::GetKey(input::KeyCode key) const { return input::Input::KeyHeld(key); }
bool ScriptInputProxy::GetKeyDown(input::KeyCode key) const { return input::Input::KeyDown(key); }
bool ScriptInputProxy::GetKeyUp(input::KeyCode key) const { return input::Input::KeyUp(key); }

math::Vector2 ScriptInputProxy::GetMouseDelta() const { return input::Input::MouseDelta(); }
math::Vector2 ScriptInputProxy::GetMousePosition() const { return input::Input::MousePosition(); }
float ScriptInputProxy::GetMouseScrollDelta() const { return input::Input::MouseScrollDelta(); }
bool ScriptInputProxy::MouseButton(MouseBtn btn) const { return input::Input::MouseButton(static_cast<int>(btn)); }
bool ScriptInputProxy::MouseButtonDown(MouseBtn btn) const { return input::Input::MouseButtonDown(static_cast<int>(btn)); }
bool ScriptInputProxy::MouseButtonUp(MouseBtn btn) const { return input::Input::MouseButtonUp(static_cast<int>(btn)); }

// ── アクション層 ─────────────────────────────────────────────────────────────

bool ScriptInputProxy::GetAction(std::string_view name) const
{
    return input::InputActionMap::GetAction(name);
}
bool ScriptInputProxy::GetActionDown(std::string_view name) const
{
    return input::InputActionMap::GetActionDown(name);
}
bool ScriptInputProxy::GetActionUp(std::string_view name) const
{
    return input::InputActionMap::GetActionUp(name);
}
float ScriptInputProxy::GetActionAxis(std::string_view name) const
{
    return input::InputActionMap::GetAxis(name);
}

math::Vector2 ScriptInputProxy::GetMoveAxis() const
{
    return input::InputActionMap::GetAxis2D("MoveX", "MoveY");
}
math::Vector2 ScriptInputProxy::GetLookAxis() const
{
    return input::InputActionMap::GetAxis2D("LookX", "LookY");
}

// ── ゲームパッド直接アクセス ─────────────────────────────────────────────────

namespace {
// pad = -1 を「接続中の最初のパッド」へ解決する。
// 1 台も接続されていない場合は 0 を返す (未接続スロットへの問い合わせは false / 0)。
int ResolveScriptPad(int pad)
{
    if (pad >= 0) return pad;
    const int first = input::Gamepad::GetFirstConnectedPad();
    return first >= 0 ? first : 0;
}
} // namespace

bool ScriptInputProxy::GetPadButton(input::GamepadButton button, int pad) const
{
    return input::Gamepad::ButtonHeld(button, ResolveScriptPad(pad));
}
bool ScriptInputProxy::GetPadButtonDown(input::GamepadButton button, int pad) const
{
    return input::Gamepad::ButtonDown(button, ResolveScriptPad(pad));
}
bool ScriptInputProxy::GetPadButtonUp(input::GamepadButton button, int pad) const
{
    return input::Gamepad::ButtonUp(button, ResolveScriptPad(pad));
}
float ScriptInputProxy::GetPadAxis(input::GamepadAxis axis, int pad) const
{
    return input::Gamepad::Axis(axis, ResolveScriptPad(pad));
}
bool ScriptInputProxy::IsPadConnected(int pad) const
{
    if (pad >= 0) return input::Gamepad::IsConnected(pad);
    return input::Gamepad::GetFirstConnectedPad() >= 0;
}

void ScriptInputProxy::SetVibration(float lowFrequency, float highFrequency,
                                    float durationSeconds, int pad) const
{
    input::Gamepad::SetVibration(lowFrequency, highFrequency, durationSeconds,
                                 ResolveScriptPad(pad));
}
void ScriptInputProxy::StopVibration(int pad) const
{
    input::Gamepad::StopVibration(ResolveScriptPad(pad));
}

void ScriptCursorProxy::SetVisible(bool visible) const
{
    core::Cursor::SetVisible(visible);
}

bool ScriptCursorProxy::IsVisible() const
{
    return core::Cursor::IsVisible();
}

void ScriptCursorProxy::SetLockMode(CursorLockMode mode) const
{
    core::Cursor::SetLockMode(mode);
}

CursorLockMode ScriptCursorProxy::GetLockMode() const
{
    return core::Cursor::GetLockMode();
}

void ScriptCursorProxy::ResetForEditor() const
{
    core::Cursor::ResetForEditor();
}

void ScriptApplicationProxy::Quit() const
{
    core::Application::Get().Quit();
}

bool ScriptApplicationProxy::IsRunning() const
{
    return core::Application::Get().IsRunning();
}

uint32_t ScriptApplicationProxy::GetWindowWidth() const
{
    return core::Application::Get().GetWindowWidth();
}

uint32_t ScriptApplicationProxy::GetWindowHeight() const
{
    return core::Application::Get().GetWindowHeight();
}

float ScriptTimeProxy::DeltaTime() const { return fbzz::Time::deltaTime; }
float ScriptTimeProxy::UnscaledDeltaTime() const { return fbzz::Time::unscaledDeltaTime; }
float ScriptTimeProxy::FixedDeltaTime() const { return fbzz::Time::fixedDeltaTime; }
float ScriptTimeProxy::Time() const { return fbzz::Time::time; }
float ScriptTimeProxy::UnscaledTime() const { return fbzz::Time::unscaledTime; }
uint64_t ScriptTimeProxy::FrameCount() const { return fbzz::Time::frameCount; }

void ScriptTimeProxy::SetTimeScale(float scale) const { fbzz::Time::SetTimeScale(scale); }
float ScriptTimeProxy::GetTimeScale() const { return fbzz::Time::GetTimeScale(); }
void ScriptTimeProxy::SetTargetFps(int fps) const { fbzz::Time::SetTargetFps(fps); }
int ScriptTimeProxy::GetTargetFps() const { return fbzz::Time::GetTargetFps(); }

void ScriptPhysicsProxy::AddForce(const math::Vector3& v) const
{
    if (auto* rb = SelfRigidBody(script)) rb->ApplyForce(v);
}

void ScriptPhysicsProxy::AddImpulse(const math::Vector3& v) const
{
    if (auto* rb = SelfRigidBody(script)) rb->ApplyImpulse(v);
}

void ScriptPhysicsProxy::AddForceAtPoint(const math::Vector3& force, const math::Vector3& worldPoint) const
{
    if (auto* rb = SelfRigidBody(script)) rb->ApplyForceAtPoint(force, worldPoint);
}

bool ScriptPhysicsProxy::HasRigidBody() const
{
    return SelfRigidBody(script) != nullptr;
}

bool ScriptPhysicsProxy::HasRigidBody(GameObject* go) const
{
    return ObjectRigidBody(go) != nullptr;
}

float ScriptPhysicsProxy::GetMass() const
{
    if (auto* rb = SelfRigidBody(script)) return rb->GetMass();
    return 0.0f;
}

void ScriptPhysicsProxy::SetMass(float mass) const
{
    if (auto* rb = SelfRigidBody(script)) rb->SetMass(mass);
}

void ScriptPhysicsProxy::SetStatic(bool isStatic) const
{
    if (auto* rb = SelfRigidBody(script)) {
        rb->m_isStatic = isStatic;
        rb->SetMass(rb->GetMass());
    }
}

float ScriptPhysicsProxy::GetGravityScale() const
{
    if (auto* rb = SelfRigidBody(script)) return rb->m_gravityScale;
    return 1.0f;
}

void ScriptPhysicsProxy::SetGravityScale(float scale) const
{
    if (auto* rb = SelfRigidBody(script)) rb->m_gravityScale = scale;
}

math::Vector3 ScriptPhysicsProxy::GetWorldGravity() const
{
    if (!Script::s_physicsWorld) return { 0.0f, -9.81f, 0.0f };
    return Script::s_physicsWorld->GetGravity();
}

void ScriptPhysicsProxy::SetVelocity(const math::Vector3& v) const
{
    if (auto* rb = SelfRigidBody(script)) rb->SetVelocity(v);
}

math::Vector3 ScriptPhysicsProxy::GetVelocity() const
{
    if (auto* rb = SelfRigidBody(script)) return rb->GetVelocity();
    return math::Vector3::ZERO;
}

void ScriptPhysicsProxy::SetVelocity(GameObject* go, const math::Vector3& v) const
{
    if (auto* rb = ObjectRigidBody(go)) rb->SetVelocity(v);
}

math::Vector3 ScriptPhysicsProxy::GetVelocity(GameObject* go) const
{
    if (auto* rb = ObjectRigidBody(go)) return rb->GetVelocity();
    return math::Vector3::ZERO;
}

void ScriptPhysicsProxy::AddImpulse(GameObject* go, const math::Vector3& v) const
{
    if (auto* rb = ObjectRigidBody(go)) rb->ApplyImpulse(v);
}

float ScriptPhysicsProxy::GetMass(GameObject* go) const
{
    if (auto* rb = ObjectRigidBody(go)) return rb->GetMass();
    return 0.0f;
}

void ScriptPhysicsProxy::SetAngularVelocity(const math::Vector3& v) const
{
    if (auto* rb = SelfRigidBody(script)) rb->SetAngularVelocity(v);
}

math::Vector3 ScriptPhysicsProxy::GetAngularVelocity() const
{
    if (auto* rb = SelfRigidBody(script)) return rb->GetAngularVelocity();
    return math::Vector3::ZERO;
}

void ScriptPhysicsProxy::AddTorque(const math::Vector3& v) const
{
    if (auto* rb = SelfRigidBody(script)) rb->ApplyTorque(v);
}

void ScriptPhysicsProxy::SetFreezePosition(bool x, bool y, bool z) const
{
    if (auto* rb = SelfRigidBody(script)) rb->SetFreezePosition({ x, y, z });
}

void ScriptPhysicsProxy::SetFreezeRotation(bool x, bool y, bool z) const
{
    if (auto* rb = SelfRigidBody(script)) rb->SetFreezeRotation({ x, y, z });
}

bool ScriptPhysicsProxy::Raycast(const math::Vector3& origin, const math::Vector3& dir, float dist, RaycastHit& hit) const
{
    hit = {};
    if (!script || !Script::s_physicsWorld) return false;

    physics::World::RaycastHit worldHit{};
    if (!Script::s_physicsWorld->Raycast(origin, dir, dist, worldHit))
        return false;

    hit = ToScriptHit(script->m_scene, worldHit);
    return true;
}

std::vector<RaycastHit> ScriptPhysicsProxy::RaycastAll(const math::Vector3& origin, const math::Vector3& dir, float dist) const
{
    std::vector<RaycastHit> hits;
    if (!script || !Script::s_physicsWorld) return hits;

    const auto worldHits = Script::s_physicsWorld->RaycastAll(origin, dir, dist);
    hits.reserve(worldHits.size());
    for (const auto& worldHit : worldHits)
        hits.push_back(ToScriptHit(script->m_scene, worldHit));
    return hits;
}

bool ScriptPhysicsProxy::SphereCast(const math::Vector3& center, float radius, const math::Vector3& dir, float dist, RaycastHit& hit) const
{
    hit = {};
    if (!script || !Script::s_physicsWorld) return false;

    physics::World::RaycastHit worldHit{};
    if (!Script::s_physicsWorld->SphereCast(center, radius, dir, dist, worldHit))
        return false;

    hit = ToScriptHit(script->m_scene, worldHit);
    return true;
}

std::vector<GameObject*> ScriptPhysicsProxy::OverlapSphere(const math::Vector3& center, float radius) const
{
    std::vector<GameObject*> result;
    if (!script || !Script::s_physicsWorld) return result;

    const auto overlaps = Script::s_physicsWorld->OverlapSphere(center, radius);
    result.reserve(overlaps.size());
    for (const auto* inst : overlaps) {
        auto* go = inst ? FindGameObjectByCollider(script->m_scene, inst->collider) : nullptr;
        if (go && std::find(result.begin(), result.end(), go) == result.end())
            result.push_back(go);
    }
    return result;
}

namespace {
ColliderComponent* SelfAnyCollider(const Script* script)
{
    if (!script) return nullptr;
    if (auto* c = script->GetComponent<BoxColliderComponent>()) return c;
    if (auto* c = script->GetComponent<AabbColliderComponent>()) return c;
    if (auto* c = script->GetComponent<SphereColliderComponent>()) return c;
    if (auto* c = script->GetComponent<CapsuleColliderComponent>()) return c;
    if (auto* c = script->GetComponent<CylinderColliderComponent>()) return c;
    if (auto* c = script->GetComponent<MeshColliderComponent>()) return c;
    if (auto* c = script->GetComponent<ConvexHullColliderComponent>()) return c;
    if (auto* c = script->GetComponent<TerrainColliderComponent>()) return c;
    return nullptr;
}
} // namespace

void ScriptColliderProxy::SetEnabled(bool enabled) const
{
    if (auto* c = SelfAnyCollider(script)) c->SetEnabled(enabled);
}

void ScriptColliderProxy::SetTrigger(bool trigger) const
{
    if (auto* c = SelfAnyCollider(script)) c->SetTrigger(trigger);
}

void ScriptColliderProxy::SetCenter(const math::Vector3& center) const
{
    if (auto* c = SelfAnyCollider(script)) c->SetCenter(center);
}

void ScriptColliderProxy::SetBoxSize(const math::Vector3& size) const
{
    if (!script || !script->m_gameObject) return;
    if (auto* c = script->m_gameObject->GetComponent<BoxColliderComponent>()) c->SetSize(size);
    if (auto* c = script->m_gameObject->GetComponent<AabbColliderComponent>()) c->SetSize(size);
}

void ScriptColliderProxy::SetSphereRadius(float radius) const
{
    if (!script || !script->m_gameObject) return;
    if (auto* c = script->m_gameObject->GetComponent<SphereColliderComponent>()) c->SetRadius(radius);
}

void ScriptColliderProxy::SetCapsule(float radius, float halfHeight) const
{
    if (!script || !script->m_gameObject) return;
    if (auto* c = script->m_gameObject->GetComponent<CapsuleColliderComponent>())
        c->SetCapsule(radius, halfHeight);
}

void ScriptColliderProxy::SetCylinder(float radius, float halfHeight) const
{
    if (!script || !script->m_gameObject) return;
    if (auto* c = script->m_gameObject->GetComponent<CylinderColliderComponent>())
        c->SetCylinder(radius, halfHeight);
}

void ScriptColliderProxy::SetMesh(std::string_view meshPath, int meshIndex) const
{
    if (!script || !script->m_gameObject) return;
    if (auto* c = script->m_gameObject->GetComponent<MeshColliderComponent>())
        c->SetMesh(std::string(meshPath), meshIndex);
    if (auto* c = script->m_gameObject->GetComponent<ConvexHullColliderComponent>())
        c->SetMesh(std::string(meshPath), meshIndex);
}

void ScriptColliderProxy::SetFriction(float staticFriction, float dynamicFriction) const
{
    auto* c = SelfAnyCollider(script);
    if (!c) return;
    // 共有アセット参照を外してから書く。残したままだと次のフレームで
    // ResolvePhysicsMaterial() に上書きされ、書いた値が消える。
    c->physicsMaterialPath.clear();
    c->material.staticFriction = staticFriction;
    c->material.dynamicFriction = dynamicFriction;
}

void ScriptColliderProxy::SetRestitution(float restitution) const
{
    auto* c = SelfAnyCollider(script);
    if (!c) return;
    c->physicsMaterialPath.clear();
    c->material.restitution = restitution;
}

void ScriptColliderProxy::SetDensity(float density) const
{
    auto* c = SelfAnyCollider(script);
    if (!c) return;
    c->physicsMaterialPath.clear();
    c->material.density = density;
}

void ScriptColliderProxy::SetPhysicsMaterial(std::string_view assetPath) const
{
    if (auto* c = SelfAnyCollider(script))
        c->SetPhysicsMaterialPath(std::string(assetPath));
}

std::string ScriptColliderProxy::GetPhysicsMaterial() const
{
    const auto* c = SelfAnyCollider(script);
    return c ? c->physicsMaterialPath : std::string{};
}

float ScriptColliderProxy::GetRestitution() const
{
    const auto* c = SelfAnyCollider(script);
    return c ? c->material.restitution : 0.0f;
}

float ScriptColliderProxy::GetStaticFriction() const
{
    const auto* c = SelfAnyCollider(script);
    return c ? c->material.staticFriction : 0.0f;
}

float ScriptColliderProxy::GetDynamicFriction() const
{
    const auto* c = SelfAnyCollider(script);
    return c ? c->material.dynamicFriction : 0.0f;
}

float ScriptColliderProxy::GetDensity() const
{
    const auto* c = SelfAnyCollider(script);
    return c ? c->material.density : 0.0f;
}

bool ScriptColliderProxy::ApplyPhysicsMaterialPreset(std::string_view presetName) const
{
    auto* c = SelfAnyCollider(script);
    if (!c) return false;
    // string_view は終端 NUL を保証しないため、C API へ渡す前に string 化する。
    const std::string name(presetName);
    const auto* preset = physics::PhysicsMaterial::FindPreset(name.c_str());
    if (!preset) {
        FBZZ_LOG_WARN("Unknown physics material preset: %s", name.c_str());
        return false;
    }
    c->SetMaterial(*preset);
    return true;
}

void ScriptAudioProxy::Play(std::string_view clipPath) const
{
    if (auto* audio = SelfComponent<AudioSourceComponent>(script)) {
        audio->clipPath       = std::string(clipPath);
        audio->enabled        = true;
        audio->m_pendingPlay  = true;
        audio->m_pendingStop  = false;
        audio->m_pendingPause = false;
    }
}

void ScriptAudioProxy::Stop() const
{
    if (auto* audio = SelfComponent<AudioSourceComponent>(script)) {
        audio->m_pendingStop  = true;
        audio->m_pendingPlay  = false;
        audio->m_pendingPause = false;
    }
}

void ScriptAudioProxy::Pause() const
{
    if (auto* audio = SelfComponent<AudioSourceComponent>(script)) {
        audio->m_pendingPause = true;
        audio->m_pendingPlay  = false;
    }
}

void ScriptAudioProxy::SetVolume(float v) const
{
    if (auto* audio = SelfComponent<AudioSourceComponent>(script))
        audio->volume = std::clamp(v, 0.0f, 1.0f);
}

void ScriptAudioProxy::SetLoop(bool loop) const
{
    if (auto* audio = SelfComponent<AudioSourceComponent>(script))
        audio->loop = loop;
}

bool ScriptAudioProxy::IsPlaying() const
{
    const auto* audio = SelfComponent<AudioSourceComponent>(script);
    return audio && audio->m_isPlaying;
}

float ScriptAudioProxy::GetVolume() const
{
    const auto* audio = SelfComponent<AudioSourceComponent>(script);
    return audio ? audio->volume : 0.0f;
}

void ScriptAudioProxy::SetPitch(float pitch) const
{
    if (auto* audio = SelfComponent<AudioSourceComponent>(script))
        audio->pitch = std::clamp(pitch, 0.01f, 4.0f);
}

void ScriptAudioProxy::SetSpatialBlend(float blend) const
{
    if (auto* audio = SelfComponent<AudioSourceComponent>(script))
        audio->spatialBlend = std::clamp(blend, 0.0f, 1.0f);
}

void ScriptAudioProxy::Set3DDistances(float minDistance, float maxDistance, float rolloff) const
{
    if (auto* audio = SelfComponent<AudioSourceComponent>(script)) {
        audio->minDistance = (std::max)(minDistance, 0.0f);
        audio->maxDistance = (std::max)(maxDistance, audio->minDistance + 0.001f);
        audio->rolloffFactor = (std::max)(rolloff, 0.01f);
    }
}

void ScriptAudioProxy::PlayOneShot(std::string_view clipPath) const
{
    if (auto* audio = SelfComponent<AudioSourceComponent>(script)) {
        audio->m_oneShotPath   = std::string(clipPath);
        audio->m_pendingOneShot = true;
    }
}

void ScriptLightProxy::SetColor(const math::Vector3& color) const
{
    if (auto* l = SelfComponent<LightComponent>(script)) l->color = color;
}

void ScriptLightProxy::SetType(LightType type) const
{
    if (auto* l = SelfComponent<LightComponent>(script))
        l->type = static_cast<LightComponent::Type>(static_cast<int>(type));
}

void ScriptLightProxy::SetIntensity(float intensity) const
{
    if (auto* l = SelfComponent<LightComponent>(script)) l->intensity = intensity;
}

void ScriptLightProxy::SetRange(float range) const
{
    if (auto* l = SelfComponent<LightComponent>(script)) l->range = range;
}

void ScriptLightProxy::SetInnerCone(float degrees) const
{
    if (auto* l = SelfComponent<LightComponent>(script)) l->innerCone = degrees;
}

void ScriptLightProxy::SetOuterCone(float degrees) const
{
    if (auto* l = SelfComponent<LightComponent>(script)) l->outerCone = degrees;
}

void ScriptLightProxy::SetEnabled(bool enabled) const
{
    if (auto* l = SelfComponent<LightComponent>(script)) l->enabled = enabled;
}

void ScriptCameraProxy::SetAsMain() const
{
    if (!script || !script->m_scene || !script->m_gameObject) return;
    for (auto& go : script->m_scene->GameObjects()) {
        if (auto* cam = go.GetComponent<CameraComponent>())
            cam->isMain = false;
    }
    if (auto* cam = script->m_gameObject->GetComponent<CameraComponent>())
        cam->isMain = true;
}

void ScriptCameraProxy::SetFOV(float fovY) const
{
    if (auto* cam = SelfComponent<CameraComponent>(script)) cam->fovY = fovY;
}

void ScriptCameraProxy::SetAspectRatio(float aspectRatio) const
{
    if (auto* cam = SelfComponent<CameraComponent>(script)) cam->aspectRatio = (std::max)(aspectRatio, 0.0001f);
}

void ScriptCameraProxy::SetNearFar(float nearZ, float farZ) const
{
    if (auto* cam = SelfComponent<CameraComponent>(script)) {
        cam->nearZ = nearZ;
        cam->farZ = farZ;
    }
}

void ScriptCameraProxy::SetCullingMask(fbzz::LayerMask mask) const
{
    if (auto* cam = SelfComponent<CameraComponent>(script)) cam->cullingMask = mask;
}

math::Vector3 ScriptCameraProxy::WorldToScreenPoint(const math::Vector3& worldPos) const
{
    const auto* gameObject = script ? script->m_gameObject : nullptr;
    const renderer::Camera camera = BuildCameraFromComponent(gameObject, SelfComponent<CameraComponent>(script));
    const auto rt = ScriptRuntime::GetCurrent();
    const float width  = static_cast<float>((std::max)(rt.viewportWidth,  1u));
    const float height = static_cast<float>((std::max)(rt.viewportHeight, 1u));

    const math::Vector4 clip = camera.GetViewProjection() * math::Vector4(worldPos, 1.0f);
    if (std::abs(clip.w) <= 0.000001f) {
        return { 0.0f, 0.0f, 0.0f };
    }

    const float invW = 1.0f / clip.w;
    const float ndcX = clip.x * invW;
    const float ndcY = clip.y * invW;
    const float ndcZ = clip.z * invW;

    // WHAT: 左上原点の pixel 座標へ変換する。z は DirectX depth range の 0..1 を返す。
    return {
        (ndcX * 0.5f + 0.5f) * width,
        (0.5f - ndcY * 0.5f) * height,
        ndcZ
    };
}

math::Vector3 ScriptCameraProxy::ScreenToWorldPoint(const math::Vector3& screenPos) const
{
    const auto* gameObject = script ? script->m_gameObject : nullptr;
    const renderer::Camera camera = BuildCameraFromComponent(gameObject, SelfComponent<CameraComponent>(script));
    const auto rt = ScriptRuntime::GetCurrent();
    const float width  = static_cast<float>((std::max)(rt.viewportWidth,  1u));
    const float height = static_cast<float>((std::max)(rt.viewportHeight, 1u));

    const float ndcX = (screenPos.x / width) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (screenPos.y / height) * 2.0f;
    const float ndcZ = std::clamp(screenPos.z, 0.0f, 1.0f);

    const math::Matrix4 invVP = math::Matrix4::Inverse(camera.GetViewProjection());
    const math::Vector4 world = invVP * math::Vector4(ndcX, ndcY, ndcZ, 1.0f);
    if (std::abs(world.w) <= 0.000001f) {
        return math::Vector3::ZERO;
    }
    return world.XYZ() * (1.0f / world.w);
}

float ScriptCameraProxy::GetFOV() const
{
    const auto* cam = SelfComponent<CameraComponent>(script);
    return cam ? cam->fovY : 60.0f;
}

float ScriptCameraProxy::GetNearZ() const
{
    const auto* cam = SelfComponent<CameraComponent>(script);
    return cam ? cam->nearZ : 0.1f;
}

float ScriptCameraProxy::GetFarZ() const
{
    const auto* cam = SelfComponent<CameraComponent>(script);
    return cam ? cam->farZ : 1000.0f;
}

bool ScriptCameraProxy::IsVisible(const math::Vector3& worldPos) const
{
    const auto screen = WorldToScreenPoint(worldPos);
    if (screen.z <= 0.0f) return false;
    const auto rt = ScriptRuntime::GetCurrent();
    const float w = static_cast<float>((std::max)(rt.viewportWidth,  1u));
    const float h = static_cast<float>((std::max)(rt.viewportHeight, 1u));
    return screen.x >= 0.0f && screen.x <= w && screen.y >= 0.0f && screen.y <= h;
}

Ray ScriptCameraProxy::ScreenPointToRay(float screenX, float screenY) const
{
    const math::Vector3 nearPt = ScreenToWorldPoint({ screenX, screenY, 0.0f });
    const math::Vector3 farPt  = ScreenToWorldPoint({ screenX, screenY, 1.0f });
    const math::Vector3 dir    = (farPt - nearPt).Normalized();
    return { nearPt, dir };
}

// スロットを所有する MaterialComponent を解決する。
// SetEnabled のようなコンポーネント全体の操作はこちらを使う。
MaterialComponent* MaterialInstance::ResolveOwner(bool ensure) const
{
    if (!m_script || !m_script->m_scene) return nullptr;
    GameObject* object = m_target.IsValid()
        ? m_target.Resolve(*m_script->m_scene)
        : m_script->m_gameObject;
    if (!object || !object->IsValid()) return nullptr;
    if (auto* material = object->GetComponent<MaterialComponent>())
        return material;
    return ensure ? &object->AddComponent<MaterialComponent>() : nullptr;
}

// m_slot が指す MaterialSlot を解決する。
// WHY: SkinnedMeshRenderer が 1 GameObject = モデル全体を描くようになり、
//      submesh ごとのマテリアルはスロットとして同じ GameObject に並ぶ。
//      スクリプトから「バイザーだけ光らせる」ような操作をスロット番号で行えるようにする。
//      ensure=true のときは必要な数までスロットを伸ばす。
void* MaterialInstance::ResolveComponent(bool ensure) const
{
    MaterialComponent* owner = ResolveOwner(ensure);
    if (!owner) return nullptr;
    if (m_slot >= owner->SlotCount()) {
        if (!ensure) return nullptr;
        owner->ResizeSlots(static_cast<size_t>(m_slot) + 1u);
    }
    return &owner->RawSlotAt(static_cast<size_t>(m_slot));
}

bool MaterialInstance::IsValid() const
{
    return ResolveComponent(false) != nullptr;
}

bool MaterialInstance::HasProperty(MaterialPropertyId property) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || !property.IsValid() || !material->EnsureMaterialAsset()) return false;
    const auto* asset = asset::AssetManager::GetMaterial(material->materialAsset);
    if (!asset) return false;
    if (asset->params.contains(std::string(property.name)) ||
        asset->textures.contains(std::string(property.name)))
        return true;
    if (m_script) {
        const auto* descriptor = m_script->GetShaderDescriptor(material->GetShaderPath());
        if (descriptor && descriptor->FindVar(property.name))
            return true;
        if (descriptor) {
            for (const auto& texture : descriptor->textures)
                if (texture.name == property.name) return true;
        }
    }
    return ParseTextureSlot(property.name) < 5u;
}

const std::string& MaterialInstance::ResolvePropertyName(
    void* component,
    MaterialPropertyId property) const
{
    auto& material = *static_cast<MaterialSlot*>(component);
    auto [it, inserted] = material.propertyNameCache.try_emplace(
        property.hash, std::string(property.name));
    if (!inserted && it->second != property.name)
        it->second.assign(property.name);
    return it->second;
}

bool MaterialInstance::ValidateProperty(
    MaterialPropertyId property,
    PropertyKind kind) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || !property.IsValid() || !material->EnsureMaterialAsset())
        return false;
    const auto* descriptor = m_script
        ? m_script->GetShaderDescriptor(material->GetShaderPath())
        : nullptr;
    if (material->propertyValidationDescriptor != descriptor) {
        material->propertyValidationDescriptor = descriptor;
        material->propertyValidationCache.clear();
    }
    const uint8_t validationBit = static_cast<uint8_t>(
        uint8_t{1} << static_cast<uint8_t>(kind));
    const auto cachedName = material->propertyNameCache.find(property.hash);
    if (cachedName != material->propertyNameCache.end() &&
        cachedName->second != property.name) {
        material->propertyValidationCache.erase(property.hash);
    }
    if ((material->propertyValidationCache[property.hash] & validationBit) != 0)
        return true;
    if (!descriptor) {
        const auto* shared = asset::AssetManager::GetMaterial(material->materialAsset);
        bool valid = false;
        if (shared) {
            if (kind == PropertyKind::Texture) {
                valid = shared->textures.contains(
                    std::string(CanonicalTextureProperty(property.name)));
            } else {
                const auto parameter = shared->params.find(std::string(property.name));
                const size_t requiredComponents =
                    kind == PropertyKind::Vector3 ? 3u :
                    kind == PropertyKind::Vector4 ? 4u : 1u;
                valid = parameter != shared->params.end() &&
                        parameter->second.size() >= requiredComponents;
            }
        }
        if (valid) material->propertyValidationCache[property.hash] |= validationBit;
        return valid;
    }

    if (kind == PropertyKind::Texture) {
        const std::string_view canonicalName = CanonicalTextureProperty(property.name);
        for (const auto& texture : descriptor->textures) {
            if (texture.name == property.name ||
                texture.name == canonicalName ||
                texture.slot == ParseTextureSlot(property.name)) {
                material->propertyValidationCache[property.hash] |= validationBit;
                return true;
            }
        }
        return false;
    }

    const renderer::ShaderVarDesc* variable = descriptor->FindVar(property.name);
    if (!variable) return false;
    bool valid = false;
    switch (kind) {
    case PropertyKind::Float:
        valid = variable->columns == 1 &&
                variable->varType == renderer::ShaderVarType::Float;
        break;
    case PropertyKind::Int:
        valid = variable->columns == 1 &&
                variable->varType != renderer::ShaderVarType::Float;
        break;
    case PropertyKind::Vector3:
        valid = variable->columns == 3 &&
                variable->varType == renderer::ShaderVarType::Float;
        break;
    case PropertyKind::Vector4:
        valid = variable->columns == 4 &&
                variable->varType == renderer::ShaderVarType::Float;
        break;
    default:
        break;
    }
    if (valid) material->propertyValidationCache[property.hash] |= validationBit;
    return valid;
}

bool MaterialInstance::SetFloat(MaterialPropertyId property, float value) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || !ValidateProperty(property, PropertyKind::Float)) {
        FBZZ_LOG_WARN("Material property not found: %.*s",
                      static_cast<int>(property.name.size()), property.name.data());
        return false;
    }
    material->paramOverrides[ResolvePropertyName(material, property)] = { value };
    return true;
}

bool MaterialInstance::SetInt(MaterialPropertyId property, int value) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || !ValidateProperty(property, PropertyKind::Int)) {
        FBZZ_LOG_WARN("Material int property not found or type mismatch: %.*s",
                      static_cast<int>(property.name.size()), property.name.data());
        return false;
    }
    material->paramOverrides[ResolvePropertyName(material, property)] = {
        static_cast<float>(value)
    };
    return true;
}

bool MaterialInstance::SetVector3(MaterialPropertyId property, const math::Vector3& value) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || !ValidateProperty(property, PropertyKind::Vector3)) {
        FBZZ_LOG_WARN("Material Vector3 property not found or type mismatch: %.*s",
                      static_cast<int>(property.name.size()), property.name.data());
        return false;
    }
    material->paramOverrides[ResolvePropertyName(material, property)] = {
        value.x, value.y, value.z
    };
    return true;
}

bool MaterialInstance::SetVector4(MaterialPropertyId property, const math::Vector4& value) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || !ValidateProperty(property, PropertyKind::Vector4)) {
        FBZZ_LOG_WARN("Material Vector4 property not found or type mismatch: %.*s",
                      static_cast<int>(property.name.size()), property.name.data());
        return false;
    }
    material->paramOverrides[ResolvePropertyName(material, property)] = {
        value.x, value.y, value.z, value.w
    };
    return true;
}

bool MaterialInstance::SetColor(MaterialPropertyId property, const math::Vector4& value) const
{
    return SetVector4(property, value);
}

bool MaterialInstance::SetTexture(MaterialPropertyId property, const TextureRef& texture) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || !ValidateProperty(property, PropertyKind::Texture)) {
        FBZZ_LOG_WARN("Material texture property not found or type mismatch: %.*s",
                      static_cast<int>(property.name.size()), property.name.data());
        return false;
    }
    const MaterialPropertyId canonical(CanonicalTextureProperty(property.name));
    const std::string& key = ResolvePropertyName(material, canonical);
    material->textureOverrides[key] = texture.ResolvePath();
    return true;
}

bool MaterialInstance::TryGetFloat(MaterialPropertyId property, float& value) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || !property.IsValid()) return false;
    const std::string& key = ResolvePropertyName(material, property);
    const auto overrideIt = material->paramOverrides.find(key);
    if (overrideIt != material->paramOverrides.end() && !overrideIt->second.empty()) {
        value = overrideIt->second[0];
        return true;
    }
    const auto* shared = asset::AssetManager::GetMaterial(material->materialAsset);
    if (!shared) return false;
    const auto it = shared->params.find(key);
    if (it == shared->params.end() || it->second.empty()) return false;
    value = it->second[0];
    return true;
}

bool MaterialInstance::TryGetInt(MaterialPropertyId property, int& value) const
{
    float result = 0.0f;
    if (!TryGetFloat(property, result)) return false;
    value = static_cast<int>(result);
    return true;
}

bool MaterialInstance::TryGetVector3(MaterialPropertyId property, math::Vector3& value) const
{
    math::Vector4 vector;
    if (!TryGetVector4(property, vector)) return false;
    value = { vector.x, vector.y, vector.z };
    return true;
}

bool MaterialInstance::TryGetVector4(MaterialPropertyId property, math::Vector4& value) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || !property.IsValid()) return false;
    const std::string& key = ResolvePropertyName(material, property);
    const auto overrideIt = material->paramOverrides.find(key);
    const std::vector<float>* values = overrideIt != material->paramOverrides.end()
        ? &overrideIt->second : nullptr;
    if (!values) {
        const auto* shared = asset::AssetManager::GetMaterial(material->materialAsset);
        if (!shared) return false;
        const auto sharedIt = shared->params.find(key);
        if (sharedIt == shared->params.end()) return false;
        values = &sharedIt->second;
    }
    if (values->empty()) return false;
    value = {
        (*values)[0],
        values->size() > 1 ? (*values)[1] : 0.0f,
        values->size() > 2 ? (*values)[2] : 0.0f,
        values->size() > 3 ? (*values)[3] : 0.0f
    };
    return true;
}

bool MaterialInstance::TryGetColor(
    MaterialPropertyId property,
    math::Vector4& value) const
{
    return TryGetVector4(property, value);
}

bool MaterialInstance::TryGetTexture(MaterialPropertyId property, TextureRef& texture) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || !property.IsValid()) return false;
    const MaterialPropertyId canonical(CanonicalTextureProperty(property.name));
    const std::string& key = ResolvePropertyName(material, canonical);
    const auto overrideIt = material->textureOverrides.find(key);
    if (overrideIt != material->textureOverrides.end()) {
        texture.SetPath(overrideIt->second);
        return true;
    }
    const auto* shared = asset::AssetManager::GetMaterial(material->materialAsset);
    if (!shared) return false;
    const auto it = shared->textures.find(key);
    if (it == shared->textures.end()) return false;
    texture.SetPath(it->second);
    return true;
}

bool MaterialInstance::ClearOverride(MaterialPropertyId property) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material) return false;
    const std::string key = ResolvePropertyName(material, property);
    const MaterialPropertyId canonical(CanonicalTextureProperty(property.name));
    const std::string textureKey = ResolvePropertyName(material, canonical);
    const size_t removed = material->paramOverrides.erase(key) +
                           material->textureOverrides.erase(textureKey);
    return removed > 0;
}

bool MaterialInstance::ClearAllOverrides() const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material) return false;
    material->paramOverrides.clear();
    material->textureOverrides.clear();
    material->hasBlendModeOverride = false;
    material->hasDoubleSidedOverride = false;
    material->hasRenderQueueOverride = false;
    return true;
}

bool MaterialInstance::SetBlendMode(MaterialBlendMode blendMode) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material) return false;
    switch (blendMode) {
    case MaterialBlendMode::Alpha:    material->blendModeOverride = renderer::BlendMode::ALPHA_BLEND; break;
    case MaterialBlendMode::Additive: material->blendModeOverride = renderer::BlendMode::ADDITIVE; break;
    default:                          material->blendModeOverride = renderer::BlendMode::OPAQUE_BLEND; break;
    }
    material->hasBlendModeOverride = true;
    return true;
}

bool MaterialInstance::SetDoubleSided(bool doubleSided) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material) return false;
    material->doubleSidedOverride = doubleSided;
    material->hasDoubleSidedOverride = true;
    return true;
}

bool MaterialInstance::SetRenderQueue(int32_t renderQueue) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material) return false;
    material->renderQueueOverride = renderQueue;
    material->hasRenderQueueOverride = true;
    return true;
}

MaterialInstance ScriptMaterialProxy::Instance(uint32_t slot) const
{
    return { script, {}, slot };
}

MaterialInstance ScriptMaterialProxy::Instance(EntityRef target, uint32_t slot) const
{
    return { script, target, slot };
}

bool ScriptMaterialProxy::SetSharedMaterial(const MaterialRef& material, uint32_t slot) const
{
    return SetSharedMaterial({}, material, slot);
}

bool ScriptMaterialProxy::SetSharedMaterial(EntityRef target,
                                            const MaterialRef& materialRef,
                                            uint32_t slot) const
{
    MaterialInstance instance{ script, target, slot };
    auto* material = static_cast<MaterialSlot*>(instance.ResolveComponent(true));
    if (!material) return false;
    const std::string path = materialRef.ResolvePath();
    const auto assetHandle = path.empty()
        ? renderer::ResourceHandle<renderer::MaterialAssetTag>{}
        : asset::AssetManager::LoadMaterial(path);
    if (!path.empty() && !assetHandle.IsValid()) {
        FBZZ_LOG_WARN("Shared material could not be loaded: %s", path.c_str());
        return false;
    }
    if (material->materialPath != path)
        instance.ClearAllOverrides();
    material->materialPath = path;
    material->propertyValidationCache.clear();
    material->propertyValidationDescriptor = nullptr;
    material->materialAsset = assetHandle;
    return true;
}

// ── 共有 .mat の読み取り ────────────────────────────────────────────────────

namespace {

// MaterialRef から共有アセットを解決する。未ロードならここでロードする。
const asset::MaterialAsset* ResolveSharedMaterial(const MaterialRef& material)
{
    const std::string path = material.ResolvePath();
    if (path.empty()) return nullptr;
    const auto handle = asset::AssetManager::LoadMaterial(path);
    if (!handle.IsValid()) return nullptr;
    return asset::AssetManager::GetMaterial(handle);
}

} // namespace

bool ScriptMaterialProxy::HasSharedProperty(const MaterialRef& material,
                                            MaterialPropertyId property) const
{
    const auto* shared = ResolveSharedMaterial(material);
    if (!shared || !property.IsValid()) return false;
    const std::string key(property.name);
    return shared->params.contains(key) || shared->textures.contains(key);
}

bool ScriptMaterialProxy::TryGetSharedFloat(const MaterialRef& material,
                                            MaterialPropertyId property, float& value) const
{
    const auto* shared = ResolveSharedMaterial(material);
    if (!shared || !property.IsValid()) return false;
    const auto it = shared->params.find(std::string(property.name));
    if (it == shared->params.end() || it->second.empty()) return false;
    value = it->second[0];
    return true;
}

bool ScriptMaterialProxy::TryGetSharedVector4(const MaterialRef& material,
                                              MaterialPropertyId property,
                                              math::Vector4& value) const
{
    const auto* shared = ResolveSharedMaterial(material);
    if (!shared || !property.IsValid()) return false;
    const auto it = shared->params.find(std::string(property.name));
    if (it == shared->params.end() || it->second.empty()) return false;

    // .mat は float / float2 / float3 / float4 を同じ形式で持つため、
    // 足りない成分は 0 で埋める (Instance() の読み出しと同じ規則)。
    const auto& values = it->second;
    value = {
        values[0],
        values.size() > 1 ? values[1] : 0.0f,
        values.size() > 2 ? values[2] : 0.0f,
        values.size() > 3 ? values[3] : 0.0f
    };
    return true;
}

bool ScriptMaterialProxy::TryGetSharedVector3(const MaterialRef& material,
                                              MaterialPropertyId property,
                                              math::Vector3& value) const
{
    math::Vector4 vector;
    if (!TryGetSharedVector4(material, property, vector)) return false;
    value = { vector.x, vector.y, vector.z };
    return true;
}

bool ScriptMaterialProxy::TryGetSharedColor(const MaterialRef& material,
                                            MaterialPropertyId property,
                                            math::Vector4& value) const
{
    return TryGetSharedVector4(material, property, value);
}

bool ScriptMaterialProxy::TryGetSharedTexture(const MaterialRef& material,
                                              MaterialPropertyId property,
                                              TextureRef& texture) const
{
    const auto* shared = ResolveSharedMaterial(material);
    if (!shared || !property.IsValid()) return false;
    const MaterialPropertyId canonical(CanonicalTextureProperty(property.name));
    const auto it = shared->textures.find(std::string(canonical.name));
    if (it == shared->textures.end()) return false;
    texture.SetPath(it->second);
    return true;
}

bool ScriptMaterialProxy::EnsureMaterial(std::string_view materialPath) const
{
    auto* material = static_cast<MaterialSlot*>(Instance().ResolveComponent(true));
    if (!material) return false;
    if (material->materialPath != materialPath)
        return SetSharedMaterial(MaterialRef(materialPath));
    return material->EnsureMaterialAsset();
}

bool ScriptMaterialProxy::HasParam(std::string_view param) const
{
    return Instance().HasProperty(MaterialPropertyId(param));
}

bool ScriptMaterialProxy::SetFloat(std::string_view param, float value) const
{
    return Instance().SetFloat(MaterialPropertyId(param), value);
}

bool ScriptMaterialProxy::SetInt(std::string_view param, int value) const
{
    return Instance().SetInt(MaterialPropertyId(param), value);
}

bool ScriptMaterialProxy::SetVector3(std::string_view param, const math::Vector3& value) const
{
    return Instance().SetVector3(MaterialPropertyId(param), value);
}

bool ScriptMaterialProxy::SetVector4(std::string_view param, const math::Vector4& value) const
{
    return Instance().SetVector4(MaterialPropertyId(param), value);
}

bool ScriptMaterialProxy::SetTexture(std::string_view slot, std::string_view texturePath) const
{
    return Instance().SetTexture(MaterialPropertyId(slot), TextureRef(texturePath));
}

float ScriptMaterialProxy::GetFloat(std::string_view param) const
{
    float value = 0.0f;
    (void)Instance().TryGetFloat(MaterialPropertyId(param), value);
    return value;
}

math::Vector3 ScriptMaterialProxy::GetVector3(std::string_view param) const
{
    math::Vector3 value;
    (void)Instance().TryGetVector3(MaterialPropertyId(param), value);
    return value;
}

bool ScriptMaterialProxy::SetEnabled(bool enabled) const
{
    // enabled はコンポーネント全体の有効/無効。submesh 単位の表示切替は
    // SetSlotVisible() を使う。
    MaterialComponent* material = Instance().ResolveOwner(false);
    if (!material) return false;
    material->enabled = enabled;
    return true;
}

bool ScriptMaterialProxy::SetSlotVisible(uint32_t slot, bool visible) const
{
    auto* material = static_cast<MaterialSlot*>(Instance(slot).ResolveComponent(true));
    if (!material) return false;
    material->visible = visible;
    return true;
}

bool ScriptMaterialProxy::SetOnlyVisibleSlot(int slot) const
{
    MaterialComponent* material = Instance().ResolveOwner(false);
    if (!material) return false;
    material->SetOnlyVisibleSlot(slot);
    return true;
}

bool ScriptMaterialProxy::SetBlendMode(MaterialBlendMode blendMode) const
{
    return Instance().SetBlendMode(blendMode);
}

bool ScriptMaterialProxy::SetDoubleSided(bool doubleSided) const
{
    return Instance().SetDoubleSided(doubleSided);
}

bool ScriptMaterialProxy::SetRenderQueue(int32_t renderQueue) const
{
    return Instance().SetRenderQueue(renderQueue);
}

void ScriptMaterialProxy::QueueRenderPass(UserRenderPassDesc desc) const
{
    if (script) script->QueueRenderPass(std::move(desc));
}

void ScriptParticleProxy::SetEmitRate(float rate) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->emitRate = rate;
}

void ScriptParticleProxy::SetEmitPosition(const math::Vector3& position) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->emitPosition = position;
}

void ScriptParticleProxy::SetEmitVelocity(const math::Vector3& velocity) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->emitVelocity = velocity;
}

void ScriptParticleProxy::SetVelocitySpread(float spread) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->velocitySpread = (std::max)(spread, 0.0f);
}

void ScriptParticleProxy::SetEnabled(bool enabled) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->enabled = enabled;
}

void ScriptParticleProxy::Play(bool restart) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->Play(restart);
}

void ScriptParticleProxy::Stop(bool clear) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->Stop(clear);
}

void ScriptParticleProxy::Burst(int count) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->Burst(count);
}

void ScriptParticleProxy::Clear() const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->ClearParticles();
}

void ScriptParticleProxy::SetGravity(const math::Vector3& gravity) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->gravity = gravity;
}

void ScriptParticleProxy::SetColor(const math::Vector4& start, const math::Vector4& end) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->colorStart = start;
        p->colorEnd = end;
    }
}

void ScriptParticleProxy::SetSize(float start, float end) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->sizeStart = (std::max)(start, 0.0f);
        p->sizeEnd = (std::max)(end, 0.0f);
    }
}

void ScriptParticleProxy::SetLifetime(float seconds) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->lifetime = (std::max)(seconds, 0.01f);
}

void ScriptParticleProxy::SetMaxParticles(int maxParticles) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        const int clamped = (std::max)(maxParticles, 1);
        if (p->maxParticles != clamped) {
            p->maxParticles = clamped;
            p->gpuInitialized = false;
            p->gpuCapacity = 0;
        }
    }
}

void ScriptParticleProxy::SetPlayback(bool loop, float duration, bool clearOnStop) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->loop = loop;
        p->duration = (std::max)(duration, 0.0f);
        p->clearOnStop = clearOnStop;
    }
}

void ScriptParticleProxy::SetShape(ParticleEmitterShape shape) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->shape = shape;
}

void ScriptParticleProxy::SetSphereShape(float radius) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->shape = ParticleEmitterShape::Sphere;
        p->sphereRadius = (std::max)(radius, 0.0f);
    }
}

void ScriptParticleProxy::SetConeShape(float radius, float angleDegrees) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->shape = ParticleEmitterShape::Cone;
        p->coneRadius = (std::max)(radius, 0.0f);
        p->coneAngleDegrees = (std::max)(angleDegrees, 0.0f);
    }
}

void ScriptParticleProxy::SetBoxShape(const math::Vector3& extents) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->shape = ParticleEmitterShape::Box;
        p->boxExtents = {
            (std::max)(extents.x, 0.0f),
            (std::max)(extents.y, 0.0f),
            (std::max)(extents.z, 0.0f)
        };
    }
}

void ScriptParticleProxy::SetMeshShape(std::string_view modelPath, int meshIndex, float scale,
                                       bool followSkinnedAnimation) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->shape = ParticleEmitterShape::MeshSurface;
        p->meshShapePath = std::string(modelPath);
        p->meshShapeIndex = meshIndex;
        p->meshShapeScale = (std::max)(scale, 0.0001f);
        p->meshShapeFollowSkinnedAnimation = followSkinnedAnimation;
        // パス・サブメッシュ変更時は次のスポーンでFBX頂点を再構築する。
        p->loadedMeshShapePath.clear();
        p->loadedMeshShapeIndex = -2;
        p->meshShapeVertices.clear();
    }
}

void ScriptParticleProxy::SetBlendMode(ParticleBlendMode blendMode) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->blendMode = blendMode;
}

void ScriptParticleProxy::SetSortMode(ParticleSortMode sortMode) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->sortMode = sortMode;
}

void ScriptParticleProxy::SetSimulationMode(ParticleSimulationMode simulationMode) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        if (p->simulationMode != simulationMode) p->gpuClearPending = true;
        p->simulationMode = simulationMode;
    }
}

void ScriptParticleProxy::SetSimulationSpace(ParticleSimulationSpace space) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->simulationSpace = space;
        if (space == ParticleSimulationSpace::Local)
            p->simulationMode = ParticleSimulationMode::Cpu;
    }
}

void ScriptParticleProxy::SetRenderMode(ParticleRenderMode mode, float stretchScale) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->renderMode = mode;
        p->stretchedVelocityScale = (std::max)(stretchScale, 0.0f);
    }
}

void ScriptParticleProxy::SetCollision(ParticleCollisionMode mode,
                                       ParticleCollisionResponse response,
                                       float radius, float bounciness) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->collisionMode = mode;
        p->collisionResponse = response;
        p->collisionRadius = (std::max)(radius, 0.0f);
        p->collisionBounciness = (std::max)(0.0f, (std::min)(bounciness, 1.0f));
        if (mode == ParticleCollisionMode::Physics)
            p->simulationMode = ParticleSimulationMode::Cpu;
    }
}

int ScriptParticleProxy::GetCollisionCount() const
{
    if (const auto* p = SelfComponent<ParticleEmitter>(script))
        return p->collisionCountThisFrame;
    return 0;
}

void ScriptParticleProxy::SetRateOverDistance(float particlesPerMeter) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script))
        p->rateOverDistance = (std::max)(particlesPerMeter, 0.0f);
}

void ScriptParticleProxy::SetPrewarm(bool enabled) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->prewarm = enabled;
        p->prewarmed = false;
        p->prewarmSpawnPending = 0;
    }
}

void ScriptParticleProxy::SetSoftParticles(bool enabled, float fadeDistance) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->softParticles = enabled;
        p->softParticleFadeDistance = (std::max)(fadeDistance, 0.001f);
    }
}

void ScriptParticleProxy::SetFlipbookMode(ParticleFlipbookMode mode, float framesPerSecond) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->flipbookMode = mode;
        p->flipbookFramesPerSecond = (std::max)(framesPerSecond, 0.0f);
    }
}

void ScriptParticleProxy::SetSubEmitters(std::string_view birthEmitter,
                                         std::string_view deathEmitter,
                                         std::string_view collisionEmitter,
                                         int burstCount) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->birthSubEmitter = std::string(birthEmitter);
        p->deathSubEmitter = std::string(deathEmitter);
        p->collisionSubEmitter = std::string(collisionEmitter);
        p->subEmitterBurstCount = (std::max)(burstCount, 1);
    }
}

void ScriptParticleProxy::SetVelocityDamping(float damping) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->velocityDamping = (std::max)(damping, 0.0f);
}

void ScriptParticleProxy::SetAngularVelocity(float minValue, float maxValue) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->angularVelocityMin = minValue;
        p->angularVelocityMax = maxValue;
    }
}

void ScriptParticleProxy::SetNoise(float strength, float frequency, float speed) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->noiseStrength  = (std::max)(strength, 0.0f);
        p->noiseFrequency = (std::max)(frequency, 0.0001f);
        p->noiseSpeed     = speed;
    }
}

void ScriptParticleProxy::SetReceiveForceFields(bool receive) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->receiveForceFields = receive;
}

void ScriptVFXProxy::Play(bool restart) const
{
    if (auto* graph = SelfComponent<VFXGraphComponent>(script)) {
        if (restart) graph->Restart();
        else graph->Resume();
    }
}

void ScriptVFXProxy::Pause() const
{
    if (auto* graph = SelfComponent<VFXGraphComponent>(script)) graph->Pause();
}

void ScriptVFXProxy::Stop() const
{
    if (auto* graph = SelfComponent<VFXGraphComponent>(script)) graph->Stop();
}

void ScriptVFXProxy::SetSpeed(float speed) const
{
    if (auto* graph = SelfComponent<VFXGraphComponent>(script))
        graph->speed = (std::max)(speed, 0.0f);
}

bool ScriptVFXProxy::SetGraph(const VFXRef& reference, bool restart) const
{
    auto* graph = SelfComponent<VFXGraphComponent>(script);
    const std::string path = reference.ResolvePath();
    if (graph == nullptr || path.empty()) return false;
    graph->graphPath = path;
    graph->reloadRequested = true;
    if (restart) graph->Restart();
    return true;
}

bool ScriptVFXProxy::Trigger(std::string_view name) const
{
    auto* graph = SelfComponent<VFXGraphComponent>(script);
    if (graph == nullptr || name.empty()) return false;
    graph->Trigger(name);
    return true;
}

namespace {
bool SetVFXOverride(Script* script, std::string_view name, asset::VFXParamValue value)
{
    auto* graph = SelfComponent<VFXGraphComponent>(script);
    if (graph == nullptr || name.empty()) return false;
    auto iterator = std::find_if(graph->parameterOverrides.begin(), graph->parameterOverrides.end(),
        [name](const asset::VFXParamOverride& item) { return item.paramName == name; });
    if (iterator == graph->parameterOverrides.end())
        graph->parameterOverrides.push_back({ std::string(name), std::move(value) });
    else iterator->value = std::move(value);
    graph->reloadRequested = true;
    return true;
}
}

bool ScriptVFXProxy::SetFloat(std::string_view name, float value) const
{ return SetVFXOverride(script, name, { asset::VFXConstant{ value } }); }
bool ScriptVFXProxy::SetInt(std::string_view name, int value) const
{ return SetVFXOverride(script, name, { asset::VFXConstant{ value } }); }
bool ScriptVFXProxy::SetBool(std::string_view name, bool value) const
{ return SetVFXOverride(script, name, { asset::VFXConstant{ value } }); }
bool ScriptVFXProxy::SetColor(std::string_view name, float r, float g, float b, float a) const
{ return SetVFXOverride(script, name, { asset::VFXConstant{ math::Vector4{ r, g, b, a } } }); }
bool ScriptVFXProxy::SetVector3(std::string_view name, float x, float y, float z) const
{ return SetVFXOverride(script, name, { asset::VFXConstant{ math::Vector3{ x, y, z } } }); }
bool ScriptVFXProxy::SetAsset(std::string_view name, std::string_view path) const
{ return SetVFXOverride(script, name, { asset::VFXConstant{ std::string(path) } }); }
bool ScriptVFXProxy::ClearOverride(std::string_view name) const
{
    auto* graph = SelfComponent<VFXGraphComponent>(script);
    if (graph == nullptr) return false;
    const auto oldSize = graph->parameterOverrides.size();
    std::erase_if(graph->parameterOverrides,
        [name](const asset::VFXParamOverride& item) { return item.paramName == name; });
    if (graph->parameterOverrides.size() == oldSize) return false;
    graph->reloadRequested = true;
    return true;
}

bool ScriptVFXProxy::IsPlaying() const
{
    const auto* graph = SelfComponent<VFXGraphComponent>(script);
    return graph != nullptr && graph->playing;
}

float ScriptVFXProxy::GetTime() const
{
    const auto* graph = SelfComponent<VFXGraphComponent>(script);
    return graph != nullptr ? graph->playTime : 0.0f;
}

float ScriptVFXProxy::GetDuration() const
{
    const auto* graph = SelfComponent<VFXGraphComponent>(script);
    return graph != nullptr ? graph->graphDuration : 0.0f;
}

void ScriptTrailProxy::SetEnabled(bool enabled, bool clearWhenDisabled) const
{
    if (auto* t = SelfComponent<TrailComponent>(script)) {
        t->enabled = enabled;
        t->clearOnDisable = clearWhenDisabled;
        if (!enabled && clearWhenDisabled)
            Clear();
    }
}

void ScriptTrailProxy::Clear() const
{
    if (auto* t = SelfComponent<TrailComponent>(script)) {
        t->ringHead = 0;
        t->ringTail = 0;
        t->ringCount = 0;
        t->lastSampleTime = -1.0f;
    }
}

void ScriptTrailProxy::SetDuration(float seconds) const
{
    if (auto* t = SelfComponent<TrailComponent>(script))
        t->duration = (std::max)(seconds, 0.01f);
}

void ScriptTrailProxy::SetMaxPoints(int maxPoints) const
{
    if (auto* t = SelfComponent<TrailComponent>(script))
        t->maxPoints = (std::max)(maxPoints, 2);
}

void ScriptTrailProxy::SetSampling(float sampleInterval, float minVertexDist) const
{
    if (auto* t = SelfComponent<TrailComponent>(script)) {
        t->sampleInterval = (std::max)(sampleInterval, 0.0f);
        t->minVertexDist = (std::max)(minVertexDist, 0.0f);
    }
}

void ScriptTrailProxy::SetWidth(float start, float end) const
{
    if (auto* t = SelfComponent<TrailComponent>(script)) {
        t->widthStart = (std::max)(start, 0.0f);
        t->widthEnd = (std::max)(end, 0.0f);
    }
}

void ScriptTrailProxy::SetWidthEasing(TrailWidthEasing easing) const
{
    if (auto* t = SelfComponent<TrailComponent>(script))
        t->widthEasing = easing;
}

void ScriptTrailProxy::SetColor(const math::Vector4& start, const math::Vector4& end) const
{
    if (auto* t = SelfComponent<TrailComponent>(script)) {
        t->colorStart = start;
        t->colorEnd = end;
    }
}

void ScriptTrailProxy::SetMaterial(std::string_view materialPath) const
{
    if (auto* t = SelfComponent<TrailComponent>(script)) {
        t->materialPath = std::string(materialPath);
        t->loadedMaterialPath.clear();
        t->texture = {};
    }
}

void ScriptTrailProxy::SetUVMode(TrailUVMode mode) const
{
    if (auto* t = SelfComponent<TrailComponent>(script))
        t->uvMode = mode;
}

void ScriptTrailProxy::SetAlignment(TrailAlignment alignment) const
{
    if (auto* t = SelfComponent<TrailComponent>(script))
        t->alignment = alignment;
}

void ScriptTrailProxy::SetCameraFacing() const
{
    SetAlignment(TrailAlignment::CameraFacing);
}

void ScriptTrailProxy::SetWorldUp() const
{
    SetAlignment(TrailAlignment::WorldUp);
}

void ScriptTrailProxy::SetSmoothSubdivisions(int subdivisions) const
{
    if (auto* t = SelfComponent<TrailComponent>(script))
        t->smoothSubdivisions = (std::max)(subdivisions, 0);
}

void ScriptTrailProxy::SetAttachBone(std::string_view boneName, const math::Vector3& offset) const
{
    if (auto* t = SelfComponent<TrailComponent>(script)) {
        t->attachBone = std::string(boneName);
        t->attachOffset = offset;
    }
}

void ScriptTrailProxy::ClearAttachBone() const
{
    if (auto* t = SelfComponent<TrailComponent>(script)) {
        t->attachBone.clear();
        t->attachOffset = math::Vector3::ZERO;
    }
}

void ScriptMeshTrailProxy::SetEnabled(bool enabled, bool clearWhenDisabled) const
{
    if (auto* t = SelfComponent<MeshTrailComponent>(script)) {
        t->enabled = enabled;
        t->clearOnDisable = clearWhenDisabled;
        if (!enabled && clearWhenDisabled)
            Clear();
    }
}

void ScriptMeshTrailProxy::Clear() const
{
    if (auto* t = SelfComponent<MeshTrailComponent>(script))
        t->clearRequested = true;
}

void ScriptMeshTrailProxy::SetDuration(float seconds) const
{
    if (auto* t = SelfComponent<MeshTrailComponent>(script))
        t->duration = (std::max)(seconds, 0.01f);
}

void ScriptMeshTrailProxy::SetSampling(float sampleInterval, float minVertexDist) const
{
    if (auto* t = SelfComponent<MeshTrailComponent>(script)) {
        t->sampleInterval = (std::max)(sampleInterval, 0.0f);
        t->minVertexDist = (std::max)(minVertexDist, 0.0f);
    }
}

void ScriptMeshTrailProxy::SetMaxSamples(int maxSamples) const
{
    if (auto* t = SelfComponent<MeshTrailComponent>(script))
        t->maxSamples = (std::max)(maxSamples, 1);
}

void ScriptMeshTrailProxy::SetColor(const math::Vector4& start, const math::Vector4& end) const
{
    if (auto* t = SelfComponent<MeshTrailComponent>(script)) {
        t->colorStart = start;
        t->colorEnd = end;
    }
}

void ScriptMeshTrailProxy::SetDoubleSided(bool doubleSided) const
{
    if (auto* t = SelfComponent<MeshTrailComponent>(script))
        t->doubleSided = doubleSided;
}

void ScriptMeshTrailProxy::AddExcludedMeshIndex(int meshIndex) const
{
    if (auto* t = SelfComponent<MeshTrailComponent>(script)) {
        if (meshIndex < 0)
            return;
        if (std::find(t->excludedMeshIndices.begin(), t->excludedMeshIndices.end(), meshIndex) == t->excludedMeshIndices.end())
            t->excludedMeshIndices.push_back(meshIndex);
    }
}

void ScriptMeshTrailProxy::ClearExcludedMeshIndices() const
{
    if (auto* t = SelfComponent<MeshTrailComponent>(script))
        t->excludedMeshIndices.clear();
}

// ── UIButton 入力の取得 ──────────────────────────────────────────────────────
// UISystem が毎フレーム立て直す onClick / onEnter / onExit / state をそのまま読む。
// 取得系はボタンが無い場合に「押されていない」を返す方が呼び出し側の分岐が減るため、
// nullptr は一律 false / Disabled に潰す。
namespace {

UIButtonPhase ToButtonPhase(const UIButton* button)
{
    if (!button || !button->enabled || !button->isInteractable)
        return UIButtonPhase::Disabled;
    switch (button->state) {
    case UIButtonState::HOVERED: return UIButtonPhase::Hovered;
    case UIButtonState::PRESSED: return UIButtonPhase::Pressed;
    default:                     return UIButtonPhase::Normal;
    }
}

} // namespace

bool ScriptUIProxy::WasClicked() const
{
    const auto* b = SelfComponent<UIButton>(script);
    return b && b->onClick;
}

bool ScriptUIProxy::WasHoverEnter() const
{
    const auto* b = SelfComponent<UIButton>(script);
    return b && b->onEnter;
}

bool ScriptUIProxy::WasHoverExit() const
{
    const auto* b = SelfComponent<UIButton>(script);
    return b && b->onExit;
}

bool ScriptUIProxy::IsHovered() const
{
    return GetButtonPhase() == UIButtonPhase::Hovered;
}

bool ScriptUIProxy::IsPressed() const
{
    return GetButtonPhase() == UIButtonPhase::Pressed;
}

bool ScriptUIProxy::IsInteractable() const
{
    const auto* b = SelfComponent<UIButton>(script);
    return b && b->enabled && b->isInteractable;
}

UIButtonPhase ScriptUIProxy::GetButtonPhase() const
{
    return ToButtonPhase(SelfComponent<UIButton>(script));
}

bool ScriptUIProxy::WasClicked(GameObject* go) const
{
    const auto* b = ObjectComponent<UIButton>(go);
    return b && b->onClick;
}

bool ScriptUIProxy::WasHoverEnter(GameObject* go) const
{
    const auto* b = ObjectComponent<UIButton>(go);
    return b && b->onEnter;
}

bool ScriptUIProxy::WasHoverExit(GameObject* go) const
{
    const auto* b = ObjectComponent<UIButton>(go);
    return b && b->onExit;
}

bool ScriptUIProxy::IsHovered(GameObject* go) const
{
    return GetButtonPhase(go) == UIButtonPhase::Hovered;
}

bool ScriptUIProxy::IsPressed(GameObject* go) const
{
    return GetButtonPhase(go) == UIButtonPhase::Pressed;
}

bool ScriptUIProxy::IsInteractable(GameObject* go) const
{
    const auto* b = ObjectComponent<UIButton>(go);
    return b && b->enabled && b->isInteractable;
}

UIButtonPhase ScriptUIProxy::GetButtonPhase(GameObject* go) const
{
    return ToButtonPhase(ObjectComponent<UIButton>(go));
}

void ScriptUIProxy::SetButtonInteractable(bool v) const
{
    if (auto* b = SelfComponent<UIButton>(script)) b->isInteractable = v;
}

void ScriptUIProxy::SetButtonInteractable(GameObject* go, bool v) const
{
    if (auto* b = ObjectComponent<UIButton>(go)) b->isInteractable = v;
}

void ScriptUIProxy::SetImageColor(const math::Vector4& color) const
{
    if (auto* image = SelfComponent<UIImage>(script)) image->color = color;
}

void ScriptUIProxy::SetText(std::string_view text) const
{
    if (auto* t = SelfComponent<UIText>(script)) t->text = std::string(text);
}

void ScriptUIProxy::SetCanvasSortOrder(int order) const
{
    if (auto* canvas = SelfComponent<UICanvas>(script)) canvas->sortOrder = order;
}

void ScriptUIProxy::SetImageSpriteRect(float x, float y, float w, float h, float texW, float texH) const
{
    if (auto* img = SelfComponent<UIImage>(script)) {
        const auto [uvMin, uvMax] = UIImage::PixelRectToUV(x, y, w, h, texW, texH);
        img->uvMin = uvMin;
        img->uvMax = uvMax;
    }
}

void ScriptUIProxy::SetImageFillAmount(float amount) const
{
    if (auto* img = SelfComponent<UIImage>(script))
        img->fillAmount = std::clamp(amount, 0.0f, 1.0f);
}

void ScriptUIProxy::SetImageColor(GameObject* go, const math::Vector4& color) const
{
    if (auto* image = ObjectComponent<UIImage>(go)) image->color = color;
}

void ScriptUIProxy::SetImageFillAmount(GameObject* go, float amount) const
{
    if (auto* img = ObjectComponent<UIImage>(go))
        img->fillAmount = std::clamp(amount, 0.0f, 1.0f);
}

void ScriptUIProxy::SetText(GameObject* go, std::string_view text) const
{
    if (auto* t = ObjectComponent<UIText>(go)) t->text = std::string(text);
}

void ScriptUIProxy::SetImageEnabled(GameObject* go, bool enabled) const
{
    if (auto* image = ObjectComponent<UIImage>(go)) image->enabled = enabled;
}

void ScriptUIAnimatorProxy::PlayColor(const math::Vector4& from, const math::Vector4& to, float duration) const
{
    PlayColor(from, to, duration, UIEasingType::Linear);
}

void ScriptUIAnimatorProxy::PlayColor(const math::Vector4& from, const math::Vector4& to,
                                      float duration, UIEasingType easing, bool loop, bool pingPong) const
{
    if (auto* anim = SelfComponent<UIAnimator>(script))
        anim->PlayColor(from, to, duration, easing, loop, pingPong);
}

void ScriptUIAnimatorProxy::PlayPosition(const math::Vector2& from, const math::Vector2& to, float duration) const
{
    PlayPosition(from, to, duration, UIEasingType::Linear);
}

void ScriptUIAnimatorProxy::PlayPosition(const math::Vector2& from, const math::Vector2& to,
                                         float duration, UIEasingType easing, bool loop, bool pingPong) const
{
    if (auto* anim = SelfComponent<UIAnimator>(script))
        anim->PlayPosition(from, to, duration, easing, loop, pingPong);
}

void ScriptUIAnimatorProxy::PlayScale(const math::Vector2& from, const math::Vector2& to, float duration) const
{
    PlayScale(from, to, duration, UIEasingType::Linear);
}

void ScriptUIAnimatorProxy::PlayScale(const math::Vector2& from, const math::Vector2& to,
                                      float duration, UIEasingType easing, bool loop, bool pingPong) const
{
    if (auto* anim = SelfComponent<UIAnimator>(script))
        anim->PlayScale(from, to, duration, easing, loop, pingPong);
}

void ScriptUIAnimatorProxy::StopColor() const
{
    if (auto* anim = SelfComponent<UIAnimator>(script)) anim->StopColor();
}

void ScriptUIAnimatorProxy::StopPosition() const
{
    if (auto* anim = SelfComponent<UIAnimator>(script)) anim->StopPosition();
}

void ScriptUIAnimatorProxy::StopScale() const
{
    if (auto* anim = SelfComponent<UIAnimator>(script)) anim->StopScale();
}

void ScriptUIAnimatorProxy::StopAll() const
{
    if (auto* anim = SelfComponent<UIAnimator>(script)) anim->StopAll();
}

bool ScriptUIAnimatorProxy::IsPlaying() const
{
    if (auto* anim = SelfComponent<UIAnimator>(script)) return anim->IsPlaying();
    return false;
}

GameObject* ScriptSceneProxy::Find(std::string_view name) const
{
    return script && script->m_scene ? script->m_scene->Find(std::string(name)) : nullptr;
}

GameObject* ScriptSceneProxy::FindWithTag(std::string_view tag) const
{
    return script && script->m_scene ? script->m_scene->FindWithTag(std::string(tag)) : nullptr;
}

GameObject* ScriptSceneProxy::Self() const
{
    return script ? script->m_gameObject : nullptr;
}

GameObject* ScriptSceneProxy::GetGameObject(EntityID id) const
{
    return script && script->m_scene ? script->m_scene->GetGameObject(id) : nullptr;
}

GameObject* ScriptSceneProxy::GetMainCameraObject() const
{
    if (!script || !script->m_scene) return nullptr;
    for (auto& go : script->m_scene->GameObjects()) {
        auto* camera = go.GetComponent<CameraComponent>();
        if (camera && camera->enabled && camera->isMain) return &go;
    }
    return nullptr;
}

GameObject& ScriptSceneProxy::Create(std::string_view name) const
{
    assert(script && script->m_scene && "Script context is not set");
    return script->m_scene->CreateGameObject(std::string(name));
}

void ScriptSceneProxy::Destroy(GameObject& go, float delay) const
{
    GameObject::Destroy(go, delay);
}

void ScriptSceneProxy::DestroySelf(float delay) const
{
    if (script && script->m_gameObject)
        GameObject::Destroy(*script->m_gameObject, delay);
}

bool ScriptSceneProxy::IsActiveAndEnabled() const
{
    // Unity の isActiveAndEnabled と同じく、親 GameObject の無効化も実効状態へ反映する。
    return script && script->m_gameObject && script->m_gameObject->activeInHierarchy() && script->enabled;
}

GameObject* ScriptSceneProxy::Instantiate(const PrefabRef& prefab) const
{
    return Instantiate(prefab.path);
}

GameObject* ScriptSceneProxy::Instantiate(const std::string& prefabPath) const
{
    if (!script || !script->m_scene || prefabPath.empty()) return nullptr;
    // WHY: PrefabSerializer::Instantiate は内部で SceneIO::Deserialize を呼び全 GameObject を
    //      再構築する。これにより呼び出し元 Script (と m_gameObject) が解放されるため、
    //      呼び出し後に script->m_scene を参照するとクラッシュする。
    //      Scene* はデシリアライズ後も同アドレスに存在し続けるため、先に退避しておく。
    Scene* scene = script->m_scene;
    std::vector<EntityID> roots;
    if (!Script::InstantiatePrefab(*scene, prefabPath, roots) || roots.empty())
        return nullptr;
    return scene->GetGameObject(roots.front());
}

GameObject* ScriptSceneProxy::Instantiate(const PrefabRef& prefab,
                                           std::function<void(GameObject&)> init) const
{
    auto* go = Instantiate(prefab.path);
    if (go && init) init(*go);
    return go;
}

GameObject* ScriptSceneProxy::Instantiate(const std::string& prefabPath,
                                           std::function<void(GameObject&)> init) const
{
    auto* go = Instantiate(prefabPath);
    if (go && init) init(*go);
    return go;
}

// ── オブジェクトプール ──────────────────────────────────────────────────────

GameObject* ScriptSceneProxy::Spawn(const PrefabRef& prefab,
                                    const math::Vector3& position,
                                    const math::Quaternion& rotation) const
{
    return Spawn(prefab.path, position, rotation);
}

GameObject* ScriptSceneProxy::Spawn(const std::string& prefabPath,
                                    const math::Vector3& position,
                                    const math::Quaternion& rotation) const
{
    if (!script || !script->m_scene) return nullptr;
    // Instantiate と同じ理由で Scene* を先に退避する。プールが空だった場合は
    // 内部で Instantiate が走り、GameObject 配列が再確保され得る。
    Scene* scene = script->m_scene;
    return PrefabPool::Spawn(*scene, prefabPath, position, rotation);
}

GameObject* ScriptSceneProxy::Spawn(const PrefabRef& prefab) const
{
    if (!script || !script->m_gameObject) return nullptr;
    // WHY 値へコピーしてから渡すか: Spawn の内部で Instantiate が走ると Scene の
    //     GameObject 配列が再確保され、m_gameObject->transform の参照先が無効になる。
    const math::Vector3    position = script->m_gameObject->transform.worldPosition;
    const math::Quaternion rotation = script->m_gameObject->transform.worldRotation;
    return Spawn(prefab.path, position, rotation);
}

bool ScriptSceneProxy::Despawn(GameObject& gameObject) const
{
    if (!script || !script->m_scene) return false;
    return PrefabPool::Despawn(*script->m_scene, gameObject);
}

bool ScriptSceneProxy::DespawnSelf() const
{
    if (!script || !script->m_scene || !script->m_gameObject) return false;
    return PrefabPool::Despawn(*script->m_scene, *script->m_gameObject);
}

int ScriptSceneProxy::Prewarm(const PrefabRef& prefab, int count) const
{
    return Prewarm(prefab.path, count);
}

int ScriptSceneProxy::Prewarm(const std::string& prefabPath, int count) const
{
    if (!script || !script->m_scene) return 0;
    Scene* scene = script->m_scene;
    return PrefabPool::Prewarm(*scene, prefabPath, count);
}

size_t ScriptSceneProxy::PooledCount(const std::string& prefabPath) const
{
    if (!script || !script->m_scene) return 0;
    return PrefabPool::AvailableCount(*script->m_scene, prefabPath);
}

bool ScriptSceneProxy::LoadScene(std::string_view name) const
{
    auto* mgr = ScriptRuntime::GetCurrent().sceneManager;
    return mgr && mgr->LoadScene(std::string(name));
}

std::string ScriptSceneProxy::GetSceneName() const
{
    return {};
}

std::string ScriptSceneProxy::GetName() const
{
    return (script && script->m_gameObject) ? script->m_gameObject->name : std::string{};
}

void ScriptSceneProxy::SetName(const std::string& n) const
{
    if (script && script->m_gameObject) script->m_gameObject->name = n;
}

std::string ScriptSceneProxy::GetTag() const
{
    return (script && script->m_gameObject) ? script->m_gameObject->tag : std::string{};
}

void ScriptSceneProxy::SetTag(const std::string& t) const
{
    if (script && script->m_gameObject) script->m_gameObject->tag = t;
}

float ScriptSceneProxy::GetTerrainHeightAt(const math::Vector3& worldPos) const
{
    // WHY: 地形なし時は lowest() を返し、呼び出し側の「nextPos.y <= terrainH」が常に
    //      true になる問題を防ぐ。worldPos.y を返すと高さが一致して誤判定する。
    if (!script || !script->m_scene) return std::numeric_limits<float>::lowest();
    for (auto& go : script->m_scene->GameObjects()) {
        if (auto* terrain = go.GetComponent<TerrainComponent>())
            return go.transform.worldPosition.y + terrain->GetHeightAt(worldPos.x - go.transform.worldPosition.x,
                                                                  worldPos.z - go.transform.worldPosition.z);
    }
    return std::numeric_limits<float>::lowest();
}

math::Vector3 ScriptSceneProxy::GetTerrainNormalAt(const math::Vector3& worldPos) const
{
    if (!script || !script->m_scene) return math::Vector3::UP;
    for (auto& go : script->m_scene->GameObjects()) {
        if (auto* terrain = go.GetComponent<TerrainComponent>())
            return terrain->GetNormalAt(worldPos.x - go.transform.worldPosition.x,
                                        worldPos.z - go.transform.worldPosition.z);
    }
    return math::Vector3::UP;
}

float ScriptSceneProxy::GetWaterSurfaceHeight(const math::Vector3& worldPos, float time) const
{
    if (!script || !script->m_scene) return worldPos.y;
    for (auto& go : script->m_scene->GameObjects()) {
        if (auto* water = go.GetComponent<WaterComponent>()) {
            const float localX = worldPos.x - go.transform.worldPosition.x;
            const float localZ = worldPos.z - go.transform.worldPosition.z;
            return go.transform.worldPosition.y + water->GetSurfaceHeightAt(localX, localZ, time);
        }
    }
    return worldPos.y;
}

void ScriptAnimatorProxy::SetFloat(std::string_view name, float v) const
{
    if (auto* a = SelfComponent<AnimatorComponent>(script)) a->SetFloat(name, v);
}

void ScriptAnimatorProxy::SetInt(std::string_view name, int v) const
{
    if (auto* a = SelfComponent<AnimatorComponent>(script)) a->SetInt(name, v);
}

void ScriptAnimatorProxy::SetBool(std::string_view name, bool v) const
{
    if (auto* a = SelfComponent<AnimatorComponent>(script)) a->SetBool(name, v);
}

void ScriptAnimatorProxy::SetTrigger(std::string_view name) const
{
    if (auto* a = SelfComponent<AnimatorComponent>(script)) a->SetTrigger(name);
}

bool ScriptAnimatorProxy::IsInState(std::string_view name) const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a && a->IsInState(name);
}

void ScriptAnimatorProxy::SetFloat(GameObject* go, std::string_view name, float v) const
{
    if (auto* a = ObjectComponent<AnimatorComponent>(go)) a->SetFloat(name, v);
}

void ScriptAnimatorProxy::SetInt(GameObject* go, std::string_view name, int v) const
{
    if (auto* a = ObjectComponent<AnimatorComponent>(go)) a->SetInt(name, v);
}

void ScriptAnimatorProxy::SetBool(GameObject* go, std::string_view name, bool v) const
{
    if (auto* a = ObjectComponent<AnimatorComponent>(go)) a->SetBool(name, v);
}

void ScriptAnimatorProxy::SetTrigger(GameObject* go, std::string_view name) const
{
    if (auto* a = ObjectComponent<AnimatorComponent>(go)) a->SetTrigger(name);
}

bool ScriptAnimatorProxy::IsInState(GameObject* go, std::string_view name) const
{
    const auto* a = ObjectComponent<AnimatorComponent>(go);
    return a && a->IsInState(name);
}

float ScriptAnimatorProxy::GetFloat(std::string_view name) const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a ? a->GetFloat(name) : 0.0f;
}

int ScriptAnimatorProxy::GetInt(std::string_view name) const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a ? a->GetInt(name) : 0;
}

bool ScriptAnimatorProxy::GetBool(std::string_view name) const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a && a->GetBool(name);
}

float ScriptAnimatorProxy::GetNormalizedTime() const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a ? a->GetNormalizedTime() : 0.0f;
}

float ScriptAnimatorProxy::GetFloat(GameObject* go, std::string_view name) const
{
    const auto* a = ObjectComponent<AnimatorComponent>(go);
    return a ? a->GetFloat(name) : 0.0f;
}

int ScriptAnimatorProxy::GetInt(GameObject* go, std::string_view name) const
{
    const auto* a = ObjectComponent<AnimatorComponent>(go);
    return a ? a->GetInt(name) : 0;
}

bool ScriptAnimatorProxy::GetBool(GameObject* go, std::string_view name) const
{
    const auto* a = ObjectComponent<AnimatorComponent>(go);
    return a && a->GetBool(name);
}

float ScriptAnimatorProxy::GetNormalizedTime(GameObject* go) const
{
    const auto* a = ObjectComponent<AnimatorComponent>(go);
    return a ? a->GetNormalizedTime() : 0.0f;
}

std::vector<std::pair<std::string, float>>
ScriptAnimatorProxy::GetCurrentBlendWeights() const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a ? a->currentBlendWeights
             : std::vector<std::pair<std::string, float>>{};
}

std::string ScriptAnimatorProxy::GetCurrentState() const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a ? a->currentStateName : std::string{};
}

std::string ScriptAnimatorProxy::GetBlendToState() const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a ? a->GetBlendToState() : std::string{};
}

std::string ScriptAnimatorProxy::GetBlendToState(GameObject* go) const
{
    const auto* a = ObjectComponent<AnimatorComponent>(go);
    return a ? a->GetBlendToState() : std::string{};
}

float ScriptAnimatorProxy::GetBlendToNormalizedTime() const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a ? a->GetBlendToNormalizedTime() : 0.0f;
}

float ScriptAnimatorProxy::GetBlendToNormalizedTime(GameObject* go) const
{
    const auto* a = ObjectComponent<AnimatorComponent>(go);
    return a ? a->GetBlendToNormalizedTime() : 0.0f;
}

void ScriptAnimatorProxy::SetSpeed(float speed) const
{
    if (auto* a = SelfComponent<AnimatorComponent>(script)) a->speed = speed;
}

void ScriptAnimatorProxy::Play(std::string_view stateName) const
{
    if (auto* a = SelfComponent<AnimatorComponent>(script)) {
        a->currentStateName = std::string(stateName);
        a->stateTime        = 0.0f;
        a->blendToState.clear();
        a->blendWeight      = 0.0f;
    }
}

void ScriptAnimatorProxy::SetSpeed(GameObject* go, float speed) const
{
    if (auto* a = ObjectComponent<AnimatorComponent>(go)) a->speed = speed;
}

void ScriptAnimatorProxy::Play(GameObject* go, std::string_view stateName) const
{
    if (auto* a = ObjectComponent<AnimatorComponent>(go)) {
        a->currentStateName = std::string(stateName);
        a->stateTime        = 0.0f;
        a->blendToState.clear();
        a->blendWeight      = 0.0f;
    }
}

void ScriptAnimatorProxy::SetLayerWeight(std::string_view layerName, float weight) const
{
    if (auto* animator = SelfComponent<AnimatorComponent>(script)) {
        for (auto& layer : animator->layers)
            if (layer.name == layerName) {
                layer.weight = std::clamp(weight, 0.0f, MAX_LAYER_WEIGHT);
                return;
            }
    }
}

float ScriptAnimatorProxy::GetLayerWeight(std::string_view layerName) const
{
    if (const auto* animator = SelfComponent<AnimatorComponent>(script))
        for (const auto& layer : animator->layers)
            if (layer.name == layerName) return layer.weight;
    return 0.0f;
}

// ── レイヤー制御 / Slot ──────────────────────────────────────────────────────
// WHY: 判定と状態遷移は AnimatorComponent 側のメソッドに集約済み。
//      ここは Script から自 GameObject の Animator を引くだけの薄い委譲に留める。

std::string ScriptAnimatorProxy::GetLayerState(std::string_view layerName) const
{
    if (const auto* animator = SelfComponent<AnimatorComponent>(script))
        return animator->GetLayerState(layerName);
    return {};
}

bool ScriptAnimatorProxy::IsLayerInState(
    std::string_view layerName, std::string_view stateName) const
{
    if (const auto* animator = SelfComponent<AnimatorComponent>(script))
        return animator->IsLayerInState(layerName, stateName);
    return false;
}

void ScriptAnimatorProxy::PlayLayerState(
    std::string_view layerName, std::string_view stateName) const
{
    if (auto* animator = SelfComponent<AnimatorComponent>(script))
        animator->PlayLayerState(layerName, stateName);
}

void ScriptAnimatorProxy::SetLayerMask(
    std::string_view layerName, std::string_view maskPath) const
{
    auto* animator = SelfComponent<AnimatorComponent>(script);
    if (!animator) return;
    AnimationLayer* layer = animator->FindLayer(layerName);
    if (!layer) return;
    layer->mask.path = std::string(maskPath);
    // ロード済みキャッシュを落とし、次フレームの AnimatorSystem に読み直させる。
    layer->mask.Invalidate();
}

void ScriptAnimatorProxy::PlaySlot(std::string_view layerName,
                                   std::string_view sourcePath,
                                   std::string_view clipName,
                                   float fadeIn, float fadeOut,
                                   float speed, bool loop) const
{
    if (auto* animator = SelfComponent<AnimatorComponent>(script))
        animator->PlaySlot(layerName, sourcePath, clipName, fadeIn, fadeOut, speed, loop);
}

void ScriptAnimatorProxy::StopSlot(std::string_view layerName, float fadeOut) const
{
    if (auto* animator = SelfComponent<AnimatorComponent>(script))
        animator->StopSlot(layerName, fadeOut);
}

bool ScriptAnimatorProxy::IsSlotPlaying(std::string_view layerName) const
{
    if (const auto* animator = SelfComponent<AnimatorComponent>(script))
        return animator->IsSlotPlaying(layerName);
    return false;
}

float ScriptAnimatorProxy::GetSlotWeight(std::string_view layerName) const
{
    if (const auto* animator = SelfComponent<AnimatorComponent>(script))
        return animator->GetSlotWeight(layerName);
    return 0.0f;
}

void ScriptAnimatorProxy::SetMorphWeight(std::string_view morphName, float weight) const
{
    if (!script || !script->m_gameObject) return;
    GameObject* owner = script->m_gameObject;
    SkinnedMeshRenderer* renderer = owner->GetComponent<SkinnedMeshRenderer>();
    if (!renderer) {
        for (int i = 0; i < owner->GetChildCount(); ++i) {
            GameObject* child = owner->GetChild(i);
            if (child && (renderer = child->GetComponent<SkinnedMeshRenderer>())) break;
        }
    }
    if (renderer) renderer->morphWeights[std::string(morphName)] = weight;
}

float ScriptAnimatorProxy::GetMorphWeight(std::string_view morphName) const
{
    if (!script || !script->m_gameObject) return 0.0f;
    GameObject* owner = script->m_gameObject;
    SkinnedMeshRenderer* renderer = owner->GetComponent<SkinnedMeshRenderer>();
    if (!renderer) {
        for (int i = 0; i < owner->GetChildCount(); ++i) {
            GameObject* child = owner->GetChild(i);
            if (child && (renderer = child->GetComponent<SkinnedMeshRenderer>())) break;
        }
    }
    if (!renderer) return 0.0f;
    const auto it = renderer->morphWeights.find(std::string(morphName));
    return it != renderer->morphWeights.end() ? it->second : 0.0f;
}

math::Vector3 ScriptAnimatorProxy::GetRootMotionDeltaPosition() const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    return animator ? animator->rootMotionDeltaPosition : math::Vector3::ZERO;
}

math::Quaternion ScriptAnimatorProxy::GetRootMotionDeltaRotation() const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    return animator ? animator->rootMotionDeltaRotation : math::Quaternion::Identity();
}

math::Vector3 ScriptAnimatorProxy::GetRootMotionWorldDelta() const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    return animator ? animator->rootMotionWorldDelta : math::Vector3::ZERO;
}

math::Vector3 ScriptAnimatorProxy::GetRootMotionWorldVelocity() const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    return animator ? animator->rootMotionWorldVelocity : math::Vector3::ZERO;
}

float ScriptAnimatorProxy::GetRootMotionDeltaTime() const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    return animator ? animator->rootMotionDeltaTime : 0.0f;
}

bool ScriptAnimatorProxy::IsRootMotionAppliedByEngine() const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    return animator && animator->rootMotionAppliedByEngine;
}

void ScriptAnimatorProxy::SetRootMotionMode(int mode) const
{
    auto* animator = SelfComponent<AnimatorComponent>(script);
    if (!animator) return;
    if (mode < static_cast<int>(RootMotionMode::None) ||
        mode > static_cast<int>(RootMotionMode::ApplyToRigidBody)) return;
    animator->rootMotion.mode = static_cast<RootMotionMode>(mode);
}

int ScriptAnimatorProxy::GetRootMotionMode() const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    return animator ? static_cast<int>(animator->rootMotion.mode)
                    : static_cast<int>(RootMotionMode::None);
}

void ScriptAnimatorProxy::SetRootMotionPositionScale(float scale) const
{
    if (auto* animator = SelfComponent<AnimatorComponent>(script))
        animator->rootMotion.positionScale = scale;
}

void ScriptAnimatorProxy::SetRootMotionRotationScale(float scale) const
{
    if (auto* animator = SelfComponent<AnimatorComponent>(script))
        animator->rootMotion.rotationScale = scale;
}

void ScriptAnimatorProxy::SetRootMotionNodeName(std::string_view nodeName) const
{
    auto* animator = SelfComponent<AnimatorComponent>(script);
    if (!animator) return;
    animator->rootMotion.nodeName = nodeName;
    // 名前を指定したら解決方法も NodeName へ切り替える。空なら クリップ指定へ戻す。
    animator->rootMotion.source = nodeName.empty()
        ? RootMotionSource::ClipDefined
        : RootMotionSource::NodeName;
    // トラックが変わるとサンプル位置の連続性が失われるため、キャッシュを捨てる。
    animator->rootMotionSamples.clear();
}

void ScriptDebugProxy::Log(std::string_view msg) const
{
    core::Logger::Info("%.*s", static_cast<int>(msg.size()), msg.data());
}

void ScriptDebugProxy::LogWarning(std::string_view msg) const
{
    core::Logger::Warn("%.*s", static_cast<int>(msg.size()), msg.data());
}

void ScriptDebugProxy::LogError(std::string_view msg) const
{
    core::Logger::Error("%.*s", static_cast<int>(msg.size()), msg.data());
}

void ScriptDebugProxy::DrawLine(const math::Vector3& a, const math::Vector3& b, const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    script->m_scene->QueueScriptDebugDraw({ ScriptDebugDrawType::Line, a, b, {}, color, 0.0f, duration });
}

void ScriptDebugProxy::DrawSphere(const math::Vector3& center, float radius, const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    script->m_scene->QueueScriptDebugDraw({ ScriptDebugDrawType::Sphere, center, {}, {}, color, radius, duration });
}

void ScriptDebugProxy::DrawBox(const math::Vector3& center, const math::Vector3& halfExtents, const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    script->m_scene->QueueScriptDebugDraw({ ScriptDebugDrawType::Box, center, {}, halfExtents, color, 0.0f, duration });
}

void ScriptDebugProxy::DrawRay(const math::Vector3& origin, const math::Vector3& dir, const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    script->m_scene->QueueScriptDebugDraw({ ScriptDebugDrawType::Ray, origin, origin + dir, {}, color, 0.0f, duration });
}

void ScriptDebugProxy::DrawArrow(const math::Vector3& from, const math::Vector3& to,
                                  float headLength, float headRadius,
                                  const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    // headLength を radius、headRadius を halfExtents.x に格納する (ScriptDebugDrawCommand の多重利用)
    script->m_scene->QueueScriptDebugDraw({
        ScriptDebugDrawType::Arrow, from, to, { headRadius, 0.0f, 0.0f }, color, headLength, duration
    });
}

void ScriptDebugProxy::DrawCone(const math::Vector3& apex, const math::Vector3& direction,
                                 float height, float baseRadius,
                                 const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    // height を halfExtents.x、baseRadius を radius に格納する
    script->m_scene->QueueScriptDebugDraw({
        ScriptDebugDrawType::Cone, apex, direction, { height, 0.0f, 0.0f }, color, baseRadius, duration
    });
}

renderer::PostProcessSettings& ScriptPostProcessProxy::Get() const
{
    assert(script && "Script context is not set");
    return script->GetRuntimePostProcessSettings();
}

const renderer::PostProcessSettings* ScriptPostProcessProxy::TryGet() const
{
    return script ? script->TryGetRuntimePostProcessSettings() : nullptr;
}

void ScriptPostProcessProxy::Set(const renderer::PostProcessSettings& settings) const
{
    if (script) script->SetRuntimePostProcessSettings(settings);
}

void ScriptPostProcessProxy::Clear() const
{
    if (script) script->ClearRuntimePostProcessSettings();
}

renderer::CustomPostProcessSettings& ScriptPostProcessProxy::AddCustom(
    std::string_view name,
    std::string_view shaderPath,
    bool enabled) const
{
    auto& settings = Get();
    auto& custom = settings.customEffects.emplace_back();
    custom.name = std::string(name);
    custom.shaderPath = std::string(shaderPath);
    custom.enabled = enabled;
    return custom;
}

renderer::CustomPostProcessSettings& ScriptPostProcessProxy::EnsureCustom(
    std::string_view name,
    std::string_view shaderPath,
    bool enabled) const
{
    if (auto* custom = FindCustom(name)) {
        if (!shaderPath.empty())
            custom->shaderPath = std::string(shaderPath);
        custom->enabled = enabled;
        return *custom;
    }

    return AddCustom(name, shaderPath, enabled);
}

renderer::CustomPostProcessSettings* ScriptPostProcessProxy::FindCustom(std::string_view name) const
{
    if (!script) return nullptr;

    auto& effects = script->GetRuntimePostProcessSettings().customEffects;
    auto it = std::find_if(effects.begin(), effects.end(), [name](const renderer::CustomPostProcessSettings& custom) {
        return custom.name == name;
    });
    return it != effects.end() ? &(*it) : nullptr;
}

bool ScriptPostProcessProxy::RemoveCustom(std::string_view name) const
{
    if (!script) return false;

    auto& effects = script->GetRuntimePostProcessSettings().customEffects;
    const auto oldSize = effects.size();
    effects.erase(
        std::remove_if(effects.begin(), effects.end(), [name](const renderer::CustomPostProcessSettings& custom) {
            return custom.name == name;
        }),
        effects.end());
    return effects.size() != oldSize;
}

bool ScriptPostProcessProxy::SetCustomEnabled(std::string_view name, bool enabled) const
{
    auto* custom = FindCustom(name);
    if (!custom) return false;
    custom->enabled = enabled;
    return true;
}

bool ScriptPostProcessProxy::SetCustomParameter(std::string_view name, uint32_t index, float value) const
{
    if (index >= 4) return false;

    auto* custom = FindCustom(name);
    if (!custom) return false;
    custom->parameters[index] = value;
    return true;
}

bool ScriptPostProcessProxy::SetCustomParameters(std::string_view name, float x, float y, float z, float w) const
{
    auto* custom = FindCustom(name);
    if (!custom) return false;

    custom->parameters[0] = x;
    custom->parameters[1] = y;
    custom->parameters[2] = z;
    custom->parameters[3] = w;
    return true;
}

bool ScriptPostProcessProxy::LoadProfile(std::string_view profilePath) const
{
    // WHY DataAssetRegistry を経由するか: 同じプロファイルを複数箇所から読んでも
    //     TOML の再パースが起きず、エディタでの編集が即座に反映される。
    //     ファイルを直接パースしていた旧 .fzpp 経路より安く、共有実体とも一致する。
    auto* profile = dynamic_cast<asset::PostProcessProfile*>(
        asset::DataAssetRegistry::Resolve(std::string(profilePath)));
    if (!profile) return false;

    // プロファイルは「効果のリスト」なので、まず既定値へ重み 1 で解決して
    // 具体的な設定へ落としてから渡す。
    // WHY ポストプロセス部分だけ渡すか: このプロキシが書き込むランタイム上書きの器は
    //     Scene の PostProcessSettings で、SSR や TAA といった高度グラフィクスの
    //     置き場が無い。恒久的にプロファイル全体を効かせたい場合は、
    //     PostProcessVolume からこのプロファイルを参照させる。
    renderer::VolumeSettings resolved;
    profile->ApplyTo(resolved, 1.0f);
    Set(resolved.post);
    return true;
}

// =============================================================================
// GizmoProxy — OnDrawGizmos 区間内で Gizmo:: / DebugDraw:: を直接呼ぶプロキシ
// =============================================================================
// WHY: OnDrawGizmos は DebugDraw::BeginFrame/Flush の間で呼ばれるため、
//      コマンドキューを介さず直接描画できる。renderer ポインタは RenderSystem が注入する。

#define GIZMO_ASSERT assert(renderer && "GizmoProxy is only valid inside OnDrawGizmos()")

void GizmoProxy::DrawTransformAxes(const math::Vector3& position,
                                    const math::Quaternion& rotation,
                                    float size) const
{
    GIZMO_ASSERT;
    renderer::Gizmo::TransformAxes(*renderer, position, rotation, size);
}

void GizmoProxy::DrawLine(const math::Vector3& from, const math::Vector3& to,
                           const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::Line(*renderer, from, to, color);
}

void GizmoProxy::DrawArrow(const math::Vector3& from, const math::Vector3& to,
                             float headLength, float headRadius,
                             const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::Arrow(*renderer, from, to, headLength, headRadius, color);
}

void GizmoProxy::DrawSphere(const math::Vector3& center, float radius,
                              const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::Sphere(*renderer, center, radius, color);
}

void GizmoProxy::DrawBox(const math::Vector3& center, const math::Vector3& halfExtents,
                          const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::Box(*renderer, center, halfExtents, color);
}

void GizmoProxy::DrawBox(const math::Vector3& center, const math::Vector3& halfExtents,
                          const math::Quaternion& rotation,
                          const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::Box(*renderer, center, halfExtents, rotation, color);
}

void GizmoProxy::DrawSightCone(const math::Vector3& position,
                                 const math::Vector3& forward,
                                 float fovDegrees, float range,
                                 const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::Gizmo::SightCone(*renderer, position, forward, fovDegrees, range, color);
}

void GizmoProxy::DrawWaypointPath(std::span<const math::Vector3> waypoints,
                                    bool loop,
                                    const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::Gizmo::WaypointPath(*renderer, waypoints, loop, color);
}

void GizmoProxy::DrawDetectionRange(const math::Vector3& center,
                                      float innerRadius, float outerRadius,
                                      const math::Vector4& innerColor,
                                      const math::Vector4& outerColor) const
{
    GIZMO_ASSERT;
    renderer::Gizmo::DetectionRange(*renderer, center, innerRadius, outerRadius,
                                     innerColor, outerColor);
}

void GizmoProxy::DrawTargetLine(const math::Vector3& from, const math::Vector3& to,
                                  const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::Gizmo::TargetLine(*renderer, from, to, color);
}

#undef GIZMO_ASSERT

// ── ScriptNavigationProxy 実装 ───────────────────────────────────────────────
// WHY: フリー関数は Script::m_gameObject (protected) へアクセスできない
//      (friend 指定は ScriptNavigationProxy のメンバー関数にのみ及ぶ)。
//      既存の SelfComponent<T>() (public な Script::GetComponent<T>() 経由) を再利用する。
namespace {
NavMeshAgentComponent* SelfNavAgent(const Script* script)
{
    return SelfComponent<NavMeshAgentComponent>(script);
}
} // namespace

void ScriptNavigationProxy::SetDestination(const math::Vector3& worldPos) const
{
    if (auto* agent = SelfNavAgent(script)) agent->SetDestination(worldPos);
}

void ScriptNavigationProxy::CancelPath() const
{
    if (auto* agent = SelfNavAgent(script)) agent->Stop();
}

void ScriptNavigationProxy::Pause() const
{
    if (auto* agent = SelfNavAgent(script)) agent->isStopped = true;
}

void ScriptNavigationProxy::Resume() const
{
    if (auto* agent = SelfNavAgent(script)) agent->isStopped = false;
}

void ScriptNavigationProxy::Warp(const math::Vector3& worldPos) const
{
    if (!script || !script->m_gameObject) return;
    if (auto* agent = SelfNavAgent(script)) agent->Stop();
    script->m_gameObject->transform.position      = worldPos;
    script->m_gameObject->transform.worldPosition = worldPos;
}

void ScriptNavigationProxy::SetTarget(EntityID target, float repathInterval) const
{
    if (auto* agent = SelfNavAgent(script)) agent->SetTarget(target, repathInterval);
}

void ScriptNavigationProxy::SetTarget(const GameObject& target, float repathInterval) const
{
    SetTarget(target.GetID(), repathInterval);
}

void ScriptNavigationProxy::ClearTarget() const
{
    if (auto* agent = SelfNavAgent(script)) agent->ClearTarget();
}

bool ScriptNavigationProxy::IsFollowingTarget() const
{
    auto* agent = SelfNavAgent(script);
    return agent && agent->target.IsValid();
}

void ScriptNavigationProxy::BakeNavMesh() const
{
    if (!script || !script->m_scene) return;
    auto* agent = SelfNavAgent(script);
    const int typeId = agent ? agent->agentTypeId : 0;
    for (EntityID eid : script->m_scene->GetEntities<NavMeshSurfaceComponent>()) {
        auto* surf = script->m_scene->GetComponent<NavMeshSurfaceComponent>(eid);
        if (surf && surf->agentTypeId == typeId) surf->needsBake = true;
    }
}

bool ScriptNavigationProxy::IsPaused() const
{
    auto* agent = SelfNavAgent(script);
    return agent && agent->isStopped;
}

bool ScriptNavigationProxy::IsMoving() const
{
    auto* agent = SelfNavAgent(script);
    if (!agent || agent->isStopped) return false;
    return agent->state == NavMeshAgentState::MOVING
        || agent->state == NavMeshAgentState::TRAVERSING_LINK;
}

bool ScriptNavigationProxy::HasPath() const
{
    auto* agent = SelfNavAgent(script);
    return agent && agent->HasPath();
}

bool ScriptNavigationProxy::HasArrived() const
{
    auto* agent = SelfNavAgent(script);
    return agent && agent->destinationReached;
}

float ScriptNavigationProxy::GetRemainingDistance() const
{
    auto* agent = SelfNavAgent(script);
    return agent ? agent->remainingDistance : 0.0f;
}

bool ScriptNavigationProxy::IsStuck() const
{
    auto* agent = SelfNavAgent(script);
    return agent && agent->isStuck;
}

math::Vector3 ScriptNavigationProxy::GetVelocity() const
{
    auto* agent = SelfNavAgent(script);
    return agent ? agent->velocity : math::Vector3::ZERO;
}

void ScriptNavigationProxy::SetSpeed(float speed) const
{
    if (auto* agent = SelfNavAgent(script)) agent->maxSpeed = speed > 0.0f ? speed : 0.0f;
}

void ScriptNavigationProxy::SetAngularSpeed(float degPerSec) const
{
    if (auto* agent = SelfNavAgent(script)) agent->angularSpeedDeg = degPerSec >= 0.0f ? degPerSec : 0.0f;
}

int ScriptNavigationProxy::GetAreaMask() const
{
    auto* agent = SelfNavAgent(script);
    return agent ? agent->areaMask : -1;
}

void ScriptNavigationProxy::SetAreaMask(int mask) const
{
    if (auto* agent = SelfNavAgent(script)) agent->areaMask = mask;
}

bool ScriptNavigationProxy::CanSeeTarget() const
{
    if (!script || !script->m_gameObject) return false;
    auto* sensor = script->m_gameObject->GetComponent<NavMeshSensorComponent>();
    return sensor && sensor->targetVisible;
}

GameObject* ScriptNavigationProxy::GetDetectedTarget() const
{
    if (!script || !script->m_gameObject) return nullptr;
    auto* sensor = script->m_gameObject->GetComponent<NavMeshSensorComponent>();
    if (!sensor || !sensor->targetVisible) return nullptr;
    return script->m_scene ? script->m_scene->GetGameObject(sensor->detectedTarget) : nullptr;
}

// ── EntityRef 実装 ──────────────────────────────────────────────────────────
GameObject* EntityRef::Resolve(const ScriptSceneProxy& scene) const
{
    return id.IsValid() ? scene.GetGameObject(id) : nullptr;
}

GameObject* EntityRef::Resolve(Scene& scene) const
{
    return id.IsValid() ? scene.GetGameObject(id) : nullptr;
}

// ---------------------------------------------------------------------------
// ScriptCharacterProxy
// ---------------------------------------------------------------------------
namespace {
CharacterControllerComponent* SelfCharacter(const Script* script)
{
    return SelfComponent<CharacterControllerComponent>(script);
}
} // namespace

void ScriptCharacterProxy::Tick(float dt) const
{
    auto* cc = SelfCharacter(script);
    if (!cc) return;
    // RigidBody は SelfRigidBody() が script→Component→rigidBody と辿る。
    cc->Tick(SelfRigidBody(script), dt);
}

void ScriptCharacterProxy::Jump(const math::Vector3& impulse) const
{
    auto* cc = SelfCharacter(script);
    if (!cc) return;
    cc->Jump(SelfRigidBody(script), impulse);
}

void ScriptCharacterProxy::JumpAtVelocity(float verticalSpeed) const
{
    auto* cc = SelfCharacter(script);
    if (cc) cc->JumpAtVelocity(SelfRigidBody(script), verticalSpeed);
}

void ScriptCharacterProxy::SetHorizontalVelocity(const math::Vector3& velocity) const
{
    auto* cc = SelfCharacter(script);
    if (cc) cc->SetHorizontalVelocity(SelfRigidBody(script), velocity);
}

void ScriptCharacterProxy::AddHorizontalVelocity(const math::Vector3& velocity) const
{
    auto* cc = SelfCharacter(script);
    if (cc) cc->AddHorizontalVelocity(SelfRigidBody(script), velocity);
}

void ScriptCharacterProxy::Move(const math::Vector3& desiredVelocity,
                                float dt,
                                float acceleration,
                                float deceleration) const
{
    auto* cc = SelfCharacter(script);
    if (cc) cc->Move(SelfRigidBody(script), desiredVelocity, dt, acceleration, deceleration);
}

math::Vector3 ScriptCharacterProxy::GetVelocity() const
{
    auto* cc = SelfCharacter(script);
    return cc ? cc->GetVelocity(SelfRigidBody(script)) : math::Vector3::ZERO;
}

math::Vector3 ScriptCharacterProxy::GetHorizontalVelocity() const
{
    auto* cc = SelfCharacter(script);
    return cc ? cc->GetHorizontalVelocity(SelfRigidBody(script)) : math::Vector3::ZERO;
}

void ScriptCharacterProxy::RegisterGroundContact(const CollisionInfo& info) const
{
    if (auto* cc = SelfCharacter(script)) cc->RegisterGroundContact(info);
}

bool ScriptCharacterProxy::IsGrounded() const
{
    auto* cc = SelfCharacter(script);
    return cc && cc->isGrounded;
}

float ScriptCharacterProxy::GetVerticalSpeed() const
{
    auto* cc = SelfCharacter(script);
    return cc ? cc->verticalSpeed : 0.0f;
}

math::Vector3 ScriptCharacterProxy::GetGroundNormal() const
{
    auto* cc = SelfCharacter(script);
    return cc ? cc->groundNormal : math::Vector3::UP;
}

void ScriptCharacterProxy::ForceGrounded(const math::Vector3& normal) const
{
    if (auto* cc = SelfCharacter(script)) cc->ForceGrounded(normal);
}

void ScriptCharacterProxy::ForceAirborne() const
{
    if (auto* cc = SelfCharacter(script)) cc->ForceAirborne();
}

void ScriptCharacterProxy::UseAutomaticGrounding() const
{
    if (auto* cc = SelfCharacter(script)) cc->UseAutomaticGrounding();
}

void ScriptCharacterProxy::SetEnabled(bool enabled) const
{
    if (auto* cc = SelfCharacter(script)) cc->enabled = enabled;
}

// ---------------------------------------------------------------------------
// ScriptMeshProxy
// ---------------------------------------------------------------------------
void ScriptMeshProxy::SetEnabled(bool enabled) const
{
    if (!script || !script->m_gameObject) return;
    if (auto* mr = script->m_gameObject->GetComponent<MeshRenderer>())
        mr->enabled = enabled;
    if (auto* smr = script->m_gameObject->GetComponent<SkinnedMeshRenderer>())
        smr->enabled = enabled;
}

bool ScriptMeshProxy::IsEnabled() const
{
    if (!script || !script->m_gameObject) return false;
    if (auto* mr = script->m_gameObject->GetComponent<MeshRenderer>())
        if (mr->enabled) return true;
    if (auto* smr = script->m_gameObject->GetComponent<SkinnedMeshRenderer>())
        if (smr->enabled) return true;
    return false;
}

void ScriptMeshProxy::SetCastShadows(bool castShadows) const
{
    if (!script || !script->m_gameObject) return;
    if (auto* mr = script->m_gameObject->GetComponent<MeshRenderer>())
        mr->castShadows = castShadows;
    if (auto* smr = script->m_gameObject->GetComponent<SkinnedMeshRenderer>())
        smr->castShadows = castShadows;
}

bool ScriptMeshProxy::GetCastShadows() const
{
    if (!script || !script->m_gameObject) return false;
    if (auto* mr = script->m_gameObject->GetComponent<MeshRenderer>())
        if (mr->castShadows) return true;
    if (auto* smr = script->m_gameObject->GetComponent<SkinnedMeshRenderer>())
        if (smr->castShadows) return true;
    return false;
}

void ScriptMeshProxy::SetMeshPath(std::string_view path) const
{
    if (!script || !script->m_gameObject) return;
    if (auto* mr = script->m_gameObject->GetComponent<MeshRenderer>())
        mr->meshPath = std::string(path);
}

void ScriptMeshProxy::SetModelPath(std::string_view path) const
{
    if (!script || !script->m_gameObject) return;
    if (auto* smr = script->m_gameObject->GetComponent<SkinnedMeshRenderer>())
        smr->modelPath = std::string(path);
}

// ---------------------------------------------------------------------------
// ScriptIKProxy
// ---------------------------------------------------------------------------
namespace {
IKSolverComponent* SelfIK(const Script* script)
{
    return SelfComponent<IKSolverComponent>(script);
}

IKChain* FindChainByTarget(IKSolverComponent* ik, std::string_view targetName)
{
    if (!ik) return nullptr;
    for (IKChain& c : ik->chains)
        if (c.targetName == targetName) return &c;
    return nullptr;
}
} // namespace

void ScriptIKProxy::SetChainEnabled(std::string_view targetName, bool enabled) const
{
    if (auto* c = FindChainByTarget(SelfIK(script), targetName)) c->enabled = enabled;
}

void ScriptIKProxy::SetAllEnabled(bool enabled) const
{
    auto* ik = SelfIK(script);
    if (!ik) return;
    for (IKChain& c : ik->chains) c.enabled = enabled;
}

void ScriptIKProxy::SetChainWeight(std::string_view targetName, float weight) const
{
    if (auto* c = FindChainByTarget(SelfIK(script), targetName))
        c->weight = weight < 0.0f ? 0.0f : (weight > 1.0f ? 1.0f : weight);
}

float ScriptIKProxy::GetChainWeight(std::string_view targetName) const
{
    auto* c = FindChainByTarget(SelfIK(script), targetName);
    return c ? c->weight : 0.0f;
}

void ScriptIKProxy::SetChainTarget(std::string_view targetName, EntityID target) const
{
    if (auto* c = FindChainByTarget(SelfIK(script), targetName))
        c->targetEntity = target;
}

void ScriptIKProxy::SetChainTarget(std::string_view targetName, const GameObject& target) const
{
    SetChainTarget(targetName, target.GetID());
}

void ScriptIKProxy::SetEnabled(bool enabled) const
{
    if (auto* ik = SelfIK(script)) ik->enabled = enabled;
}

// ---------------------------------------------------------------------------
// ScriptWaterProxy
// ---------------------------------------------------------------------------
namespace {
WaterComponent* SelfWater(const Script* script)
{
    return SelfComponent<WaterComponent>(script);
}
} // namespace

float ScriptWaterProxy::GetSurfaceHeightWorld(float worldX, float worldZ, float time) const
{
    auto* wc = SelfWater(script);
    if (!wc || !script->m_gameObject) return 0.0f;

    // ワールド座標 → ローカル座標 (回転・スケールを考慮)
    const auto& t = script->m_gameObject->transform;
    math::Vector3 delta = { worldX - t.worldPosition.x, 0.0f, worldZ - t.worldPosition.z };
    const math::Vector3 local = t.worldRotation.Inverse() * delta;
    const float lx = t.worldScale.x > 0.0f ? local.x / t.worldScale.x : local.x;
    const float lz = t.worldScale.z > 0.0f ? local.z / t.worldScale.z : local.z;

    return t.worldPosition.y + wc->GetSurfaceHeightAt(lx, lz, time);
}

float ScriptWaterProxy::GetSurfaceHeightLocal(float localX, float localZ, float time) const
{
    auto* wc = SelfWater(script);
    return wc ? wc->GetSurfaceHeightAt(localX, localZ, time) : 0.0f;
}

void ScriptWaterProxy::SetWaveAmplitude(int index, float amplitude) const
{
    auto* wc = SelfWater(script);
    if (wc && index >= 0 && index < 4) wc->waves[static_cast<size_t>(index)].amplitude = amplitude;
}

void ScriptWaterProxy::SetWaveWavelength(int index, float wavelength) const
{
    auto* wc = SelfWater(script);
    if (wc && index >= 0 && index < 4) wc->waves[static_cast<size_t>(index)].wavelength = wavelength;
}

void ScriptWaterProxy::SetWaveSteepness(int index, float steepness) const
{
    auto* wc = SelfWater(script);
    if (!wc || index < 0 || index >= 4) return;
    float s = steepness < 0.0f ? 0.0f : (steepness > 1.0f ? 1.0f : steepness);
    wc->waves[static_cast<size_t>(index)].steepness = s;
}

void ScriptWaterProxy::SetWaveDirection(int index, math::Vector2 dir) const
{
    auto* wc = SelfWater(script);
    if (wc && index >= 0 && index < 4) wc->waves[static_cast<size_t>(index)].direction = dir;
}

void ScriptWaterProxy::SetGerstnerEnabled(bool enabled) const
{
    if (auto* wc = SelfWater(script)) wc->enableGerstnerWaves = enabled;
}

void ScriptWaterProxy::SetEnabled(bool enabled) const
{
    if (auto* wc = SelfWater(script)) wc->enabled = enabled;
}

bool ScriptWaterProxy::IsEnabled() const
{
    auto* wc = SelfWater(script);
    return wc && wc->enabled;
}

// ---------------------------------------------------------------------------
namespace {
TerrainComponent* SelfTerrain(const Script* script)
{
    return SelfComponent<TerrainComponent>(script);
}

math::Vector3 WorldToTerrainLocal(const GameObject* gameObject, const math::Vector3& worldPos)
{
    if (!gameObject) return worldPos;
    const auto& t = gameObject->transform;
    const math::Vector3 delta = worldPos - t.worldPosition;
    const math::Vector3 local = t.worldRotation.Inverse() * delta;
    return {
        t.worldScale.x > 0.0f ? local.x / t.worldScale.x : local.x,
        t.worldScale.y > 0.0f ? local.y / t.worldScale.y : local.y,
        t.worldScale.z > 0.0f ? local.z / t.worldScale.z : local.z
    };
}
} // namespace

float ScriptTerrainProxy::GetHeightLocal(float localX, float localZ) const
{
    auto* terrain = SelfTerrain(script);
    return terrain ? terrain->GetHeightAt(localX, localZ) : 0.0f;
}

math::Vector3 ScriptTerrainProxy::GetNormalLocal(float localX, float localZ) const
{
    auto* terrain = SelfTerrain(script);
    return terrain ? terrain->GetNormalAt(localX, localZ) : math::Vector3::UP;
}

float ScriptTerrainProxy::GetHeightWorld(const math::Vector3& worldPos) const
{
    auto* terrain = SelfTerrain(script);
    if (!terrain || !script || !script->m_gameObject) return worldPos.y;
    const math::Vector3 local = WorldToTerrainLocal(script->m_gameObject, worldPos);
    return script->m_gameObject->transform.worldPosition.y + terrain->GetHeightAt(local.x, local.z);
}

math::Vector3 ScriptTerrainProxy::GetNormalWorld(const math::Vector3& worldPos) const
{
    auto* terrain = SelfTerrain(script);
    if (!terrain) return math::Vector3::UP;
    const math::Vector3 local = WorldToTerrainLocal(script ? script->m_gameObject : nullptr, worldPos);
    return terrain->GetNormalAt(local.x, local.z);
}

bool ScriptTerrainProxy::SetHeightAtGrid(int x, int z, float worldHeight) const
{
    auto* terrain = SelfTerrain(script);
    return terrain && terrain->SetHeightAtGrid(x, z, worldHeight);
}

bool ScriptTerrainProxy::PaintLayerAtGrid(int x, int z, int layer, float weight) const
{
    auto* terrain = SelfTerrain(script);
    return terrain && terrain->PaintLayerAtGrid(x, z, layer, weight);
}

bool ScriptTerrainProxy::SetLayerMaterial(int layer, std::string_view materialPath) const
{
    auto* terrain = SelfTerrain(script);
    return terrain && terrain->SetLayerMaterial(layer, std::string(materialPath));
}

void ScriptTerrainProxy::RequestRebuild() const
{
    if (auto* terrain = SelfTerrain(script)) {
        terrain->RequestHeightRebuild();
        terrain->RequestSplatRebuild();
        terrain->RequestMaterialRebuild();
    }
}

void ScriptFoliageProxy::SetEnabled(bool enabled) const
{
    if (auto* foliage = SelfComponent<FoliageComponent>(script)) foliage->SetEnabled(enabled);
}

void ScriptFoliageProxy::RequestBake(bool rebuildChildren) const
{
    if (auto* foliage = SelfComponent<FoliageComponent>(script)) foliage->RequestBake(rebuildChildren);
}

bool ScriptFoliageProxy::AddStamp(size_t speciesIndex, const math::Vector3& localPosition,
                                  float rotationY, float scale) const
{
    auto* foliage = SelfComponent<FoliageComponent>(script);
    return foliage && foliage->AddStamp(speciesIndex, localPosition, rotationY, scale);
}

bool ScriptFoliageProxy::ClearStamps(size_t speciesIndex) const
{
    auto* foliage = SelfComponent<FoliageComponent>(script);
    return foliage && foliage->ClearStamps(speciesIndex);
}

bool ScriptFoliageProxy::SetDensity(size_t speciesIndex, float densityPer100SquareMeters) const
{
    auto* foliage = SelfComponent<FoliageComponent>(script);
    return foliage && foliage->SetDensity(speciesIndex, densityPer100SquareMeters);
}

bool ScriptFoliageProxy::SetDrawDistance(size_t speciesIndex, float distance) const
{
    auto* foliage = SelfComponent<FoliageComponent>(script);
    return foliage && foliage->SetDrawDistance(speciesIndex, distance);
}

// ScriptEnvironmentProxy
// ---------------------------------------------------------------------------
namespace {
// シーン全体から最初のコンポーネントを検索する汎用ヘルパー。
// WHY: EnvironmentLight / AtmosphericScattering / SkyRenderer は通常シーンに 1 つしかなく、
//      どの Script からでもアクセスできる設計にするためシーン全探索を行う。
//      enabled チェックを行わないのは、SetEnabled(false) を呼ぶためにコンポーネントを
//      見つける必要があるため。
// NOTE: Script* ではなく Scene* を受け取る — Script::m_scene は protected のため
//       free function から直接アクセスできない。呼び出し側 (friend の proxy メソッド) で
//       script->m_scene を取り出して渡す。
template<typename T>
T* FindFirstInScene(Scene* scene)
{
    if (!scene) return nullptr;
    for (EntityID eid : scene->GetEntities<T>()) {
        if (auto* c = scene->GetComponent<T>(eid)) return c;
    }
    return nullptr;
}
} // namespace

void ScriptEnvironmentProxy::SetIBLEnabled(bool enabled) const
{
    if (auto* c = FindFirstInScene<EnvironmentLightComponent>(script->m_scene)) c->enabled = enabled;
}
void ScriptEnvironmentProxy::SetIBLIntensity(float intensity) const
{
    if (auto* c = FindFirstInScene<EnvironmentLightComponent>(script->m_scene)) c->intensity = intensity;
}
void ScriptEnvironmentProxy::SetIBLDiffuseScale(float scale) const
{
    if (auto* c = FindFirstInScene<EnvironmentLightComponent>(script->m_scene)) c->diffuseScale = scale;
}
void ScriptEnvironmentProxy::SetIBLSpecularScale(float scale) const
{
    if (auto* c = FindFirstInScene<EnvironmentLightComponent>(script->m_scene)) c->specularScale = scale;
}

void ScriptEnvironmentProxy::SetFogEnabled(bool enabled) const
{
    if (auto* c = FindFirstInScene<AtmosphericScatteringComponent>(script->m_scene)) c->fogEnabled = enabled;
}
void ScriptEnvironmentProxy::SetFogDensity(float density) const
{
    if (auto* c = FindFirstInScene<AtmosphericScatteringComponent>(script->m_scene)) c->fogDensity = density;
}
void ScriptEnvironmentProxy::SetFogFar(float fogFar) const
{
    if (auto* c = FindFirstInScene<AtmosphericScatteringComponent>(script->m_scene)) c->fogFar = fogFar;
}
void ScriptEnvironmentProxy::SetFogColor(const math::Vector3& rgb) const
{
    if (auto* c = FindFirstInScene<AtmosphericScatteringComponent>(script->m_scene)) c->fogColor = rgb;
}
bool ScriptEnvironmentProxy::IsFogEnabled() const
{
    auto* c = FindFirstInScene<AtmosphericScatteringComponent>(script->m_scene);
    return c && c->fogEnabled;
}
float ScriptEnvironmentProxy::GetFogDensity() const
{
    auto* c = FindFirstInScene<AtmosphericScatteringComponent>(script->m_scene);
    return c ? c->fogDensity : 0.0f;
}

void ScriptEnvironmentProxy::SetSunIntensity(float intensity) const
{
    if (auto* c = FindFirstInScene<SkyRenderer>(script->m_scene)) c->sunIntensity = intensity;
}
void ScriptEnvironmentProxy::SetMieScattering(float mie) const
{
    if (auto* c = FindFirstInScene<SkyRenderer>(script->m_scene)) c->mieScattering = mie;
}
void ScriptEnvironmentProxy::SetMieG(float g) const
{
    if (auto* c = FindFirstInScene<SkyRenderer>(script->m_scene)) c->mieG = g;
}
float ScriptEnvironmentProxy::GetSunIntensity() const
{
    auto* c = FindFirstInScene<SkyRenderer>(script->m_scene);
    return c ? c->sunIntensity : 0.0f;
}

// ---------------------------------------------------------------------------
// ScriptDecalProxy
// ---------------------------------------------------------------------------
namespace {
DecalComponent* SelfDecal(const Script* script)
{
    return SelfComponent<DecalComponent>(script);
}
} // namespace

void ScriptDecalProxy::SetEnabled(bool enabled) const
{
    if (auto* d = SelfDecal(script)) d->enabled = enabled;
}
void ScriptDecalProxy::SetLifetime(float seconds) const
{
    if (auto* d = SelfDecal(script)) d->lifetime = seconds;
}
void ScriptDecalProxy::SetFadeTime(float fadeTime) const
{
    if (auto* d = SelfDecal(script)) d->fadeTime = fadeTime;
}
void ScriptDecalProxy::ResetAge() const
{
    if (auto* d = SelfDecal(script)) d->age = 0.0f;
}
void ScriptDecalProxy::SetAlbedoColor(float r, float g, float b, float a) const
{
    auto* d = SelfDecal(script);
    if (!d) return;
    d->albedoColor[0] = r; d->albedoColor[1] = g;
    d->albedoColor[2] = b; d->albedoColor[3] = a;
}
void ScriptDecalProxy::SetNormalStrength(float strength) const
{
    if (auto* d = SelfDecal(script)) d->normalStrength = strength;
}
void ScriptDecalProxy::SetEmissiveColor(float r, float g, float b) const
{
    auto* d = SelfDecal(script);
    if (!d) return;
    d->emissiveColor[0] = r; d->emissiveColor[1] = g; d->emissiveColor[2] = b;
}
void ScriptDecalProxy::SetEmissiveScale(float scale) const
{
    if (auto* d = SelfDecal(script)) d->emissiveScale = scale;
}
void ScriptDecalProxy::SetAlbedoTexture(std::string_view path) const
{
    if (auto* d = SelfDecal(script)) d->albedoTexPath = std::string(path);
}
void ScriptDecalProxy::SetNormalTexture(std::string_view path) const
{
    if (auto* d = SelfDecal(script)) d->normalTexPath = std::string(path);
}
void ScriptDecalProxy::SetEmissiveTexture(std::string_view path) const
{
    if (auto* d = SelfDecal(script)) d->emissiveTexPath = std::string(path);
}

// ---------------------------------------------------------------------------
// ScriptVolumeProxy
// ---------------------------------------------------------------------------
namespace {
VolumeComponent* SelfVolume(const Script* script)
{
    return SelfComponent<VolumeComponent>(script);
}
} // namespace

void ScriptVolumeProxy::SetEnabled(bool enabled) const
{
    if (auto* v = SelfVolume(script)) v->enabled = enabled;
}
void ScriptVolumeProxy::SetGravity(const math::Vector3& gravity) const
{
    if (auto* v = SelfVolume(script)) v->gravity = gravity;
}
void ScriptVolumeProxy::SetSwirlStrength(float strength) const
{
    if (auto* v = SelfVolume(script)) v->swirlStrength = strength;
}
void ScriptVolumeProxy::SetBuoyancy(float buoyancy) const
{
    if (auto* v = SelfVolume(script)) v->buoyancy = buoyancy;
}
void ScriptVolumeProxy::SetExplosionImpulse(float impulse) const
{
    if (auto* v = SelfVolume(script)) v->explosionImpulse = impulse;
}
void ScriptVolumeProxy::SetTimeScale(float scale) const
{
    if (auto* v = SelfVolume(script)) v->timeScale = scale;
}
void ScriptVolumeProxy::SetDuration(float seconds) const
{
    if (auto* v = SelfVolume(script)) v->duration = seconds;
}
void ScriptVolumeProxy::ResetElapsed() const
{
    if (auto* v = SelfVolume(script)) v->elapsed = 0.0f;
}

// ---------------------------------------------------------------------------
// ScriptReflectionProbeProxy
// ---------------------------------------------------------------------------
namespace {
ReflectionProbeComponent* SelfReflectionProbe(const Script* script)
{
    return SelfComponent<ReflectionProbeComponent>(script);
}
} // namespace

void ScriptReflectionProbeProxy::SetEnabled(bool enabled) const
{
    if (auto* rp = SelfReflectionProbe(script)) rp->enabled = enabled;
}
void ScriptReflectionProbeProxy::SetIntensity(float intensity) const
{
    if (auto* rp = SelfReflectionProbe(script)) rp->intensity = intensity;
}
void ScriptReflectionProbeProxy::SetInfluenceRadius(float radius) const
{
    if (auto* rp = SelfReflectionProbe(script)) rp->influenceRadius = radius;
}
void ScriptReflectionProbeProxy::SetBoxInfluence(bool useBox) const
{
    if (auto* rp = SelfReflectionProbe(script)) rp->boxInfluence = useBox;
}
void ScriptReflectionProbeProxy::SetBoxExtents(const math::Vector3& halfExtents) const
{
    if (auto* rp = SelfReflectionProbe(script)) rp->boxExtents = halfExtents;
}
void ScriptReflectionProbeProxy::SetCubemap(std::string_view path) const
{
    if (auto* rp = SelfReflectionProbe(script)) rp->cubemapPath = std::string(path);
}

// ---------------------------------------------------------------------------
// ScriptLifetimeProxy
// ---------------------------------------------------------------------------
namespace {
LifetimeComponent* SelfLifetime(const Script* script)
{
    return SelfComponent<LifetimeComponent>(script);
}
} // namespace

void ScriptLifetimeProxy::SetRemaining(float seconds) const
{
    if (auto* lc = SelfLifetime(script)) lc->remaining = seconds;
}
float ScriptLifetimeProxy::GetRemaining() const
{
    auto* lc = SelfLifetime(script);
    return lc ? lc->remaining : 0.0f;
}
void ScriptLifetimeProxy::SetEnabled(bool enabled) const
{
    if (auto* lc = SelfLifetime(script)) lc->enabled = enabled;
}
void ScriptLifetimeProxy::Kill() const
{
    // remaining = 0 にして LifetimeSystem に次フレームで GO を破棄させる。
    if (auto* lc = SelfLifetime(script)) lc->remaining = 0.0f;
}

// ScriptParticleForceFieldProxy
void ScriptParticleForceFieldProxy::SetEnabled(bool enabled) const
{
    if (auto* field = SelfComponent<ParticleForceField>(script)) field->enabled = enabled;
}
void ScriptParticleForceFieldProxy::SetType(ScriptParticleForceFieldType type) const
{
    if (auto* field = SelfComponent<ParticleForceField>(script)) {
        const int value = (std::max)(0, (std::min)(static_cast<int>(type), 5));
        field->fieldType = static_cast<ParticleForceFieldType>(value);
    }
}
void ScriptParticleForceFieldProxy::SetStrength(float strength) const
{
    if (auto* field = SelfComponent<ParticleForceField>(script)) field->strength = strength;
}
void ScriptParticleForceFieldProxy::SetRadius(float radius, float falloffPower) const
{
    if (auto* field = SelfComponent<ParticleForceField>(script)) {
        field->radius = radius;
        field->falloffPower = (std::max)(falloffPower, 0.01f);
    }
}
void ScriptParticleForceFieldProxy::SetDirection(const math::Vector3& direction) const
{
    if (auto* field = SelfComponent<ParticleForceField>(script)) field->direction = direction;
}
void ScriptParticleForceFieldProxy::SetTurbulence(float frequency, float speed) const
{
    if (auto* field = SelfComponent<ParticleForceField>(script)) {
        field->noiseFrequency = (std::max)(frequency, 0.0f);
        field->noiseSpeed = speed;
    }
}

// ScriptCloudProxy
void ScriptCloudProxy::SetEnabled(bool enabled) const
{
    if (auto* cloud = SelfComponent<VolumetricCloudComponent>(script)) cloud->enabled = enabled;
}
void ScriptCloudProxy::SetLayer(float bottomHeight, float thickness) const
{
    if (auto* cloud = SelfComponent<VolumetricCloudComponent>(script)) {
        cloud->bottomHeight = bottomHeight;
        cloud->thickness = (std::max)(thickness, 1.0f);
    }
}
void ScriptCloudProxy::SetCoverage(float coverage, float density) const
{
    if (auto* cloud = SelfComponent<VolumetricCloudComponent>(script)) {
        cloud->coverage = (std::max)(0.0f, (std::min)(coverage, 1.0f));
        cloud->density = (std::max)(density, 0.0f);
    }
}
void ScriptCloudProxy::SetWind(const math::Vector2& direction, float speed) const
{
    if (auto* cloud = SelfComponent<VolumetricCloudComponent>(script)) {
        cloud->windDirection = direction;
        cloud->windSpeed = speed;
    }
}
void ScriptCloudProxy::SetLighting(float absorption, float ambientStrength,
                                   float silverLining, const math::Vector3& albedo) const
{
    if (auto* cloud = SelfComponent<VolumetricCloudComponent>(script)) {
        cloud->lightAbsorption = (std::max)(absorption, 0.0f);
        cloud->ambientStrength = (std::max)(0.0f, (std::min)(ambientStrength, 1.0f));
        cloud->silverLining = (std::max)(silverLining, 0.0f);
        cloud->albedo = albedo;
    }
}
void ScriptCloudProxy::SetQuality(int stepCount, float maxDistance) const
{
    if (auto* cloud = SelfComponent<VolumetricCloudComponent>(script)) {
        cloud->stepCount = (std::max)(8, (std::min)(stepCount, 96));
        cloud->maxDistance = (std::max)(maxDistance, 100.0f);
    }
}

// ScriptSunMoonProxy
void ScriptSunMoonProxy::SetEnabled(bool enabled) const
{
    if (auto* sunMoon = SelfComponent<SunMoonRenderer>(script)) sunMoon->enabled = enabled;
}
void ScriptSunMoonProxy::SetSun(bool enabled, float intensity) const
{
    if (auto* sunMoon = SelfComponent<SunMoonRenderer>(script)) {
        sunMoon->sunEnabled = enabled;
        sunMoon->sunIntensity = (std::max)(intensity, 0.0f);
    }
}
void ScriptSunMoonProxy::SetMoon(bool enabled, float size, float brightness,
                                 const math::Vector3& color) const
{
    if (auto* sunMoon = SelfComponent<SunMoonRenderer>(script)) {
        sunMoon->moonEnabled = enabled;
        sunMoon->moonSize = (std::max)(size, 0.0f);
        sunMoon->moonBrightness = (std::max)(brightness, 0.0f);
        sunMoon->moonColor = color;
    }
}

// ScriptTerrainDetailProxy
void ScriptTerrainDetailProxy::SetEnabled(bool enabled) const
{
    if (auto* detail = SelfComponent<TerrainDetailComponent>(script)) detail->enabled = enabled;
}
int ScriptTerrainDetailProxy::GetLayerCount() const
{
    const auto* detail = SelfComponent<TerrainDetailComponent>(script);
    return detail ? static_cast<int>(detail->layers.size()) : 0;
}
bool ScriptTerrainDetailProxy::SetDensity(size_t layerIndex, float density) const
{
    auto* detail = SelfComponent<TerrainDetailComponent>(script);
    if (!detail || layerIndex >= detail->layers.size()) return false;
    detail->layers[layerIndex].density = (std::max)(density, 0.0f);
    detail->needsBake = true;
    return true;
}
bool ScriptTerrainDetailProxy::SetScaleRange(size_t layerIndex, float minScale, float maxScale) const
{
    auto* detail = SelfComponent<TerrainDetailComponent>(script);
    if (!detail || layerIndex >= detail->layers.size()) return false;
    minScale = (std::max)(minScale, 0.001f);
    detail->layers[layerIndex].minScale = minScale;
    detail->layers[layerIndex].maxScale = (std::max)(maxScale, minScale);
    detail->needsBake = true;
    return true;
}
bool ScriptTerrainDetailProxy::SetDrawDistance(size_t layerIndex, float fadeStartDistance,
                                                float drawDistance) const
{
    auto* detail = SelfComponent<TerrainDetailComponent>(script);
    if (!detail || layerIndex >= detail->layers.size()) return false;
    drawDistance = (std::max)(drawDistance, 0.0f);
    detail->layers[layerIndex].drawDistance = drawDistance;
    detail->layers[layerIndex].fadeStartDist = (std::max)(0.0f, (std::min)(fadeStartDistance, drawDistance));
    return true;
}
bool ScriptTerrainDetailProxy::SetWind(size_t layerIndex, float strength, float frequency) const
{
    auto* detail = SelfComponent<TerrainDetailComponent>(script);
    if (!detail || layerIndex >= detail->layers.size()) return false;
    detail->layers[layerIndex].windStrength = strength;
    detail->layers[layerIndex].windFrequency = (std::max)(frequency, 0.0f);
    return true;
}
void ScriptTerrainDetailProxy::RequestBake() const
{
    if (auto* detail = SelfComponent<TerrainDetailComponent>(script)) detail->needsBake = true;
}

// ScriptPatrolProxy
void ScriptPatrolProxy::SetEnabled(bool enabled) const
{
    if (auto* patrol = SelfComponent<NavMeshPatrolComponent>(script)) patrol->enabled = enabled;
}
void ScriptPatrolProxy::SetMode(ScriptPatrolMode mode) const
{
    if (auto* patrol = SelfComponent<NavMeshPatrolComponent>(script)) {
        patrol->mode = mode == ScriptPatrolMode::PING_PONG
            ? NavMeshPatrolComponent::Mode::PING_PONG
            : NavMeshPatrolComponent::Mode::LOOP;
    }
}
void ScriptPatrolProxy::SetWaitTime(float seconds) const
{
    if (auto* patrol = SelfComponent<NavMeshPatrolComponent>(script)) patrol->waitTime = (std::max)(seconds, 0.0f);
}
void ScriptPatrolProxy::ClearWaypoints() const
{
    if (auto* patrol = SelfComponent<NavMeshPatrolComponent>(script)) {
        patrol->waypoints.clear();
        patrol->waypointWaitTimes.clear();
        patrol->waypointSpeeds.clear();
        patrol->currentIndex = 0;
        patrol->started = false;
        patrol->waiting = false;
    }
}
void ScriptPatrolProxy::AddWaypoint(const math::Vector3& position, float waitTime, float speed) const
{
    if (auto* patrol = SelfComponent<NavMeshPatrolComponent>(script)) {
        patrol->waypoints.push_back(position);
        patrol->waypointWaitTimes.push_back(waitTime < 0.0f ? patrol->waitTime : waitTime);
        patrol->waypointSpeeds.push_back((std::max)(speed, 0.0f));
        patrol->started = false;
    }
}
bool ScriptPatrolProxy::SetWaypoint(size_t index, const math::Vector3& position) const
{
    auto* patrol = SelfComponent<NavMeshPatrolComponent>(script);
    if (!patrol || index >= patrol->waypoints.size()) return false;
    patrol->waypoints[index] = position;
    patrol->started = false;
    return true;
}
void ScriptPatrolProxy::Restart(size_t startIndex) const
{
    if (auto* patrol = SelfComponent<NavMeshPatrolComponent>(script)) {
        patrol->currentIndex = patrol->waypoints.empty() ? 0 : (std::min)(startIndex, patrol->waypoints.size() - 1);
        patrol->direction = 1;
        patrol->waiting = false;
        patrol->waitTimer = 0.0f;
        patrol->started = false;
    }
}
int ScriptPatrolProxy::GetCurrentIndex() const
{
    const auto* patrol = SelfComponent<NavMeshPatrolComponent>(script);
    return patrol ? static_cast<int>(patrol->currentIndex) : -1;
}

// ScriptWindProxy
void ScriptWindProxy::SetEnabled(bool enabled) const
{
    if (auto* wind = SelfComponent<WindZoneComponent>(script)) wind->enabled = enabled;
}
void ScriptWindProxy::SetDirection(const math::Vector3& direction) const
{
    if (auto* wind = SelfComponent<WindZoneComponent>(script)) wind->direction = direction;
}
void ScriptWindProxy::SetStrength(float strength) const
{
    if (auto* wind = SelfComponent<WindZoneComponent>(script)) wind->strength = strength;
}
void ScriptWindProxy::SetTurbulence(float turbulence) const
{
    if (auto* wind = SelfComponent<WindZoneComponent>(script)) wind->turbulence = (std::max)(turbulence, 0.0f);
}
void ScriptWindProxy::SetPulseFrequency(float frequency) const
{
    if (auto* wind = SelfComponent<WindZoneComponent>(script)) wind->pulseFrequency = (std::max)(frequency, 0.0f);
}

bool ScriptGameplayProxy::SetSprite(std::string_view assetPath) const
{
    auto* component = SelfComponent<SpriteRendererComponent>(script);
    if (!component)
        return false;
    component->spritePath = std::string(assetPath);
    component->runtimeSignature = 0;
    return true;
}

bool ScriptGameplayProxy::SetSpriteColor(float r, float g, float b, float a) const
{
    auto* component = SelfComponent<SpriteRendererComponent>(script);
    if (!component)
        return false;
    component->color = { r, g, b, a };
    return true;
}

bool ScriptGameplayProxy::HasLineRenderer() const
{
    return SelfComponent<LineRendererComponent>(script) != nullptr;
}

bool ScriptGameplayProxy::SetLineEnabled(bool enabled) const
{
    auto* component = SelfComponent<LineRendererComponent>(script);
    if (!component)
        return false;
    component->enabled = enabled;
    return true;
}

bool ScriptGameplayProxy::SetLine(const math::Vector3& start,
                                  const math::Vector3& end,
                                  bool worldSpace) const
{
    const math::Vector3 points[] = { start, end };
    return SetLinePoints(points, worldSpace);
}

bool ScriptGameplayProxy::SetLinePoints(std::span<const math::Vector3> points,
                                        bool worldSpace) const
{
    auto* component = SelfComponent<LineRendererComponent>(script);
    if (!component)
        return false;

    // WHY assign か: 毎フレーム呼ばれる想定なので、点数が変わらない限り
    //      vector の再確保が起きない代入で更新する。
    component->points.assign(points.begin(), points.end());
    component->space = worldSpace ? LineSpace::World : LineSpace::Local;
    // runtimeSignature は points から毎フレーム計算されるため、ここで触る必要はない
    // (SpriteRenderer と違い、内容が変わればメッシュは自動で作り直される)。
    return true;
}

bool ScriptGameplayProxy::SetLineColors(const math::Vector4& startColor,
                                        const math::Vector4& endColor) const
{
    auto* component = SelfComponent<LineRendererComponent>(script);
    if (!component)
        return false;
    component->startColor = startColor;
    component->endColor = endColor;
    return true;
}

bool ScriptGameplayProxy::SetLineWidth(float startWidth, float endWidth) const
{
    auto* component = SelfComponent<LineRendererComponent>(script);
    if (!component)
        return false;
    component->startWidth = startWidth;
    component->endWidth = endWidth;
    return true;
}

bool ScriptGameplayProxy::SetLineMaterial(std::string_view materialPath) const
{
    auto* component = SelfComponent<LineRendererComponent>(script);
    if (!component)
        return false;
    component->materialPath = std::string(materialPath);
    return true;
}

bool ScriptGameplayProxy::PlaySpline(bool restart) const
{
    auto* component = SelfComponent<SplineFollowerComponent>(script);
    if (!component)
        return false;
    if (restart)
        component->normalizedPosition = component->reverse ? 1.0f : 0.0f;
    component->playing = true;
    return true;
}

bool ScriptGameplayProxy::PauseSpline() const
{
    auto* component = SelfComponent<SplineFollowerComponent>(script);
    if (!component)
        return false;
    component->playing = false;
    return true;
}

bool ScriptGameplayProxy::SetSplinePosition(float normalizedPosition) const
{
    auto* component = SelfComponent<SplineFollowerComponent>(script);
    if (!component)
        return false;
    component->normalizedPosition = std::clamp(normalizedPosition, 0.0f, 1.0f);
    return true;
}

bool ScriptGameplayProxy::SetSliderValue(float value) const
{
    auto* component = SelfComponent<UISlider>(script);
    if (!component)
        return false;
    const float next = std::clamp(value, (std::min)(component->minimum, component->maximum),
                                  (std::max)(component->minimum, component->maximum));
    component->onValueChanged = next != component->value;
    component->value = next;
    return true;
}

float ScriptGameplayProxy::GetSliderValue() const
{
    const auto* component = SelfComponent<UISlider>(script);
    return component ? component->value : 0.0f;
}

bool ScriptGameplayProxy::SetToggle(bool value) const
{
    auto* component = SelfComponent<UIToggle>(script);
    if (!component)
        return false;
    component->onValueChanged = component->isOn != value;
    component->isOn = value;
    return true;
}

bool ScriptGameplayProxy::GetToggle() const
{
    const auto* component = SelfComponent<UIToggle>(script);
    return component && component->isOn;
}

bool ScriptGameplayProxy::SetInputText(std::string_view text) const
{
    auto* component = SelfComponent<UIInputField>(script);
    if (!component)
        return false;
    component->text = std::string(text);
    if (component->characterLimit > 0
        && component->text.size() > static_cast<size_t>(component->characterLimit))
        component->text.resize(static_cast<size_t>(component->characterLimit));
    component->caretPosition = component->text.size();
    component->onValueChanged = true;
    return true;
}

std::string_view ScriptGameplayProxy::GetInputText() const
{
    const auto* component = SelfComponent<UIInputField>(script);
    return component ? std::string_view(component->text) : std::string_view{};
}

bool ScriptGameplayProxy::StartCameraShake(float amplitude, float duration, int seed) const
{
    auto* component = SelfComponent<CameraShakeComponent>(script);
    if (!component)
        return false;
    component->amplitude = (std::max)(amplitude, 0.0f);
    component->duration = (std::max)(duration, 0.0f);
    component->seed = seed;
    component->elapsed = 0.0f;
    component->playing = true;
    return true;
}

bool ScriptGameplayProxy::SetVirtualCameraPriority(int priority) const
{
    auto* component = SelfComponent<VirtualCameraComponent>(script);
    if (!component)
        return false;
    component->priority = priority;
    return true;
}

bool ScriptGameplayProxy::SetAudioMixerSend(std::string_view busName, float level) const
{
    auto* component = SelfComponent<AudioMixerSendComponent>(script);
    if (!component || busName.empty())
        return false;
    component->busName = std::string(busName);
    component->sendLevel = std::clamp(level, 0.0f, 1.0f);
    return true;
}

// ── ScriptSaveProxy ─────────────────────────────────────────────────────────
// util::SaveData へそのまま転送する。スクリプトに Engine 実装を include させないための層。

void ScriptSaveProxy::SetSlot(std::string_view path) const
{
    util::SaveData::SetSlotPath(std::string(path));
}

std::string ScriptSaveProxy::GetSlot() const
{
    return util::SaveData::GetSlotPath();
}

void ScriptSaveProxy::SetBool(std::string_view key, bool value) const
{
    util::SaveData::SetBool(key, value);
}

void ScriptSaveProxy::SetInt(std::string_view key, int value) const
{
    util::SaveData::SetInt(key, value);
}

void ScriptSaveProxy::SetFloat(std::string_view key, float value) const
{
    util::SaveData::SetFloat(key, value);
}

void ScriptSaveProxy::SetString(std::string_view key, std::string_view value) const
{
    util::SaveData::SetString(key, value);
}

void ScriptSaveProxy::SetVector2(std::string_view key, const math::Vector2& value) const
{
    util::SaveData::SetVector2(key, value);
}

void ScriptSaveProxy::SetVector3(std::string_view key, const math::Vector3& value) const
{
    util::SaveData::SetVector3(key, value);
}

void ScriptSaveProxy::SetVector4(std::string_view key, const math::Vector4& value) const
{
    util::SaveData::SetVector4(key, value);
}

bool ScriptSaveProxy::GetBool(std::string_view key, bool defaultValue) const
{
    return util::SaveData::GetBool(key, defaultValue);
}

int ScriptSaveProxy::GetInt(std::string_view key, int defaultValue) const
{
    return util::SaveData::GetInt(key, defaultValue);
}

float ScriptSaveProxy::GetFloat(std::string_view key, float defaultValue) const
{
    return util::SaveData::GetFloat(key, defaultValue);
}

std::string ScriptSaveProxy::GetString(std::string_view key, std::string_view defaultValue) const
{
    return util::SaveData::GetString(key, defaultValue);
}

math::Vector2 ScriptSaveProxy::GetVector2(std::string_view key, const math::Vector2& defaultValue) const
{
    return util::SaveData::GetVector2(key, defaultValue);
}

math::Vector3 ScriptSaveProxy::GetVector3(std::string_view key, const math::Vector3& defaultValue) const
{
    return util::SaveData::GetVector3(key, defaultValue);
}

math::Vector4 ScriptSaveProxy::GetVector4(std::string_view key, const math::Vector4& defaultValue) const
{
    return util::SaveData::GetVector4(key, defaultValue);
}

bool ScriptSaveProxy::Has(std::string_view key) const
{
    return util::SaveData::Has(key);
}

void ScriptSaveProxy::Remove(std::string_view key) const
{
    util::SaveData::Remove(key);
}

void ScriptSaveProxy::Clear() const
{
    util::SaveData::Clear();
}

bool ScriptSaveProxy::Save() const
{
    return util::SaveData::Save();
}

bool ScriptSaveProxy::Load() const
{
    return util::SaveData::Load();
}

bool ScriptSaveProxy::IsDirty() const
{
    return util::SaveData::IsDirty();
}

// ── ScriptEventProxy ────────────────────────────────────────────────────────
// Subscribe / Publish はテンプレートなのでヘッダ側。ここは非テンプレート分だけ。

void ScriptEventProxy::UnsubscribeAll() const
{
    ScriptEventBus::UnsubscribeOwner(script);
}

// ── ScriptRandomProxy ───────────────────────────────────────────────────────

float ScriptRandomProxy::Value() const
{
    return util::Random::Value();
}

float ScriptRandomProxy::Range(float min, float max) const
{
    return util::Random::Range(min, max);
}

int ScriptRandomProxy::Range(int min, int max) const
{
    return util::Random::Range(min, max);
}

bool ScriptRandomProxy::Chance(float probability) const
{
    return util::Random::Value() < probability;
}

float ScriptRandomProxy::Sign() const
{
    return util::Random::Value() < 0.5f ? -1.0f : 1.0f;
}

math::Vector2 ScriptRandomProxy::InsideUnitCircle() const
{
    return util::Random::InsideUnitCircle();
}

math::Vector2 ScriptRandomProxy::OnUnitCircle() const
{
    return util::Random::OnUnitCircle();
}

math::Vector3 ScriptRandomProxy::InsideUnitSphere() const
{
    return util::Random::InsideUnitSphere();
}

math::Vector3 ScriptRandomProxy::OnUnitSphere() const
{
    return util::Random::OnUnitSphere();
}

math::Vector3 ScriptRandomProxy::ConeDirection(const math::Vector3& axis, float maxAngleDeg) const
{
    const math::Vector3 forward = axis.Normalized();
    if (maxAngleDeg <= 0.0f) return forward;

    // 円錐内の一様サンプリング。cos を一様に引くことで、頂点付近に偏らせない。
    const float maxCos = std::cos(std::clamp(maxAngleDeg, 0.0f, 180.0f) * DEG_TO_RAD);
    const float cosTheta = util::Random::Range(maxCos, 1.0f);
    const float sinTheta = std::sqrt((std::max)(0.0f, 1.0f - cosTheta * cosTheta));
    const float phi = util::Random::Range(0.0f, 6.28318530718f);

    // forward に直交する基底を作る。forward と平行になりにくい軸を選んで外積する。
    const math::Vector3 reference =
        std::abs(forward.y) < 0.99f ? math::Vector3{ 0.0f, 1.0f, 0.0f }
                                    : math::Vector3{ 1.0f, 0.0f, 0.0f };
    const math::Vector3 right = math::Vector3::Cross(forward, reference).Normalized();
    const math::Vector3 up    = math::Vector3::Cross(right, forward);

    return (right * (sinTheta * std::cos(phi))
          + up    * (sinTheta * std::sin(phi))
          + forward * cosTheta).Normalized();
}

void ScriptRandomProxy::SetSeed(uint32_t seed) const
{
    util::Random::SetSeed(static_cast<unsigned int>(seed));
}

// ── ScriptTweenProxy ────────────────────────────────────────────────────────

float ScriptTweenProxy::Evaluate(TweenEase ease, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    switch (ease) {
    case TweenEase::InQuad:     return util::Easing::EaseInQuad(t);
    case TweenEase::OutQuad:    return util::Easing::EaseOutQuad(t);
    case TweenEase::InOutQuad:  return util::Easing::EaseInOutQuad(t);
    case TweenEase::InCubic:    return util::Easing::EaseInCubic(t);
    case TweenEase::OutCubic:   return util::Easing::EaseOutCubic(t);
    case TweenEase::InOutCubic: return util::Easing::EaseInOutCubic(t);
    case TweenEase::InSine:     return util::Easing::EaseInSine(t);
    case TweenEase::OutSine:    return util::Easing::EaseOutSine(t);
    case TweenEase::InOutSine:  return util::Easing::EaseInOutSine(t);
    case TweenEase::InExpo:     return util::Easing::EaseInExpo(t);
    case TweenEase::OutExpo:    return util::Easing::EaseOutExpo(t);
    case TweenEase::InOutExpo:  return util::Easing::EaseInOutExpo(t);
    case TweenEase::InBack:     return util::Easing::EaseInBack(t);
    case TweenEase::OutBack:    return util::Easing::EaseOutBack(t);
    case TweenEase::InOutBack:  return util::Easing::EaseInOutBack(t);
    case TweenEase::OutElastic: return util::Easing::EaseOutElastic(t);
    case TweenEase::OutBounce:  return util::Easing::EaseOutBounce(t);
    case TweenEase::Linear:
    default:                    return t;
    }
}

namespace {

// Tween 1 ステップぶんの経過時間。Scaled / Unscaled の分岐をここへ集約する。
float TweenStepDelta(const Script* script, TweenClock clock)
{
    if (!script) return 0.0f;
    return clock == TweenClock::Unscaled ? script->time.UnscaledDeltaTime()
                                         : script->time.DeltaTime();
}

} // namespace

Coroutine ScriptTweenProxy::MoveTo(math::Vector3 target, float duration,
                                   TweenEase ease, TweenClock clock) const
{
    // WHY 引数を値で受けるか: コルーチンの引数は最初の中断で保存されるが、参照は
    //     呼び出し側の一時オブジェクトを指したまま残り得るため必ずコピーで持つ。
    Script* owner = script;
    if (!owner) co_return;

    if (duration <= 0.0f) {
        owner->transform.position = target;
        co_return;
    }

    const math::Vector3 start = owner->transform.position;
    float elapsed = 0.0f;
    while (elapsed < duration) {
        co_await WaitForFrames(1);
        elapsed += TweenStepDelta(owner, clock);
        const float t = Evaluate(ease, elapsed / duration);
        owner->transform.position = math::Vector3::Lerp(start, target, t);
    }
    // 端数で終値に届かないことがあるため、最後に必ず合わせる。
    owner->transform.position = target;
}

Coroutine ScriptTweenProxy::MoveBy(math::Vector3 delta, float duration,
                                   TweenEase ease, TweenClock clock) const
{
    Script* owner = script;
    if (!owner) co_return;

    // 開始位置は呼ばれた「今」を基準にする。連続で呼べば相対移動として積み上がる。
    // WHY MoveTo を co_await せず展開するか: Coroutine 自体は awaiter ではないため
    //     (待機命令は WaitForSeconds 等のみ)、入れ子にできない。処理を直接書く。
    const math::Vector3 start  = owner->transform.position;
    const math::Vector3 target = start + delta;

    if (duration <= 0.0f) {
        owner->transform.position = target;
        co_return;
    }

    float elapsed = 0.0f;
    while (elapsed < duration) {
        co_await WaitForFrames(1);
        elapsed += TweenStepDelta(owner, clock);
        const float t = Evaluate(ease, elapsed / duration);
        owner->transform.position = math::Vector3::Lerp(start, target, t);
    }
    owner->transform.position = target;
}

Coroutine ScriptTweenProxy::ScaleTo(math::Vector3 target, float duration,
                                    TweenEase ease, TweenClock clock) const
{
    Script* owner = script;
    if (!owner) co_return;

    if (duration <= 0.0f) {
        owner->transform.scale = target;
        co_return;
    }

    const math::Vector3 start = owner->transform.scale;
    float elapsed = 0.0f;
    while (elapsed < duration) {
        co_await WaitForFrames(1);
        elapsed += TweenStepDelta(owner, clock);
        const float t = Evaluate(ease, elapsed / duration);
        owner->transform.scale = math::Vector3::Lerp(start, target, t);
    }
    owner->transform.scale = target;
}

Coroutine ScriptTweenProxy::RotateTo(math::Quaternion target, float duration,
                                     TweenEase ease, TweenClock clock) const
{
    Script* owner = script;
    if (!owner) co_return;

    if (duration <= 0.0f) {
        owner->transform.rotation = target;
        co_return;
    }

    const math::Quaternion start = owner->transform.rotation;
    float elapsed = 0.0f;
    while (elapsed < duration) {
        co_await WaitForFrames(1);
        elapsed += TweenStepDelta(owner, clock);
        const float t = Evaluate(ease, elapsed / duration);
        // WHY Slerp か: 角速度が一定になるため、イージング曲線の形がそのまま
        //     見た目の速度変化になる。Lerp だと曲線に回転量の歪みが乗る。
        owner->transform.rotation = math::Quaternion::Slerp(start, target, t);
    }
    owner->transform.rotation = target;
}

Coroutine ScriptTweenProxy::Value(float from, float to, float duration,
                                  std::function<void(float)> apply,
                                  TweenEase ease, TweenClock clock) const
{
    Script* owner = script;
    if (!owner || !apply) co_return;

    if (duration <= 0.0f) {
        apply(to);
        co_return;
    }

    float elapsed = 0.0f;
    while (elapsed < duration) {
        co_await WaitForFrames(1);
        elapsed += TweenStepDelta(owner, clock);
        const float t = Evaluate(ease, elapsed / duration);
        apply(from + (to - from) * t);
    }
    apply(to);
}

Coroutine ScriptTweenProxy::ShakePosition(float amplitude, float duration,
                                          float frequency, TweenClock clock) const
{
    Script* owner = script;
    if (!owner || duration <= 0.0f || amplitude <= 0.0f) co_return;

    const math::Vector3 origin = owner->transform.position;
    // 揺れの向きは毎フレーム引き直すのではなく、位相を進めた正弦で決める。
    // WHY: 毎フレーム乱数だとフレームレートで揺れの速さが変わってしまう。
    //      周波数で定義すれば、何 fps でも同じ速さに見える。
    util::RandomStream rng{ static_cast<uint64_t>(
        static_cast<uint32_t>(amplitude * 1000.0f) + 1u) };
    const math::Vector3 axisA = rng.OnUnitSphere();
    const math::Vector3 axisB = rng.OnUnitSphere();

    float elapsed = 0.0f;
    while (elapsed < duration) {
        co_await WaitForFrames(1);
        elapsed += TweenStepDelta(owner, clock);

        const float normalized = std::clamp(elapsed / duration, 0.0f, 1.0f);
        const float decay = 1.0f - normalized;               // 線形に収束させる
        const float phase = elapsed * frequency;
        const math::Vector3 offset =
            axisA * (std::sin(phase) * amplitude * decay) +
            axisB * (std::cos(phase * 1.37f) * amplitude * decay);
        owner->transform.position = origin + offset;
    }
    owner->transform.position = origin;
}

} // namespace fbzz::scene
