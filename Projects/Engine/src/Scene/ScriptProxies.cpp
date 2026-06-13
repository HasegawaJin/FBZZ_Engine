// FBZZ Engine
// ScriptProxies.cpp | fbzz::scene
// Script Proxy 群の転送処理
// Script 本体を肥大化させず、Component / System ごとの便利 API をここで具体化する。
#include <Engine/Scene/Script.hpp>
#include <limits>

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Renderer/Gizmo.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneManager.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
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

physics::RigidBody* SelfRigidBody(const Script* script)
{
    auto* component = SelfComponent<RigidBodyComponent>(script);
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

Transform* ScriptTransformProxy::operator->() const
{
    auto* t = Get();
    assert(t && "Script context is not set");
    return t;
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

float ScriptInputProxy::GetAxis(std::string_view name) const
{
    // WHAT: Unity 互換の代表的な仮想軸だけを Script 層で合成する。
    // WHY: InputSystem のアクションマップをまだ持たないため、現状の KeyCode API から決定的に作れる範囲に絞る。
    float value = 0.0f;
    if (name == "Horizontal") {
        if (GetKey(input::KeyCode::A) || GetKey(input::KeyCode::LEFT)) value -= 1.0f;
        if (GetKey(input::KeyCode::D) || GetKey(input::KeyCode::RIGHT)) value += 1.0f;
    } else if (name == "Vertical") {
        if (GetKey(input::KeyCode::S) || GetKey(input::KeyCode::DOWN)) value -= 1.0f;
        if (GetKey(input::KeyCode::W) || GetKey(input::KeyCode::UP)) value += 1.0f;
    } else if (name == "Mouse X") {
        value = input::Input::MouseDelta().x;
    } else if (name == "Mouse Y") {
        value = input::Input::MouseDelta().y;
    }
    return value;
}

math::Vector2 ScriptInputProxy::GetMouseDelta() const { return input::Input::MouseDelta(); }
math::Vector2 ScriptInputProxy::GetMousePosition() const { return input::Input::MousePosition(); }
float ScriptInputProxy::GetMouseScrollDelta() const { return input::Input::MouseScrollDelta(); }
bool ScriptInputProxy::MouseButton(MouseBtn btn) const { return input::Input::MouseButton(static_cast<int>(btn)); }
bool ScriptInputProxy::MouseButtonDown(MouseBtn btn) const { return input::Input::MouseButtonDown(static_cast<int>(btn)); }
bool ScriptInputProxy::MouseButtonUp(MouseBtn btn) const { return input::Input::MouseButtonUp(static_cast<int>(btn)); }
bool ScriptInputProxy::MouseButton(int button) const { return input::Input::MouseButton(button); }
bool ScriptInputProxy::MouseButtonDown(int button) const { return input::Input::MouseButtonDown(button); }
bool ScriptInputProxy::MouseButtonUp(int button) const { return input::Input::MouseButtonUp(button); }

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

void ScriptPhysicsProxy::SetVelocity(const math::Vector3& v) const
{
    if (auto* rb = SelfRigidBody(script)) rb->SetVelocity(v);
}

math::Vector3 ScriptPhysicsProxy::GetVelocity() const
{
    if (auto* rb = SelfRigidBody(script)) return rb->GetVelocity();
    return math::Vector3::ZERO;
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
    const auto& renderer = core::Application::Get().GetRenderer();
    const float width = static_cast<float>((std::max)(renderer.GetWidth(), 1u));
    const float height = static_cast<float>((std::max)(renderer.GetHeight(), 1u));

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
    const auto& renderer = core::Application::Get().GetRenderer();
    const float width = static_cast<float>((std::max)(renderer.GetWidth(), 1u));
    const float height = static_cast<float>((std::max)(renderer.GetHeight(), 1u));

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
    const auto& renderer = core::Application::Get().GetRenderer();
    const float w = static_cast<float>((std::max)(renderer.GetWidth(),  1u));
    const float h = static_cast<float>((std::max)(renderer.GetHeight(), 1u));
    return screen.x >= 0.0f && screen.x <= w && screen.y >= 0.0f && screen.y <= h;
}

Ray ScriptCameraProxy::ScreenPointToRay(float screenX, float screenY) const
{
    const math::Vector3 nearPt = ScreenToWorldPoint({ screenX, screenY, 0.0f });
    const math::Vector3 farPt  = ScreenToWorldPoint({ screenX, screenY, 1.0f });
    const math::Vector3 dir    = (farPt - nearPt).Normalized();
    return { nearPt, dir };
}

MaterialComponent* ScriptMaterialProxy::Get() const
{
    return SelfComponent<MaterialComponent>(script);
}

MaterialComponent* ScriptMaterialProxy::Ensure() const
{
    if (!script || !script->m_gameObject) return nullptr;
    if (auto* material = script->m_gameObject->GetComponent<MaterialComponent>())
        return material;
    return &script->m_gameObject->AddComponent<MaterialComponent>();
}

bool ScriptMaterialProxy::SetMaterial(std::string_view materialPath) const
{
    auto* material = Ensure();
    if (!material) return false;

    material->materialPath = std::string(materialPath);
    material->materialAsset = material->materialPath.empty()
        ? renderer::ResourceHandle<renderer::MaterialAssetTag>{}
        : asset::AssetManager::LoadMaterial(material->materialPath);
    return material->materialAsset.IsValid() || material->materialPath.empty();
}

bool ScriptMaterialProxy::EnsureMaterial(std::string_view materialPath) const
{
    auto* material = Ensure();
    if (!material) return false;
    if (material->materialPath != materialPath)
        return SetMaterial(materialPath);
    return material->EnsureMaterialAsset();
}

bool ScriptMaterialProxy::HasParam(std::string_view param) const
{
    auto* material = Get();
    if (!material || !material->EnsureMaterialAsset()) return false;
    auto* a = asset::AssetManager::GetMaterial(material->materialAsset);
    if (!a) return false;
    return a->params.find(std::string(param)) != a->params.end();
}

void ScriptMaterialProxy::SetFloat(std::string_view param, float v) const
{
    auto* m = SelfComponent<MaterialComponent>(script);
    if (!m || !m->EnsureMaterialAsset()) return;
    auto* a = asset::AssetManager::GetMaterial(m->materialAsset);
    if (!a) return;
    a->params[std::string(param)] = { v };
}

void ScriptMaterialProxy::SetInt(std::string_view param, int v) const
{
    SetFloat(param, static_cast<float>(v));
}

void ScriptMaterialProxy::SetVector3(std::string_view param, const math::Vector3& v) const
{
    auto* m = SelfComponent<MaterialComponent>(script);
    if (!m || !m->EnsureMaterialAsset()) return;
    auto* a = asset::AssetManager::GetMaterial(m->materialAsset);
    if (!a) return;
    a->params[std::string(param)] = { v.x, v.y, v.z };
}

void ScriptMaterialProxy::SetVector4(std::string_view param, const math::Vector4& v) const
{
    auto* m = SelfComponent<MaterialComponent>(script);
    if (!m || !m->EnsureMaterialAsset()) return;
    auto* a = asset::AssetManager::GetMaterial(m->materialAsset);
    if (!a) return;
    a->params[std::string(param)] = { v.x, v.y, v.z, v.w };
}

void ScriptMaterialProxy::SetTexture(std::string_view slot, std::string_view texPath) const
{
    auto* m = SelfComponent<MaterialComponent>(script);
    if (!m || !m->EnsureMaterialAsset()) return;
    auto* a = asset::AssetManager::GetMaterial(m->materialAsset);
    if (!a) return;

    static constexpr const char* kSlots[] = { "albedo", "normal", "metallic", "emissive", "ao" };
    std::string key(slot);
    const uint32_t targetSlot = ParseTextureSlot(slot);
    if (targetSlot != UINT32_MAX && targetSlot < 5u)
        key = kSlots[targetSlot];
    a->textures[key] = std::string(texPath);
}

float ScriptMaterialProxy::GetFloat(std::string_view param) const
{
    const auto* m = Get();
    if (!m) return 0.0f;
    const auto* a = asset::AssetManager::GetMaterial(m->materialAsset);
    if (!a) return 0.0f;
    const auto it = a->params.find(std::string(param));
    return (it != a->params.end() && !it->second.empty()) ? it->second[0] : 0.0f;
}

math::Vector3 ScriptMaterialProxy::GetVector3(std::string_view param) const
{
    const auto* m = Get();
    if (!m) return {};
    const auto* a = asset::AssetManager::GetMaterial(m->materialAsset);
    if (!a) return {};
    const auto it = a->params.find(std::string(param));
    if (it == a->params.end() || it->second.size() < 3) return {};
    return { it->second[0], it->second[1], it->second[2] };
}

bool ScriptMaterialProxy::SetEnabled(bool enabled) const
{
    auto* material = Get();
    if (!material) return false;
    material->enabled = enabled;
    return true;
}

bool ScriptMaterialProxy::SetBlendMode(renderer::BlendMode blendMode) const
{
    auto* material = Get();
    if (!material || !material->EnsureMaterialAsset()) return false;
    auto* a = asset::AssetManager::GetMaterial(material->materialAsset);
    if (!a) return false;
    a->blendMode = blendMode;
    return true;
}

bool ScriptMaterialProxy::SetDoubleSided(bool doubleSided) const
{
    auto* material = Get();
    if (!material || !material->EnsureMaterialAsset()) return false;
    auto* a = asset::AssetManager::GetMaterial(material->materialAsset);
    if (!a) return false;
    a->doubleSided = doubleSided;
    return true;
}

bool ScriptMaterialProxy::SetRenderQueue(int32_t renderQueue) const
{
    auto* material = Get();
    if (!material || !material->EnsureMaterialAsset()) return false;
    auto* a = asset::AssetManager::GetMaterial(material->materialAsset);
    if (!a) return false;
    a->renderQueue = renderQueue;
    return true;
}

void ScriptMaterialProxy::QueueRenderPass(UserRenderPassDesc desc) const
{
    if (script) script->QueueRenderPass(std::move(desc));
}

void ScriptParticleProxy::SetEmitRate(float rate) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->emitRate = rate;
}

void ScriptParticleProxy::SetEnabled(bool enabled) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->enabled = enabled;
}

