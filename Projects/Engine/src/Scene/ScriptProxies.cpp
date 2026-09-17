/// @file    ScriptProxies.cpp
/// @brief   Script Proxy 群の転送処理。
/// @author  Hasegawa Jin
/// @date    2026-06-01
///
/// @brief Script 本体を肥大化させず、Component / System ごとの便利 API をここで具体化する。
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <limits>

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/SynthAsset.hpp>
#include <Engine/Audio/AudioManager.hpp>
#include <Engine/Audio/Synth.hpp>
#include <Engine/Asset/PostProcessProfile.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Cursor.hpp>
#include <Engine/Scene/ScriptRuntime.hpp>
#include <Engine/Scene/UIPointer.hpp>
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
#include <Engine/Util/SaveStore.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/Components/NavMeshSensorComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/ParticleGpuSimulation.hpp>
#include <Engine/Scene/Components/VFXComponent.hpp>
#include <Engine/Scene/Environment/SceneEnvironment.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/Components/VolumetricCloudComponent.hpp>
#include <Engine/Scene/Components/SunMoonRenderer.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/UIAnimator.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Systems/JointSync.hpp>
#include <Engine/Scene/Systems/ColliderSync.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/Systems/RenderPasses/Geometry/WaterRenderPass.hpp>
#include <Engine/Scene/Components/AtmosphericScatteringComponent.hpp>
#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/EnvironmentLightComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/JointComponent.hpp>
#include <Engine/Scene/Components/LifetimeComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MotionWarpComponent.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Engine/Scene/Components/RagdollComponent.hpp>
#include <Engine/Scene/Components/ReflectionProbeComponent.hpp>
#include <Engine/Scene/Components/SequencePlayerComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/SpringBoneComponent.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <Engine/Scene/Components/VolumeComponent.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/CameraRigComponents.hpp>
#include <Engine/Scene/Components/UIControls.hpp>
#include <Engine/Scene/Components/UICanvasGroup.hpp>
#include <Engine/Scene/Systems/UISystem.hpp>
#include <Engine/Scene/Components/AudioSpatialComponents.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/World.hpp>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

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
    camera.m_backgroundColor = component->backgroundColor;
    camera.m_clearMode       = component->clearMode;
    return camera;
}

bool ResolveProjectionCamera(const Script* script, GameObject* object, renderer::Camera& out)
{
    if (!object && script) {
        object = script->scene.Self();
        if (!ObjectComponent<CameraComponent>(object)) object = script->scene.GetMainCameraObject();
    }
    const auto* component = ObjectComponent<CameraComponent>(object);
    if (!component || !component->enabled || !object->activeInHierarchy()) return false;
    if (!std::isfinite(component->fovY) || component->fovY <= 0.0f || component->fovY >= 180.0f ||
        !std::isfinite(component->aspectRatio) || component->aspectRatio <= 0.0f ||
        !std::isfinite(component->nearZ) || component->nearZ <= 0.0f ||
        !std::isfinite(component->farZ) || component->farZ <= component->nearZ) return false;
    out = BuildCameraFromComponent(object, component);
    return true;
}

bool ProjectToViewport(const renderer::Camera& camera, const math::Vector3& worldPos,
                       math::Vector3& outViewport)
{
    const math::Vector4 clip = camera.GetViewProjection() * math::Vector4(worldPos, 1.0f);
    if (!std::isfinite(clip.w) || clip.w <= 0.000001f) return false;
    const math::Vector3 result{0.5f + 0.5f * clip.x / clip.w,
                              0.5f - 0.5f * clip.y / clip.w, clip.z / clip.w};
    if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z)) return false;
    outViewport = result;
    return true;
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

    /// @note 既存領域を保持したまま容量だけ変えると旧ポインタの寿命が曖昧になるため、明示的に作り直す。
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

    /// @note 小規模 Script が OnAwake で明示初期化しなくても使えるよう、初回確保時に既定容量を用意する。
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

    /// @note PoolAllocator は固定 stride のため、用途変更時は古いブロックを破棄してから作り直す。
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

/// @name ローカル空間 property getter / setter
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

/// @name ワールド空間 property getter / setter
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

/// @name 算出値
math::Vector3 ScriptTransformProxy::_GetFwd()   const { auto* t = Get(); return t ? t->Forward() : math::Vector3::FORWARD; }
math::Vector3 ScriptTransformProxy::_GetUp()    const { auto* t = Get(); return t ? t->Up()      : math::Vector3::UP; }
math::Vector3 ScriptTransformProxy::_GetRight() const { auto* t = Get(); return t ? t->Right()   : math::Vector3::RIGHT; }

/// @name メソッド
void ScriptTransformProxy::Translate(const math::Vector3& v) const
{
    if (auto* t = Get()) t->Translate(v);
}

