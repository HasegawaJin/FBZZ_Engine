/// @file    ScriptSceneProxies.cpp
/// @brief   Scene 領域の Script 転送と関連する内部ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include "ScriptProxyCommon.hpp"
#include "ScriptControlPointProxyHelpers.hpp"
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Engine/Scene/ScriptRuntime.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/PrefabPool.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/PresentationControlPoints.hpp>
#include <Engine/Scene/Components/LifetimeComponent.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/CameraRigComponents.hpp>
#include <Engine/Scene/Components/UIControls.hpp>
#include <Engine/Scene/Components/AudioSpatialComponents.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Physics/World.hpp>
#include <limits>
#include <type_traits>
#include <string_view>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <Engine/Scene/SceneManager.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
namespace fbzz::scene {
using namespace script_proxy_detail;
namespace {
LineControlPointSnapshot ResolveScriptLinePoints(
    Scene& scene, EntityID owner, LineRendererComponent& line)
{
    FlushWorldTransforms(scene);
    return ResolveLineControlPoints(scene, owner, line, ScriptControlPointReadContext());
}

/// @brief Copy a resolved route to legacy numeric arrays without changing them on failure.


LifetimeComponent* SelfLifetime(const Script* script)
{
    return SelfComponent<LifetimeComponent>(script);
}

} /// @note namespace
GameObject* ScriptSceneProxy::Find(std::string_view name, bool includeInactive) const
{
    if (!script || !script->m_scene) return nullptr;
    /// @note Scene::Find は参照解決用で無効な物も返す。スクリプト向けは同名の有効な物を優先して探す。
    for (auto& go : script->m_scene->GameObjects())
        if (go.name == name && (includeInactive || go.activeInHierarchy())) return &go;
    return nullptr;
}


GameObject* ScriptSceneProxy::FindWithTag(std::string_view tag, bool includeInactive) const
{
    if (!script || !script->m_scene) return nullptr;
    for (auto& go : script->m_scene->GameObjects())
        if (go.tag == tag && (includeInactive || go.activeInHierarchy())) return &go;
    return nullptr;
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


GameObject* ScriptSceneProxy::Create(std::string_view name) const
{
    return script && script->m_scene
        ? script->m_scene->TryCreateGameObject(std::string(name)) : nullptr;
}


bool ScriptSceneProxy::CanCreate(std::size_t count) const
{
    return script && script->m_scene && script->m_scene->CanCreateGameObjects(count);
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
    return script && script->m_scene && !script->m_scene->IsExecutionParked() && script->m_gameObject && script->m_gameObject->activeInHierarchy()
        && script->IsContextEnabled();
}


GameObject* ScriptSceneProxy::Instantiate(const PrefabRef& prefab) const
{
    return Instantiate(prefab.path);
}


GameObject* ScriptSceneProxy::Instantiate(const std::string& prefabPath) const
{
    if (!script || !script->m_scene || prefabPath.empty()) return nullptr;
    /// @note Prefab の追記は既存の GameObject と Script を維持し、容量不足では追加しない。
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
    Scene* scene = script->m_scene;
    return PrefabPool::Spawn(*scene, prefabPath, position, rotation);
}


GameObject* ScriptSceneProxy::Spawn(const PrefabRef& prefab) const
{
    if (!script || !script->m_gameObject) return nullptr;
    /// @note Spawn が初期化する側の Transform と引数が別の値になるよう、位置・回転を先に固定する。
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
    /// @note «nextPos.y <= terrainH» が常に true になる誤判定を防ぐ。
    if (!script || !script->m_scene) return std::numeric_limits<float>::lowest();
    /// @note 親ごと無効化された地形・水面は «無い» ものとして扱う (描画も物理も拾わないので、問い合わせだけ答えると食い違う)。
    float highest = std::numeric_limits<float>::lowest();
    for (auto& go : script->m_scene->GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        if (const auto* terrain = go.GetComponent<TerrainComponent>(); terrain && terrain->enabled) {
            TerrainSurfaceSample sample;
            if (TrySampleTerrainWorld(*terrain, go.transform, worldPos.x, worldPos.z, sample))
                highest = (std::max)(highest, sample.position.y);
        }
    }
    return highest;
}


math::Vector3 ScriptSceneProxy::GetTerrainNormalAt(const math::Vector3& worldPos) const
{
    if (!script || !script->m_scene) return math::Vector3::UP;
    float highest = std::numeric_limits<float>::lowest();
    math::Vector3 normal = math::Vector3::UP;
    for (auto& go : script->m_scene->GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        if (const auto* terrain = go.GetComponent<TerrainComponent>(); terrain && terrain->enabled) {
            TerrainSurfaceSample sample;
            if (TrySampleTerrainWorld(*terrain, go.transform, worldPos.x, worldPos.z, sample) && sample.position.y > highest) {
                highest = sample.position.y;
                normal = sample.normal;
            }
        }
    }
    return normal;
}



bool ScriptSceneProxy::TryGetTerrainFloorAt(const math::Vector3& reference, float maxDistance,
    math::Vector3& position, math::Vector3& normal) const
{
    FBZZ_PROFILE_SCOPE("ScriptTerrain::Floor");
    if (!script || !script->m_scene) return false;
    bool found = false;
    TerrainSurfaceSample best;
    GameObject* bestOwner = nullptr;
    /// @note Component dense order can differ after removal/reorder. Only equal-height ties need the old flat Scene order.
    const auto earlierInScene = [&](GameObject* candidate) {
        for (auto& object : script->m_scene->GameObjects()) {
            if (&object == candidate) return true;
            if (&object == bestOwner) return false;
        }
        return false;
    };
    for (EntityID id : script->m_scene->GetEntities<TerrainComponent>()) {
        GameObject* owner = script->m_scene->GetGameObject(id);
        const auto* terrain = script->m_scene->GetComponent<TerrainComponent>(id);
        if (!terrain || !terrain->enabled || !owner) continue;
        if (owner->activeInHierarchy()) {
            TerrainSurfaceSample sample;
            if (TrySampleTerrainFloorWorld(*terrain, owner->transform, reference, maxDistance, sample)
                && (!found || sample.position.y > best.position.y
                    || (sample.position.y == best.position.y && earlierInScene(owner)))) {
                best = sample; bestOwner = owner; found = true;
            }
        }
    }
    if (!found) return false;
    position = best.position; normal = best.normal;
    return true;
}

bool ScriptSceneProxy::TryGetTerrainCeilingAt(const math::Vector3& reference, float maxDistance,
    math::Vector3& position, math::Vector3& normal) const
{
    FBZZ_PROFILE_SCOPE("ScriptTerrain::Ceiling");
    if (!script || !script->m_scene) return false;
    bool found = false;
    TerrainSurfaceSample best;
    GameObject* bestOwner = nullptr;
    /// @note Component dense order can differ after removal/reorder. Only equal-height ties need the old flat Scene order.
    const auto earlierInScene = [&](GameObject* candidate) {
        for (auto& object : script->m_scene->GameObjects()) {
            if (&object == candidate) return true;
            if (&object == bestOwner) return false;
        }
        return false;
    };
    for (EntityID id : script->m_scene->GetEntities<TerrainComponent>()) {
        GameObject* owner = script->m_scene->GetGameObject(id);
        const auto* terrain = script->m_scene->GetComponent<TerrainComponent>(id);
        if (!terrain || !terrain->enabled || !owner) continue;
        if (owner->activeInHierarchy()) {
            TerrainSurfaceSample sample;
            if (TrySampleTerrainCeilingWorld(*terrain, owner->transform, reference, maxDistance, sample)
                && (!found || sample.position.y < best.position.y
                    || (sample.position.y == best.position.y && earlierInScene(owner)))) {
                best = sample; bestOwner = owner; found = true;
            }
        }
    }
    if (!found) return false;
    position = best.position; normal = best.normal;
    return true;
}


float ScriptSceneProxy::GetWaterSurfaceHeight(const math::Vector3& worldPos, float time) const
{
    if (!script || !script->m_scene) return worldPos.y;
    PrepareWaterSceneSurfaces(*script->m_scene);
    for (auto& go : script->m_scene->GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        if (auto* water = go.GetComponent<WaterComponent>(); water && water->enabled
            && !water->zoneOwner.IsValid()) {
            WaterSurfaceHeightSample sample;
            if (TrySampleWaterSurfaceHeightWorld(*water, go.transform, worldPos, time, sample))
                return sample.position.y;
        }
    }
    return worldPos.y;
}


GameObject* EntityRef::Resolve(const ScriptSceneProxy& scene) const
{
    return scene.ResolveReference(*this);
}


GameObject* EntityRef::Resolve(Scene& scene) const
{
    const uint64_t currentGeneration = scene.GetRenderSceneGeneration();
    if (guid.empty()) {
        if (sceneGeneration != 0 && sceneGeneration != currentGeneration) {
            id = EntityID::INVALID;
            sceneGeneration = currentGeneration;
            return nullptr;
        }
        GameObject* object = id.IsValid() ? scene.GetGameObject(id) : nullptr;
        id = object ? object->GetID() : EntityID::INVALID;
        sceneGeneration = currentGeneration;
        if (object) guid = object->instanceId;
        return object;
    }
    GameObject* object = sceneGeneration == currentGeneration && id.IsValid()
        ? scene.GetGameObject(id) : nullptr;
    if (!object || object->instanceId != guid) object = scene.FindByGuid(guid);
    id = object ? object->GetID() : EntityID::INVALID;
    sceneGeneration = currentGeneration;
    return object;
}


GameObject* ScriptSceneProxy::ResolveReference(const EntityRef& reference) const
{
    return script && script->m_scene ? reference.Resolve(*script->m_scene) : nullptr;
}

/// @brief ScriptCharacterProxy

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
/// @brief 場は «1 本» から «リスト» になった。スクリプト API は 1 本を触る形のままで、
/// @brief 対象は **先頭の流れ**。
/// @note 先頭なのは、このプロキシを使うスクリプトが «この GameObject の流れ» を 1 つだと
/// @note 思って書かれているため。黙って全部へ配ると、流れだけ強めたつもりで乱れまで動く。

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


size_t ScriptGameplayProxy::GetLinePointCount() const
{
    auto* component = SelfComponent<LineRendererComponent>(script);
    auto* owner = ScriptSceneProxy{script}.Self();
    if (!component || !owner || !owner->GetScene()) return 0;
    const auto snapshot = ResolveScriptLinePoints(*owner->GetScene(), owner->GetID(), *component);
    return snapshot.valid ? snapshot.points.size() : 0;
}


bool ScriptGameplayProxy::TryGetLinePoint(size_t index, math::Vector3& point) const
{
    auto* component = SelfComponent<LineRendererComponent>(script);
    auto* owner = ScriptSceneProxy{script}.Self();
    if (!component || !owner || !owner->GetScene()) return false;
    const auto snapshot = ResolveScriptLinePoints(*owner->GetScene(), owner->GetID(), *component);
    if (!snapshot.valid || index >= snapshot.points.size()) return false;
    point = snapshot.points[index];
    return true;
}


bool ScriptGameplayProxy::SetLineEnabled(bool enabled) const
{
    auto* component = SelfComponent<LineRendererComponent>(script);
    if (!component)
        return false;
    component->enabled = enabled;
    return true;
}


bool ScriptGameplayProxy::SetLineLoop(bool loop) const
{
    auto* component = SelfComponent<LineRendererComponent>(script);
    if (!component) return false;
    component->loop = loop;
    return true;
}


bool ScriptGameplayProxy::SetLineBillboard(bool billboard) const
{
    auto* component = SelfComponent<LineRendererComponent>(script);
    if (!component) return false;
    component->billboard = billboard;
    return true;
}


bool ScriptGameplayProxy::SetLineSpace(LineSpace space) const
{
    if (space != LineSpace::Local && space != LineSpace::World) return false;
    auto* component = SelfComponent<LineRendererComponent>(script);
    if (!component) return false;
    component->space = space;
    return true;
}


bool ScriptGameplayProxy::SetLineShape(LineShape shape) const
{
    if (shape != LineShape::Ribbon && shape != LineShape::Tube) return false;
    auto* component = SelfComponent<LineRendererComponent>(script);
    if (!component) return false;
    component->shape = shape;
    return true;
}


bool ScriptGameplayProxy::SetLineRadialSegments(int segments) const
{
    if (segments < 3 || segments > 32) return false;
    auto* component = SelfComponent<LineRendererComponent>(script);
    if (!component) return false;
    component->radialSegments = segments;
    return true;
}


bool ScriptGameplayProxy::SetLineSorting(int layer, int order) const
{
    auto* component = SelfComponent<LineRendererComponent>(script);
    if (!component) return false;
    component->sortingLayer = layer;
    component->orderInLayer = order;
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

    if (points.size() > MAX_CONTROL_POINT_BINDINGS
        || !std::all_of(points.begin(), points.end(), [](const math::Vector3& point) {
            return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
        })) return false;
    std::vector<math::Vector3> numericPoints(points.begin(), points.end());
    component->points = std::move(numericPoints);
    component->space = worldSpace ? LineSpace::World : LineSpace::Local;
    component->pointSourceMode = ControlPointSourceMode::INLINE;
    component->pointRuntimeMode = ControlPointRuntimeMode::LIVE;
    /// @note ordered snapshot が点列と空間を比較するため、cache の明示無効化は不要。
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


} /// @note namespace fbzz::scene