void ScriptParticleProxy::Play(bool restart) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->playing = true;
        if (restart) {
            p->playTime = 0.0f;
            p->delayTime = 0.0f;
            p->emitAccum = 0.0f;
            p->randomState = p->randomSeed;
        }
    }
}

void ScriptParticleProxy::Stop(bool clear) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->playing = false;
        p->emitAccum = 0.0f;
        p->burstPending = 0;
        if (clear)
            Clear();
    }
}

void ScriptParticleProxy::Burst(int count) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        if (count > 0)
            p->burstPending += count;
    }
}

void ScriptParticleProxy::Clear() const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->particles.clear();
        p->emitAccum = 0.0f;
        p->burstPending = 0;
        p->playTime = 0.0f;
        p->delayTime = 0.0f;
    }
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

void ScriptParticleProxy::SetTexture(std::string_view texturePath, int columns, int rows) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->texturePath = std::string(texturePath);
        p->spriteColumns = (std::max)(columns, 1);
        p->spriteRows = (std::max)(rows, 1);
        p->texture = {};
        p->loadedTexturePath.clear();
    }
}

void ScriptParticleProxy::SetShape(ParticleEmitterShape shape) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->shape = shape;
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
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->simulationMode = simulationMode;
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

void ScriptTrailProxy::SetTexture(std::string_view texturePath, float uvTiling, float uvScrollSpeed) const
{
    if (auto* t = SelfComponent<TrailComponent>(script)) {
        t->texturePath = std::string(texturePath);
        t->uvTiling = (std::max)(uvTiling, 0.001f);
        t->uvScrollSpeed = uvScrollSpeed;
        t->texture = {};
        t->loadedTexturePath.clear();
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

void ScriptMeshTrailProxy::SetTexture(std::string_view texturePath) const
{
    if (auto* t = SelfComponent<MeshTrailComponent>(script)) {
        t->texturePath = std::string(texturePath);
        t->texture = {};
        t->loadedTexturePath.clear();
    }
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

void ScriptUIProxy::SetButtonInteractable(bool v) const
{
    if (auto* b = SelfComponent<UIButton>(script)) b->isInteractable = v;
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

GameObject* ScriptSceneProxy::Find(std::string_view name) const
{
    return script ? script->Find(std::string(name)) : nullptr;
}

GameObject* ScriptSceneProxy::FindWithTag(std::string_view tag) const
{
    return script ? script->FindWithTag(std::string(tag)) : nullptr;
}

GameObject* ScriptSceneProxy::Self() const
{
    return script ? script->m_gameObject : nullptr;
}

GameObject* ScriptSceneProxy::GetGameObject(EntityID id) const
{
    return script ? script->GetGameObject(id) : nullptr;
}

GameObject* ScriptSceneProxy::GetMainCameraObject() const
{
    return script ? script->GetMainCameraObject() : nullptr;
}

GameObject& ScriptSceneProxy::Create(std::string_view name) const
{
    assert(script && "Script context is not set");
    return script->CreateGameObject(std::string(name));
}

void ScriptSceneProxy::Destroy(GameObject& go, float delay) const
{
    Script::Destroy(go, delay);
}

void ScriptSceneProxy::DestroySelf(float delay) const
{
    if (script && script->m_gameObject)
        Script::Destroy(*script->m_gameObject, delay);
}

bool ScriptSceneProxy::IsActiveAndEnabled() const
{
    return script && script->m_gameObject && script->m_gameObject->activeSelf() && script->enabled;
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
    if (!Script::InvokePrefabInstantiate(*scene, prefabPath, roots) || roots.empty())
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

void ScriptSceneProxy::LoadScene(std::string_view name) const
{
    core::Application::Get().GetSceneManager().LoadScene(std::string(name));
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

// ── EntityRef 実装 ──────────────────────────────────────────────────────────
GameObject* EntityRef::Resolve(const ScriptSceneProxy& scene) const
{
    return id.IsValid() ? scene.GetGameObject(id) : nullptr;
}

GameObject* EntityRef::Resolve(Scene& scene) const
{
    return id.IsValid() ? scene.GetGameObject(id) : nullptr;
}

} // namespace fbzz::scene