void ScriptTransformProxy::Rotate(const math::Vector3& axis, float deg) const
{
    if (auto* t = Get()) {
        /// @note 軸が潰れていたら回しようがない。スクリプトの引数ミスでエディターごと
        ///       落とさないよう、何もしないで返す。
        if (axis.LengthSq() < math::EPSILON * math::EPSILON) return;
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
    /// @note 相手と重なっている間は向きが無い。距離 0 は追跡や射撃で普通に通る状態なので、
    ///       「方向なし」を表す ZERO をそのまま返す (呼び出し側は既に 0 判定を持っている)。
    return t ? (other.transform.worldPosition - t->worldPosition).NormalizedOr(math::Vector3::ZERO)
             : math::Vector3::ZERO;
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

/// @name アクション層

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

/// @name ゲームパッド直接アクセス

namespace {
/// @brief pad = -1 を「接続中の最初のパッド」へ解決する。
/// @brief 1 台も接続されていない場合は 0 を返す (未接続スロットへの問い合わせは false / 0)。
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

/// @name キーコンフィグ

int ScriptInputProxy::GetActionBindingCount(std::string_view action) const
{
    const auto* found = input::InputActionMap::FindAction(action);
    return found ? static_cast<int>(found->bindings.size()) : 0;
}

ScriptInputBinding ScriptInputProxy::GetActionBinding(std::string_view action, int index) const
{
    const auto* found = input::InputActionMap::FindAction(action);
    if (!found || index < 0 || index >= static_cast<int>(found->bindings.size())) return {};

    const input::InputBinding& binding = found->bindings[static_cast<size_t>(index)];
    return { static_cast<int>(binding.source), binding.code };
}

bool ScriptInputProxy::SetActionBinding(std::string_view action, int index,
                                        ScriptInputBinding binding) const
{
    auto* found = input::InputActionMap::FindAction(action);
    if (!found || !binding.IsValid()) return false;

    /// @note scale / padIndex / buttonThreshold は既存のバインドのものを引き継ぐ。閾値 0.5 のボタン扱い等の
    ///       設定は割り当てを差し替えても効いてほしく、値まで初期化するとリバインドした軸だけ既定へ戻る。
    if (index >= 0 && index < static_cast<int>(found->bindings.size())) {
        input::InputBinding& slot = found->bindings[static_cast<size_t>(index)];
        slot.source = static_cast<input::BindingSource>(binding.source);
        slot.code   = binding.code;
        return true;
    }

    input::InputBinding added{};
    added.source = static_cast<input::BindingSource>(binding.source);
    added.code   = binding.code;
    found->bindings.push_back(added);
    return true;
}

std::string ScriptInputProxy::DescribeActionBinding(std::string_view action, int index) const
{
    const auto* found = input::InputActionMap::FindAction(action);
    if (!found || index < 0 || index >= static_cast<int>(found->bindings.size())) return {};
    return input::InputActionMap::DescribeBinding(found->bindings[static_cast<size_t>(index)]);
}

void ScriptInputProxy::BeginRebindAction(std::string_view action, int index) const
{
    input::InputActionMap::BeginRebindAction(action, index);
}

bool ScriptInputProxy::IsRebinding() const
{
    return input::InputActionMap::IsRebinding();
}

bool ScriptInputProxy::ConsumeRebindCompleted() const
{
    return input::InputActionMap::ConsumeRebindCompleted();
}

void ScriptInputProxy::CancelRebind() const
{
    input::InputActionMap::CancelRebind();
}

CursorRequest ScriptCursorProxy::Push(CursorLockMode mode, bool visible, int priority) const
{
    /// @note owner に index+1 を渡す。index 0 は正当な GameObject なので、
    ///       0 を «無所属» に使っている Cursor 側と衝突させない。
    std::uint32_t owner = 0;
    if (script) {
        if (GameObject* self = script->scene.Self(); self && self->IsValid())
            owner = self->GetID().index + 1;
    }
    const char* label = script ? script->GetTypeName() : nullptr;
    return CursorRequest(core::Cursor::Push({ mode, visible }, priority, owner, label));
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

void ScriptCursorProxy::SetShape(CursorShape shape) const
{
    core::Cursor::SetShape(shape);
}

CursorShape ScriptCursorProxy::GetShape() const
{
    return core::Cursor::GetShape();
}

bool ScriptCursorProxy::HasShapeImage(CursorShape shape) const
{
    return core::Cursor::HasShapeImage(shape);
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

bool ScriptApplicationProxy::IsPlaying() const
{
    return Script::IsInPlayMode();
}

bool ScriptApplicationProxy::IsEditMode() const
{
    return !Script::IsInPlayMode();
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

bool ScriptPhysicsProxy::IsStatic() const
{
    const auto* rb = SelfRigidBody(script);
    return rb && rb->IsStatic();
}

bool ScriptPhysicsProxy::IsSleeping() const
{
    const auto* rb = SelfRigidBody(script);
    return rb && rb->IsSleeping();
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

void ScriptPhysicsProxy::SetLocalTimeScale(float scale) const
{
    if (auto* rb = SelfRigidBody(script)) rb->m_timeScale = std::clamp(scale, 0.0f, 8.0f);
}

void ScriptPhysicsProxy::SetFlowCoupling(float coupling) const
{
    auto* component = SelfComponent<RigidBodyComponent>(script);
    if (!component) return;
    /// @note 正本はコンポーネント側 (PhysicsSystem が毎フレーム剛体へ押し込むため、剛体だけに書くと
    ///       次の固定ステップで巻き戻る)。剛体にも写さないと効果が 1 ステップ遅れて出る。
    component->flowCoupling = coupling;
    if (component->rigidBody) component->rigidBody->SetFlowCoupling(coupling);
}

float ScriptPhysicsProxy::GetFlowCoupling() const
{
    const auto* component = SelfComponent<RigidBodyComponent>(script);
    return component ? component->flowCoupling : 0.0f;
}

math::Vector3 ScriptPhysicsProxy::GetWorldGravity() const
{
    if (!Script::s_physicsWorld) return { 0.0f, -9.81f, 0.0f };
    return Script::s_physicsWorld->GetGravity();
}

bool ScriptPhysicsProxy::LayersCollide(int a, int b) const
{
    /// @note World が無い (編集中) ときは «ぶつかる» を返す。無いことを «切れている» と
    ///       読ませると、Play していない間だけ組み立てが安全側へ倒れて、
    ///       エディタで見ている当たりと Play 中の当たりが別物になる。
    if (!Script::s_physicsWorld) return true;
    return Script::s_physicsWorld->LayersCollide(a, b);
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

bool ScriptColliderProxy::HasCollider() const
{
    return SelfAnyCollider(script) != nullptr;
}

bool ScriptColliderProxy::IsEnabled() const
{
    const auto* c = SelfAnyCollider(script);
    return c && c->enabled;
}

bool ScriptColliderProxy::IsTrigger() const
{
    const auto* c = SelfAnyCollider(script);
    return c && c->isTrigger;
}

math::Vector3 ScriptColliderProxy::GetCenter() const
{
    const auto* c = SelfAnyCollider(script);
    return c ? c->center : math::Vector3::ZERO;
}

bool ScriptColliderProxy::TryGetPrimitiveWorldBounds(GameObject* object,
    math::Vector3& outMin, math::Vector3& outMax)
{
    physics::AABB bounds;
    if (!object || !TryGetPrimitiveColliderBounds(*object, bounds)) return false;
    outMin = bounds.min;
    outMax = bounds.max;
    return true;
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
    /// @note 共有アセット参照を外してから書く。残したままだと次のフレームで
    ///       ResolvePhysicsMaterial() に上書きされ、書いた値が消える。
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
    /// @note string_view は終端 NUL を保証しないため、C API へ渡す前に string 化する。
    const std::string name(presetName);
    const auto* preset = physics::PhysicsMaterial::FindPreset(name.c_str());
    if (!preset) {
        FBZZ_LOG_WARN("Unknown physics material preset: %s", name.c_str());
        return false;
    }
    c->SetMaterial(*preset);
    return true;
}

namespace {

audio::AudioManager* ActiveAudioManager()
{
    return core::Application::Get().GetAudioManager();
}

bool LoadSynthSpecFromPath(std::string_view path, audio::SynthSpec& out)
{
    if (path.empty()) return false;
    const std::string requested(path);
    const std::string resolved = asset::AssetManager::ResolveAssetPath(requested);
    asset::SynthAsset synth;
    if (!asset::LoadSynthAssetFromFile(resolved.empty() ? requested : resolved, synth))
        return false;
    out = synth.spec;
    return true;
}

/// @brief 手続きクリップの再生要求は「フィールドが参照を 1 つ握る」規約で動く。
/// @brief 同じフレームに 2 回積まれたら、置き換えられる側の参照をここで返す。
void HandOffPendingClip(audio::AudioManager& manager, uint32_t& slot, uint32_t clip)
{
    if (slot != 0) manager.ReleaseClip(slot);
    slot = clip;
}

/// @brief AudioSource が無い GameObject で音声 API を呼んだことを一度だけ報告する。
/// @note 音声は「鳴らない」以外の症状が出ずコンポーネントの付け忘れに気付きにくいため警告する。
///       OnUpdate から呼ばれても Console が埋まらないよう (オブジェクト名, API 名) で重複を 1 回に絞る。
void WarnMissingAudioSource(const Script* script, const char* api)
{
    static std::unordered_set<std::string> reported;
    /// @note m_gameObject は protected で、friend なのは Proxy 型だけ。free function から
    ///       直接は引けないため、公開されている scene プロキシ経由で取り出す。
    const GameObject* go = script ? script->scene.Self() : nullptr;
    std::string key = go ? go->name : std::string("<no object>");
    key += '/';
    key += api;
    if (!reported.insert(key).second) return;
    FBZZ_LOG_WARN("audio.%s: '%s' に AudioSourceComponent がありません — この呼び出しは無視されます",
                  api, go ? go->name.c_str() : "<no object>");
}

/// @brief 書き込み系の音声 API 用。見つからなければ警告してから nullptr を返す。
AudioSourceComponent* SelfAudioSource(const Script* script, const char* api)
{
    auto* source = SelfComponent<AudioSourceComponent>(script);
    if (!source) WarnMissingAudioSource(script, api);
    return source;
}

/// @brief one-shot 要求を積む。上限を超えたぶんは捨て、握っていた参照を返す。
void PushOneShot(AudioSourceComponent& source, AudioSourceComponent::OneShotRequest request)
{
    if (source.m_pendingOneShots.size() >= AudioSourceComponent::MAX_PENDING_ONE_SHOTS) {
        if (request.clipId != 0) {
            if (auto* manager = core::Application::Get().GetAudioManager())
                manager->ReleaseClip(request.clipId);
        }
        return;
    }
    source.m_pendingOneShots.push_back(std::move(request));
}

} // namespace

bool ScriptAudioProxy::Preload(std::string_view clipPath) const
{
    auto* manager = ActiveAudioManager();
    if (!manager || clipPath.empty()) {
        FBZZ_LOG_WARN("Audio Preload: %s", manager ? "empty clip path" : "audio manager is unavailable");
        return false;
    }
    return manager->AcquireClip(std::string(clipPath)) != 0;
}

bool ScriptAudioProxy::Preload(const AudioClipRef& clip) const
{
    return Preload(clip.ResolvePath());
}

void ScriptAudioProxy::Play() const
{
    auto* audio = SelfAudioSource(script, "Play");
    if (!audio) return;
    if (audio->clipPath.empty()) {
        FBZZ_LOG_WARN("audio.Play(): Clip Path が未設定です (Inspector で指定するか "
                      "Play(path) を使ってください)");
        return;
    }
    audio->enabled        = true;
    audio->m_pendingPlay  = true;
    audio->m_pendingStop  = false;
    audio->m_pendingPause = false;
}

void ScriptAudioProxy::Play(std::string_view clipPath) const
{
    auto* audio = SelfAudioSource(script, "Play");
    if (!audio) return;
    audio->clipPath       = std::string(clipPath);
    audio->enabled        = true;
    audio->m_pendingPlay  = true;
    audio->m_pendingStop  = false;
    audio->m_pendingPause = false;
}

void ScriptAudioProxy::Play(const AudioClipRef& clip) const
{
    Play(clip.ResolvePath());
}

void ScriptAudioProxy::Stop() const
{
    auto* audio = SelfAudioSource(script, "Stop");
    if (!audio) return;
    audio->m_pendingStop   = true;
    audio->m_pendingPlay   = false;
    audio->m_pendingPause  = false;
    audio->m_pendingResume = false;
}

void ScriptAudioProxy::Pause() const
{
    auto* audio = SelfAudioSource(script, "Pause");
    if (!audio) return;
    audio->m_pendingPause  = true;
    audio->m_pendingPlay   = false;
    audio->m_pendingResume = false;
}

void ScriptAudioProxy::Resume() const
{
    auto* audio = SelfAudioSource(script, "Resume");
    if (!audio) return;
    audio->m_pendingResume = true;
    audio->m_pendingPause  = false;
}

void ScriptAudioProxy::SetVolume(float v) const
{
    if (auto* audio = SelfAudioSource(script, "SetVolume"))
        audio->volume = std::clamp(v, 0.0f, 1.0f);
}

void ScriptAudioProxy::SetLoop(bool loop) const
{
    if (auto* audio = SelfAudioSource(script, "SetLoop"))
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
    if (auto* audio = SelfAudioSource(script, "SetPitch"))
        audio->pitch = std::clamp(pitch, 0.01f, 4.0f);
}

float ScriptAudioProxy::GetPitch() const
{
    const auto* audio = SelfComponent<AudioSourceComponent>(script);
    return audio ? audio->pitch : 1.0f;
}

bool ScriptAudioProxy::GetLoop() const
{
    const auto* audio = SelfComponent<AudioSourceComponent>(script);
    return audio && audio->loop;
}

bool ScriptAudioProxy::IsPaused() const
{
    const auto* audio = SelfComponent<AudioSourceComponent>(script);
    return audio && audio->m_isPaused;
}

void ScriptAudioProxy::SetSpatialBlend(float blend) const
{
    if (auto* audio = SelfAudioSource(script, "SetSpatialBlend"))
        audio->spatialBlend = std::clamp(blend, 0.0f, 1.0f);
}

float ScriptAudioProxy::GetSpatialBlend() const
{
    const auto* audio = SelfComponent<AudioSourceComponent>(script);
    return audio ? audio->spatialBlend : 0.0f;
}

void ScriptAudioProxy::Set3DDistances(float minDistance, float maxDistance, float rolloff) const
{
    if (auto* audio = SelfAudioSource(script, "Set3DDistances")) {
        audio->minDistance = (std::max)(minDistance, 0.0f);
        audio->maxDistance = (std::max)(maxDistance, audio->minDistance + 0.001f);
        audio->rolloffFactor = (std::max)(rolloff, 0.01f);
    }
}

void ScriptAudioProxy::PlayOneShot(std::string_view clipPath, float volumeScale) const
{
    auto* audio = SelfAudioSource(script, "PlayOneShot");
    if (!audio || clipPath.empty()) return;
    AudioSourceComponent::OneShotRequest request;
    request.path        = std::string(clipPath);
    request.volumeScale = (std::max)(volumeScale, 0.0f);
    PushOneShot(*audio, std::move(request));
}

void ScriptAudioProxy::PlayOneShot(const AudioClipRef& clip, float volumeScale) const
{
    PlayOneShot(clip.ResolvePath(), volumeScale);
}

audio::SynthSpec ScriptAudioProxy::MakeSpec(audio::SynthPreset preset, uint32_t seed) const
{
    return audio::MakePreset(preset, seed);
}

audio::SynthSpec ScriptAudioProxy::MutateSpec(const audio::SynthSpec& base,
                                              float amount, uint32_t seed) const
{
    return audio::Mutate(base, amount, seed);
}

bool ScriptAudioProxy::LoadSpec(const AudioClipRef& synthAsset, audio::SynthSpec& out) const
{
    return LoadSynthSpecFromPath(synthAsset.ResolvePath(), out);
}

bool ScriptAudioProxy::LoadSpec(std::string_view synthAssetPath, audio::SynthSpec& out) const
{
    return LoadSynthSpecFromPath(synthAssetPath, out);
}

SynthClip ScriptAudioProxy::Synthesize(const audio::SynthSpec& spec) const
{
    auto* manager = ActiveAudioManager();
    return manager ? SynthClip{ manager->AcquireGeneratedClip(spec) } : SynthClip{};
}

SynthClip ScriptAudioProxy::Synthesize(audio::SynthPreset preset, uint32_t seed) const
{
    return Synthesize(audio::MakePreset(preset, seed));
}

void ScriptAudioProxy::ReleaseClip(SynthClip clip) const
{
    if (auto* manager = ActiveAudioManager()) manager->ReleaseClip(clip.id);
}

void ScriptAudioProxy::PlayClip(SynthClip clip) const
{
    auto* manager = ActiveAudioManager();
    auto* audio   = SelfAudioSource(script, "PlayClip");
    if (!manager || !audio || !clip.IsValid()) return;
    /// @note 要求が参照を 1 つ握る。AudioSystem が再生後に手放す。
    manager->AddClipRef(clip.id);
    AudioSourceComponent::OneShotRequest request;
    request.clipId = clip.id;
    PushOneShot(*audio, std::move(request));
}

void ScriptAudioProxy::PlayClipAsSource(SynthClip clip) const
{
    auto* manager = ActiveAudioManager();
    auto* audio   = SelfAudioSource(script, "PlayClipAsSource");
    if (!manager || !audio || !clip.IsValid()) return;
    manager->AddClipRef(clip.id);
    HandOffPendingClip(*manager, audio->m_pendingClipId, clip.id);
    audio->enabled        = true;
    audio->m_pendingStop  = false;
    audio->m_pendingPause = false;
}

void ScriptAudioProxy::PlaySynth(const audio::SynthSpec& spec) const
{
    auto* manager = ActiveAudioManager();
    auto* audio   = SelfAudioSource(script, "PlaySynth");
    if (!manager || !audio) return;
    /// @note AcquireGeneratedClip が返す参照をそのまま要求へ譲る。
    const uint32_t clip = manager->AcquireGeneratedClip(spec);
    if (clip == 0) return;
    AudioSourceComponent::OneShotRequest request;
    request.clipId = clip;
    PushOneShot(*audio, std::move(request));
}

void ScriptAudioProxy::PlaySynth(audio::SynthPreset preset, uint32_t seed) const
{
    PlaySynth(audio::MakePreset(preset, seed));
}

void ScriptAudioProxy::PlaySynth2D(const audio::SynthSpec& spec, std::string_view bus) const
{
    auto* manager = ActiveAudioManager();
    if (!manager) return;
    const uint32_t clip = manager->AcquireGeneratedClip(spec);
    if (clip == 0) return;
    (void)manager->PlayClipVoice(clip, false, manager->FindBus(bus));
    /// @note 再生中は voice 側が実体を押さえる。ここでの参照はもう要らない。
    manager->ReleaseClip(clip);
}

void ScriptAudioProxy::PlayClip2D(SynthClip clip, std::string_view bus) const
{
    auto* manager = ActiveAudioManager();
    if (!manager || !clip.IsValid()) return;
    (void)manager->PlayClipVoice(clip.id, false, manager->FindBus(bus));
}

void ScriptAudioProxy::PlayAtPoint(std::string_view clipPath, const math::Vector3& position,
                                   float volume) const
{
    auto* manager = ActiveAudioManager();
    if (!manager || clipPath.empty()) return;

    audio::AudioManager::PositionalRequest request;
    request.path   = std::string(clipPath);
    request.volume = (std::max)(volume, 0.0f);
    request.x = position.x; request.y = position.y; request.z = position.z;
    /// @note 自 GameObject に AudioSource があれば、その 3D 設定を引き継ぐ (無ければ既定値)。
    ///       «同じ銃の弾着音» を発生源の減衰カーブと揃えたい場合が普通なので設定を流用する。
    if (const auto* source = SelfComponent<AudioSourceComponent>(script)) {
        request.minDistance   = source->minDistance;
        request.maxDistance   = source->maxDistance;
        request.rolloff       = source->rolloffFactor;
        request.airAbsorption = source->airAbsorption;
        request.priority      = source->priority;
        request.bus           = manager->FindBus(source->busName);
    } else {
        request.bus = manager->FindBus("SE");
    }
    manager->QueuePositional(std::move(request));
}

void ScriptAudioProxy::PlayAtPoint(const AudioClipRef& clip, const math::Vector3& position,
                                   float volume) const
{
    PlayAtPoint(clip.ResolvePath(), position, volume);
}

void ScriptAudioProxy::PlayClipAtPoint(SynthClip clip, const math::Vector3& position,
                                       float volume) const
{
    auto* manager = ActiveAudioManager();
    if (!manager || !clip.IsValid()) return;

    manager->AddClipRef(clip.id);
    audio::AudioManager::PositionalRequest request;
    request.clip   = clip.id;
    request.volume = (std::max)(volume, 0.0f);
    request.x = position.x; request.y = position.y; request.z = position.z;
    if (const auto* source = SelfComponent<AudioSourceComponent>(script)) {
        request.minDistance   = source->minDistance;
        request.maxDistance   = source->maxDistance;
        request.rolloff       = source->rolloffFactor;
        request.airAbsorption = source->airAbsorption;
        request.priority      = source->priority;
        request.bus           = manager->FindBus(source->busName);
    } else {
        request.bus = manager->FindBus("SE");
    }
    manager->QueuePositional(std::move(request));
}

void ScriptAudioProxy::PlayBGM(std::string_view clipPath, bool loop, float fadeSeconds) const
{
    if (auto* manager = ActiveAudioManager())
        manager->PlayBGM(std::string(clipPath), loop, (std::max)(fadeSeconds, 0.0f));
}

void ScriptAudioProxy::PlayBGM(const AudioClipRef& clip, bool loop, float fadeSeconds) const
{
    PlayBGM(clip.ResolvePath(), loop, fadeSeconds);
}

void ScriptAudioProxy::StopBGM(float fadeSeconds) const
{
    if (auto* manager = ActiveAudioManager()) manager->StopBGM((std::max)(fadeSeconds, 0.0f));
}

void ScriptAudioProxy::FadeTo(float gain, float seconds) const
{
    auto* manager = ActiveAudioManager();
    auto* audio   = SelfAudioSource(script, "FadeTo");
    if (!manager || !audio || audio->m_voiceId == 0) return;
    manager->FadeVoice(audio->m_voiceId, gain, (std::max)(seconds, 0.0f));
}

void ScriptAudioProxy::FadeOutAndStop(float seconds) const
{
    auto* manager = ActiveAudioManager();
    auto* audio   = SelfAudioSource(script, "FadeOutAndStop");
    if (!manager || !audio || audio->m_voiceId == 0) return;
    manager->FadeOutAndStop(audio->m_voiceId, (std::max)(seconds, 0.0f));
    /// @note ハンドルはこの時点で AudioManager 側の持ち物になる。
    audio->m_voiceId   = 0;
    audio->m_isPlaying = false;
    audio->m_isPaused  = false;
}

bool ScriptAudioProxy::IsBGMPlaying() const
{
    const auto* manager = ActiveAudioManager();
    return manager && manager->IsBGMPlaying();
}

void ScriptAudioProxy::SetBus(std::string_view bus) const
{
    if (auto* audio = SelfAudioSource(script, "SetBus"))
        audio->busName = std::string(bus);
}

std::string_view ScriptAudioProxy::GetBus() const
{
    /// @note 参照先は AudioSourceComponent が所有する文字列。呼び出し側が SetBus を
    ///       挟まない限り有効で、コピーを返さずに済む。
    const auto* audio = SelfComponent<AudioSourceComponent>(script);
    return audio ? std::string_view(audio->busName) : std::string_view{};
}

void ScriptAudioProxy::SetBusVolume(std::string_view bus, float volume) const
{
    if (auto* manager = ActiveAudioManager()) manager->SetBusVolume(bus, volume);
}

float ScriptAudioProxy::GetBusVolume(std::string_view bus) const
{
    auto* manager = ActiveAudioManager();
    return manager ? manager->GetBusVolume(bus) : 0.0f;
}

void ScriptAudioProxy::SetBusLowPass(std::string_view bus, float normalizedCutoff) const
{
    if (auto* manager = ActiveAudioManager()) manager->SetBusLowPass(bus, normalizedCutoff);
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

math::Vector3 ScriptLightProxy::GetColor() const
{
    const auto* l = SelfComponent<LightComponent>(script);
    return l ? l->color : math::Vector3::ONE;
}

LightType ScriptLightProxy::GetType() const
{
    const auto* l = SelfComponent<LightComponent>(script);
    return l ? static_cast<LightType>(static_cast<int>(l->type)) : LightType::Directional;
}

float ScriptLightProxy::GetIntensity() const
{
    const auto* l = SelfComponent<LightComponent>(script);
    return l ? l->intensity : 0.0f;
}

float ScriptLightProxy::GetRange() const
{
    const auto* l = SelfComponent<LightComponent>(script);
    return l ? l->range : 0.0f;
}

float ScriptLightProxy::GetInnerCone() const
{
    const auto* l = SelfComponent<LightComponent>(script);
    return l ? l->innerCone : 0.0f;
}

float ScriptLightProxy::GetOuterCone() const
{
    const auto* l = SelfComponent<LightComponent>(script);
    return l ? l->outerCone : 0.0f;
}

bool ScriptLightProxy::IsEnabled() const
{
    const auto* l = SelfComponent<LightComponent>(script);
    return l && l->enabled;
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

void ScriptCameraProxy::SetBackgroundColor(const math::Vector4& color) const
{
    if (auto* cam = SelfComponent<CameraComponent>(script)) cam->backgroundColor = color;
}

void ScriptCameraProxy::SetClearMode(renderer::CameraClearMode mode) const
{
    if (auto* cam = SelfComponent<CameraComponent>(script)) cam->clearMode = mode;
}

math::Vector3 ScriptCameraProxy::WorldToScreenPoint(const math::Vector3& worldPos) const
{
    math::Vector3 viewport;
    if (!TryWorldToViewportPoint(worldPos, viewport)) return math::Vector3::ZERO;
    const auto rt = ScriptRuntime::GetCurrent();
    const float width  = static_cast<float>((std::max)(rt.viewportWidth,  1u));
    const float height = static_cast<float>((std::max)(rt.viewportHeight, 1u));
    return {viewport.x * width, viewport.y * height, viewport.z};
}

bool ScriptCameraProxy::TryWorldToViewportPoint(const math::Vector3& worldPos,
    math::Vector3& outViewport, GameObject* cameraObject) const
{
    renderer::Camera camera;
    return ResolveProjectionCamera(script, cameraObject, camera) &&
           ProjectToViewport(camera, worldPos, outViewport);
}

bool ScriptCameraProxy::TryViewportToWorldPoint(const math::Vector2& uv, float depth,
    math::Vector3& outWorld, GameObject* cameraObject) const
{
    if (!std::isfinite(depth) || depth <= 0.0f || !std::isfinite(uv.x) || !std::isfinite(uv.y))
        return false;
    renderer::Camera camera;
    if (!ResolveProjectionCamera(script, cameraObject, camera)) return false;
    const float halfHeight = std::tan(camera.m_fovY * DEG_TO_RAD * 0.5f) * depth;
    const math::Vector3 result = camera.m_position + camera.GetForward() * depth
        + camera.GetRight() * ((uv.x * 2.0f - 1.0f) * halfHeight * camera.m_aspect)
        + camera.GetUp() * ((1.0f - uv.y * 2.0f) * halfHeight);
    if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z)) return false;
    outWorld = result;
    return true;
}

math::Vector3 ScriptCameraProxy::ScreenToWorldPoint(const math::Vector3& screenPos) const
{
    renderer::Camera camera;
    if (!ResolveProjectionCamera(script, nullptr, camera)) return math::Vector3::ZERO;
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

float ScriptCameraProxy::GetAspectRatio() const
{
    const auto* cam = SelfComponent<CameraComponent>(script);
    return cam ? cam->aspectRatio : 16.0f / 9.0f;
}

fbzz::LayerMask ScriptCameraProxy::GetCullingMask() const
{
    const auto* cam = SelfComponent<CameraComponent>(script);
    return cam ? cam->cullingMask : fbzz::Layer::Everything;
}

math::Vector4 ScriptCameraProxy::GetBackgroundColor() const
{
    const auto* cam = SelfComponent<CameraComponent>(script);
    return cam ? cam->backgroundColor : renderer::kDefaultBackgroundColor;
}

renderer::CameraClearMode ScriptCameraProxy::GetClearMode() const
{
    const auto* cam = SelfComponent<CameraComponent>(script);
    return cam ? cam->clearMode : renderer::CameraClearMode::SolidColor;
}

bool ScriptCameraProxy::IsMain() const
{
    const auto* cam = SelfComponent<CameraComponent>(script);
    return cam && cam->isMain;
}

bool ScriptCameraProxy::IsVisible(const math::Vector3& worldPos) const
{
    math::Vector3 viewport;
    return TryWorldToViewportPoint(worldPos, viewport) && viewport.z >= 0.0f && viewport.z <= 1.0f &&
           viewport.x >= 0.0f && viewport.x <= 1.0f && viewport.y >= 0.0f && viewport.y <= 1.0f;
}

Ray ScriptCameraProxy::ScreenPointToRay(float screenX, float screenY) const
{
    const math::Vector3 nearPt = ScreenToWorldPoint({ screenX, screenY, 0.0f });
    const math::Vector3 farPt  = ScreenToWorldPoint({ screenX, screenY, 1.0f });
    const math::Vector3 dir    = (farPt - nearPt).NormalizedOr(math::Vector3::ZERO);
    return { nearPt, dir };
}

/// @brief スロットを所有する MaterialComponent を解決する。
/// @brief SetEnabled のようなコンポーネント全体の操作はこちらを使う。
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

/// @brief m_slot が指す MaterialSlot を解決する。ensure=true なら必要な数までスロットを伸ばす。
/// @note SkinnedMeshRenderer は 1 GameObject でモデル全体を描くため、submesh ごとのマテリアルは
///       同じ GameObject 上のスロットとして並ぶ (例: «バイザーだけ光らせる» をスロット番号で行える)。
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
    const auto* asset = asset::AssetManager::Get<asset::MaterialAsset>(material->materialAsset);
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
        const auto* shared = asset::AssetManager::Get<asset::MaterialAsset>(material->materialAsset);
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
    const auto* shared = asset::AssetManager::Get<asset::MaterialAsset>(material->materialAsset);
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
        const auto* shared = asset::AssetManager::Get<asset::MaterialAsset>(material->materialAsset);
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
    const auto* shared = asset::AssetManager::Get<asset::MaterialAsset>(material->materialAsset);
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
        ? asset::AssetHandle<asset::MaterialAsset>{}
        : asset::AssetManager::Load<asset::MaterialAsset>(path);
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

/// @name 共有 .mat の読み取り

namespace {

/// @brief MaterialRef から共有アセットを解決する。未ロードならここでロードする。
const asset::MaterialAsset* ResolveSharedMaterial(const MaterialRef& material)
{
    const std::string path = material.ResolvePath();
    if (path.empty()) return nullptr;
    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(path);
    if (!handle.IsValid()) return nullptr;
    return asset::AssetManager::Get<asset::MaterialAsset>(handle);
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

    /// @note .mat は float / float2 / float3 / float4 を同じ形式で持つため、
    ///       足りない成分は 0 で埋める (Instance() の読み出しと同じ規則)。
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

int ScriptMaterialProxy::GetInt(std::string_view param) const
{
    int value = 0;
    (void)Instance().TryGetInt(MaterialPropertyId(param), value);
    return value;
}

math::Vector3 ScriptMaterialProxy::GetVector3(std::string_view param) const
{
    math::Vector3 value;
    (void)Instance().TryGetVector3(MaterialPropertyId(param), value);
    return value;
}

math::Vector4 ScriptMaterialProxy::GetVector4(std::string_view param) const
{
    math::Vector4 value;
    (void)Instance().TryGetVector4(MaterialPropertyId(param), value);
    return value;
}

bool ScriptMaterialProxy::SetEnabled(bool enabled) const
{
    /// @note enabled はコンポーネント全体の有効/無効。submesh 単位の表示切替は
    ///       SetSlotVisible() を使う。
    MaterialComponent* material = Instance().ResolveOwner(false);
    if (!material) return false;
    material->enabled = enabled;
    return true;
}

bool ScriptMaterialProxy::IsEnabled() const
{
    const MaterialComponent* material = Instance().ResolveOwner(false);
    return material && material->enabled;
}

bool ScriptMaterialProxy::SetSlotVisible(uint32_t slot, bool visible) const
{
    auto* material = static_cast<MaterialSlot*>(Instance(slot).ResolveComponent(true));
    if (!material) return false;
    material->visible = visible;
    return true;
}

bool ScriptMaterialProxy::IsSlotVisible(uint32_t slot) const
{
    /// @note ensure = false。存在しないスロットを問い合わせただけで作らない。
    const auto* material = static_cast<const MaterialSlot*>(Instance(slot).ResolveComponent(false));
    return material && material->visible;
}

std::string ScriptMaterialProxy::GetSharedMaterialPath(uint32_t slot) const
{
    const auto* material = static_cast<const MaterialSlot*>(Instance(slot).ResolveComponent(false));
    return material ? material->materialPath : std::string{};
}

std::string ScriptMaterialProxy::GetSharedMaterialPath(EntityRef target, uint32_t slot) const
{
    const auto* material =
        static_cast<const MaterialSlot*>(Instance(target, slot).ResolveComponent(false));
    return material ? material->materialPath : std::string{};
}

uint32_t ScriptMaterialProxy::GetSlotCount() const
{
    const MaterialComponent* material = Instance().ResolveOwner(false);
    /// @note スロット 0 は MaterialComponent 自身が兼ねるので extraSlots + 1。
    return material ? static_cast<uint32_t>(material->extraSlots.size() + 1) : 0u;
}

bool ScriptMaterialProxy::IsDoubleSided() const
{
    const auto* material = static_cast<const MaterialSlot*>(Instance().ResolveComponent(false));
    return material && material->IsDoubleSided();
}

int32_t ScriptMaterialProxy::GetRenderQueue() const
{
    const auto* material = static_cast<const MaterialSlot*>(Instance().ResolveComponent(false));
    return material ? material->GetRenderQueue() : renderer::RenderQueue::GEOMETRY;
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
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.emitRate = rate;
}

void ScriptParticleProxy::SetEmitPosition(const math::Vector3& position) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.emitPosition = position;
}

void ScriptParticleProxy::SetEmitVelocity(const math::Vector3& velocity) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.emitVelocity = velocity;
}

void ScriptParticleProxy::SetVelocitySpread(float spread) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.velocitySpread = (std::max)(spread, 0.0f);
}

void ScriptParticleProxy::SetEnabled(bool enabled) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.enabled = enabled;
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
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.SetGravityAcceleration(gravity);
}

void ScriptParticleProxy::SetColor(const math::Vector4& start, const math::Vector4& end) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.colorStart = start;
        p->settings.colorEnd = end;
    }
}

void ScriptParticleProxy::SetSize(float start, float end) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.sizeStart = (std::max)(start, 0.0f);
        p->settings.sizeEnd = (std::max)(end, 0.0f);
    }
}

void ScriptParticleProxy::SetLifetime(float seconds) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.lifetime = (std::max)(seconds, 0.01f);
}

void ScriptParticleProxy::SetMaxParticles(int maxParticles) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        const int clamped = (std::max)(maxParticles, 1);
        if (p->settings.maxParticles != clamped) {
            p->settings.maxParticles = clamped;
            p->runtime.gpuInitialized = false;
            p->runtime.gpuCapacity = 0;
        }
    }
}

void ScriptParticleProxy::SetPlayback(bool loop, float duration, bool clearOnStop) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.loop = loop;
        p->settings.duration = (std::max)(duration, 0.0f);
        p->settings.clearOnStop = clearOnStop;
    }
}

void ScriptParticleProxy::SetShape(ParticleEmitterShape shape) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.shape = shape;
}

void ScriptParticleProxy::SetSphereShape(float radius) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.shape = ParticleEmitterShape::Sphere;
        p->settings.sphereRadius = (std::max)(radius, 0.0f);
    }
}

void ScriptParticleProxy::SetConeShape(float radius, float angleDegrees) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.shape = ParticleEmitterShape::Cone;
        p->settings.coneRadius = (std::max)(radius, 0.0f);
        p->settings.coneAngleDegrees = (std::max)(angleDegrees, 0.0f);
    }
}

