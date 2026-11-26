/// @file    Scene.cpp
/// @brief   Scene の Entity 管理と GameObject 所有。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include "Engine/Scene/Scene.hpp"
#include "Components/SceneComponentStorage.hpp"
#include <Engine/Scene/World/WorldPartition.hpp>
#include <Graphics/Renderer/View/RenderSettings.hpp>
#include "Engine/Scene/Prefab/PrefabPool.hpp"
#include "Engine/Scene/Rendering/SceneRenderResources.hpp"
#include "Engine/Scene/Scripting/ScriptComponent.hpp"
#include "Engine/Scene/Managed/Runtime/ManagedScriptRuntime.hpp"
#include "Engine/Core/Diagnostics/Logger.hpp"
#include "Engine/Core/Timing/Time.hpp"
#include "Graphics/Renderer/Resources/ResourceManager.hpp"
#include "Engine/Util/Types/Uuid.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string_view>
#include <utility>

namespace fbzz::scene {

namespace {

std::atomic<uint64_t> s_renderSceneGeneration{0};

}

Scene::Scene() : m_renderSceneGeneration(++s_renderSceneGeneration),
    m_rendererOwnerRevision(AllocateComponentLifetime()), m_components(std::make_unique<ComponentStorage>()) {}

Scene::~Scene()
{
    ForgetWorldPartitionRuntime(*this);
    PrefabPool::Clear(*this);
    ManagedScriptRuntime::RetireScene(*this);
    // 終了済み subsystem を Script が触らないよう、破棄では Clear / OnDestroy を呼ばない。
    ReleaseSceneOwnedGpuResources(*this);
}

void Scene::SetResourceManager(renderer::ResourceManager* resources)
{
    m_resourceOwner = resources ? resources->GetInstanceId() : 0;
}

renderer::ResourceManager* Scene::GetResourceManager() const
{
    return renderer::ResourceManager::FindOwner(m_resourceOwner);
}

Scene::Scene(Scene&& other) noexcept : Scene()
{
    *this = std::move(other);
}

Scene& Scene::operator=(Scene&& other) noexcept
{
    if (this == &other) {
        return *this;
    }
    assert(m_destroyBatchDepth == 0 && other.m_destroyBatchDepth == 0 && "Scene move requires no active destruction callback");


    Clear();

    m_generations = std::move(other.m_generations);
    m_resourceOwner = std::exchange(other.m_resourceOwner, 0);
    m_nextIndex = other.m_nextIndex;
    m_freeIndices = std::move(other.m_freeIndices);
    m_gameObjects = std::move(other.m_gameObjects);
    m_gameObjectCount = other.m_gameObjectCount;
    m_entityToGameObject = std::move(other.m_entityToGameObject);

    m_components->MoveFrom(*other.m_components);
    m_destroyQueue = std::move(other.m_destroyQueue);
    m_runtimePostProcessSettings = std::move(other.m_runtimePostProcessSettings);
    m_userRenderPasses = std::move(other.m_userRenderPasses);
    m_scriptDebugDrawCommands = std::move(other.m_scriptDebugDrawCommands);
    m_lastScriptDebugDrawTickFrame = other.m_lastScriptDebugDrawTickFrame;
    m_environment = other.m_environment;
    // 同一フレームでも移動元と内容が異なるため、流れは移動先で集め直す。
    InvalidateFlowFrame();

    FixupOwnership();
    TransferWorldPartitionRuntime(other, *this);
    other.Clear();
    return *this;
}

EntityID Scene::TryAllocateEntity() {
    if (m_freeIndices.empty() && m_nextIndex >= MAX_ENTITIES) {
        return EntityID::INVALID;
    }

    uint32_t idx;
    if (!m_freeIndices.empty()) {
        idx = m_freeIndices.back();
        m_freeIndices.pop_back();
    } else {
        m_generations.push_back(0);
        m_entityToGameObject.push_back(nullptr);
        idx = m_nextIndex++;
    }
    ++m_generations[idx];
    return EntityID{ idx, m_generations[idx] };
}

bool Scene::IsValid(EntityID id) const {
    if (!id.IsValid()) {
        return false;
    }
    if (id.index >= m_nextIndex) {
        return false;
    }
    // 破棄後の空きスロットにも世代があるため、個体の存在も確認する。
    return m_generations[id.index] == id.generation && m_entityToGameObject[id.index] != nullptr;
}

GameObject* Scene::TryCreateGameObject(const std::string& name) {
    const EntityID id = TryAllocateEntity();
    if (!id.IsValid()) {
        FBZZ_LOG_WARN("Scene::TryCreateGameObject: EntityID range %u exhausted; cannot create '%s'",
                      MAX_ENTITIES, name.c_str());
        return nullptr;
    }

    auto go            = std::make_unique<GameObject>();
    go->name           = name;
    go->instanceId     = util::GenerateUUID();
    go->m_id           = id;
    go->m_scene        = this;
    go->m_sceneStorageIndex = m_gameObjects.size();

    GameObject* ptr = go.get();
    m_entityToGameObject[id.index] = ptr;
    m_gameObjects.push_back(std::move(go));
    ++m_gameObjectCount;
    ++m_hierarchyRevision;

    return ptr;
}

GameObject& Scene::CreateGameObject(const std::string& name) {
    GameObject* gameObject = TryCreateGameObject(name);
    assert(gameObject && "CreateGameObject requires available capacity; use TryCreateGameObject");
    if (!gameObject) {
        std::abort();
    }
    return *gameObject;
}

size_t Scene::RemainingEntityCapacity() const {
    return static_cast<size_t>(MAX_ENTITIES - m_nextIndex) + m_freeIndices.size();
}

bool Scene::CanCreateGameObjects(size_t count) const {
    return count <= RemainingEntityCapacity();
}

GameObject* Scene::FindByGuid(const std::string& guid) const {
    if (guid.empty()) {
        return nullptr;
    }
    for (const auto& go : m_gameObjects)
        if (go && go->instanceId == guid) {
            return go.get();
        }
    return nullptr;
}

GameObject* Scene::Find(const std::string& name) const {
    for (auto& go : m_gameObjects)
        if (go && go->name == name) {
            return go.get();
        }
    return nullptr;
}

GameObject* Scene::FindWithTag(const std::string& t) const {
    for (auto& go : m_gameObjects)
        if (go && go->tag == t) {
            return go.get();
        }
    return nullptr;
}

GameObject* Scene::FindWithLayer(int layer) const {
    for (auto& go : m_gameObjects)
        if (go && go->layer == layer) {
            return go.get();
        }
    return nullptr;
}

std::vector<GameObject*> Scene::FindAllWithTag(const std::string& t) const {
    std::vector<GameObject*> result;
    for (auto& go : m_gameObjects)
        if (go && go->tag == t) {
            result.push_back(go.get());
        }
    return result;
}

std::vector<GameObject*> Scene::FindAllWithLayer(int layer) const {
    std::vector<GameObject*> result;
    for (auto& go : m_gameObjects)
        if (go && go->layer == layer) {
            result.push_back(go.get());
        }
    return result;
}

std::vector<GameObject*> Scene::GetRootGameObjects() const {
    std::vector<GameObject*> result;
    for (auto& go : m_gameObjects)
        if (go && !go->m_parent.IsValid()) {
            result.push_back(go.get());
        }
    return result;
}

GameObjectRange Scene::GameObjects() {
    return GameObjectRange(m_gameObjects.begin(), m_gameObjects.end());
}

void Scene::SortEntitiesBySceneOrder(std::span<EntityID> entities) const
{
    std::sort(entities.begin(), entities.end(), [this](EntityID left, EntityID right) {
        const auto* leftObject = GetGameObject(left);
        const auto* rightObject = GetGameObject(right);
        if (leftObject && rightObject) {
            return leftObject->m_sceneStorageIndex < rightObject->m_sceneStorageIndex;
        }
        if (leftObject || rightObject) {
            return leftObject != nullptr;
        }
        if (left.index != right.index) {
            return left.index < right.index;
        }
        return left.generation < right.generation;
    });
}

GameObject* Scene::GetGameObject(EntityID id) const {
    if (!IsValid(id)) {
        return nullptr;
    }
    return m_entityToGameObject[id.index];
}

void Scene::Clear()
{
    InvokeClearDestroyScripts();

    // GPU 資源のハンドルが残っている間に、元の資源管理者へ返す。
    ManagedScriptRuntime::RetireScene(*this);
    ReleaseSceneOwnedGpuResources(*this);

    // Play / Stop で Scene* と EntityID が再利用されるため、旧内容のプール参照を破棄する。
    PrefabPool::Clear(*this);

    m_destroyQueue.clear();
    m_gameObjects.clear();
    m_gameObjectCount = 0;

    m_components->Clear();

    m_generations.clear();
    m_entityToGameObject.clear();
    m_nextIndex = 0;
    m_freeIndices.clear();
    ClearRuntimePostProcessSettings();
    ClearUserRenderPasses();
    m_scriptDebugDrawCommands.clear();
    m_lastScriptDebugDrawTickFrame = 0;
    m_environment = SceneEnvironment{};
    m_flowFrame = FlowFieldFrame{};
    m_renderSceneGeneration = ++s_renderSceneGeneration;
    m_rendererOwnerRevision = AllocateComponentLifetime();
    InvalidateFlowFrame();
    ForgetWorldPartitionRuntime(*this);
}

renderer::PostProcessSettings& Scene::GetRuntimePostProcessSettings()
{
    if (!m_runtimePostProcessSettings) {
        m_runtimePostProcessSettings = std::make_unique<renderer::PostProcessSettings>();
    }
    return *m_runtimePostProcessSettings;
}

const renderer::PostProcessSettings* Scene::TryGetRuntimePostProcessSettings() const
{
    return m_runtimePostProcessSettings.get();
}

void Scene::SetRuntimePostProcessSettings(const renderer::PostProcessSettings& settings)
{
    if (!m_runtimePostProcessSettings) {
        m_runtimePostProcessSettings = std::make_unique<renderer::PostProcessSettings>(settings);
    }
    else {
        *m_runtimePostProcessSettings = settings;
    }
}

void Scene::ClearRuntimePostProcessSettings()
{
    m_runtimePostProcessSettings.reset();
}

void Scene::QueueUserRenderPass(UserRenderPassDesc desc)
{
    m_userRenderPasses.push_back(std::move(desc));
}

void Scene::ClearUserRenderPasses()
{
    m_userRenderPasses.clear();
}

const std::vector<UserRenderPassDesc>& Scene::GetUserRenderPasses() const
{
    return m_userRenderPasses;
}

void Scene::QueueScriptDebugDraw(ScriptDebugDrawCommand command)
{
    // 発行フレームを記録して、duration=0 の描画も同一フレーム内の複数ビューに表示する。
    command.frameCreated = Time::frameCount;
    m_scriptDebugDrawCommands.push_back(command);
}

void Scene::TickScriptDebugDrawCommands(float dt)
{
    const uint64_t currentFrame = Time::frameCount;
    if (m_lastScriptDebugDrawTickFrame == currentFrame) {
        return;
    }

    m_lastScriptDebugDrawTickFrame = currentFrame;
    for (auto& command : m_scriptDebugDrawCommands) {
        if (command.frameCreated < currentFrame) {
            command.duration -= dt;
        }
    }

    m_scriptDebugDrawCommands.erase(
        std::remove_if(m_scriptDebugDrawCommands.begin(), m_scriptDebugDrawCommands.end(),
            [currentFrame](const ScriptDebugDrawCommand& command) {
                return command.frameCreated < currentFrame && command.duration <= 0.0f;
            }),
        m_scriptDebugDrawCommands.end());
}

const std::vector<ScriptDebugDrawCommand>& Scene::GetScriptDebugDrawCommands() const
{
    return m_scriptDebugDrawCommands;
}

void Scene::FixupOwnership()
{
    m_rendererOwnerRevision = AllocateComponentLifetime();
    // move 前の Scene を指す非所有参照と逆引きを新しい所有者へ付け替える。
    for (auto& go : m_gameObjects) {
        if (!go) {
            continue;
        }
        go->m_scene = this;
        m_entityToGameObject[go->m_id.index] = go.get();
    }

    // Script が保持する Scene* / GameObject* も同様に旧ポインタになっているため更新する。
    bool managedReferences = false;
    for (EntityID id : GetArray<ScriptComponent>().Entities()) {
        auto& sc = GetArray<ScriptComponent>().Get(id);
        for (auto& entry : sc.scripts) {
            if (!entry.script) {
                continue;
            }
            entry.script->SetContext(this, GetGameObject(id));
            const std::string_view type = entry.script->GetTypeName();
            managedReferences |= type.starts_with("cs:") || type.starts_with("cs-internal:");
        }
    }
    // 保存参照の再解決は Scene 移動の一部。通常の owner 再束縛では古い wrapper を復活させない。
    if (managedReferences && !ManagedScriptRuntime::Instance().RebindSceneReferences(*this))
        FBZZ_LOG_ERROR("Scene move could not rebind managed saved references.");
}

}
