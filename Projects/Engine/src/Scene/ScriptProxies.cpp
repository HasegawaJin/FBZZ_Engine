// FBZZ Engine
// ScriptProxies.cpp | fbzz::scene
// Script Proxy 群の転送処理
// Script 本体を肥大化させず、Component / System ごとの便利 API をここで具体化する。
#include <Engine/Scene/Script.hpp>

#include <Engine/Core/Application.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneManager.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
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

void ScriptTransformProxy::SetPosition(const math::Vector3& v) const
{
    if (auto* t = Get()) {
        t->position = v;
        t->localPosition = v;
    }
}

void ScriptTransformProxy::Translate(const math::Vector3& v) const
{
    if (auto* t = Get())
        t->Translate(v);
}

void ScriptTransformProxy::Rotate(const math::Vector3& axis, float degrees) const
{
    if (auto* t = Get()) {
        const auto delta = math::Quaternion::FromAxisAngle(axis.Normalized(), degrees * DEG_TO_RAD);
        t->rotation = (delta * t->rotation).Normalized();
        t->localRotation = (delta * t->localRotation).Normalized();
    }
}

void ScriptTransformProxy::LookAt(const math::Vector3& target) const
{
    if (auto* t = Get())
        t->LookAt(target);
}

float ScriptTransformProxy::DistanceTo(const GameObject& other) const
{
    const auto* t = Get();
    return t ? (other.transform.position - t->position).Length() : 0.0f;
}

math::Vector3 ScriptTransformProxy::DirectionTo(const GameObject& other) const
{
    const auto* t = Get();
    return t ? (other.transform.position - t->position).Normalized() : math::Vector3::ZERO;
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

void ScriptPhysicsProxy::SetVelocity(const math::Vector3& v) const
{
    if (auto* rb = SelfRigidBody(script)) rb->SetVelocity(v);
}

math::Vector3 ScriptPhysicsProxy::GetVelocity() const
{
    if (auto* rb = SelfRigidBody(script)) return rb->GetVelocity();
    return math::Vector3::ZERO;
}

void ScriptPhysicsProxy::AddTorque(const math::Vector3& v) const
{
    if (auto* rb = SelfRigidBody(script)) rb->ApplyTorque(v);
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
        auto* go = inst ? FindGameObjectByCollider(script->m_scene, inst->collider.get()) : nullptr;
        if (go && std::find(result.begin(), result.end(), go) == result.end())
            result.push_back(go);
    }
    return result;
}

void ScriptAudioProxy::Play(std::string_view clipPath) const
{
    if (auto* audio = SelfComponent<AudioSourceComponent>(script)) {
        audio->clipPath = std::string(clipPath);
        audio->enabled = true;
        audio->playOnAwake = true;
        audio->m_played = false;
    }
}

void ScriptAudioProxy::Stop() const
{
    if (auto* audio = SelfComponent<AudioSourceComponent>(script)) {
        audio->enabled = false;
        audio->m_played = false;
    }
}