void ScriptParticleProxy::SetBoxShape(const math::Vector3& extents) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.shape = ParticleEmitterShape::Box;
        p->settings.boxExtents = {
            (std::max)(extents.x, 0.0f),
            (std::max)(extents.y, 0.0f),
            (std::max)(extents.z, 0.0f)
        };
    }
}

void ScriptParticleProxy::SetMeshShape(std::string_view modelPath, int meshIndex, float scale,
                                       bool followSkinnedAnimation, float normalVelocity) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.shape = ParticleEmitterShape::MeshSurface;
        p->settings.meshShapePath = std::string(modelPath);
        p->settings.meshShapeIndex = meshIndex;
        p->settings.meshShapeScale = (std::max)(scale, 0.0001f);
        p->settings.meshShapeFollowSkinnedAnimation = followSkinnedAnimation;
        p->settings.meshShapeNormalVelocity = normalVelocity;
        /// @note パス・サブメッシュ変更時は次のスポーンでFBX頂点を再構築する。
        p->runtime.loadedMeshShapePath.clear();
        p->runtime.loadedMeshShapeIndex = -2;
        p->runtime.meshShapeVertices.clear();
        p->runtime.meshShapeTriangles.clear();
    }
}

void ScriptParticleProxy::SetMeshShapeNormalVelocity(float normalVelocity) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script))
        p->settings.meshShapeNormalVelocity = normalVelocity;
}

void ScriptParticleProxy::SetSortMode(ParticleSortMode sortMode) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.sortMode = sortMode;
}

void ScriptParticleProxy::SetSimulationMode(ParticleSimulationMode simulationMode) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        if (p->settings.simulationMode != simulationMode) p->runtime.gpuClearPending = true;
        p->settings.simulationMode = simulationMode;
    }
}

void ScriptParticleProxy::SetSimulationSpace(ParticleSimulationSpace space) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.simulationSpace = space;
        if (space == ParticleSimulationSpace::Local)
            p->settings.simulationMode = ParticleSimulationMode::Cpu;
    }
}

void ScriptParticleProxy::SetRenderMode(ParticleRenderMode mode, float stretchScale) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.renderMode = mode;
        p->settings.stretchedVelocityScale = (std::max)(stretchScale, 0.0f);
    }
}

void ScriptParticleProxy::SetCollision(ParticleCollisionMode mode,
                                       ParticleCollisionResponse response,
                                       float radius, float bounciness) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.collisionMode = mode;
        p->settings.collisionResponse = response;
        p->settings.collisionRadius = (std::max)(radius, 0.0f);
        p->settings.collisionBounciness = (std::max)(0.0f, (std::min)(bounciness, 1.0f));
        if (mode == ParticleCollisionMode::Physics)
            p->settings.simulationMode = ParticleSimulationMode::Cpu;
    }
}

int ScriptParticleProxy::GetCollisionCount() const
{
    if (const auto* p = SelfComponent<ParticleEmitter>(script))
        return p->runtime.collisionCountThisFrame;
    return 0;
}

void ScriptParticleProxy::SetRateOverDistance(float particlesPerMeter) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script))
        p->settings.rateOverDistance = (std::max)(particlesPerMeter, 0.0f);
}

void ScriptParticleProxy::SetPrewarm(bool enabled) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.prewarm = enabled;
        p->runtime.prewarmed = false;
        p->runtime.prewarmSpawnPending = 0;
    }
}

void ScriptParticleProxy::SetSubEmitters(std::string_view birthEmitter,
                                         std::string_view deathEmitter,
                                         std::string_view collisionEmitter,
                                         int burstCount) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.birthSubEmitter = std::string(birthEmitter);
        p->settings.deathSubEmitter = std::string(deathEmitter);
        p->settings.collisionSubEmitter = std::string(collisionEmitter);
        p->settings.subEmitterBurstCount = (std::max)(burstCount, 1);
    }
}

void ScriptParticleProxy::SetVelocityDamping(float damping) const
{
    /// @note @note 旧 API 名のまま。減衰は «流れへ寄る速さ» になったので flowCoupling を触る。
    if (auto* p = SelfComponent<ParticleEmitter>(script))
        p->settings.flowCoupling = (std::max)(damping, 0.0f);
}

void ScriptParticleProxy::SetAngularVelocity(float minValue, float maxValue) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.angularVelocityMin = minValue;
        p->settings.angularVelocityMax = maxValue;
    }
}

void ScriptParticleProxy::SetNoise(float strength, float frequency, float speed) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        FlowFieldSettings& curl = p->settings.EnsureLocalForce(FlowFieldType::Curl);
        curl.strength       = (std::max)(strength, 0.0f);
        curl.noiseFrequency = (std::max)(frequency, 0.0001f);
        curl.noiseSpeed     = speed;
    }
}

void ScriptParticleProxy::SetReceiveFlowFields(bool receive) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.receiveFlowFields = receive;
}

void ScriptParticleProxy::SetFlowFieldChannels(uint32_t channels) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.flowFieldChannels = channels;
}

bool ScriptParticleProxy::IsEnabled() const
{
    const auto* p = SelfComponent<ParticleEmitter>(script);
    return p && p->settings.enabled;
}

bool ScriptParticleProxy::IsPlaying() const
{
    const auto* p = SelfComponent<ParticleEmitter>(script);
    return p && p->settings.playing;
}

int ScriptParticleProxy::GetParticleCount() const
{
    const auto* p = SelfComponent<ParticleEmitter>(script);
    return p ? static_cast<int>(p->runtime.particles.size()) : 0;
}

float ScriptParticleProxy::GetEmitRate() const
{
    const auto* p = SelfComponent<ParticleEmitter>(script);
    return p ? p->settings.emitRate : 0.0f;
}

int ScriptParticleProxy::GetMaxParticles() const
{
    const auto* p = SelfComponent<ParticleEmitter>(script);
    return p ? p->settings.maxParticles : 0;
}

float ScriptParticleProxy::GetLifetime() const
{
    const auto* p = SelfComponent<ParticleEmitter>(script);
    return p ? p->settings.lifetime : 0.0f;
}

uint32_t ScriptParticleProxy::GetFlowFieldChannels() const
{
    const auto* p = SelfComponent<ParticleEmitter>(script);
    return p ? p->settings.flowFieldChannels : 0u;
}

float ScriptParticleProxy::GetPlayTime() const
{
    const auto* p = SelfComponent<ParticleEmitter>(script);
    return p ? p->runtime.playTime : 0.0f;
}

ParticleSimulationMode ScriptParticleProxy::GetSimulationMode() const
{
    const auto* p = SelfComponent<ParticleEmitter>(script);
    return p ? p->settings.simulationMode : ParticleSimulationMode::Cpu;
}

ParticleGpuFallbackReason ScriptParticleProxy::GetGpuFallbackReason() const
{
    const auto* p = SelfComponent<ParticleEmitter>(script);
    if (!p) return ParticleGpuFallbackReason::NotRequested;
    /// @note ParticlePass と同じ引数で呼ぶ。runtime.material を渡さないと .mat 由来の 3 条件
    ///       (フリップブック補間・モーションベクター・自己影) が判定から抜け、
    ///       実際は CPU で回っているのに「GPU で回る」と答えてしまう。
    return GetParticleGpuFallbackReason(p->settings, &p->runtime.material);
}

bool ScriptParticleProxy::IsGpuSimulated() const
{
    return GetGpuFallbackReason() == ParticleGpuFallbackReason::None;
}

const char* ScriptParticleProxy::GetGpuFallbackField() const
{
    return ParticleGpuFallbackFieldName(GetGpuFallbackReason());
}

const char* ScriptParticleProxy::GetGpuFallbackDescription() const
{
    return ParticleGpuFallbackDescription(GetGpuFallbackReason());
}

void ScriptVFXProxy::Play(bool restart) const
{
    if (auto* vfx = SelfComponent<VFXComponent>(script)) {
        if (restart) vfx->Restart();
        else vfx->Resume();
    }
}

void ScriptVFXProxy::Pause() const
{
    if (auto* vfx = SelfComponent<VFXComponent>(script)) vfx->Pause();
}

void ScriptVFXProxy::Stop() const
{
    if (auto* vfx = SelfComponent<VFXComponent>(script)) vfx->Stop();
}

void ScriptVFXProxy::SetSpeed(float speed) const
{
    if (auto* vfx = SelfComponent<VFXComponent>(script))
        vfx->speed = (std::max)(speed, 0.0f);
}

bool ScriptVFXProxy::Trigger(std::string_view name) const
{
    if (name.empty()) return false;
    auto* vfx = SelfComponent<VFXComponent>(script);
    if (vfx == nullptr) return false;
    vfx->Trigger(name);
    return true;
}

bool ScriptVFXProxy::IsPlaying() const
{
    const auto* vfx = SelfComponent<VFXComponent>(script);
    return vfx != nullptr && vfx->playing;
}

float ScriptVFXProxy::GetTime() const
{
    const auto* vfx = SelfComponent<VFXComponent>(script);
    return vfx != nullptr ? vfx->time : 0.0f;
}

float ScriptVFXProxy::GetDuration() const
{
    const auto* vfx = SelfComponent<VFXComponent>(script);
    if (vfx == nullptr) return 0.0f;
    /// @note duration が 0 のときは配下から算出した実効尺が正。
    return vfx->duration > 0.0f ? vfx->duration : vfx->resolvedDuration;
}

