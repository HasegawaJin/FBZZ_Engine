/// @file    Scene.cpp
/// @brief   Scene の Entity 管理と GameObject 所有。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// EntityID の生成・破棄、Destroy キュー、Component 複製を扱う。
/// フレーム中の削除は遅延させ、System 走査中の参照破壊を避ける。
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/SceneSerializer.hpp"
#include "Engine/Scene/ScriptComponent.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Renderer/Material.hpp"
#include "Engine/Renderer/IShader.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Util/Uuid.hpp"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <memory>
#include <utility>

namespace fbzz::scene {

Scene::Scene(Scene&& other) noexcept
{
    *this = std::move(other);
}

Scene& Scene::operator=(Scene&& other) noexcept
{
    if (this == &other) return *this;

    Clear();

    std::memcpy(m_generations, other.m_generations, sizeof(m_generations));
    m_nextIndex = other.m_nextIndex;
    m_freeIndices = std::move(other.m_freeIndices);
    m_gameObjects = std::move(other.m_gameObjects);
    std::memset(m_entityToGameObject, 0, sizeof(m_entityToGameObject));

    m_arrays       = std::move(other.m_arrays);
    m_destroyQueue = std::move(other.m_destroyQueue);
    m_runtimePostProcessSettings = std::move(other.m_runtimePostProcessSettings);
    m_userRenderPasses = std::move(other.m_userRenderPasses);
    m_scriptDebugDrawCommands = std::move(other.m_scriptDebugDrawCommands);
    m_lastScriptDebugDrawTickFrame = other.m_lastScriptDebugDrawTickFrame;

    FixupOwnership();
    other.Clear();
    return *this;
}

// -----------------------------------------------------------------------
// Entity 管理
// -----------------------------------------------------------------------
EntityID Scene::AllocateEntity() {
    uint32_t idx;
    if (!m_freeIndices.empty()) {
        idx = m_freeIndices.back();
        m_freeIndices.pop_back();
    } else {
        assert(m_nextIndex < MAX_ENTITIES && "Entity 数が MAX_ENTITIES を超えました");
        idx = m_nextIndex++;
    }
    ++m_generations[idx];
    return EntityID{ idx, m_generations[idx] };
}

void Scene::DestroyImmediate(EntityID id) {
    if (!IsValid(id)) return;

    GameObject* go = GetGameObject(id);

    // 子を再帰的に破棄
    if (go) {
        std::vector<EntityID> children = go->m_children; // コピーして走査
        for (EntityID child : children)
            DestroyImmediate(child);

        // 親から切り離す
        if (go->m_parent.IsValid()) {
            if (auto* parent = GetGameObject(go->m_parent)) {
                auto& pc = parent->m_children;
                pc.erase(std::remove(pc.begin(), pc.end(), id), pc.end());
            }
        }
    }

    // Script の後処理。
    // WHY 添字ループか (不具合修正): OnDestroy から AddScript / Create が呼ばれると
    //     ScriptComponent 配列も sc->scripts も再確保される。範囲 for が握る参照は
    //     そこで無効になるため、毎回 id と添字から引き直す。
    //     FBZZ_EXECUTE_ALWAYS の導入で編集中も m_started が立つようになり、
    //     エディタ上の削除でもこの経路を通るようになった。
    if (auto* sc = GetComponent<ScriptComponent>(id)) {
        const size_t initialCount = sc->scripts.size();
        for (size_t i = 0; i < initialCount; ++i) {
            sc = GetComponent<ScriptComponent>(id);
            if (!sc || i >= sc->scripts.size()) break;
            ScriptEntry& entry = sc->scripts[i];
            if (entry.script && entry.m_started)
                entry.script->ExecuteCallback(&Script::OnDestroy, "OnDestroy");
        }
    }

    // Component 削除
    RemoveAllComponents(id);

    // Entity 解放
    m_entityToGameObject[id.index] = nullptr;
    ++m_generations[id.index]; // generation をインクリメントして古い参照を無効化
    m_freeIndices.push_back(id.index);

    // m_gameObjects から削除
    if (go) {
        auto it = std::find_if(m_gameObjects.begin(), m_gameObjects.end(),
            [go](const auto& u) { return u.get() == go; });
        if (it != m_gameObjects.end())
            m_gameObjects.erase(it);
    }
}

bool Scene::IsValid(EntityID id) const {
    if (!id.IsValid()) return false;
    if (id.index >= m_nextIndex) return false;
    return m_generations[id.index] == id.generation;
}

// -----------------------------------------------------------------------
// GameObject 生成
// -----------------------------------------------------------------------
GameObject& Scene::CreateGameObject(const std::string& name) {
    EntityID id = AllocateEntity();

    auto go            = std::make_unique<GameObject>();
    go->name           = name;
    go->instanceId     = util::GenerateUUID();
    go->m_id           = id;
    go->m_scene        = this;

    GameObject* ptr = go.get();
    m_entityToGameObject[id.index] = ptr;
    m_gameObjects.push_back(std::move(go));

    return *ptr;
}

GameObject* Scene::FindByGuid(const std::string& guid) const {
    if (guid.empty()) return nullptr;
    for (const auto& go : m_gameObjects)
        if (go->instanceId == guid) return go.get();
    return nullptr;
}

// -----------------------------------------------------------------------
// 検索
// -----------------------------------------------------------------------
GameObject* Scene::Find(const std::string& name) const {
    for (auto& go : m_gameObjects)
        if (go->name == name) return go.get();
    return nullptr;
}

GameObject* Scene::FindWithTag(const std::string& t) const {
    for (auto& go : m_gameObjects)
        if (go->tag == t) return go.get();
    return nullptr;
}

GameObject* Scene::FindWithLayer(int layer) const {
    for (auto& go : m_gameObjects)
        if (go->layer == layer) return go.get();
    return nullptr;
}

std::vector<GameObject*> Scene::FindAllWithTag(const std::string& t) const {
    std::vector<GameObject*> result;
    for (auto& go : m_gameObjects)
        if (go->tag == t) result.push_back(go.get());
    return result;
}

std::vector<GameObject*> Scene::FindAllWithLayer(int layer) const {
    std::vector<GameObject*> result;
    for (auto& go : m_gameObjects)
        if (go->layer == layer) result.push_back(go.get());
    return result;
}

std::vector<GameObject*> Scene::GetRootGameObjects() const {
    std::vector<GameObject*> result;
    for (auto& go : m_gameObjects)
        if (!go->m_parent.IsValid())
            result.push_back(go.get());
    return result;
}

// -----------------------------------------------------------------------
// イテレーション
// -----------------------------------------------------------------------
GameObjectRange Scene::GameObjects() {
    return GameObjectRange(m_gameObjects.begin(), m_gameObjects.end());
}

bool Scene::DestroyGameObject(EntityID id)
{
    if (!IsValid(id)) return false;
    DestroyImmediate(id);
    return true;
}

bool Scene::MoveGameObject(EntityID id, int offset)
{
    if (offset == 0 || !IsValid(id) || m_gameObjects.empty()) return false;

    auto it = std::find_if(m_gameObjects.begin(), m_gameObjects.end(),
        [id](const auto& go) { return go->GetID() == id; });
    if (it == m_gameObjects.end()) return false;

    const auto currentIndex = static_cast<int>(std::distance(m_gameObjects.begin(), it));
    int targetIndex = currentIndex + offset;
    targetIndex = std::max(0, std::min(targetIndex, static_cast<int>(m_gameObjects.size()) - 1));
    if (targetIndex == currentIndex) return false;

    return MoveGameObjectToIndex(id, static_cast<size_t>(targetIndex));
}

bool Scene::MoveGameObjectToIndex(EntityID id, size_t newIndex)
{
    if (!IsValid(id) || m_gameObjects.empty()) return false;

    auto it = std::find_if(m_gameObjects.begin(), m_gameObjects.end(),
        [id](const auto& go) { return go->GetID() == id; });
    if (it == m_gameObjects.end()) return false;

    const size_t currentIndex = static_cast<size_t>(std::distance(m_gameObjects.begin(), it));
    newIndex = std::min(newIndex, m_gameObjects.size() - 1);
    if (newIndex == currentIndex) return false;

    auto moved = std::move(*it);
    m_gameObjects.erase(it);
    m_gameObjects.insert(m_gameObjects.begin() + static_cast<std::ptrdiff_t>(newIndex), std::move(moved));
    return true;
}

bool Scene::SetRootSiblingIndex(EntityID id, int newRootIndex)
{
    if (!IsValid(id)) return false;
    GameObject* target = GetGameObject(id);
    if (!target || target->m_parent.IsValid()) return false;

    // 対象を除いたルート一覧をフラット index 付きで集める
    std::vector<size_t> otherRootFlatIndices;
    size_t currentFlat = SIZE_MAX;
    for (size_t i = 0; i < m_gameObjects.size(); ++i) {
        const auto& go = m_gameObjects[i];
        if (go->m_parent.IsValid()) continue;
        if (go->GetID() == id) { currentFlat = i; continue; }
        otherRootFlatIndices.push_back(i);
    }
    if (currentFlat == SIZE_MAX) return false;

    newRootIndex = std::clamp(newRootIndex, 0, static_cast<int>(otherRootFlatIndices.size()));

    // 挿入先フラット index (対象を取り除いた後の座標系で求める)
    size_t targetFlat;
    if (newRootIndex >= static_cast<int>(otherRootFlatIndices.size())) {
        targetFlat = m_gameObjects.size() - 1;  // 最後のルートより後ろ = 配列末尾
    } else {
        targetFlat = otherRootFlatIndices[static_cast<size_t>(newRootIndex)];
        if (targetFlat > currentFlat) --targetFlat;  // 削除で 1 つ前へ詰まる分を補正
    }
    return MoveGameObjectToIndex(id, targetFlat);
}

bool Scene::SyncSiblingFlatOrder(EntityID id)
{
    GameObject* go = GetGameObject(id);
    if (!go) return false;
    if (!go->m_parent.IsValid()) return true;  // ルートは flat 順そのものが正本
    GameObject* parent = GetGameObject(go->m_parent);
    if (!parent) return false;

    const auto& siblings = parent->m_children;
    const auto pos = std::find(siblings.begin(), siblings.end(), id);
    if (pos == siblings.end()) return false;
    const size_t k = static_cast<size_t>(std::distance(siblings.begin(), pos));
    if (siblings.size() <= 1) return true;

    auto flatIndexOf = [this](EntityID target) -> size_t {
        for (size_t i = 0; i < m_gameObjects.size(); ++i)
            if (m_gameObjects[i]->GetID() == target) return i;
        return SIZE_MAX;
    };
    const size_t curFlat = flatIndexOf(id);
    if (curFlat == SIZE_MAX) return false;

    // 兄弟順が「直前の兄弟の後 / 先頭なら次の兄弟の前」になるよう flat 位置を移す。
    // (削除後の座標系で挿入位置を求める)
    size_t insertPos;
    if (k > 0) {
        size_t prevFlat = flatIndexOf(siblings[k - 1]);
        if (prevFlat == SIZE_MAX) return false;
        if (prevFlat > curFlat) --prevFlat;
        insertPos = prevFlat + 1;
    } else {
        size_t nextFlat = flatIndexOf(siblings[1]);
        if (nextFlat == SIZE_MAX) return false;
        if (nextFlat > curFlat) --nextFlat;
        insertPos = nextFlat;
    }
    if (insertPos == curFlat) return true;
    return MoveGameObjectToIndex(id, insertPos);
}

// -----------------------------------------------------------------------
// EntityID → GameObject* O(1) 逆引き
// -----------------------------------------------------------------------
GameObject* Scene::GetGameObject(EntityID id) const {
    if (!IsValid(id)) return nullptr;
    return m_entityToGameObject[id.index];
}

// -----------------------------------------------------------------------
// Destroy キュー処理
// -----------------------------------------------------------------------
void Scene::FlushDestroyQueue(float dt) {
    for (auto& entry : m_destroyQueue)
        entry.delay -= dt;

    std::vector<EntityID> toDestroy;
    m_destroyQueue.erase(
        std::remove_if(m_destroyQueue.begin(), m_destroyQueue.end(),
            [&](DestroyEntry& e) {
                if (e.delay <= 0.0f) { toDestroy.push_back(e.id); return true; }
                return false;
            }),
        m_destroyQueue.end());

    for (EntityID id : toDestroy)
        DestroyImmediate(id);
}

void Scene::Clear()
{
    // シーン破棄前に全スクリプトの OnDestroy を呼ぶ
    std::vector<EntityID> scriptIds(GetEntities<ScriptComponent>().begin(),
                                    GetEntities<ScriptComponent>().end());
    for (EntityID id : scriptIds) {
        auto* sc = GetComponent<ScriptComponent>(id);
        if (!sc) continue;
        for (auto& entry : sc->scripts)
            if (entry.script && entry.m_started)
                entry.script->ExecuteCallback(&Script::OnDestroy, "OnDestroy");
    }

    m_destroyQueue.clear();
    m_gameObjects.clear();

    std::apply([](auto&... arrs) { (..., arrs.Clear()); }, m_arrays);

    std::memset(m_generations,       0, sizeof(m_generations));
    std::memset(m_entityToGameObject, 0, sizeof(m_entityToGameObject));
    m_nextIndex = 0;
    m_freeIndices.clear();
    ClearRuntimePostProcessSettings();
    ClearUserRenderPasses();
    m_scriptDebugDrawCommands.clear();
    m_lastScriptDebugDrawTickFrame = 0;
}

renderer::PostProcessSettings& Scene::GetRuntimePostProcessSettings()
{
    if (!m_runtimePostProcessSettings)
        m_runtimePostProcessSettings = std::make_unique<renderer::PostProcessSettings>();
    return *m_runtimePostProcessSettings;
}

const renderer::PostProcessSettings* Scene::TryGetRuntimePostProcessSettings() const
{
    return m_runtimePostProcessSettings.get();
}

void Scene::SetRuntimePostProcessSettings(const renderer::PostProcessSettings& settings)
{
    if (!m_runtimePostProcessSettings)
        m_runtimePostProcessSettings = std::make_unique<renderer::PostProcessSettings>(settings);
    else
        *m_runtimePostProcessSettings = settings;
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
    // WHAT: 発行フレームを記録して、duration=0 の描画も同一フレーム内の複数ビューに表示する。
    command.frameCreated = Time::frameCount;
    m_scriptDebugDrawCommands.push_back(command);
}

void Scene::TickScriptDebugDrawCommands(float dt)
{
    const uint64_t currentFrame = Time::frameCount;
    if (m_lastScriptDebugDrawTickFrame == currentFrame)
        return;

    m_lastScriptDebugDrawTickFrame = currentFrame;
    for (auto& command : m_scriptDebugDrawCommands) {
        if (command.frameCreated < currentFrame)
            command.duration -= dt;
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

void Scene::RemoveAllComponents(EntityID id)
{
    std::apply([&](auto&... arrs) {
        (..., RemoveIfHas(arrs, id));
    }, m_arrays);
}

void Scene::DuplicateComponents(EntityID src, EntityID dst)
{
    std::apply([&](auto&... arrs) {
        (..., CopyIfHas(arrs, src, dst));
    }, m_arrays);

    CopyScriptComponentFrom(*this, src, dst);
}

void Scene::CopyComponentsFrom(const Scene& srcScene, EntityID src, EntityID dst)
{
    std::apply([&](const auto&... srcArrs) {
        std::apply([&](auto&... dstArrs) {
            (..., CopyFromOtherIfHas(srcArrs, dstArrs, src, dst));
        }, m_arrays);
    }, srcScene.m_arrays);

    CopyScriptComponentFrom(srcScene, src, dst);
}

void Scene::CopyScriptComponentFrom(const Scene& srcScene, EntityID src, EntityID dst)
{
    const auto& srcArr = srcScene.GetArray<ScriptComponent>();
    auto&       dstArr = GetArray<ScriptComponent>();
    if (!srcArr.Has(src) || dstArr.Has(dst)) return;

    ScriptComponent cloned =
        CloneScriptComponent(srcArr.Get(src), &srcScene, this, GetGameObject(dst));
    if (!cloned.scripts.empty())
        dstArr.Add(dst, std::move(cloned));
}

void Scene::FixupOwnership()
{
    // ムーブ後、GameObject が保持する m_scene 生ポインタは旧 Scene を指したままになる。
    // m_entityToGameObject も新アドレスで再構築が必要。ここで両方を修正する。
    for (auto& go : m_gameObjects) {
        go->m_scene = this;
        m_entityToGameObject[go->m_id.index] = go.get();
    }

    // Script が保持する Scene* / GameObject* も同様に旧ポインタになっているため更新する。
    for (EntityID id : GetArray<ScriptComponent>().Entities()) {
        auto& sc = GetArray<ScriptComponent>().Get(id);
        for (auto& entry : sc.scripts)
            if (entry.script) entry.script->SetContext(this, GetGameObject(id));
    }
}

} // namespace fbzz::scene