void ScriptAudioProxy::Pause() const
{
    if (auto* audio = SelfComponent<AudioSourceComponent>(script))
        audio->enabled = false;
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

void ScriptLightProxy::SetColor(const math::Vector3& color) const
{
    if (auto* l = SelfComponent<LightComponent>(script)) l->color = color;
}

void ScriptLightProxy::SetIntensity(float intensity) const
{
    if (auto* l = SelfComponent<LightComponent>(script)) l->intensity = intensity;
}

void ScriptLightProxy::SetRange(float range) const
{
    if (auto* l = SelfComponent<LightComponent>(script)) l->range = range;
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

void ScriptCameraProxy::SetNearFar(float nearZ, float farZ) const
{
    if (auto* cam = SelfComponent<CameraComponent>(script)) {
        cam->nearZ = nearZ;
        cam->farZ = farZ;
    }
}

math::Vector3 ScriptCameraProxy::WorldToScreenPoint(const math::Vector3& worldPos) const
{
    return worldPos;
}

math::Vector3 ScriptCameraProxy::ScreenToWorldPoint(const math::Vector3& screenPos) const
{
    return screenPos;
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

bool ScriptMaterialProxy::SetShader(std::string_view shaderPath, bool resetParameters) const
{
    auto* material = Ensure();
    if (!material) return false;

    material->shaderPath = std::string(shaderPath);
    if (!resetParameters) return true;

    if (const auto* desc = script->GetShaderDescriptor(shaderPath)) {
        material->InitFromDescriptor(*desc);
        return true;
    }

    // WHY: shader がまだロードできない場合でも、Script から t0〜t4 を先に設定できる余地を残す。
    if (material->texturePaths.empty())
        material->texturePaths.resize(5);
    material->paramData.clear();
    return false;
}

bool ScriptMaterialProxy::EnsureCustomMaterial(std::string_view shaderPath, bool resetParameters) const
{
    auto* material = Ensure();
    if (!material) return false;

    const bool shaderChanged = material->shaderPath != shaderPath;
    const bool needsLayout = resetParameters && material->paramData.empty();
    if (shaderChanged || needsLayout)
        return SetShader(shaderPath, resetParameters);

    if (resetParameters) {
        if (const auto* desc = script->GetShaderDescriptor(shaderPath)) {
            if (material->paramData.size() != desc->cbufferSize)
                material->InitFromDescriptor(*desc);
        }
    }

    return true;
}

bool ScriptMaterialProxy::HasParam(std::string_view param) const
{
    const auto* material = Get();
    if (!material || !script) return false;

    const auto* desc = script->GetShaderDescriptor(material->shaderPath);
    return desc && desc->FindVar(param) != nullptr;
}

void ScriptMaterialProxy::SetFloat(std::string_view param, float v) const
{
    if (auto* m = SelfComponent<MaterialComponent>(script)) {
        if (const auto* desc = script->GetShaderDescriptor(m->shaderPath))
            m->SetParam(param, v, *desc);
    }
}

void ScriptMaterialProxy::SetInt(std::string_view param, int v) const
{
    if (auto* m = SelfComponent<MaterialComponent>(script)) {
        if (const auto* desc = script->GetShaderDescriptor(m->shaderPath))
            m->SetParam(param, v, *desc);
    }
}

void ScriptMaterialProxy::SetVector3(std::string_view param, const math::Vector3& v) const
{
    if (auto* m = SelfComponent<MaterialComponent>(script)) {
        if (const auto* desc = script->GetShaderDescriptor(m->shaderPath))
            m->SetParam(param, v, *desc);
    }
}

void ScriptMaterialProxy::SetVector4(std::string_view param, const math::Vector4& v) const
{
    if (auto* m = SelfComponent<MaterialComponent>(script)) {
        if (const auto* desc = script->GetShaderDescriptor(m->shaderPath))
            m->SetParam(param, v, *desc);
    }
}

void ScriptMaterialProxy::SetTexture(std::string_view slot, std::string_view texPath) const
{
    auto* m = SelfComponent<MaterialComponent>(script);
    if (!m) return;

    uint32_t targetSlot = UINT32_MAX;
    if (const auto* desc = script->GetShaderDescriptor(m->shaderPath)) {
        for (const auto& tex : desc->textures) {
            if (tex.name == slot) {
                targetSlot = tex.slot;
                break;
            }
        }
    }
    if (targetSlot == UINT32_MAX)
        targetSlot = ParseTextureSlot(slot);
    if (targetSlot == UINT32_MAX) return;

    if (m->texturePaths.size() <= targetSlot)
        m->texturePaths.resize(targetSlot + 1u);
    m->texturePaths[targetSlot] = std::string(texPath);
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
    if (!material) return false;
    material->blendMode = blendMode;
    return true;
}

bool ScriptMaterialProxy::SetDoubleSided(bool doubleSided) const
{
    auto* material = Get();
    if (!material) return false;
    material->doubleSided = doubleSided;
    return true;
}

bool ScriptMaterialProxy::SetRenderQueue(int32_t renderQueue) const
{
    auto* material = Get();
    if (!material) return false;
    material->renderQueue = renderQueue;
    return true;
}

void ScriptMaterialProxy::QueueRenderPass(UserRenderPassDesc desc) const
{
    if (script) script->QueueRenderPass(std::move(desc));
}

const renderer::ShaderDescriptor* ScriptMaterialProxy::GetShaderDescriptor(std::string_view path) const
{
    return script ? script->GetShaderDescriptor(path) : nullptr;
}

void ScriptParticleProxy::SetEmitRate(float rate) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->emitRate = rate;
}

void ScriptParticleProxy::SetEnabled(bool enabled) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->enabled = enabled;
}

void ScriptParticleProxy::Clear() const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->particles.clear();
        p->emitAccum = 0.0f;
    }
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

bool ScriptSceneProxy::IsActiveAndEnabled() const
{
    return script && script->m_gameObject && script->m_gameObject->activeSelf() && script->enabled;
}

void ScriptSceneProxy::LoadScene(std::string_view name) const
{
    core::Application::Get().GetSceneManager().LoadScene(std::string(name));
}

std::string ScriptSceneProxy::GetSceneName() const
{
    return {};
}

float ScriptSceneProxy::GetTerrainHeightAt(const math::Vector3& worldPos) const
{
    if (!script || !script->m_scene) return worldPos.y;
    for (auto& go : script->m_scene->GameObjects()) {
        if (auto* terrain = go.GetComponent<TerrainComponent>())
            return go.transform.position.y + terrain->GetHeightAt(worldPos.x - go.transform.position.x,
                                                                  worldPos.z - go.transform.position.z);
    }
    return worldPos.y;
}

math::Vector3 ScriptSceneProxy::GetTerrainNormalAt(const math::Vector3& worldPos) const
{
    if (!script || !script->m_scene) return math::Vector3::UP;
    for (auto& go : script->m_scene->GameObjects()) {
        if (auto* terrain = go.GetComponent<TerrainComponent>())
            return terrain->GetNormalAt(worldPos.x - go.transform.position.x,
                                        worldPos.z - go.transform.position.z);
    }
    return math::Vector3::UP;
}

float ScriptSceneProxy::GetWaterSurfaceHeight(const math::Vector3& worldPos, float time) const
{
    if (!script || !script->m_scene) return worldPos.y;
    for (auto& go : script->m_scene->GameObjects()) {
        if (auto* water = go.GetComponent<WaterComponent>()) {
            const float localX = worldPos.x - go.transform.position.x;
            const float localZ = worldPos.z - go.transform.position.z;
            return go.transform.position.y + water->GetSurfaceHeightAt(localX, localZ, time);
        }
    }
    return worldPos.y;
}

void ScriptAnimatorProxy::SetFloat(std::string_view name, float v) const
{
    if (script) script->SetAnimatorFloat(name, v);
}

void ScriptAnimatorProxy::SetInt(std::string_view name, int v) const
{
    if (script) script->SetAnimatorInt(name, v);
}

void ScriptAnimatorProxy::SetBool(std::string_view name, bool v) const
{
    if (script) script->SetAnimatorBool(name, v);
}

void ScriptAnimatorProxy::SetTrigger(std::string_view name) const
{
    if (script) script->SetAnimatorTrigger(name);
}

bool ScriptAnimatorProxy::IsInState(std::string_view name) const
{
    return script && script->IsAnimatorInState(name);
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

} // namespace fbzz::scene