float ScriptVFXProxy::GetSpeed() const
{
    const auto* vfx = SelfComponent<VFXComponent>(script);
    return vfx != nullptr ? vfx->speed : 1.0f;
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

bool ScriptTrailProxy::IsEnabled() const
{
    const auto* t = SelfComponent<TrailComponent>(script);
    return t && t->enabled;
}

float ScriptTrailProxy::GetDuration() const
{
    const auto* t = SelfComponent<TrailComponent>(script);
    return t ? t->duration : 0.0f;
}

int ScriptTrailProxy::GetPointCount() const
{
    const auto* t = SelfComponent<TrailComponent>(script);
    return t ? t->ringCount : 0;
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

bool ScriptMeshTrailProxy::IsEnabled() const
{
    const auto* t = SelfComponent<MeshTrailComponent>(script);
    return t && t->enabled;
}

float ScriptMeshTrailProxy::GetDuration() const
{
    const auto* t = SelfComponent<MeshTrailComponent>(script);
    return t ? t->duration : 0.0f;
}

int ScriptMeshTrailProxy::GetSampleCount() const
{
    const auto* t = SelfComponent<MeshTrailComponent>(script);
    return t ? t->sampleCount : 0;
}

/// @name UIButton 入力の取得
/// @brief UISystem が毎フレーム立て直す onClick / onEnter / onExit / state をそのまま読む。
/// @brief 取得系はボタンが無い場合に「押されていない」を返す方が呼び出し側の分岐が減るため、
/// @brief nullptr は一律 false / Disabled に潰す。
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

void ScriptUIProxy::SetTextColor(const math::Vector4& color) const
{
    if (auto* t = SelfComponent<UIText>(script)) t->color = color;
}

void ScriptUIProxy::SetCanvasSortOrder(int order) const
{
    if (auto* canvas = SelfComponent<UICanvas>(script)) canvas->sortOrder = order;
}

void ScriptUIProxy::SetPointer(const math::Vector2& canvasPosition, bool pressed) const
{
    UIPointer::Set(canvasPosition, pressed);
}

void ScriptUIProxy::ClearPointer() const
{
    UIPointer::Clear();
}

bool ScriptUIProxy::IsPointerOverridden() const
{
    return UIPointer::IsActive();
}

bool ScriptUIProxy::TryGetCanvasSize(math::Vector2& outSize, GameObject* canvasObject) const
{
    if (!canvasObject) {
        canvasObject = script ? script->m_gameObject : nullptr;
        if (!ObjectComponent<UICanvas>(canvasObject))
            canvasObject = ScriptSceneProxy{script}.FindObjectOfType<UICanvas>();
    }
    const auto* canvas = ObjectComponent<UICanvas>(canvasObject);
    const auto runtime = ScriptRuntime::GetCurrent();
    if (!canvas || runtime.viewportWidth == 0 || runtime.viewportHeight == 0) return false;
    const auto size = GetCanvasRectSize(*canvas, static_cast<float>(runtime.viewportWidth),
                                       static_cast<float>(runtime.viewportHeight));
    if (!std::isfinite(size.x) || !std::isfinite(size.y) || size.x <= 0.0f || size.y <= 0.0f) return false;
    outSize = size;
    return true;
}

math::Vector2 ScriptUIProxy::GetCanvasMousePosition() const
{
    /// @note 自 GO が Canvas ならそれを、そうでなければシーンの最初の Canvas を使う。
    if (auto* self = SelfComponent<UICanvas>(script)) return self->resolvedMousePosition;
    const ScriptSceneProxy sceneProxy{ script };
    if (GameObject* go = sceneProxy.FindObjectOfType<UICanvas>())
        if (auto* canvas = ObjectComponent<UICanvas>(go)) return canvas->resolvedMousePosition;
    return {};
}

void ScriptUIProxy::SetImageSpriteRect(float x, float y, float w, float h, float texW, float texH) const
{
    if (auto* img = SelfComponent<UIImage>(script)) {
        const auto [uvMin, uvMax] = UIImage::PixelRectToUV(x, y, w, h, texW, texH);
        img->uvMin = uvMin;
        img->uvMax = uvMax;
    }
}

void ScriptUIProxy::SetImageTexture(std::string_view path) const
{
    if (auto* img = SelfComponent<UIImage>(script)) img->texturePath = std::string(path);
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

void ScriptUIProxy::SetImageTexture(GameObject* go, std::string_view path) const
{
    if (auto* img = ObjectComponent<UIImage>(go)) img->texturePath = std::string(path);
}

void ScriptUIProxy::SetText(GameObject* go, std::string_view text) const
{
    if (auto* t = ObjectComponent<UIText>(go)) t->text = std::string(text);
}

void ScriptUIProxy::SetTextColor(GameObject* go, const math::Vector4& color) const
{
    if (auto* t = ObjectComponent<UIText>(go)) t->color = color;
}

void ScriptUIProxy::SetImageEnabled(GameObject* go, bool enabled) const
{
    if (auto* image = ObjectComponent<UIImage>(go)) image->enabled = enabled;
}

void ScriptUIProxy::SetTextEnabled(GameObject* go, bool enabled) const
{
    if (auto* t = ObjectComponent<UIText>(go)) t->enabled = enabled;
}

float ScriptUIProxy::GetImageFillAmount() const
{
    const auto* img = SelfComponent<UIImage>(script);
    return img ? img->fillAmount : 0.0f;
}

math::Vector4 ScriptUIProxy::GetImageColor() const
{
    const auto* img = SelfComponent<UIImage>(script);
    return img ? img->color : math::Vector4{ 1.0f, 1.0f, 1.0f, 1.0f };
}

std::string ScriptUIProxy::GetText() const
{
    const auto* t = SelfComponent<UIText>(script);
    return t ? t->text : std::string{};
}

math::Vector4 ScriptUIProxy::GetTextColor() const
{
    const auto* t = SelfComponent<UIText>(script);
    return t ? t->color : math::Vector4{ 1.0f, 1.0f, 1.0f, 1.0f };
}

bool ScriptUIProxy::IsImageEnabled() const
{
    const auto* img = SelfComponent<UIImage>(script);
    return img && img->enabled;
}

bool ScriptUIProxy::IsTextEnabled() const
{
    const auto* t = SelfComponent<UIText>(script);
    return t && t->enabled;
}

float ScriptUIProxy::GetImageFillAmount(GameObject* go) const
{
    const auto* img = ObjectComponent<UIImage>(go);
    return img ? img->fillAmount : 0.0f;
}

math::Vector4 ScriptUIProxy::GetImageColor(GameObject* go) const
{
    const auto* img = ObjectComponent<UIImage>(go);
    return img ? img->color : math::Vector4{ 1.0f, 1.0f, 1.0f, 1.0f };
}

std::string ScriptUIProxy::GetText(GameObject* go) const
{
    const auto* t = ObjectComponent<UIText>(go);
    return t ? t->text : std::string{};
}

math::Vector4 ScriptUIProxy::GetTextColor(GameObject* go) const
{
    const auto* t = ObjectComponent<UIText>(go);
    return t ? t->color : math::Vector4{ 1.0f, 1.0f, 1.0f, 1.0f };
}

bool ScriptUIProxy::IsImageEnabled(GameObject* go) const
{
    const auto* img = ObjectComponent<UIImage>(go);
    return img && img->enabled;
}

bool ScriptUIProxy::IsTextEnabled(GameObject* go) const
{
    const auto* t = ObjectComponent<UIText>(go);
    return t && t->enabled;
}

void ScriptUIProxy::SetMaterial(GameObject* go, std::string_view materialPath) const
{
    auto* image = ObjectComponent<UIImage>(go);
    if (!image) return;
    const std::string next(materialPath);
    if (image->materialPath == next) return;
    image->materialPath = next;
    /// @note 別のマテリアルへ移ると、前のシェーダーに合わせた上書きは意味を失う。
    ///       残すと「効かない上書き」が黙って積まれ、綴り間違いと区別が付かなくなる。
    image->materialParamOverrides.clear();
    image->materialTextureOverrides.clear();
}

namespace {

/// @brief 上書きを「その場で」書き換える。
/// @note 代入 (`map[key] = {...}`) は初期化子リストから毎回 vector を作って move するため、
///       要素ごと毎フレーム呼ばれるこの Setter では確保・解放が定常的に走る。既存エントリの
///       容量へ assign すれば 2 フレーム目以降の確保は 0 (キーの std::string も短ければ SSO で済む)。
void AssignUIMaterialOverride(UIImage& image, std::string_view param,
                              const float* values, std::size_t count)
{
    auto& target = image.materialParamOverrides[std::string(param)];
    target.assign(values, values + count);
}

} // namespace

void ScriptUIProxy::SetMaterialFloat(GameObject* go, std::string_view param, float value) const
{
    if (auto* image = ObjectComponent<UIImage>(go))
        AssignUIMaterialOverride(*image, param, &value, 1);
}

void ScriptUIProxy::SetMaterialVector2(GameObject* go, std::string_view param,
                                       float x, float y) const
{
    const float values[2] = { x, y };
    if (auto* image = ObjectComponent<UIImage>(go))
        AssignUIMaterialOverride(*image, param, values, 2);
}

void ScriptUIProxy::SetMaterialVector4(GameObject* go, std::string_view param,
                                       const math::Vector4& value) const
{
    const float values[4] = { value.x, value.y, value.z, value.w };
    if (auto* image = ObjectComponent<UIImage>(go))
        AssignUIMaterialOverride(*image, param, values, 4);
}

void ScriptUIProxy::SetMaterialColor(GameObject* go, std::string_view param,
                                     const math::Vector4& color) const
{
    SetMaterialVector4(go, param, color);
}

void ScriptUIProxy::SetMaterialTexture(GameObject* go, std::string_view slot,
                                       std::string_view texturePath) const
{
    if (auto* image = ObjectComponent<UIImage>(go)) {
        /// @note 値側も assign で容量を使い回す。テクスチャパスは SSO に収まらない長さになる。
        image->materialTextureOverrides[std::string(slot)].assign(
            texturePath.data(), texturePath.size());
    }
}

void ScriptUIProxy::ClearMaterialOverride(GameObject* go, std::string_view param) const
{
    if (auto* image = ObjectComponent<UIImage>(go)) {
        const std::string key(param);
        image->materialParamOverrides.erase(key);
        image->materialTextureOverrides.erase(key);
    }
}

void ScriptUIProxy::ClearMaterialOverrides(GameObject* go) const
{
    if (auto* image = ObjectComponent<UIImage>(go)) {
        image->materialParamOverrides.clear();
        image->materialTextureOverrides.clear();
    }
}

bool ScriptUIProxy::HasMaterialOverride(GameObject* go, std::string_view param) const
{
    const auto* image = ObjectComponent<UIImage>(go);
    if (!image) return false;
    const std::string key(param);
    return image->materialParamOverrides.count(key) > 0
        || image->materialTextureOverrides.count(key) > 0;
}

/// @name ウィジェットの値
/// @brief 子オブジェクトを名前で探す。
/// @note スクリプトが Engine の実装型に直接依存しないための層 (AGENTS.md)。無いと
///       オプション画面が GetComponent<UISlider>() を直に呼ぶ形になる。
GameObject* ScriptUIProxy::Find(GameObject* parent, std::string_view childName) const
{
    if (!parent || !parent->IsValid()) return nullptr;
    for (int i = 0; i < parent->GetChildCount(); ++i) {
        GameObject* child = parent->GetChild(i);
        if (child && child->name == childName) return child;
    }
    return nullptr;
}

bool ScriptUIProxy::IsPointerOverUI() const
{
    return UIPointerOverUI();
}

float ScriptUIProxy::GetSliderValue(GameObject* go) const
{
    const auto* s = ObjectComponent<UISlider>(go);
    return s ? s->value : 0.0f;
}

void ScriptUIProxy::SetSliderValue(GameObject* go, float value) const
{
    auto* s = ObjectComponent<UISlider>(go);
    if (!s) return;
    const float low  = (std::min)(s->minimum, s->maximum);
    const float high = (std::max)(s->minimum, s->maximum);
    s->value = std::clamp(s->wholeNumbers ? std::round(value) : value, low, high);
    /// @note 見た目の追従は UISystem がドラッグ中にしかやらない。外から入れた値でも
    ///       ゲージが動くよう、同じ規則をここでも当てる。
    if (auto* image = ObjectComponent<UIImage>(go)) {
        const float range = s->maximum - s->minimum;
        image->fillAmount = std::abs(range) > 0.000001f
            ? std::clamp((s->value - s->minimum) / range, 0.0f, 1.0f) : 0.0f;
    }
}

bool ScriptUIProxy::WasSliderChanged(GameObject* go) const
{
    const auto* s = ObjectComponent<UISlider>(go);
    return s && s->onValueChanged;
}

void ScriptUIProxy::SetSliderRange(GameObject* go, float minimum, float maximum) const
{
    if (auto* s = ObjectComponent<UISlider>(go)) {
        s->minimum = minimum;
        s->maximum = maximum;
    }
}

void ScriptUIProxy::SetSliderInteractable(GameObject* go, bool interactable) const
{
    if (auto* s = ObjectComponent<UISlider>(go)) s->interactable = interactable;
}

bool ScriptUIProxy::IsToggleOn(GameObject* go) const
{
    const auto* t = ObjectComponent<UIToggle>(go);
    return t && t->isOn;
}

void ScriptUIProxy::SetToggleOn(GameObject* go, bool isOn) const
{
    if (auto* t = ObjectComponent<UIToggle>(go)) t->isOn = isOn;
}

bool ScriptUIProxy::WasToggleChanged(GameObject* go) const
{
    const auto* t = ObjectComponent<UIToggle>(go);
    return t && t->onValueChanged;
}

void ScriptUIProxy::SetToggleInteractable(GameObject* go, bool interactable) const
{
    if (auto* t = ObjectComponent<UIToggle>(go)) t->interactable = interactable;
}

math::Vector2 ScriptUIProxy::GetScrollPosition(GameObject* go) const
{
    const auto* s = ObjectComponent<UIScrollView>(go);
    return s ? s->scrollPosition : math::Vector2::ZERO;
}

void ScriptUIProxy::SetScrollPosition(GameObject* go, const math::Vector2& position) const
{
    if (auto* s = ObjectComponent<UIScrollView>(go)) s->scrollPosition = position;
}

void ScriptUIProxy::SetScrollContentSize(GameObject* go, const math::Vector2& size) const
{
    if (auto* s = ObjectComponent<UIScrollView>(go)) s->contentSize = size;
}

bool ScriptUIProxy::WasScrollChanged(GameObject* go) const
{
    const auto* s = ObjectComponent<UIScrollView>(go);
    return s && s->onValueChanged;
}

std::string ScriptUIProxy::GetFieldText(GameObject* go) const
{
    const auto* f = ObjectComponent<UIInputField>(go);
    return f ? f->text : std::string{};
}

void ScriptUIProxy::SetFieldText(GameObject* go, std::string_view text) const
{
    if (auto* f = ObjectComponent<UIInputField>(go)) {
        f->text.assign(text.data(), text.size());
        f->caretPosition = f->text.size();
    }
}

bool ScriptUIProxy::WasFieldChanged(GameObject* go) const
{
    const auto* f = ObjectComponent<UIInputField>(go);
    return f && f->onValueChanged;
}

bool ScriptUIProxy::WasSubmitted(GameObject* go) const
{
    const auto* f = ObjectComponent<UIInputField>(go);
    return f && f->onSubmit;
}

bool ScriptUIProxy::IsFieldFocused(GameObject* go) const
{
    const auto* f = ObjectComponent<UIInputField>(go);
    return f && f->focused;
}

void ScriptUIProxy::SetFieldFocused(GameObject* go, bool focused) const
{
    if (auto* f = ObjectComponent<UIInputField>(go)) f->focused = focused;
}

float ScriptUIProxy::GetGroupAlpha(GameObject* go) const
{
    const auto* g = ObjectComponent<UICanvasGroup>(go);
    return g ? g->alpha : 1.0f;
}

void ScriptUIProxy::SetGroupAlpha(GameObject* go, float alpha) const
{
    if (auto* g = ObjectComponent<UICanvasGroup>(go))
        g->alpha = std::clamp(alpha, 0.0f, 1.0f);
}

void ScriptUIProxy::SetGroupInteractable(GameObject* go, bool interactable) const
{
    if (auto* g = ObjectComponent<UICanvasGroup>(go)) g->interactable = interactable;
}

void ScriptUIProxy::SetGroupBlocksRaycasts(GameObject* go, bool blocks) const
{
    if (auto* g = ObjectComponent<UICanvasGroup>(go)) g->blocksRaycasts = blocks;
}

namespace {

/// @brief go が属する Canvas を親方向に辿って探す。
/// @note フォーカスは Canvas が持つ。要素側から辿らないと、複数 Canvas がある画面でどれの
///       フォーカスを触るのか決まらない。
UICanvas* OwningCanvas(GameObject* go)
{
    for (GameObject* node = go; node != nullptr; node = node->GetParent()) {
        if (auto* canvas = node->GetComponent<UICanvas>()) return canvas;
    }
    return nullptr;
}

} // namespace

void ScriptUIProxy::SetFocus(GameObject* go) const
{
    if (!go || !go->IsValid()) return;
    if (auto* canvas = OwningCanvas(go))
        canvas->focusedObject = { go->GetID() };
}

GameObject* ScriptUIProxy::GetFocus() const
{
    const ScriptSceneProxy sceneProxy{ script };
    GameObject* canvasGO = sceneProxy.FindObjectOfType<UICanvas>();
    const auto* canvas = ObjectComponent<UICanvas>(canvasGO);
    if (!canvas || !canvas->focusedObject.IsValid()) return nullptr;
    return canvas->focusedObject.Resolve(sceneProxy);
}

bool ScriptUIProxy::IsFocused(GameObject* go) const
{
    const auto* nav = ObjectComponent<UINavigation>(go);
    return nav && nav->focused;
}

void ScriptUIProxy::ClearFocus(GameObject* go) const
{
    if (!go || !go->IsValid()) return;
    if (auto* canvas = OwningCanvas(go)) canvas->focusedObject = {};
}

bool ScriptUIProxy::IsDragging(GameObject* go) const
{
    const auto* d = ObjectComponent<UIDragSource>(go);
    return d && d->dragging;
}

bool ScriptUIProxy::WasDropped(GameObject* go) const
{
    const auto* d = ObjectComponent<UIDragSource>(go);
    return d && d->dropped;
}

GameObject* ScriptUIProxy::GetDropTarget(GameObject* go) const
{
    const auto* d = ObjectComponent<UIDragSource>(go);
    if (!d || !d->droppedOn.IsValid()) return nullptr;
    const ScriptSceneProxy sceneProxy{ script };
    return d->droppedOn.Resolve(sceneProxy);
}

bool ScriptUIProxy::WasReceived(GameObject* go) const
{
    const auto* t = ObjectComponent<UIDropTarget>(go);
    return t && t->received;
}

GameObject* ScriptUIProxy::GetReceivedFrom(GameObject* go) const
{
    const auto* t = ObjectComponent<UIDropTarget>(go);
    if (!t || !t->receivedFrom.IsValid()) return nullptr;
    const ScriptSceneProxy sceneProxy{ script };
    return t->receivedFrom.Resolve(sceneProxy);
}

std::string ScriptUIProxy::GetReceivedPayload(GameObject* go) const
{
    const auto* t = ObjectComponent<UIDropTarget>(go);
    return t ? t->receivedPayload : std::string{};
}

bool ScriptUIProxy::IsDropHovered(GameObject* go) const
{
    const auto* t = ObjectComponent<UIDropTarget>(go);
    return t && t->hovered;
}

void ScriptUIProxy::SetVisibleCharacters(GameObject* go, int count) const
{
    if (auto* t = ObjectComponent<UIText>(go)) t->visibleCharacters = count;
}

int ScriptUIProxy::GetVisibleCharacters(GameObject* go) const
{
    const auto* t = ObjectComponent<UIText>(go);
    return t ? t->visibleCharacters : -1;
}

int ScriptUIProxy::GetCharacterCount(GameObject* go) const
{
    const auto* t = ObjectComponent<UIText>(go);
    if (!t) return 0;
    return static_cast<int>(UITextVisibleLength(t->text, t->richText));
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
        if (!go.activeInHierarchy()) continue;
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
    /// @note Unity の isActiveAndEnabled と同じく、親 GameObject の無効化も実効状態へ反映する。
    return script && script->m_gameObject && script->m_gameObject->activeInHierarchy() && script->enabled;
}

GameObject* ScriptSceneProxy::Instantiate(const PrefabRef& prefab) const
{
    return Instantiate(prefab.path);
}

GameObject* ScriptSceneProxy::Instantiate(const std::string& prefabPath) const
{
    if (!script || !script->m_scene || prefabPath.empty()) return nullptr;
    /// @note PrefabSerializer::Instantiate は内部で SceneIO::Deserialize を呼び全 GameObject を再構築し、
    ///       呼び出し元 Script (と m_gameObject) を解放する。呼び出し後に script->m_scene を参照すると
    ///       クラッシュするため、同アドレスで生き続ける Scene* を先に退避しておく。
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

/// @name オブジェクトプール

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
    /// @note Instantiate と同じ理由で Scene* を先に退避する。プールが空だった場合は
    ///       内部で Instantiate が走り、GameObject 配列が再確保され得る。
    Scene* scene = script->m_scene;
    return PrefabPool::Spawn(*scene, prefabPath, position, rotation);
}

GameObject* ScriptSceneProxy::Spawn(const PrefabRef& prefab) const
{
    if (!script || !script->m_gameObject) return nullptr;
    /// @note 値へコピーしてから渡す: Spawn 内部で Instantiate が走ると Scene の GameObject 配列が
    ///       再確保され、m_gameObject->transform の参照先が無効になるため。
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
    /// @note 地形なし時は lowest() を返す。worldPos.y を返すと高さが一致し、呼び出し側の
    ///       «nextPos.y <= terrainH» が常に true になる誤判定を防ぐ。
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
        if (auto* water = go.GetComponent<WaterComponent>())
            return go.transform.worldPosition.y + water->GetSurfaceHeightAt(worldPos.x, worldPos.z, time);
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

std::string ScriptAnimatorProxy::GetCurrentState(GameObject* go) const
{
    const auto* a = ObjectComponent<AnimatorComponent>(go);
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

void ScriptAnimatorProxy::SetLocalTimeScale(float scale) const
{
    if (auto* a = SelfComponent<AnimatorComponent>(script))
        a->localTimeScale = std::clamp(scale, 0.0f, 8.0f);
}

float ScriptAnimatorProxy::GetSpeed() const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a ? a->speed : 1.0f;
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

float ScriptAnimatorProxy::GetSpeed(GameObject* go) const
{
    const auto* a = ObjectComponent<AnimatorComponent>(go);
    return a ? a->speed : 1.0f;
}

void ScriptAnimatorProxy::SetPlaying(bool playing) const
{
    if (auto* a = SelfComponent<AnimatorComponent>(script)) a->playing = playing;
}

bool ScriptAnimatorProxy::IsPlaying() const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a && a->playing;
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
    /// @note 名前引きと診断は AnimatorComponent 側に集約してある (このファイルの方針どおり
    ///       «Script から自 GameObject の Animator を引くだけ» に留める)。以前ここだけ
    ///       探索を書き写していたため、レイヤー名を間違えたときの報告が漏れていた。
    if (auto* animator = SelfComponent<AnimatorComponent>(script))
        animator->SetLayerWeight(layerName, weight);
}

float ScriptAnimatorProxy::GetLayerWeight(std::string_view layerName) const
{
    if (const auto* animator = SelfComponent<AnimatorComponent>(script))
        for (const auto& layer : animator->layers)
            if (layer.name == layerName) return layer.weight;
    return 0.0f;
}

/// @name レイヤー制御 / Slot
/// @note 判定と状態遷移は AnimatorComponent 側のメソッドに集約済み。ここは Script から
///       自 GameObject の Animator を引くだけの薄い委譲に留める。

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
    if (!layer) { ReportUnknownAnimationLayer("SetLayerMask", layerName, animator->layers); return; }
    layer->mask.path = std::string(maskPath);
    /// @note ロード済みキャッシュを落とし、次フレームの AnimatorSystem に読み直させる。
    layer->mask.Invalidate();
}

std::string ScriptAnimatorProxy::GetLayerMask(std::string_view layerName) const
{
    auto* animator = SelfComponent<AnimatorComponent>(script);
    if (!animator) return {};
    const AnimationLayer* layer = animator->FindLayer(layerName);
    return layer ? layer->mask.path : std::string{};
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

void ScriptAnimatorProxy::SetSlotSpeed(std::string_view layerName, float speed) const
{
    auto* animator = SelfComponent<AnimatorComponent>(script);
    if (!animator) return;
    AnimationLayer* layer = animator->FindLayer(layerName);
    if (!layer) { ReportUnknownAnimationLayer("SetSlotSpeed", layerName, animator->layers); return; }
    if (layer->slot.active) layer->slot.speed = speed;
}

float ScriptAnimatorProxy::GetSlotTime(std::string_view layerName) const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    if (!animator) return 0.0f;
    const AnimationLayer* layer = animator->FindLayer(layerName);
    return layer && layer->slot.active ? layer->slot.time : 0.0f;
}

void ScriptAnimatorProxy::SetStateSpeed(std::string_view stateName, float speed) const
{
    auto* animator = SelfComponent<AnimatorComponent>(script);
    if (!animator) return;
    for (AnimationState& state : animator->states)
        if (state.name == stateName) { state.speed = speed; return; }
}

float ScriptAnimatorProxy::GetStateSpeed(std::string_view stateName) const
{
    if (const auto* animator = SelfComponent<AnimatorComponent>(script))
        for (const AnimationState& state : animator->states)
            if (state.name == stateName) return state.speed;
    return 1.0f;
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

float ScriptAnimatorProxy::GetRootMotionPositionScale() const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    return animator ? animator->rootMotion.positionScale : 1.0f;
}

void ScriptAnimatorProxy::SetRootMotionRotationScale(float scale) const
{
    if (auto* animator = SelfComponent<AnimatorComponent>(script))
        animator->rootMotion.rotationScale = scale;
}

float ScriptAnimatorProxy::GetRootMotionRotationScale() const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    return animator ? animator->rootMotion.rotationScale : 1.0f;
}

std::string ScriptAnimatorProxy::GetRootMotionNodeName() const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    return animator ? animator->rootMotion.nodeName : std::string{};
}

void ScriptAnimatorProxy::SetRootMotionNodeName(std::string_view nodeName) const
{
    auto* animator = SelfComponent<AnimatorComponent>(script);
    if (!animator) return;
    animator->rootMotion.nodeName = nodeName;
    /// @note 名前を指定したら解決方法も NodeName へ切り替える。空なら クリップ指定へ戻す。
    animator->rootMotion.source = nodeName.empty()
        ? RootMotionSource::ClipDefined
        : RootMotionSource::NodeName;
    /// @note トラックが変わるとサンプル位置の連続性が失われるため、キャッシュを捨てる。
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

namespace {

/// @brief 要求 1 件の共通部分を埋める。フィールドの多重利用は ScriptDebugDrawType を参照。
ScriptDebugDrawCommand MakeDebugDrawCommand(ScriptDebugDrawType type, const math::Vector3& a,
                                            const math::Vector3& b, const math::Vector4& color,
                                            float duration, bool depthTest)
{
    ScriptDebugDrawCommand command;
    command.type      = type;
    command.a         = a;
    command.b         = b;
    command.color     = color;
    command.duration  = duration;
    command.depthTest = depthTest;
    return command;
}

} // namespace

void ScriptDebugProxy::DrawLine(const math::Vector3& a, const math::Vector3& b, const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    script->m_scene->QueueScriptDebugDraw(
        MakeDebugDrawCommand(ScriptDebugDrawType::Line, a, b, color, duration, depthTest));
}

void ScriptDebugProxy::DrawSphere(const math::Vector3& center, float radius, const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    ScriptDebugDrawCommand command =
        MakeDebugDrawCommand(ScriptDebugDrawType::Sphere, center, {}, color, duration, depthTest);
    command.radius = radius;
    script->m_scene->QueueScriptDebugDraw(command);
}

void ScriptDebugProxy::DrawBox(const math::Vector3& center, const math::Vector3& halfExtents, const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    ScriptDebugDrawCommand command =
        MakeDebugDrawCommand(ScriptDebugDrawType::Box, center, {}, color, duration, depthTest);
    command.halfExtents = halfExtents;
    script->m_scene->QueueScriptDebugDraw(command);
}

void ScriptDebugProxy::DrawBox(const math::Vector3& center, const math::Vector3& halfExtents,
                               const math::Quaternion& rotation, const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    ScriptDebugDrawCommand command =
        MakeDebugDrawCommand(ScriptDebugDrawType::OrientedBox, center, {}, color, duration, depthTest);
    command.halfExtents = halfExtents;
    command.rotation    = rotation;
    script->m_scene->QueueScriptDebugDraw(command);
}

void ScriptDebugProxy::DrawRay(const math::Vector3& origin, const math::Vector3& dir, const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    script->m_scene->QueueScriptDebugDraw(
        MakeDebugDrawCommand(ScriptDebugDrawType::Ray, origin, origin + dir, color, duration, depthTest));
}

void ScriptDebugProxy::DrawArrow(const math::Vector3& from, const math::Vector3& to,
                                  float headLength, float headRadius,
                                  const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    ScriptDebugDrawCommand command =
        MakeDebugDrawCommand(ScriptDebugDrawType::Arrow, from, to, color, duration, depthTest);
    command.radius      = headLength;
    command.halfExtents = { headRadius, 0.0f, 0.0f };
    script->m_scene->QueueScriptDebugDraw(command);
}

void ScriptDebugProxy::DrawCone(const math::Vector3& apex, const math::Vector3& direction,
                                 float height, float baseRadius,
                                 const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    ScriptDebugDrawCommand command =
        MakeDebugDrawCommand(ScriptDebugDrawType::Cone, apex, direction, color, duration, depthTest);
    command.radius      = baseRadius;
    command.halfExtents = { height, 0.0f, 0.0f };
    script->m_scene->QueueScriptDebugDraw(command);
}

void ScriptDebugProxy::DrawCapsule(const math::Vector3& center, float radius, float halfHeight,
                                   const math::Quaternion& rotation, const math::Vector4& color,
                                   float duration) const
{
    if (!script || !script->m_scene) return;
    ScriptDebugDrawCommand command =
        MakeDebugDrawCommand(ScriptDebugDrawType::Capsule, center, {}, color, duration, depthTest);
    command.radius      = radius;
    command.halfExtents = { halfHeight, 0.0f, 0.0f };
    command.rotation    = rotation;
    script->m_scene->QueueScriptDebugDraw(command);
}

void ScriptDebugProxy::DrawCircle(const math::Vector3& center, const math::Vector3& normal, float radius,
                                  const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    ScriptDebugDrawCommand command =
        MakeDebugDrawCommand(ScriptDebugDrawType::Circle, center, normal, color, duration, depthTest);
    command.radius = radius;
    script->m_scene->QueueScriptDebugDraw(command);
}

void ScriptDebugProxy::DrawArc(const math::Vector3& center, const math::Vector3& normal,
                               const math::Vector3& fromDirection, float radius, float angleDegrees,
                               const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene) return;
    ScriptDebugDrawCommand command =
        MakeDebugDrawCommand(ScriptDebugDrawType::Arc, center, normal, color, duration, depthTest);
    command.halfExtents = fromDirection;
    command.radius      = radius;
    command.angle       = math::ToRad(angleDegrees);
    script->m_scene->QueueScriptDebugDraw(command);
}

void ScriptDebugProxy::DrawPolyline(std::span<const math::Vector3> points, bool closed,
                                    const math::Vector4& color, float duration) const
{
    if (!script || !script->m_scene || points.size() < 2) return;
    for (size_t i = 0; i + 1 < points.size(); ++i)
        DrawLine(points[i], points[i + 1], color, duration);
    if (closed && points.size() > 2)
        DrawLine(points.back(), points.front(), color, duration);
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
    if (index >= 8) return false;

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
    /// @note DataAssetRegistry 経由: 同じプロファイルを複数箇所から読んでも TOML の再パースが起きず、
    ///       エディタでの編集が即座に反映される。旧 .fzpp の直接パース経路より安く共有実体とも一致する。
    auto* profile = dynamic_cast<asset::PostProcessProfile*>(
        asset::DataAssetRegistry::Resolve(std::string(profilePath)));
    if (!profile) return false;

    /// @note プロファイルは «効果のリスト» なので、まず既定値へ重み 1 で解決してから渡す。渡すのは
    ///       ポストプロセス部分のみ: このプロキシが書くランタイム上書きの器は Scene の
    ///       PostProcessSettings で SSR/TAA 等の高度グラフィクスの置き場が無い。恒久的に効かせたい
    ///       場合は PostProcessVolume からこのプロファイルを参照させる。
    renderer::VolumeSettings resolved;
    profile->ApplyTo(resolved, 1.0f);
    Set(resolved.post);
    return true;
}

/// @note OnDrawGizmos は DebugDraw の記録区間 (CaptureScriptGizmos) で呼ばれるので直接積める。
#define GIZMO_ASSERT assert(renderer && "GizmoProxy is only valid inside OnDrawGizmos()")

void GizmoProxy::SetDepthTest(bool enabled) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::SetDepthTest(enabled);
}

void GizmoProxy::DrawCapsule(const math::Vector3& center, float radius, float halfHeight,
                             const math::Quaternion& rotation, const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::Capsule(*renderer, center, radius, halfHeight, rotation, color);
}

void GizmoProxy::DrawCone(const math::Vector3& apex, const math::Vector3& direction,
                          float height, float baseRadius, const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::Cone(*renderer, apex, direction, height, baseRadius, color);
}

void GizmoProxy::DrawCircle(const math::Vector3& center, const math::Vector3& normal, float radius,
                            const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::Circle(*renderer, center, normal, radius, color);
}

void GizmoProxy::DrawArc(const math::Vector3& center, const math::Vector3& normal,
                         const math::Vector3& fromDirection, float radius, float angleDegrees,
                         const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::Arc(*renderer, center, normal, fromDirection, radius,
                             math::ToRad(angleDegrees), color);
}

void GizmoProxy::DrawPolyline(std::span<const math::Vector3> points, bool closed,
                              const math::Vector4& color) const
{
    GIZMO_ASSERT;
    renderer::DebugDraw::Polyline(*renderer, points, closed, color);
}

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

/// @name ScriptNavigationProxy 実装
/// @note フリー関数は Script::m_gameObject (protected) へアクセスできない (friend 指定は
///       ScriptNavigationProxy のメンバー関数にのみ及ぶ)。既存の SelfComponent<T>() (public な
///       Script::GetComponent<T>() 経由) を再利用する。
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

float ScriptNavigationProxy::GetSpeed() const
{
    const auto* agent = SelfNavAgent(script);
    return agent ? agent->maxSpeed : 0.0f;
}

void ScriptNavigationProxy::SetAngularSpeed(float degPerSec) const
{
    if (auto* agent = SelfNavAgent(script)) agent->angularSpeedDeg = degPerSec >= 0.0f ? degPerSec : 0.0f;
}

float ScriptNavigationProxy::GetAngularSpeed() const
{
    const auto* agent = SelfNavAgent(script);
    return agent ? agent->angularSpeedDeg : 0.0f;
}

math::Vector3 ScriptNavigationProxy::GetDestination() const
{
    const auto* agent = SelfNavAgent(script);
    return agent && agent->hasDestination ? agent->destination : math::Vector3::ZERO;
}

float ScriptNavigationProxy::GetStoppingDistance() const
{
    const auto* agent = SelfNavAgent(script);
    return agent ? agent->stoppingDistance : 0.0f;
}

void ScriptNavigationProxy::SetStoppingDistance(float distance) const
{
    if (auto* agent = SelfNavAgent(script))
        agent->stoppingDistance = distance > 0.0f ? distance : 0.0f;
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

/// @name EntityRef 実装
GameObject* EntityRef::Resolve(const ScriptSceneProxy& scene) const
{
    return id.IsValid() ? scene.GetGameObject(id) : nullptr;
}

GameObject* EntityRef::Resolve(Scene& scene) const
{
    return id.IsValid() ? scene.GetGameObject(id) : nullptr;
}

/// @brief ScriptCharacterProxy
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
    /// @note RigidBody は SelfRigidBody() が script→Component→rigidBody と辿る。
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

bool ScriptCharacterProxy::IsEnabled() const
{
    const auto* cc = SelfCharacter(script);
    return cc && cc->enabled;
}

/// @brief ScriptMeshProxy
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
    auto* mr = script->m_gameObject->GetComponent<MeshRenderer>();
    if (!mr || mr->meshPath == path) return;
    mr->meshPath      = std::string(path);
    /// @note 引き直しには ResourceManager が要る。実際に差し替えるのは RuntimeMeshSystem。
    mr->meshPathDirty = true;
}

std::string ScriptMeshProxy::GetMeshPath() const
{
    if (!script || !script->m_gameObject) return {};
    const auto* mr = script->m_gameObject->GetComponent<MeshRenderer>();
    return mr ? mr->meshPath : std::string{};
}

void ScriptMeshProxy::SetModelPath(std::string_view path) const
{
    if (!script || !script->m_gameObject) return;
    if (auto* smr = script->m_gameObject->GetComponent<SkinnedMeshRenderer>())
        smr->modelPath = std::string(path);
}

std::string ScriptMeshProxy::GetModelPath() const
{
    if (!script || !script->m_gameObject) return {};
    const auto* smr = script->m_gameObject->GetComponent<SkinnedMeshRenderer>();
    return smr ? smr->modelPath : std::string{};
}

namespace {

ProceduralMeshComponent* EnsureProceduralMesh(GameObject& go)
{
    if (auto* existing = go.GetComponent<ProceduralMeshComponent>()) return existing;
    return &go.AddComponent<ProceduralMeshComponent>();
}

void ApplyProceduralMesh(GameObject& go, const MeshBuilder& builder)
{
    ProceduralMeshComponent* procedural = EnsureProceduralMesh(go);
    if (!procedural) return;
    /// @note 三角形の数が変われば当然、同数でも繋がり方は変わりうる。値で受けた以上、
    ///       何が変わったかは判別できないので常に全更新として扱う。
    procedural->builder = builder;
    procedural->dirty   = MeshDirty::All;
    procedural->enabled = true;
}

} // namespace

void ScriptMeshProxy::Apply(const MeshBuilder& builder) const
{
    if (!script || !script->m_gameObject) return;
    ApplyProceduralMesh(*script->m_gameObject, builder);
}

void ScriptMeshProxy::Apply(GameObject& target, const MeshBuilder& builder) const
{
    ApplyProceduralMesh(target, builder);
}

std::span<MeshVertex> ScriptMeshProxy::Vertices() const
{
    if (!script || !script->m_gameObject) return {};
    auto* procedural = script->m_gameObject->GetComponent<ProceduralMeshComponent>();
    if (!procedural) return {};
    return { procedural->builder.Vertices().data(), procedural->builder.Vertices().size() };
}

void ScriptMeshProxy::Touch(MeshDirty dirty) const
{
    if (!script || !script->m_gameObject) return;
    auto* procedural = script->m_gameObject->GetComponent<ProceduralMeshComponent>();
    if (!procedural || dirty == MeshDirty::None) return;
    /// @note 既に全更新が積まれているフレームで «頂点だけ» を要求されても、弱い方へは下げない。
    if (procedural->dirty != MeshDirty::All) procedural->dirty = dirty;
}

void ScriptMeshProxy::Clear() const
{
    if (!script || !script->m_gameObject) return;
    auto* procedural = script->m_gameObject->GetComponent<ProceduralMeshComponent>();
    if (!procedural) return;
    procedural->builder.Clear();
    procedural->enabled = false;
    procedural->dirty   = MeshDirty::All;
    if (auto* mr = script->m_gameObject->GetComponent<MeshRenderer>())
        mr->enabled = false;
}

void ScriptMeshProxy::SetProceduralMaterial(std::string_view path) const
{
    if (!script || !script->m_gameObject) return;
    ProceduralMeshComponent* procedural = EnsureProceduralMesh(*script->m_gameObject);
    if (!procedural) return;
    procedural->materialPath = std::string(path);
}

/// @brief ScriptIKProxy
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

bool ScriptIKProxy::IsChainEnabled(std::string_view targetName) const
{
    const auto* c = FindChainByTarget(SelfIK(script), targetName);
    return c && c->enabled;
}

bool ScriptIKProxy::HasChain(std::string_view targetName) const
{
    return FindChainByTarget(SelfIK(script), targetName) != nullptr;
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

bool ScriptIKProxy::IsEnabled() const
{
    const auto* ik = SelfIK(script);
    return ik && ik->enabled;
}

/// @brief ScriptWaterProxy
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
    return script->m_gameObject->transform.worldPosition.y + wc->GetSurfaceHeightAt(worldX, worldZ, time);
}

float ScriptWaterProxy::GetSurfaceHeightLocal(float localX, float localZ, float time) const
{
    auto* wc = SelfWater(script);
    if (!wc || !script->m_gameObject) return 0.0f;
    /// @note 波の位相はワールド XZ で決まる。ローカルのまま評価すると描画とずれる。
    const auto& t = script->m_gameObject->transform;
    const math::Vector3 world = t.worldPosition
        + t.worldRotation * math::Vector3{ localX * t.worldScale.x, 0.0f, localZ * t.worldScale.z };
    return wc->GetSurfaceHeightAt(world.x, world.z, time);
}

void ScriptWaterProxy::SetWaveAmplitudeScale(float scale) const
{
    if (auto* wc = SelfWater(script)) wc->waveAmplitudeScale = scale < 0.0f ? 0.0f : scale;
}

float ScriptWaterProxy::GetWaveAmplitudeScale() const
{
    const auto* wc = SelfWater(script);
    return wc ? wc->waveAmplitudeScale : 0.0f;
}

void ScriptWaterProxy::SetGerstnerEnabled(bool enabled) const
{
    if (auto* wc = SelfWater(script)) wc->enableGerstnerWaves = enabled;
}

math::Vector2 ScriptWaterProxy::GetCurrent() const
{
    const auto* wc = SelfWater(script);
    return wc ? wc->current : math::Vector2::ZERO;
}

void ScriptWaterProxy::AddRipple(const math::Vector3& worldPos, float strength) const
{
    auto* wc = SelfWater(script);
    if (!wc || !script->m_gameObject) return;
    EmitWaterRipple(*wc, script->m_gameObject->transform, worldPos, strength);
}

void ScriptWaterProxy::Splash(const math::Vector3& worldPos, float intensity) const
{
    const float clamped = intensity < 0.0f ? 0.0f : (intensity > 1.0f ? 1.0f : intensity);
    AddRipple(worldPos, 0.3f + 0.7f * clamped);
    if (SelfWater(script)) QueueWaterSplash(worldPos, clamped);
}

void ScriptWaterProxy::SetBuoyancyEnabled(bool enabled) const
{
    if (auto* wc = SelfWater(script)) wc->buoyancyEnabled = enabled;
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

int ScriptTerrainProxy::GetLayerCount() const
{
    auto* terrain = SelfTerrain(script);
    return terrain ? terrain->LayerCount() : 0;
}

float ScriptTerrainProxy::GetLayerWeightAtGrid(int x, int z, int layer) const
{
    auto* terrain = SelfTerrain(script);
    return terrain ? terrain->GetLayerWeightAtGrid(x, z, layer) : 0.0f;
}

bool ScriptTerrainProxy::SetHoleAtGrid(int cellX, int cellZ, bool hole) const
{
    auto* terrain = SelfTerrain(script);
    if (!terrain) return false;
    const bool wasHole = terrain->IsHoleCell(cellX, cellZ);
    if (!terrain->SetHoleCell(cellX, cellZ, hole)) return false;
    /// @note 変化の無い書き込みでメッシュとコライダーを作り直さない (毎フレーム呼ぶスクリプトを想定)。
    if (wasHole != hole) terrain->RequestHoleRebuild();
    return true;
}

void ScriptTerrainProxy::RequestRebuild() const
{
    if (auto* terrain = SelfTerrain(script)) {
        terrain->RequestHeightRebuild();
        terrain->RequestSplatRebuild();
        terrain->RequestMaterialRebuild();
    }
}

/// @brief ScriptEnvironmentProxy
namespace {
/// @brief シーン全体から最初のコンポーネントを検索する汎用ヘルパー。
/// @note EnvironmentLight/AtmosphericScattering/SkyRenderer は通常シーンに 1 つで、どの Script
///       からでもアクセスできるよう全探索する。enabled は見ない (SetEnabled(false) を呼ぶために
///       コンポーネント自体を見つける必要があるため)。
/// @note Script* でなく Scene* を受け取る: Script::m_scene は protected で free function から
///       直接アクセスできないため、呼び出し側 (friend の proxy メソッド) で取り出して渡す。
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
    /// @note 対象は大気散乱の明るさ。太陽ディスクは ScriptSunMoonProxy::SetSun が持つ。
    if (auto* c = FindFirstInScene<SkyRenderer>(script->m_scene)) c->skyScatterIntensity = intensity;
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
    return c ? c->skyScatterIntensity : 0.0f;
}
float ScriptEnvironmentProxy::GetMieScattering() const
{
    auto* c = FindFirstInScene<SkyRenderer>(script->m_scene);
    return c ? c->mieScattering : 0.0f;
}
float ScriptEnvironmentProxy::GetMieG() const
{
    auto* c = FindFirstInScene<SkyRenderer>(script->m_scene);
    return c ? c->mieG : 0.0f;
}

bool ScriptEnvironmentProxy::IsIBLEnabled() const
{
    auto* c = FindFirstInScene<EnvironmentLightComponent>(script->m_scene);
    return c && c->enabled;
}
float ScriptEnvironmentProxy::GetIBLIntensity() const
{
    auto* c = FindFirstInScene<EnvironmentLightComponent>(script->m_scene);
    return c ? c->intensity : 0.0f;
}
float ScriptEnvironmentProxy::GetIBLDiffuseScale() const
{
    auto* c = FindFirstInScene<EnvironmentLightComponent>(script->m_scene);
    return c ? c->diffuseScale : 0.0f;
}
float ScriptEnvironmentProxy::GetIBLSpecularScale() const
{
    auto* c = FindFirstInScene<EnvironmentLightComponent>(script->m_scene);
    return c ? c->specularScale : 0.0f;
}

math::Vector3 ScriptEnvironmentProxy::GetFogColor() const
{
    auto* c = FindFirstInScene<AtmosphericScatteringComponent>(script->m_scene);
    return c ? c->fogColor : math::Vector3::ZERO;
}
float ScriptEnvironmentProxy::GetFogFar() const
{
    auto* c = FindFirstInScene<AtmosphericScatteringComponent>(script->m_scene);
    return c ? c->fogFar : 0.0f;
}

/// @brief ScriptDecalProxy
namespace {
DecalComponent* SelfDecal(const Script* script)
{
    return SelfComponent<DecalComponent>(script);
}
} // namespace

EntityRef ScriptDecalProxy::Spawn(const math::Vector3& point,
                                  const math::Vector3& normal,
                                  std::string_view materialPath,
                                  float size,
                                  float lifetime,
                                  float depth,
                                  float rollDegrees) const
{
    if (!script || !script->m_scene) return EntityRef{};
    Scene& scene = *script->m_scene;

    /// @note 投影軸はデカールのローカル +Y。法線をそのまま +Y へ向けるので、床への着弾は
    ///       無回転になる (BossShockwave / DecalScorch が置いている床デカールと同じ規約)。
    const math::Vector3 axis = normal.NormalizedOr(math::Vector3::UP);

    /// @note ローカル +Z は投影 UV の V 軸。法線に直交していれば向きは何でもよいので、
    ///       軸に平行になりにくい 2 本から選ぶ (真上 / 真横の面で基底が潰れるのを避ける)。
    math::Vector3 forward = math::Vector3::Cross(axis, math::Vector3::RIGHT);
    if (forward.LengthSq() < 1.0e-6f)
        forward = math::Vector3::Cross(axis, math::Vector3::FORWARD);
    forward = forward.NormalizedOr(math::Vector3::FORWARD);

    math::Quaternion rotation =
        math::Quaternion::LookRotation(forward, axis).Normalized();
    /// @note roll はローカル +Y (= 投影軸) まわり。右から掛けてローカル回転として足す。
    if (rollDegrees != 0.0f) {
        rotation = (rotation * math::Quaternion::FromAxisAngle(
                        math::Vector3::UP, rollDegrees * math::DEG2RAD)).Normalized();
    }

    /// @note Create / AddComponent はシーンの配列を伸ばしうる。値は必ず ID から引き直した
    ///       個体へ入れる —— 作った直後の参照は、次の確保で無効になりうる。
    const EntityID id = [&] {
        GameObject& created = scene.CreateGameObject("Decal");
        created.runtimeGenerated = true;
        return created.GetID();
    }();
    scene.AddComponent<DecalComponent>(id, DecalComponent{});

    GameObject* spawned = scene.GetGameObject(id);
    if (!spawned) return EntityRef{};
    DecalComponent* decal = spawned->GetComponent<DecalComponent>();
    if (!decal) return EntityRef{};

    const float extent = (std::max)(size, 0.001f);
    /// @note worldPosition だけでは動かない。PrePhysics が local から組み直すため position も書く。
    spawned->transform.position      = point;
    spawned->transform.worldPosition = point;
    spawned->transform.rotation      = rotation;
    spawned->transform.worldRotation = rotation;
    spawned->transform.scale         = { extent, (std::max)(depth, 0.001f), extent };

    decal->materialPath = std::string(materialPath);
    decal->lifetime     = lifetime;

    return EntityRef{ id };
}

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
void ScriptDecalProxy::SetFadeInTime(float seconds) const
{
    if (auto* d = SelfDecal(script)) d->fadeInTime = (std::max)(seconds, 0.0f);
}
void ScriptDecalProxy::SetFlipbook(int frameCount, int framesPerRow,
                                   float frameRate, bool loop) const
{
    auto* d = SelfDecal(script);
    if (!d) return;
    d->frameCount   = (std::max)(frameCount, 0);
    d->framesPerRow = std::clamp(framesPerRow, 1, (std::max)(d->frameCount, 1));
    d->frameRate    = (std::max)(frameRate, 0.0f);
    d->frameLoop    = loop;
}
void ScriptDecalProxy::SetSortOrder(int order) const
{
    if (auto* d = SelfDecal(script)) d->sortOrder = order;
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
void ScriptDecalProxy::SetOpacity(float opacity) const
{
    if (auto* d = SelfDecal(script)) d->opacity = std::clamp(opacity, 0.0f, 1.0f);
}
bool ScriptDecalProxy::IsEnabled() const
{
    const auto* d = SelfDecal(script);
    return d && d->enabled;
}
float ScriptDecalProxy::GetOpacity() const
{
    const auto* d = SelfDecal(script);
    return d ? d->opacity : 0.0f;
}
float ScriptDecalProxy::GetLifetime() const
{
    const auto* d = SelfDecal(script);
    return d ? d->lifetime : -1.0f;
}
float ScriptDecalProxy::GetFadeTime() const
{
    const auto* d = SelfDecal(script);
    return d ? d->fadeTime : 0.0f;
}
float ScriptDecalProxy::GetAge() const
{
    const auto* d = SelfDecal(script);
    return d ? d->age : 0.0f;
}
float ScriptDecalProxy::GetEffectiveOpacity() const
{
    const auto* d = SelfDecal(script);
    if (!d) return 0.0f;
    /// @note DecalPass のライフタイムフェードと同じ式。片方だけ変えると
    ///       「見た目は消えているのに Script は 1.0 を読む」というズレになる。
    float fade = 1.0f;
    if (d->lifetime >= 0.0f && d->fadeTime > 0.0f)
        fade = (std::min)(1.0f, (d->lifetime - d->age) / d->fadeTime);
    if (d->fadeInTime > 0.0f)
        fade = (std::min)(fade, d->age / d->fadeInTime);
    return std::clamp(fade, 0.0f, 1.0f) * std::clamp(d->opacity, 0.0f, 1.0f);
}
void ScriptDecalProxy::SetAngleFade(float strength, float limitDegrees) const
{
    auto* d = SelfDecal(script);
    if (!d) return;
    d->angleFadeStrength = std::clamp(strength, 0.0f, 1.0f);
    d->angleFadeDegrees  = std::clamp(limitDegrees, 0.0f, 89.0f);
}
void ScriptDecalProxy::SetReceiverLayerMask(uint32_t mask) const
{
    if (auto* d = SelfDecal(script)) d->receiverLayerMask = mask;
}
void ScriptDecalProxy::SetMaterial(std::string_view materialPath) const
{
    auto* d = SelfDecal(script);
    if (!d) return;
    std::string next(materialPath);
    if (d->materialPath == next) return;
    d->materialPath = std::move(next);
    /// @note 別のマテリアルへ移ると、前のシェーダーに合わせた上書きは意味を失う。
    ///       残すと「効かない上書き」が黙って積まれ、綴り間違いと区別が付かなくなる。
    d->materialParamOverrides.clear();
    d->materialTextureOverrides.clear();
}

namespace {
/// @brief AssignUIMaterialOverride と同じ理由で assign を使う (毎フレーム呼ばれる Setter)。
void AssignDecalMaterialOverride(DecalComponent& decal, std::string_view param,
                                 const float* values, std::size_t count)
{
    auto& target = decal.materialParamOverrides[std::string(param)];
    target.assign(values, values + count);
}
} // namespace

void ScriptDecalProxy::SetMaterialFloat(std::string_view param, float value) const
{
    if (auto* d = SelfDecal(script)) AssignDecalMaterialOverride(*d, param, &value, 1);
}
void ScriptDecalProxy::SetMaterialVector2(std::string_view param, float x, float y) const
{
    const float values[2] = { x, y };
    if (auto* d = SelfDecal(script)) AssignDecalMaterialOverride(*d, param, values, 2);
}
void ScriptDecalProxy::SetMaterialVector4(std::string_view param, const math::Vector4& value) const
{
    const float values[4] = { value.x, value.y, value.z, value.w };
    if (auto* d = SelfDecal(script)) AssignDecalMaterialOverride(*d, param, values, 4);
}
void ScriptDecalProxy::SetMaterialColor(std::string_view param, const math::Vector4& color) const
{
    SetMaterialVector4(param, color);
}
void ScriptDecalProxy::SetMaterialTexture(std::string_view slot, std::string_view texturePath) const
{
    if (auto* d = SelfDecal(script))
        d->materialTextureOverrides[std::string(slot)].assign(texturePath.data(), texturePath.size());
}
void ScriptDecalProxy::ClearMaterialOverride(std::string_view param) const
{
    auto* d = SelfDecal(script);
    if (!d) return;
    const std::string key(param);
    d->materialParamOverrides.erase(key);
    d->materialTextureOverrides.erase(key);
}
void ScriptDecalProxy::ClearMaterialOverrides() const
{
    auto* d = SelfDecal(script);
    if (!d) return;
    d->materialParamOverrides.clear();
    d->materialTextureOverrides.clear();
}

/// @brief ScriptVolumeProxy
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
bool ScriptVolumeProxy::IsEnabled() const
{
    const auto* v = SelfVolume(script);
    return v && v->enabled;
}
float ScriptVolumeProxy::GetTimeScale() const
{
    const auto* v = SelfVolume(script);
    return v ? v->timeScale : 1.0f;
}
float ScriptVolumeProxy::GetDuration() const
{
    const auto* v = SelfVolume(script);
    return v ? v->duration : -1.0f;
}
float ScriptVolumeProxy::GetElapsed() const
{
    const auto* v = SelfVolume(script);
    return v ? v->elapsed : 0.0f;
}

/// @brief ScriptReflectionProbeProxy
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
bool ScriptReflectionProbeProxy::IsEnabled() const
{
    const auto* rp = SelfReflectionProbe(script);
    return rp && rp->enabled;
}
float ScriptReflectionProbeProxy::GetIntensity() const
{
    const auto* rp = SelfReflectionProbe(script);
    return rp ? rp->intensity : 0.0f;
}
float ScriptReflectionProbeProxy::GetInfluenceRadius() const
{
    const auto* rp = SelfReflectionProbe(script);
    return rp ? rp->influenceRadius : 0.0f;
}

namespace {
MotionWarpComponent* SelfMotionWarp(const Script* script)
{
    return SelfComponent<MotionWarpComponent>(script);
}
} // namespace

void ScriptMotionWarpProxy::WarpTo(const math::Vector3& position, float duration) const
{
    auto* warp = SelfMotionWarp(script);
    if (!warp || duration <= 0.0f) return;
    warp->target.active       = true;
    warp->target.position     = position;
    warp->target.warpPosition = true;
    warp->target.warpRotation = false;
    warp->target.duration     = duration;
    warp->target.remaining    = duration;
    ++warp->runtimeWarpCount;
}

void ScriptMotionWarpProxy::WarpToPose(const math::Vector3& position,
                                       const math::Quaternion& rotation,
                                       float duration) const
{
    auto* warp = SelfMotionWarp(script);
    if (!warp || duration <= 0.0f) return;
    warp->target.active       = true;
    warp->target.position     = position;
    warp->target.rotation     = rotation.Normalized();
    warp->target.warpPosition = true;
    warp->target.warpRotation = true;
    warp->target.duration     = duration;
    warp->target.remaining    = duration;
    ++warp->runtimeWarpCount;
}

void ScriptMotionWarpProxy::SetAxisWeight(const math::Vector3& weight) const
{
    if (auto* warp = SelfMotionWarp(script)) warp->target.positionAxisWeight = weight;
}

void ScriptMotionWarpProxy::SetMaxSpeed(float metersPerSecond) const
{
    if (auto* warp = SelfMotionWarp(script)) warp->target.maxSpeed = metersPerSecond;
}

void ScriptMotionWarpProxy::Cancel() const
{
    if (auto* warp = SelfMotionWarp(script)) {
        warp->target.active    = false;
        warp->target.remaining = 0.0f;
    }
}

void ScriptMotionWarpProxy::SetEnabled(bool enabled) const
{
    if (auto* warp = SelfMotionWarp(script)) warp->enabled = enabled;
}

bool ScriptMotionWarpProxy::IsWarping() const
{
    const auto* warp = SelfMotionWarp(script);
    return warp && warp->enabled && warp->target.active;
}

float ScriptMotionWarpProxy::GetRemainingTime() const
{
    const auto* warp = SelfMotionWarp(script);
    return warp && warp->target.active ? warp->target.remaining : 0.0f;
}

float ScriptMotionWarpProxy::GetRemainingDistance() const
{
    const auto* warp = SelfMotionWarp(script);
    return warp ? warp->runtimeRemainingDistance : 0.0f;
}

/// @brief ScriptLifetimeProxy
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
bool ScriptLifetimeProxy::IsEnabled() const
{
    const auto* lc = SelfLifetime(script);
    return lc && lc->enabled;
}
bool ScriptLifetimeProxy::HasLifetime() const
{
    return SelfLifetime(script) != nullptr;
}
void ScriptLifetimeProxy::Kill() const
{
    /// @note remaining = 0 にして LifetimeSystem に次フレームで GO を破棄させる。
    if (auto* lc = SelfLifetime(script)) lc->remaining = 0.0f;
}

/// @brief ScriptFlowFieldProxy
///
/// @brief 場は «1 本» から «リスト» になった。スクリプト API は 1 本を触る形のままで、
/// @brief 対象は **先頭の流れ**。
/// @note 先頭なのは、このプロキシを使うスクリプトが «この GameObject の流れ» を 1 つだと
///       思って書かれているため。黙って全部へ配ると、流れだけ強めたつもりで乱れまで動く。
namespace {

FlowFieldSettings* SelfPrimaryForce(const Script* script)
{
    auto* field = SelfComponent<FlowField>(script);
    if (field == nullptr || field->forces.empty()) return nullptr;
    return &field->forces.front();
}

} // namespace

void ScriptFlowFieldProxy::SetEnabled(bool enabled) const
{
    if (auto* force = SelfPrimaryForce(script)) force->enabled = enabled;
}
void ScriptFlowFieldProxy::SetType(ScriptFlowFieldType type) const
{
    if (auto* force = SelfPrimaryForce(script)) {
        /// @note LEGACY_DRAG (5) と Baked (6) はスクリプトからは選べない。前者は読み込み専用、
        ///       後者は 速度場 PNG のパスが要るのでコンポーネント側で組む。
        const int value = std::clamp(static_cast<int>(type), 0,
                                     static_cast<int>(FlowFieldType::Curl));
        force->fieldType = static_cast<FlowFieldType>(value);
    }
}
void ScriptFlowFieldProxy::SetSpeed(float speed) const
{
    if (auto* force = SelfPrimaryForce(script)) force->strength = speed;
}
void ScriptFlowFieldProxy::SetRadius(float radius, float falloffPower) const
{
    if (auto* force = SelfPrimaryForce(script)) {
        force->radius = radius;
        force->falloffPower = (std::max)(falloffPower, 0.01f);
    }
}
void ScriptFlowFieldProxy::SetDirection(const math::Vector3& direction) const
{
    if (auto* force = SelfPrimaryForce(script)) force->direction = direction;
}
void ScriptFlowFieldProxy::SetTurbulence(float frequency, float speed) const
{
    if (auto* force = SelfPrimaryForce(script)) {
        force->noiseFrequency = (std::max)(frequency, 0.0f);
        force->noiseSpeed = speed;
    }
}
void ScriptFlowFieldProxy::SetChannels(uint32_t channels) const
{
    /// @note チャンネルは «この場が誰に効くか» で、束ねた流れで分ける意味が無いので全部へ配る。
    if (auto* field = SelfComponent<FlowField>(script))
        for (auto& force : field->forces) force.channels = channels;
}
bool ScriptFlowFieldProxy::IsEnabled() const
{
    const auto* force = SelfPrimaryForce(script);
    return force && force->enabled;
}
float ScriptFlowFieldProxy::GetSpeed() const
{
    const auto* force = SelfPrimaryForce(script);
    return force ? force->strength : 0.0f;
}
float ScriptFlowFieldProxy::GetRadius() const
{
    const auto* force = SelfPrimaryForce(script);
    return force ? force->radius : 0.0f;
}
ScriptFlowFieldType ScriptFlowFieldProxy::GetType() const
{
    const auto* force = SelfPrimaryForce(script);
    if (force == nullptr || force->fieldType > FlowFieldType::Curl)
        return ScriptFlowFieldType::UNIFORM;
    return static_cast<ScriptFlowFieldType>(static_cast<int>(force->fieldType));
}
uint32_t ScriptFlowFieldProxy::GetChannels() const
{
    const auto* force = SelfPrimaryForce(script);
    return force ? force->channels : 0u;
}

/// @brief ScriptCloudProxy
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

/// @brief ScriptSunMoonProxy
void ScriptSunMoonProxy::SetEnabled(bool enabled) const
{
    if (auto* sunMoon = SelfComponent<SunMoonRenderer>(script)) sunMoon->enabled = enabled;
}
void ScriptSunMoonProxy::SetSun(bool enabled, float intensity) const
{
    if (auto* sunMoon = SelfComponent<SunMoonRenderer>(script)) {
        sunMoon->sunEnabled = enabled;
        sunMoon->sunDiskIntensity = (std::max)(intensity, 0.0f);
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

/// @brief ScriptPatrolProxy
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
bool ScriptPatrolProxy::IsEnabled() const
{
    const auto* patrol = SelfComponent<NavMeshPatrolComponent>(script);
    return patrol && patrol->enabled;
}
size_t ScriptPatrolProxy::GetWaypointCount() const
{
    const auto* patrol = SelfComponent<NavMeshPatrolComponent>(script);
    return patrol ? patrol->waypoints.size() : 0u;
}
bool ScriptPatrolProxy::GetWaypoint(size_t index, math::Vector3& position) const
{
    const auto* patrol = SelfComponent<NavMeshPatrolComponent>(script);
    if (!patrol || index >= patrol->waypoints.size()) return false;
    position = patrol->waypoints[index];
    return true;
}

/// @brief ScriptWindProxy
///
/// @brief 環境風はシーン設定 (SceneEnvironment) になった。スクリプトから見た «風» の語彙は
/// @brief そのままで、触る先だけを差し替えてある。
/// @note この GameObject に FlowField が付いているかは関係しない。環境風は場所を持たない。
/// @note m_scene に触れるのはプロキシ本体だけ (Script の friend)。自由関数に畳めないので
///       各メソッドで 1 行ずつ引く。

void ScriptWindProxy::SetEnabled(bool enabled) const
{
    /// @note 風という概念全体の on/off。乱れも一緒に切る。
    if (script && script->m_scene) script->m_scene->Environment().enabled = enabled;
}
void ScriptWindProxy::SetDirection(const math::Vector3& direction) const
{
    if (script && script->m_scene) script->m_scene->Environment().direction = direction;
}
void ScriptWindProxy::SetStrength(float strength) const
{
    /// @note 旧 API 名のまま。中身は流速 [m/s]。
    if (script && script->m_scene) script->m_scene->Environment().speed = strength;
}
void ScriptWindProxy::SetTurbulence(float turbulence) const
{
    if (script && script->m_scene)
        script->m_scene->Environment().turbulence = (std::max)(turbulence, 0.0f);
}
void ScriptWindProxy::SetPulseFrequency(float frequency) const
{
    /// @note 脈動の速さ = 乱流のノイズ時間スクロール速度 (旧 WindZone と同じ対応)。
    if (script && script->m_scene)
        script->m_scene->Environment().pulseFrequency = (std::max)(frequency, 0.0f);
}

bool ScriptGameplayProxy::SetSprite(std::string_view assetPath) const
{
    auto* component = SelfComponent<SpriteRendererComponent>(script);
    if (!component)
        return false;
    component->spritePath = std::string(assetPath);
    /// @note 署名を潰して PresentationSystem に組み直させる。
    component->runtimeMesh.signature = 0;
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

    /// @note 毎フレーム呼ばれる想定のため、点数が変わらない限り vector の再確保が起きない
    ///       assign で更新する。
    component->points.assign(points.begin(), points.end());
    component->space = worldSpace ? LineSpace::World : LineSpace::Local;
    /// @note runtimeSignature は points から毎フレーム計算されるため、ここで触る必要はない
    ///       (SpriteRenderer と違い、内容が変わればメッシュは自動で作り直される)。
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

/// @name ScriptStoreProxyBase (save / config)
/// @brief util::SaveStore へそのまま転送する。スクリプトに Engine 実装を include させないための層。
/// @brief save と config の違いは Store() が返すストアだけなので、実装はここ 1 つに閉じる。

namespace {

util::SaveStore& StoreOf(SaveStoreKind kind)
{
    auto& app = core::Application::Get();
    return kind == SaveStoreKind::Config ? app.GetConfigStore() : app.GetSaveStore();
}

} // namespace

void ScriptStoreProxyBase::SetPath(std::string_view path) const
{
    StoreOf(kind).SetPath(std::string(path));
}

std::string ScriptStoreProxyBase::GetPath() const
{
    return StoreOf(kind).GetPath();
}

bool ScriptStoreProxyBase::Write(std::string_view key, IScriptSerializable& object) const
{
    return StoreOf(kind).Write(key, object);
}

bool ScriptStoreProxyBase::Read(std::string_view key, IScriptSerializable& object) const
{
    return StoreOf(kind).Read(key, object);
}

void ScriptStoreProxyBase::SetBool(std::string_view key, bool value) const
{
    StoreOf(kind).SetBool(key, value);
}

void ScriptStoreProxyBase::SetInt(std::string_view key, int value) const
{
    StoreOf(kind).SetInt(key, value);
}

void ScriptStoreProxyBase::SetFloat(std::string_view key, float value) const
{
    StoreOf(kind).SetFloat(key, value);
}

void ScriptStoreProxyBase::SetString(std::string_view key, std::string_view value) const
{
    StoreOf(kind).SetString(key, value);
}

void ScriptStoreProxyBase::SetVector2(std::string_view key, const math::Vector2& value) const
{
    StoreOf(kind).SetVector2(key, value);
}

void ScriptStoreProxyBase::SetVector3(std::string_view key, const math::Vector3& value) const
{
    StoreOf(kind).SetVector3(key, value);
}

void ScriptStoreProxyBase::SetVector4(std::string_view key, const math::Vector4& value) const
{
    StoreOf(kind).SetVector4(key, value);
}

bool ScriptStoreProxyBase::GetBool(std::string_view key, bool defaultValue) const
{
    return StoreOf(kind).GetBool(key, defaultValue);
}

int ScriptStoreProxyBase::GetInt(std::string_view key, int defaultValue) const
{
    return StoreOf(kind).GetInt(key, defaultValue);
}

float ScriptStoreProxyBase::GetFloat(std::string_view key, float defaultValue) const
{
    return StoreOf(kind).GetFloat(key, defaultValue);
}

std::string ScriptStoreProxyBase::GetString(std::string_view key, std::string_view defaultValue) const
{
    return StoreOf(kind).GetString(key, defaultValue);
}

math::Vector2 ScriptStoreProxyBase::GetVector2(std::string_view key, const math::Vector2& defaultValue) const
{
    return StoreOf(kind).GetVector2(key, defaultValue);
}

math::Vector3 ScriptStoreProxyBase::GetVector3(std::string_view key, const math::Vector3& defaultValue) const
{
    return StoreOf(kind).GetVector3(key, defaultValue);
}

math::Vector4 ScriptStoreProxyBase::GetVector4(std::string_view key, const math::Vector4& defaultValue) const
{
    return StoreOf(kind).GetVector4(key, defaultValue);
}

bool ScriptStoreProxyBase::Has(std::string_view key) const
{
    return StoreOf(kind).Has(key);
}

void ScriptStoreProxyBase::Remove(std::string_view key) const
{
    StoreOf(kind).Remove(key);
}

void ScriptStoreProxyBase::Clear() const
{
    StoreOf(kind).Clear();
}

bool ScriptStoreProxyBase::Save() const
{
    return StoreOf(kind).Save();
}

bool ScriptStoreProxyBase::Load() const
{
    return StoreOf(kind).Load();
}

bool ScriptStoreProxyBase::IsDirty() const
{
    return StoreOf(kind).IsDirty();
}

/// @name ScriptDisplayProxy

namespace {

/// @brief Editor ホスト中に game 側が要求したフルスクリーン状態。窓へは反映せず、
///        IsFullscreen() の答えだけ一致させる。
/// @note 覚えておく理由: 握り潰すだけだと Option 画面のチェックが押した直後に跳ね返り、
///       «Editor では設定 UI が壊れている» ように見える。ゲーム側コードは Standalone と
///       1 行も変えずに済むのが望ましい。
/// @note 解像度側に同じ受け皿が無い理由: 読み戻す API の GetWidth()/GetHeight() は «実際に
///       描いている面の寸法» であり、要求値を返すと嘘になるため。
bool s_editorRequestedFullscreen = false;

bool IsEditorHostedWindow()
{
    return core::Application::Get().IsEditorHosted();
}

} // namespace

void ScriptDisplayProxy::SetFullscreen(bool enabled) const
{
    if (IsEditorHostedWindow()) {
        s_editorRequestedFullscreen = enabled;
        return;
    }
    core::Application::Get().GetWindow().SetWindowMode(
        enabled ? core::WindowMode::BorderlessFullscreen : core::WindowMode::Windowed);
}

bool ScriptDisplayProxy::IsFullscreen() const
{
    if (IsEditorHostedWindow()) return s_editorRequestedFullscreen;
    return core::Application::Get().GetWindow().GetWindowMode()
        == core::WindowMode::BorderlessFullscreen;
}

void ScriptDisplayProxy::SetResolution(uint32_t width, uint32_t height) const
{
    if (IsEditorHostedWindow()) return;
    core::Application::Get().GetWindow().SetClientSize(width, height);
}

uint32_t ScriptDisplayProxy::GetWidth() const
{
    return core::Application::Get().GetWindowWidth();
}

uint32_t ScriptDisplayProxy::GetHeight() const
{
    return core::Application::Get().GetWindowHeight();
}

DisplayResolution ScriptDisplayProxy::GetMonitorSize() const
{
    DisplayResolution size;
    core::Application::Get().GetWindow().GetMonitorSize(size.width, size.height);
    return size;
}

std::vector<DisplayResolution> ScriptDisplayProxy::EnumResolutions() const
{
    const auto modes = core::Application::Get().GetWindow().EnumerateResolutions();
    std::vector<DisplayResolution> out;
    out.reserve(modes.size());
    for (const auto& [width, height] : modes) out.push_back(DisplayResolution{ width, height });
    return out;
}

void ScriptDisplayProxy::SetVSync(bool enabled) const
{
    core::Application::Get().GetRenderer().SetVSync(enabled);
}

bool ScriptDisplayProxy::GetVSync() const
{
    return core::Application::Get().GetRenderer().GetVSync();
}

/// @name ScriptGraphicsProxy
/// @brief 触る先は「実行中の RenderSettings」。Editor では Play 中しか登録されないので、
/// @brief 編集中に呼ばれた場合は静かに何もしない (ProjectSettings.toml を汚さないため)。

namespace {

renderer::RenderSettings* ActiveRenderSettings()
{
    return core::Application::Get().GetActiveRenderSettings();
}

/// @brief getter 用。未登録でも既定値を返せるよう、読み取りは静的な既定インスタンスへ落とす。
const renderer::RenderSettings& RenderSettingsForRead()
{
    static const renderer::RenderSettings s_defaults{};
    const renderer::RenderSettings* active = ActiveRenderSettings();
    return active ? *active : s_defaults;
}

} // namespace

void ScriptGraphicsProxy::SetBrightness(float value) const
{
    if (auto* settings = ActiveRenderSettings())
        settings->userBrightness = std::clamp(value, 0.1f, 4.0f);
}

float ScriptGraphicsProxy::GetBrightness() const
{
    return RenderSettingsForRead().userBrightness;
}

void ScriptGraphicsProxy::SetBloomScale(float value) const
{
    if (auto* settings = ActiveRenderSettings())
        settings->userBloomScale = std::clamp(value, 0.0f, 2.0f);
}

float ScriptGraphicsProxy::GetBloomScale() const
{
    return RenderSettingsForRead().userBloomScale;
}

void ScriptGraphicsProxy::SetRenderScale(float scale) const
{
    if (auto* settings = ActiveRenderSettings()) {
        settings->renderScale =
            std::clamp(scale, renderer::kMinRenderScale, renderer::kMaxRenderScale);
    }
}

float ScriptGraphicsProxy::GetRenderScale() const
{
    return RenderSettingsForRead().renderScale;
}

uint32_t ScriptGraphicsProxy::GetRenderWidth() const
{
    uint32_t width = 0, height = 0;
    auto& app = core::Application::Get();
    renderer::ResolveRenderResolution(app.GetWindowWidth(), app.GetWindowHeight(),
                                      RenderSettingsForRead().renderScale, width, height);
    return width;
}

uint32_t ScriptGraphicsProxy::GetRenderHeight() const
{
    uint32_t width = 0, height = 0;
    auto& app = core::Application::Get();
    renderer::ResolveRenderResolution(app.GetWindowWidth(), app.GetWindowHeight(),
                                      RenderSettingsForRead().renderScale, width, height);
    return height;
}

void ScriptGraphicsProxy::SetQualityPreset(renderer::QualityPreset preset) const
{
    if (auto* settings = ActiveRenderSettings())
        renderer::ApplyQualityPreset(*settings, preset);
}

renderer::QualityPreset ScriptGraphicsProxy::GetQualityPreset() const
{
    return renderer::DetectQualityPreset(RenderSettingsForRead());
}

void ScriptGraphicsProxy::SetShadowsEnabled(bool enabled) const
{
    if (auto* settings = ActiveRenderSettings()) settings->shadowEnabled = enabled;
}

bool ScriptGraphicsProxy::GetShadowsEnabled() const
{
    return RenderSettingsForRead().shadowEnabled;
}

void ScriptGraphicsProxy::SetShadowResolution(uint32_t resolution) const
{
    if (auto* settings = ActiveRenderSettings())
        settings->shadow.mapResolution = std::clamp(resolution, 512u, 8192u);
}

uint32_t ScriptGraphicsProxy::GetShadowResolution() const
{
    return RenderSettingsForRead().shadow.mapResolution;
}

void ScriptGraphicsProxy::SetShadowCascades(int count) const
{
    if (auto* settings = ActiveRenderSettings())
        settings->shadow.cascadeCount = std::clamp(count, 1, 4);
}

int ScriptGraphicsProxy::GetShadowCascades() const
{
    return RenderSettingsForRead().shadow.cascadeCount;
}

void ScriptGraphicsProxy::SetSSR(bool enabled) const
{
    if (auto* settings = ActiveRenderSettings()) settings->ssr.enabled = enabled;
}

bool ScriptGraphicsProxy::GetSSR() const { return RenderSettingsForRead().ssr.enabled; }

void ScriptGraphicsProxy::SetGTAO(bool enabled) const
{
    auto* settings = ActiveRenderSettings();
    if (!settings) return;
    settings->gtao.enabled = enabled;
    /// @note GTAO と SSAO は同じスロット。立てた側を残すため、先に相手を落としてから正規化する。
    if (enabled) settings->postProcess.ambientOcclusion.enabled = false;
    (void)settings->NormalizeExclusivePipelineSlots();
}

bool ScriptGraphicsProxy::GetGTAO() const { return RenderSettingsForRead().gtao.enabled; }

void ScriptGraphicsProxy::SetSSAO(bool enabled) const
{
    auto* settings = ActiveRenderSettings();
    if (!settings) return;
    settings->postProcess.ambientOcclusion.enabled = enabled;
    if (enabled) settings->gtao.enabled = false;
    (void)settings->NormalizeExclusivePipelineSlots();
}

bool ScriptGraphicsProxy::GetSSAO() const
{
    return RenderSettingsForRead().postProcess.ambientOcclusion.enabled;
}

void ScriptGraphicsProxy::SetTAA(bool enabled) const
{
    auto* settings = ActiveRenderSettings();
    if (!settings) return;
    settings->taa.enabled = enabled;
    if (enabled) settings->postProcess.fxaaEnabled = false;
    (void)settings->NormalizeExclusivePipelineSlots();
}

bool ScriptGraphicsProxy::GetTAA() const { return RenderSettingsForRead().taa.enabled; }

void ScriptGraphicsProxy::SetFXAA(bool enabled) const
{
    auto* settings = ActiveRenderSettings();
    if (!settings) return;
    settings->postProcess.fxaaEnabled = enabled;
    if (enabled) settings->taa.enabled = false;
    (void)settings->NormalizeExclusivePipelineSlots();
}

bool ScriptGraphicsProxy::GetFXAA() const
{
    return RenderSettingsForRead().postProcess.fxaaEnabled;
}

void ScriptGraphicsProxy::SetBloom(bool enabled) const
{
    if (auto* settings = ActiveRenderSettings())
        settings->postProcess.bloom.enabled = enabled;
}

bool ScriptGraphicsProxy::GetBloom() const
{
    return RenderSettingsForRead().postProcess.bloom.enabled;
}

void ScriptGraphicsProxy::SetMotionBlur(bool enabled) const
{
    if (auto* settings = ActiveRenderSettings()) settings->motionBlur.enabled = enabled;
}

bool ScriptGraphicsProxy::GetMotionBlur() const
{
    return RenderSettingsForRead().motionBlur.enabled;
}

void ScriptGraphicsProxy::SetVolumetricLight(bool enabled) const
{
    if (auto* settings = ActiveRenderSettings()) settings->volumetricLight.enabled = enabled;
}

bool ScriptGraphicsProxy::GetVolumetricLight() const
{
    return RenderSettingsForRead().volumetricLight.enabled;
}

void ScriptGraphicsProxy::SetContactShadow(bool enabled) const
{
    if (auto* settings = ActiveRenderSettings()) settings->contactShadow.enabled = enabled;
}

bool ScriptGraphicsProxy::GetContactShadow() const
{
    return RenderSettingsForRead().contactShadow.enabled;
}

/// @name ScriptEventProxy
/// @brief Subscribe / Publish はテンプレートなのでヘッダ側。ここは非テンプレート分だけ。

void ScriptEventProxy::UnsubscribeAll() const
{
    ScriptEventBus::UnsubscribeOwner(script);
}

/// @name ScriptRandomProxy

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
    const math::Vector3 forward = axis.NormalizedOr(math::Vector3::FORWARD);
    if (maxAngleDeg <= 0.0f) return forward;

    /// @note 円錐内の一様サンプリング。cos を一様に引くことで、頂点付近に偏らせない。
    const float maxCos = std::cos(std::clamp(maxAngleDeg, 0.0f, 180.0f) * DEG_TO_RAD);
    const float cosTheta = util::Random::Range(maxCos, 1.0f);
    const float sinTheta = std::sqrt((std::max)(0.0f, 1.0f - cosTheta * cosTheta));
    const float phi = util::Random::Range(0.0f, 6.28318530718f);

    /// @note forward に直交する基底を作る。forward と平行になりにくい軸を選んで外積する。
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

/// @name ScriptTweenProxy

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

/// @brief Tween 1 ステップぶんの経過時間。Scaled / Unscaled の分岐をここへ集約する。
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
    /// @note 引数は値で受ける: コルーチンの引数は最初の中断で保存されるが、参照は呼び出し側の
    ///       一時オブジェクトを指したまま残り得るため必ずコピーで持つ。
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
    /// @note 端数で終値に届かないことがあるため、最後に必ず合わせる。
    owner->transform.position = target;
}

Coroutine ScriptTweenProxy::MoveBy(math::Vector3 delta, float duration,
                                   TweenEase ease, TweenClock clock) const
{
    Script* owner = script;
    if (!owner) co_return;

    /// @note 開始位置は呼ばれた «今» を基準にする。連続で呼べば相対移動として積み上がる。
    /// @note MoveTo を co_await せず展開するのは、Coroutine 自体が awaiter ではなく (待機命令は
    ///       WaitForSeconds 等のみ) 入れ子にできないため。
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
        /// @note Slerp を使う: 角速度が一定になり、イージング曲線の形がそのまま見た目の速度変化になる。
        ///       Lerp だと曲線に回転量の歪みが乗る。
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
    /// @note 揺れの向きは毎フレーム引き直すのではなく、位相を進めた正弦で決める。毎フレーム乱数だと
    ///       フレームレートで速さが変わるため、周波数で定義すれば何 fps でも同じ速さに見える。
    util::RandomStream rng{ static_cast<uint64_t>(
        static_cast<uint32_t>(amplitude * 1000.0f) + 1u) };
    const math::Vector3 axisA = rng.OnUnitSphere();
    const math::Vector3 axisB = rng.OnUnitSphere();

    float elapsed = 0.0f;
    while (elapsed < duration) {
        co_await WaitForFrames(1);
        elapsed += TweenStepDelta(owner, clock);

        const float normalized = std::clamp(elapsed / duration, 0.0f, 1.0f);
        /// @note 線形に収束させる
        const float decay = 1.0f - normalized;
        const float phase = elapsed * frequency;
        const math::Vector3 offset =
            axisA * (std::sin(phase) * amplitude * decay) +
            axisB * (std::cos(phase * 1.37f) * amplitude * decay);
        owner->transform.position = origin + offset;
    }
    owner->transform.position = origin;
}

/// @name ScriptSequenceProxy

namespace {

/// @brief Player は「演出の再生窓口」であって、事前に置いておくものとは限らない。
/// @brief sequence.Play(path) の 1 行で始められるよう、無ければその場で足す。
SequencePlayerComponent* EnsureSequencePlayer(GameObject* object)
{
    if (!object || !object->IsValid()) return nullptr;
    if (auto* player = object->GetComponent<SequencePlayerComponent>()) return player;
    return &object->AddComponent<SequencePlayerComponent>();
}

SequencePlayerComponent* SelfSequencePlayer(const Script* script, bool ensure)
{
    if (!script) return nullptr;
    /// @note m_gameObject は protected で、friend なのは Proxy 型だけ。free function から
    ///       直接は引けないため、公開されている scene プロキシ経由で取り出す。
    GameObject* object = script->scene.Self();
    if (!object || !object->IsValid()) return nullptr;
    if (auto* player = object->GetComponent<SequencePlayerComponent>()) return player;
    return ensure ? EnsureSequencePlayer(object) : nullptr;
}

const asset::SequenceAsset* ResolveSequenceAsset(const SequencePlayerComponent& player)
{
    /// @note SequenceSystem と同じ順序で引く。エディタが編集中なら尺もそちらのもの。
    if (player.authoringSequence) return player.authoringSequence.get();
    if (player.sequencePath.empty()) return nullptr;
    const auto handle = asset::AssetManager::Load<asset::SequenceAsset>(player.sequencePath);
    return asset::AssetManager::Get<asset::SequenceAsset>(handle);
}

void BindSequenceKey(SequencePlayerComponent* player,
                     std::string_view key,
                     GameObject* target)
{
    if (!player || key.empty()) return;
    player->Bind(key, EntityRef{ target && target->IsValid() ? target->GetID()
                                                             : EntityID::INVALID });
}

} // namespace

void ScriptSequenceProxy::Play(std::string_view path) const
{
    auto* player = SelfSequencePlayer(script, true);
    if (!player) return;
    if (!path.empty()) player->sequencePath = std::string(path);
    player->Play();
}

void ScriptSequenceProxy::Play() const
{
    if (auto* player = SelfSequencePlayer(script, true)) player->Play();
}

void ScriptSequenceProxy::Stop() const
{
    if (auto* player = SelfSequencePlayer(script, false)) player->Stop();
}

void ScriptSequenceProxy::Pause() const
{
    if (auto* player = SelfSequencePlayer(script, false)) player->Pause();
}

void ScriptSequenceProxy::Resume() const
{
    if (auto* player = SelfSequencePlayer(script, false)) player->Resume();
}

void ScriptSequenceProxy::SetTime(float seconds) const
{
    if (auto* player = SelfSequencePlayer(script, false))
        player->SetTime(static_cast<double>(seconds));
}

void ScriptSequenceProxy::SetSpeed(float speed) const
{
    if (auto* player = SelfSequencePlayer(script, false)) player->speed = speed;
}

void ScriptSequenceProxy::Bind(std::string_view key, GameObject* target) const
{
    BindSequenceKey(SelfSequencePlayer(script, true), key, target);
}

void ScriptSequenceProxy::ClearBindings() const
{
    if (auto* player = SelfSequencePlayer(script, false)) player->bindings.clear();
}

bool ScriptSequenceProxy::IsPlaying() const
{
    const auto* player = SelfSequencePlayer(script, false);
    return player && player->playing;
}

float ScriptSequenceProxy::Time() const
{
    const auto* player = SelfSequencePlayer(script, false);
    return player ? static_cast<float>(player->time) : 0.0f;
}

float ScriptSequenceProxy::Duration() const
{
    const auto* player = SelfSequencePlayer(script, false);
    if (!player) return 0.0f;
    const asset::SequenceAsset* sequence = ResolveSequenceAsset(*player);
    return sequence ? static_cast<float>(sequence->GetDurationSeconds()) : 0.0f;
}

void ScriptSequenceProxy::PlayOn(GameObject& owner, std::string_view path) const
{
    auto* player = EnsureSequencePlayer(&owner);
    if (!player) return;
    if (!path.empty()) player->sequencePath = std::string(path);
    player->Play();
}

void ScriptSequenceProxy::StopOn(GameObject& owner) const
{
    if (!owner.IsValid()) return;
    if (auto* player = owner.GetComponent<SequencePlayerComponent>()) player->Stop();
}

void ScriptSequenceProxy::BindOn(GameObject& owner,
                                 std::string_view key,
                                 GameObject* target) const
{
    BindSequenceKey(EnsureSequencePlayer(&owner), key, target);
}

/// @brief ScriptObjectMaskProxy
/// @brief 触る先は ScriptGraphicsProxy と同じ「実行中の RenderSettings」。編集中は
/// @brief 登録されていないので、FBZZ_EXECUTE_ALWAYS のスクリプトから呼んでも何も起きない。
namespace {

[[nodiscard]] bool SameEntity(const renderer::RenderSelectionID& lhs, const EntityID& rhs)
{
    return lhs.index == rhs.index && lhs.generation == rhs.generation;
}

void SubmitObjectMaskRequest(GameObject* target, const math::Vector4& color,
                             float value, bool includeChildren, bool visibleOnly)
{
    if (!target || !target->IsValid()) return;
    auto* settings = ActiveRenderSettings();
    if (!settings) {
        /// @note 申告先が無い。Play 中は EditorApp が、配布ビルドは StandaloneProjectModule が
        ///       差す。ここが null だと «Set は呼べているのに何も起きない» になり、
        ///       呼んだ側からは成功と区別が付かない。
        static bool sWarned = false;
        if (!sWarned) {
            sWarned = true;
            FBZZ_LOG_WARN("ObjectMask: 申告先の RenderSettings がありません "
                          "(Application::SetActiveRenderSettings が呼ばれていない)。"
                          "この間のマスク申告はすべて捨てられます");
        }
        return;
    }

    const EntityID id = target->GetID();

    /// @note 同じ対象の行があれば上書きし、無ければ «前のフレームで途絶えた行» を再利用する。
    ///       拾わずに追加し続けると、一度でも印された対象の数だけ配列が伸びたままになる。
    renderer::RenderObjectMaskRequest* slot = nullptr;
    for (renderer::RenderObjectMaskRequest& request : settings->objectMaskRequests) {
        if (SameEntity(request.id, id)) {
            slot = &request;
            break;
        }
        if (!slot && !renderer::IsObjectMaskRequestLive(request, Time::frameCount)) slot = &request;
    }
    if (!slot) slot = &settings->objectMaskRequests.emplace_back();

    slot->id              = { id.index, id.generation };
    slot->color[0]        = color.x;
    slot->color[1]        = color.y;
    slot->color[2]        = color.z;
    slot->color[3]        = color.w;
    slot->value           = std::clamp(value, 0.0f, 1.0f);
    slot->includeChildren = includeChildren;
    slot->visibleOnly     = visibleOnly;
    slot->frame           = Time::frameCount;
}

void WithdrawObjectMaskRequest(GameObject* target)
{
    if (!target) return;
    auto* settings = ActiveRenderSettings();
    if (!settings) return;

    const EntityID id = target->GetID();
    std::erase_if(settings->objectMaskRequests,
                  [&id](const renderer::RenderObjectMaskRequest& request) {
                      return SameEntity(request.id, id);
                  });
}

} // namespace

void ScriptObjectMaskProxy::Set(const math::Vector4& color, float value) const
{
    if (script) SubmitObjectMaskRequest(script->m_gameObject, color, value, true, true);
}

void ScriptObjectMaskProxy::Set(GameObject& target, const math::Vector4& color, float value,
                                bool includeChildren, bool visibleOnly) const
{
    SubmitObjectMaskRequest(&target, color, value, includeChildren, visibleOnly);
}

void ScriptObjectMaskProxy::Clear() const
{
    if (script) WithdrawObjectMaskRequest(script->m_gameObject);
}

void ScriptObjectMaskProxy::Clear(GameObject& target) const
{
    WithdrawObjectMaskRequest(&target);
}

/// @brief ScriptSpringBoneProxy
namespace {
SpringBoneChain* FindSpringChain(const Script* script, std::string_view rootBoneName)
{
    auto* spring = SelfComponent<SpringBoneComponent>(script);
    if (!spring) return nullptr;
    for (SpringBoneChain& chain : spring->chains)
        if (chain.rootBoneName == rootBoneName) return &chain;
    return nullptr;
}
} // namespace

void ScriptSpringBoneProxy::EnsureChain(std::string_view rootBoneName, int maxDepth) const
{
    if (!script || !script->m_gameObject || rootBoneName.empty()) return;

    auto* spring = script->m_gameObject->GetComponent<SpringBoneComponent>();
    if (!spring) spring = &script->m_gameObject->AddComponent<SpringBoneComponent>();

    for (SpringBoneChain& chain : spring->chains) {
        if (chain.rootBoneName != rootBoneName) continue;
        /// @note 段数が変わったら組み直させる。nodes を空にするのが «組み直せ» の合図。
        if (chain.maxDepth != maxDepth) {
            chain.maxDepth = maxDepth;
            chain.nodes.clear();
        }
        return;
    }

    SpringBoneChain chain{};
    chain.rootBoneName = std::string(rootBoneName);
    chain.maxDepth     = maxDepth;
    /// @note 既定は «動かない»。Script が作るチェーンは、使う瞬間に weight を上げて使う。
    chain.weight       = 0.0f;
    chain.gravityPower = 0.0f;
    spring->chains.push_back(std::move(chain));
}

bool ScriptSpringBoneProxy::HasChain(std::string_view rootBoneName) const
{
    return FindSpringChain(script, rootBoneName) != nullptr;
}

void ScriptSpringBoneProxy::SetChainEnabled(std::string_view rootBoneName, bool enabled) const
{
    if (auto* chain = FindSpringChain(script, rootBoneName)) chain->enabled = enabled;
}

bool ScriptSpringBoneProxy::IsChainEnabled(std::string_view rootBoneName) const
{
    const auto* chain = FindSpringChain(script, rootBoneName);
    return chain && chain->enabled;
}

void ScriptSpringBoneProxy::SetWeight(std::string_view rootBoneName, float weight) const
{
    if (auto* chain = FindSpringChain(script, rootBoneName))
        chain->weight = math::Clamp01(weight);
}

float ScriptSpringBoneProxy::GetWeight(std::string_view rootBoneName) const
{
    const auto* chain = FindSpringChain(script, rootBoneName);
    return chain ? chain->weight : 0.0f;
}

void ScriptSpringBoneProxy::SetSpring(std::string_view rootBoneName,
                                      float stiffness, float damping) const
{
    if (auto* chain = FindSpringChain(script, rootBoneName)) {
        chain->stiffness = math::Clamp01(stiffness);
        chain->damping   = math::Clamp01(damping);
    }
}

void ScriptSpringBoneProxy::SetLimitAngle(std::string_view rootBoneName, float degrees) const
{
    if (auto* chain = FindSpringChain(script, rootBoneName))
        chain->limitAngle = math::Clamp(degrees, 0.0f, 180.0f);
}

void ScriptSpringBoneProxy::SetRadius(std::string_view rootBoneName, float radius) const
{
    if (auto* chain = FindSpringChain(script, rootBoneName))
        chain->radius = radius < 0.0f ? 0.0f : radius;
}

void ScriptSpringBoneProxy::SetForce(std::string_view rootBoneName,
                                     const math::Vector3& direction,
                                     float power) const
{
    auto* chain = FindSpringChain(script, rootBoneName);
    if (!chain) return;
    chain->gravityDirection = direction;
    chain->gravityPower     = power;
}

void ScriptSpringBoneProxy::ClearForce(std::string_view rootBoneName) const
{
    if (auto* chain = FindSpringChain(script, rootBoneName)) chain->gravityPower = 0.0f;
}

void ScriptSpringBoneProxy::ResetAll() const
{
    /// @note hasLastOwnerPosition を落とすと、次の更新で全チェーンが静止姿勢へ戻る
    ///       (SpringBoneSystem のテレポート復帰と同じ経路)。
    if (auto* spring = SelfComponent<SpringBoneComponent>(script))
        spring->hasLastOwnerPosition = false;
}

void ScriptSpringBoneProxy::SetEnabled(bool enabled) const
{
    if (auto* spring = SelfComponent<SpringBoneComponent>(script)) spring->enabled = enabled;
}

bool ScriptSpringBoneProxy::IsEnabled() const
{
    const auto* spring = SelfComponent<SpringBoneComponent>(script);
    return spring && spring->enabled;
}

/// @brief ScriptRagdollProxy
namespace {
RagdollComponent* EnsureRagdoll(const Script* script)
{
    if (!script) return nullptr;
    /// @note m_gameObject は protected で、friend なのは Proxy 型だけ。free function から
    ///       直接は引けないため、公開されている scene プロキシ経由で取り出す。
    GameObject* object = script->scene.Self();
    if (!object || !object->IsValid()) return nullptr;
    if (auto* ragdoll = object->GetComponent<RagdollComponent>()) return ragdoll;
    return &object->AddComponent<RagdollComponent>();
}
} // namespace

void ScriptRagdollProxy::Begin(float holdSeconds, float maxWeight, float gravityScale) const
{
    auto* ragdoll = EnsureRagdoll(script);
    if (!ragdoll) return;
    ragdoll->beginRequested  = true;
    ragdoll->activeRequested = false;
    ragdoll->endRequested    = false;
    ragdoll->holdRemaining   = holdSeconds > 0.0f ? holdSeconds : 0.0f;
    ragdoll->activationWeight  = math::Clamp01(maxWeight);
    ragdoll->activationGravity = gravityScale < 0.0f ? 0.0f : gravityScale;
}

void ScriptRagdollProxy::SetStandingGuard(bool enabled, float maxDistance, float maxDegrees) const
{
    auto* ragdoll = EnsureRagdoll(script);
    if (!ragdoll) return;
    ragdoll->standingGuard = enabled;
    ragdoll->standingMaxDistance = std::max(maxDistance, 0.0f);
    ragdoll->standingMaxDegrees = std::clamp(maxDegrees, 0.0f, 180.0f);
}

void ScriptRagdollProxy::SetProfile(ScriptRagdollProfile profile) const
{
    if (auto* ragdoll = EnsureRagdoll(script))
        ragdoll->profile = profile == ScriptRagdollProfile::Humanoid
            ? RagdollProfileKind::Humanoid : RagdollProfileKind::Mech;
}

void ScriptRagdollProxy::SetExcludedBranches(const std::vector<std::string>& rootBoneNames) const
{
    if (auto* ragdoll = EnsureRagdoll(script)) ragdoll->excludedRootBones = rootBoneNames;
}

void ScriptRagdollProxy::SetRootAnchor(float scale, float sag, float tiltDegrees) const
{
    if (auto* ragdoll = EnsureRagdoll(script)) {
        ragdoll->rootAnchor = std::max(scale, 0.0f);
        ragdoll->rootAnchorSag = std::max(sag, 0.0f);
        ragdoll->rootAnchorTilt = std::max(tiltDegrees, 0.0f) * math::PI / 180.0f;
    }
}

void ScriptRagdollProxy::SetContacts(bool world, bool dynamic, bool self, bool whileActive) const
{
    if (auto* ragdoll = EnsureRagdoll(script)) {
        ragdoll->contactWorld = world;
        ragdoll->contactDynamic = dynamic;
        ragdoll->contactSelf = self;
        ragdoll->contactWhileActive = whileActive;
    }
}

void ScriptRagdollProxy::SetGroundPlane(bool enabled, float offset) const
{
    if (auto* ragdoll = EnsureRagdoll(script)) {
        ragdoll->groundPlane = enabled;
        ragdoll->groundOffset = offset;
    }
}

void ScriptRagdollProxy::BeginActive(float holdSeconds) const
{
    auto* ragdoll = EnsureRagdoll(script);
    if (!ragdoll) return;
    /// @note 既に立って走っているなら捕獲し直さない。目標はどのみち毎フレーム取り直すので
    ///       呼び直す意味が無く、Begin すると押されて沈んでいた勢いだけが消える。
    if (ragdoll->IsStanding()) {
        ragdoll->endRequested  = false;
        ragdoll->holdRemaining = holdSeconds > 0.0f ? holdSeconds : 0.0f;
        /// @note 戻りかけの適用率を始点にして、連撃でも姿勢が跳ねないようにする。
        if (ragdoll->phase == RagdollPhase::BlendOut) {
            ragdoll->phaseStartWeight = math::Clamp01(ragdoll->weight);
            ragdoll->phase      = RagdollPhase::BlendIn;
            ragdoll->phaseTimer = 0.0f;
        }
        return;
    }
    ragdoll->beginRequested  = true;
    ragdoll->activeRequested = true;
    ragdoll->endRequested    = false;
    ragdoll->holdRemaining   = holdSeconds > 0.0f ? holdSeconds : 0.0f;
    /// @note 立っている絵はクリップと一致するので、薄く乗せる理由が無い。
    ragdoll->activationWeight  = 1.0f;
    ragdoll->activationGravity = 1.0f;
}

void ScriptRagdollProxy::End() const
{
    auto* ragdoll = SelfComponent<RagdollComponent>(script);
    if (!ragdoll) return;
    ragdoll->beginRequested = false;
    ragdoll->activeRequested = false;
    ragdoll->startTriggered = true;
    if (ragdoll->phase == RagdollPhase::Idle) {
        ragdoll->pendingImpulses.clear();
        return;
    }
    ragdoll->endRequested  = true;
    ragdoll->holdRemaining = 0.0f;
}

void ScriptRagdollProxy::SetRoots(const std::vector<std::string>& rootBoneNames,
                                  int maxDepth) const
{
    auto* ragdoll = EnsureRagdoll(script);
    if (!ragdoll) return;

    /// @note 先頭を rootBoneName、残りを extraRootBones へ分ける。呼ぶ側にこの非対称を
    ///       見せないための口なので、ここで畳む。空なら «骨格の根から» (＝全身) に戻る。
    ragdoll->rootBoneName = rootBoneNames.empty() ? std::string{} : rootBoneNames.front();
    ragdoll->extraRootBones.clear();
    for (std::size_t i = 1; i < rootBoneNames.size(); ++i)
        ragdoll->extraRootBones.push_back(rootBoneNames[i]);
    ragdoll->maxDepth = maxDepth;
}

int ScriptRagdollProxy::GetRootCount() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    if (!ragdoll) return 0;
    if (ragdoll->rootBoneName.empty() && ragdoll->extraRootBones.empty()) return 0;
    return 1 + static_cast<int>(ragdoll->extraRootBones.size());
}

void ScriptRagdollProxy::Push(const math::Vector3& velocity) const
{
    auto* ragdoll = SelfComponent<RagdollComponent>(script);
    if (!ragdoll) return;
    ragdoll->pendingImpulses.push_back(RagdollImpulse{ math::Vector3::ZERO, velocity, 0.0f });
}

void ScriptRagdollProxy::PushAt(const math::Vector3& origin,
                                const math::Vector3& velocity,
                                float radius) const
{
    auto* ragdoll = SelfComponent<RagdollComponent>(script);
    if (!ragdoll) return;
    ragdoll->pendingImpulses.push_back(
        RagdollImpulse{ origin, velocity, radius < 0.0f ? 0.0f : radius });
}

void ScriptRagdollProxy::PushAngular(const math::Vector3& angularVelocity) const
{
    PushAngularAt(math::Vector3::ZERO, angularVelocity, 0.0f);
}

void ScriptRagdollProxy::PushAngularAt(const math::Vector3& origin,
                                      const math::Vector3& angularVelocity, float radius) const
{
    if (auto* ragdoll = SelfComponent<RagdollComponent>(script))
        ragdoll->pendingImpulses.push_back(RagdollImpulse{
            origin, math::Vector3::ZERO, std::max(radius, 0.0f), angularVelocity});
}

bool ScriptRagdollProxy::IsActive() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    return ragdoll && ragdoll->IsActive();
}

bool ScriptRagdollProxy::IsStanding() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    return ragdoll && ragdoll->IsStanding();
}

float ScriptRagdollProxy::GetWeight() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    return ragdoll ? ragdoll->weight : 0.0f;
}

float ScriptRagdollProxy::GetDeviation() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    return ragdoll ? ragdoll->runtimeDeviation : 0.0f;
}

const char* ScriptRagdollProxy::GetStatus() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    /// @note «コンポーネントがそもそも無い» は «止まっている» と区別が付かないと調べようがない。
    if (!ragdoll) return "NoComponent";
    switch (ragdoll->runtimeStatus) {
    case RagdollStatus::Idle:          return "Idle";
    case RagdollStatus::Disabled:      return "Disabled";
    case RagdollStatus::NotPlaying:    return "NotPlaying";
    case RagdollStatus::NoAnimator:    return "NoAnimator";
    case RagdollStatus::NoSkinnedMesh: return "NoSkinnedMesh";
    case RagdollStatus::NoParticles:   return "NoParticles";
    case RagdollStatus::NoBones:       return "NoBones";
    case RagdollStatus::Running:       return "Running";
    }
    return "Unknown";
}

int ScriptRagdollProxy::GetParticleCount() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    return ragdoll ? ragdoll->runtimeBodyCount : 0;
}

int ScriptRagdollProxy::GetSaturatedJointCount() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    return ragdoll ? ragdoll->runtimeSaturated : 0;
}

int ScriptRagdollProxy::GetLimitedJointCount() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    return ragdoll ? ragdoll->runtimeLimited : 0;
}

int ScriptRagdollProxy::GetContactCount() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    return ragdoll ? ragdoll->runtimeContacts : 0;
}

int ScriptRagdollProxy::GetUnmatchedBoneCount() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    return ragdoll ? ragdoll->runtimeUnmatched : 0;
}

void ScriptRagdollProxy::SetRoot(const char* rootBoneName, int maxDepth) const
{
    auto* ragdoll = EnsureRagdoll(script);
    if (!ragdoll) return;
    const std::string next = rootBoneName ? rootBoneName : "";
    if (ragdoll->rootBoneName == next && ragdoll->maxDepth == maxDepth) return;
    /// @note 骨の並びが変わるので剛体も組み直しになるが、ここで壊す必要はない。
    ///       RagdollSystem が «組んだときの root / maxDepth» と突き合わせて自分で組み直す。
    ragdoll->rootBoneName = next;
    ragdoll->maxDepth     = maxDepth;
}

void ScriptRagdollProxy::SetGravity(float gravity) const
{
    if (auto* ragdoll = EnsureRagdoll(script))
        ragdoll->gravity = gravity < 0.0f ? 0.0f : gravity;
}

void ScriptRagdollProxy::SetBlend(float blendIn, float blendOut) const
{
    auto* ragdoll = EnsureRagdoll(script);
    if (!ragdoll) return;
    ragdoll->blendIn  = blendIn  < 0.0f ? 0.0f : blendIn;
    ragdoll->blendOut = blendOut < 0.0f ? 0.0f : blendOut;
}

void ScriptRagdollProxy::SetMuscle(float stiffness, float falloff, float damping) const
{
    auto* ragdoll = EnsureRagdoll(script);
    if (!ragdoll) return;
    /// @note どれもプロファイルが与えた値への «倍率»。1 でプロファイルどおり。
    ragdoll->driveScale   = stiffness < 0.0f ? 0.0f : stiffness;
    ragdoll->driveFalloff = math::Clamp01(falloff);
    ragdoll->driveDamping = damping < 0.0f ? 0.0f : damping;
}

void ScriptRagdollProxy::SetRecovery(float impactSlack, float recoverySeconds) const
{
    auto* ragdoll = EnsureRagdoll(script);
    if (!ragdoll) return;
    ragdoll->impactSlack     = math::Clamp01(impactSlack);
    ragdoll->recoverySeconds = recoverySeconds < 0.0f ? 0.0f : recoverySeconds;
}

void ScriptRagdollProxy::SetCollapse(float distance) const
{
    if (auto* ragdoll = EnsureRagdoll(script))
        ragdoll->collapseDistance = distance < 0.0f ? 0.0f : distance;
}

void ScriptRagdollProxy::SetEnabled(bool enabled) const
{
    if (auto* ragdoll = EnsureRagdoll(script)) ragdoll->enabled = enabled;
}

bool ScriptRagdollProxy::IsEnabled() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    return ragdoll && ragdoll->enabled;
}

/// @brief ScriptJointProxy
namespace {

JointComponent* EnsureJointComponent(const Script* script)
{
    if (script == nullptr) return nullptr;
    if (auto* joint = SelfComponent<JointComponent>(script)) return joint;
    GameObject* self = script->scene.Self();
    if (self == nullptr || !self->IsValid()) return nullptr;
    return &self->AddComponent<JointComponent>();
}

/// @brief Connect*() 共通の下ごしらえ。種別と相手を差し替え、残りは呼び出し側が詰める。
/// @brief target が null / 無効なら関節は作らず nullptr を返す («黙って自分だけ» にしない)。
JointComponent* BeginJointConnect(const Script* script, JointType type, GameObject* target)
{
    if (!target || !target->IsValid()) return nullptr;
    JointComponent* joint = EnsureJointComponent(script);
    if (!joint) return nullptr;
    joint->enabled         = true;
    joint->type            = type;
    joint->connectedBody.id = target->GetID();
    /// @note 相手を名指しした以上、祖先へのフォールバックは切る。
    ///       残したままだと、相手が消えた瞬間に «別のものへ勝手に繋ぎ変わる»。
    joint->connectToParent = false;
    joint->chainBodies.clear();
    return joint;
}

/// @brief 距離は «0 以下なら張った瞬間の間隔» という 1 つの規約で通す。
void SetAuthoredJointDistance(JointComponent& joint, float distance)
{
    joint.autoDistance = distance <= 0.0f;
    joint.distance     = joint.autoDistance ? 0.0f : distance;
}

} // namespace

void ScriptJointProxy::ConnectFixed(GameObject* target) const
{
    BeginJointConnect(script, JointType::Fixed, target);
}

void ScriptJointProxy::ConnectDistance(GameObject* target, float distance) const
{
    if (auto* joint = BeginJointConnect(script, JointType::Distance, target))
        SetAuthoredJointDistance(*joint, distance);
}

void ScriptJointProxy::ConnectRope(GameObject* target, float maxLength) const
{
    if (auto* joint = BeginJointConnect(script, JointType::Rope, target))
        SetAuthoredJointDistance(*joint, maxLength);
}

void ScriptJointProxy::ConnectSpring(GameObject* target,
                                     float restLength,
                                     float stiffness,
                                     float damping) const
{
    auto* joint = BeginJointConnect(script, JointType::Spring, target);
    if (!joint) return;
    SetAuthoredJointDistance(*joint, restLength);
    joint->spring  = stiffness < 0.0f ? 0.0f : stiffness;
    joint->damping = damping < 0.0f ? 0.0f : damping;
}

void ScriptJointProxy::ConnectHinge(GameObject* target,
                                    const math::Vector3& axis,
                                    const math::Vector3& anchor,
                                    const math::Vector3& connectedAnchor) const
{
    auto* joint = BeginJointConnect(script, JointType::Hinge, target);
    if (!joint) return;
    joint->axis            = axis;
    joint->anchor          = anchor;
    joint->connectedAnchor = connectedAnchor;
}

void ScriptJointProxy::ConnectSlider(GameObject* target, const math::Vector3& axis) const
{
    if (auto* joint = BeginJointConnect(script, JointType::Slider, target))
        joint->axis = axis;
}

void ScriptJointProxy::ConnectChain(const std::vector<GameObject*>& following,
                                    float segmentLength,
                                    int iterations) const
{
    JointComponent* joint = EnsureJointComponent(script);
    if (!joint) return;
    joint->enabled = true;
    joint->type    = JointType::Chain;
    joint->connectedBody.id = EntityID::INVALID;
    joint->chainBodies.clear();
    joint->chainBodies.reserve(following.size());
    for (GameObject* go : following) {
        /// @note 1 つでも欠けると «どこから先が繋がっていないのか» が分からなくなる。
        ///       並びの穴を許さず、丸ごと張らないことで気付ける形にする。
        if (!go || !go->IsValid()) { joint->chainBodies.clear(); return; }
        joint->chainBodies.push_back(EntityRef{ go->GetID() });
    }
    SetAuthoredJointDistance(*joint, segmentLength);
    joint->solverIterations = iterations < 1 ? 1 : iterations;
}

void ScriptJointProxy::Break() const
{
    if (!script || !script->m_gameObject) return;
    if (script->m_gameObject->GetComponent<JointComponent>())
        script->m_gameObject->RemoveComponent<JointComponent>();
}

bool ScriptJointProxy::HasJoint() const
{
    return SelfComponent<JointComponent>(script) != nullptr;
}

bool ScriptJointProxy::IsConnected() const
{
    const auto* joint = SelfComponent<JointComponent>(script);
    return joint && joint->connected;
}

void ScriptJointProxy::SetEnabled(bool enabled) const
{
    if (auto* joint = SelfComponent<JointComponent>(script)) joint->enabled = enabled;
}

bool ScriptJointProxy::IsEnabled() const
{
    const auto* joint = SelfComponent<JointComponent>(script);
    return joint && joint->enabled;
}

void ScriptJointProxy::SetLimits(float lower, float upper) const
{
    auto* joint = SelfComponent<JointComponent>(script);
    if (!joint) return;
    joint->useLimits  = true;
    /// @note 逆に渡されても入れ替えて受ける。Physics 側の SetLimits も min/max を並べ替えるが、
    ///       Inspector に «下限 > 上限» のまま残ると «可動域が効かない» に見える。
    joint->lowerLimit = std::min(lower, upper);
    joint->upperLimit = std::max(lower, upper);
}

void ScriptJointProxy::ClearLimits() const
{
    if (auto* joint = SelfComponent<JointComponent>(script)) joint->useLimits = false;
}

void ScriptJointProxy::SetMotor(float targetSpeed, float maxTorque) const
{
    auto* joint = SelfComponent<JointComponent>(script);
    if (!joint) return;
    joint->useMotor       = true;
    joint->motorSpeed     = targetSpeed;
    joint->motorMaxTorque = maxTorque < 0.0f ? 0.0f : maxTorque;
}

void ScriptJointProxy::ClearMotor() const
{
    if (auto* joint = SelfComponent<JointComponent>(script)) joint->useMotor = false;
}

void ScriptJointProxy::SetDistance(float distance) const
{
    if (auto* joint = SelfComponent<JointComponent>(script))
        SetAuthoredJointDistance(*joint, distance);
}

float ScriptJointProxy::GetDistance() const
{
    const auto* joint = SelfComponent<JointComponent>(script);
    return joint ? joint->resolvedDistance : 0.0f;
}

float ScriptJointProxy::GetCurrentDistance() const
{
    auto* joint = SelfComponent<JointComponent>(script);
    if (!joint || !script || !script->m_gameObject || !script->m_scene) return 0.0f;
    const physics::RigidBody* self = SelfRigidBody(script);
    const physics::RigidBody* other =
        ResolveJointPartner(*script->m_scene, *script->m_gameObject, *joint);
    if (!self || !other) return 0.0f;
    return (other->GetPosition() - self->GetPosition()).Length();
}

} // namespace fbzz::scene
