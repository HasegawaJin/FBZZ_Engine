// FBZZ Engine
// Scene.hpp | fbzz::scene
// GameObject 所有と ComponentArray 管理
// GameObjectRange / SceneView を提供し、System が連続メモリを走査できるようにする。
// Destroy は遅延キューを通し、フレーム中の参照破壊を避ける。
#pragma once
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Scene/Systems/RenderPassContext.hpp>
#include "Entity.hpp"
#include "ComponentArray.hpp"
#include "Transform.hpp"
#include "GameObject.hpp"
#include "ComponentRegistry.hpp"
#include <Math/Vector4.hpp>
#include <vector>
#include <memory>
#include <string>
#include <string_view>
#include <span>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <tuple>
#include <utility>

namespace fbzz::scene {

template<typename... Ts> class SceneView;

// ScriptDebugDrawType — Script から要求されたデバッグ描画の形状種別。
// WHY: DebugDraw 具体 API を Script 側へ漏らさず、Scene が描画要求だけを保持するための軽量な中間表現。
enum class ScriptDebugDrawType {
    Line,
    Sphere,
    Box,
    Ray,
    Arrow, // from=a, to=b, headLength=radius, headRadius=halfExtents.x
    Cone   // apex=a, direction=b, height=halfExtents.x, baseRadius=radius
};

// ScriptDebugDrawCommand — OnUpdate など任意のタイミングで発行されたデバッグ描画要求。
// WHY: renderer::DebugDraw は RenderSystem の BeginFrame/Flush 区間でしか使えないため、Script はコマンドを積むだけにする。
struct ScriptDebugDrawCommand {
    ScriptDebugDrawType type = ScriptDebugDrawType::Line;
    math::Vector3 a = math::Vector3::ZERO;
    math::Vector3 b = math::Vector3::ZERO;
    math::Vector3 halfExtents = math::Vector3::ZERO;
    math::Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
    float radius = 0.0f;
    float duration = 0.0f;
    uint64_t frameCreated = 0;
};

// -----------------------------------------------------------------------
// detail: ComponentList → tuple<ComponentArray<Ts>...> 変換ヘルパー
// -----------------------------------------------------------------------
namespace detail {

template<typename Tuple> struct ArrayTupleHelper;
template<typename... Ts>
struct ArrayTupleHelper<std::tuple<Ts...>> {
    using type = std::tuple<ComponentArray<Ts>...>;
};
template<typename Tuple>
using ArrayTuple = typename ArrayTupleHelper<Tuple>::type;

// T が ComponentList に含まれるか判定するトレイト
template<typename T, typename Tuple> struct IsInList;
template<typename T, typename... Ts>
struct IsInList<T, std::tuple<Ts...>>
    : std::disjunction<std::is_same<T, Ts>...> {};

} // namespace detail

// -----------------------------------------------------------------------
// GameObjectRange  —  scene.GameObjects() が返す Unity ライクな範囲 for 用 range
// -----------------------------------------------------------------------
class GameObjectRange {
    using VecT = std::vector<std::unique_ptr<GameObject>>;
    VecT::iterator m_begin, m_end;
public:
    GameObjectRange(VecT::iterator b, VecT::iterator e) : m_begin(b), m_end(e) {}

    struct Iterator {
        VecT::iterator it;
        GameObject& operator*()  const { return **it; }
        Iterator&   operator++() { ++it; return *this; }
        bool        operator!=(const Iterator& o) const { return it != o.it; }
    };

    Iterator begin() { return { m_begin }; }
    Iterator end()   { return { m_end   }; }
};

// -----------------------------------------------------------------------
// Scene
// -----------------------------------------------------------------------
class Scene {
public:
    static constexpr uint32_t MAX_ENTITIES = ComponentArray<uint8_t>::MAX;

    Scene() = default;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&& other) noexcept;
    Scene& operator=(Scene&& other) noexcept;

    // Unity: new GameObject("name")
    GameObject& CreateGameObject(const std::string& name = "GameObject");

    // Unity: GameObject.Find 系の実体
    GameObject*              Find(const std::string& name)     const;
    GameObject*              FindByGuid(const std::string& guid) const;
    GameObject*              FindWithTag(const std::string& t)  const;
    GameObject*              FindWithLayer(int layer)            const;
    std::vector<GameObject*> FindAllWithTag(const std::string& t) const;
    std::vector<GameObject*> FindAllWithLayer(int layer)          const;
    template<typename T>
    std::vector<GameObject*> FindObjectsOfType()               const;

    // Unity: scene.GetRootGameObjects()
    std::vector<GameObject*> GetRootGameObjects() const;

    // Unity ライクな範囲 for (ゲームロジック向け)
    GameObjectRange GameObjects();
    size_t GameObjectCount() const { return m_gameObjects.size(); }

    // Editor / serializer 用。階層操作を安定した API に集約する。
    bool DestroyGameObject(EntityID id);
    bool MoveGameObject(EntityID id, int offset);
    bool MoveGameObjectToIndex(EntityID id, size_t newIndex);

    // System 向け高速マルチ Component イテレータ
    template<typename... Ts>
    SceneView<Ts...> View();

    // Entity の生死を確認する
    bool IsValid(EntityID id) const;

    // フレーム末尾で呼ぶ。delay 付き Destroy を処理し、期限切れを削除する
    void FlushDestroyQueue(float dt);

    // 全 GameObject・Component を削除してシーンを空にする
    void Clear();

    renderer::PostProcessSettings& GetRuntimePostProcessSettings();
    const renderer::PostProcessSettings* TryGetRuntimePostProcessSettings() const;
    void SetRuntimePostProcessSettings(const renderer::PostProcessSettings& settings);
    void ClearRuntimePostProcessSettings();

    // QueueUserRenderPass — Script から RenderGraph へ追加するパスを 1 フレーム分キューに積む。
    // WHY: Script が RenderSystem 内部の登録順に直接依存せず、意図した挿入点だけを宣言できるようにする。
    void QueueUserRenderPass(UserRenderPassDesc desc);
    void ClearUserRenderPasses();
    const std::vector<UserRenderPassDesc>& GetUserRenderPasses() const;

    // QueueScriptDebugDraw — ScriptDebugProxy から来た描画要求を RenderSystem まで保持する。
    // WHY: DebugDraw は BeginFrame/Flush の間でしか使えないため、OnUpdate から即時描画せずキューに積む。
    void QueueScriptDebugDraw(ScriptDebugDrawCommand command);
    void TickScriptDebugDrawCommands(float dt);
    const std::vector<ScriptDebugDrawCommand>& GetScriptDebugDrawCommands() const;

    // --- GameObject / SceneView の template 本体から呼ばれる内部 API ---

    template<typename T> T&   AddComponent(EntityID id, T component);
    template<typename T> T*   GetComponent(EntityID id);
    template<typename T> bool HasComponent(EntityID id) const;
    template<typename T> void RemoveComponent(EntityID id);
    template<typename T> std::vector<T*> GetComponents();
    template<typename T> std::vector<const T*> GetComponents() const;

    // src の全 Component を dst にコピーする (Duplicate 用)
    void DuplicateComponents(EntityID src, EntityID dst);

    // SceneView が entity span を取得するために使う
    template<typename T>
    std::span<const EntityID> GetEntities() const;

    // EntityID → GameObject* の O(1) 逆引き
    GameObject* GetGameObject(EntityID id) const;

private:
    // Entity 管理
    uint32_t              m_generations[MAX_ENTITIES] = {};
    uint32_t              m_nextIndex   = 0;
    std::vector<uint32_t> m_freeIndices;

    // GameObjects 所有
    std::vector<std::unique_ptr<GameObject>> m_gameObjects;

    // EntityID.index → GameObject* (非所有)
    GameObject* m_entityToGameObject[MAX_ENTITIES] = {};

    // Component 配列 — ComponentList に登録された全型を自動展開
    detail::ArrayTuple<ComponentList> m_arrays;

    // delay 付き Destroy キュー
    struct DestroyEntry { EntityID id; float delay; };
    std::vector<DestroyEntry> m_destroyQueue;

    // runtime PostProcess は Script から一時的に上書きされる optional な状態。
    // WHY: 値メンバにすると Clear 時の全体代入で std::vector を破棄/再構築し、レイアウト変更時のクラッシュ地点になりやすい。
    std::unique_ptr<renderer::PostProcessSettings> m_runtimePostProcessSettings;
    std::vector<UserRenderPassDesc> m_userRenderPasses;
    std::vector<ScriptDebugDrawCommand> m_scriptDebugDrawCommands;
    uint64_t m_lastScriptDebugDrawTickFrame = 0;

    EntityID AllocateEntity();
    void     DestroyImmediate(EntityID id);
    void     FixupOwnership();
    void     RemoveAllComponents(EntityID id);

    // T が ComponentList に登録済みかコンパイル時に検査する
    template<typename T>
    static constexpr bool IsRegistered = detail::IsInList<T, ComponentList>::value;

    template<typename T>
    ComponentArray<T>& GetArray() {
        static_assert(IsRegistered<T>,
            "T is not in ComponentList — add it to ComponentRegistry.hpp");
        return std::get<ComponentArray<T>>(m_arrays);
    }

    template<typename T>
    const ComponentArray<T>& GetArray() const {
        static_assert(IsRegistered<T>,
            "T is not in ComponentList — add it to ComponentRegistry.hpp");
        return std::get<ComponentArray<T>>(m_arrays);
    }

    // fold expression から呼ぶ配列ごとのヘルパー
    template<typename T>
    static void RemoveIfHas(ComponentArray<T>& arr, EntityID id) {
        if (arr.Has(id)) arr.Remove(id);
    }

    template<typename T>
    static void CopyIfHas(ComponentArray<T>& arr, EntityID src, EntityID dst) {
        if constexpr (std::is_copy_constructible_v<T>) {
            if (arr.Has(src) && !arr.Has(dst)) arr.Add(dst, arr.Get(src));
        }
    }

    template<typename... Ts> friend class SceneView;
    friend class GameObject;
};

// -----------------------------------------------------------------------
// SceneView<Ts...>  —  複数 Component を持つ Entity を効率よくイテレート
// Transform は ComponentArray に入らず GameObject から取得する
// -----------------------------------------------------------------------
template<typename... Ts>
class SceneView {
public:
    explicit SceneView(Scene& scene) : m_scene(&scene) {}

    struct Iterator {
        Scene*           m_scene;
        const EntityID*  m_ids;
        size_t           m_count;
        size_t           m_index;

        Iterator(Scene* s, const EntityID* ids, size_t count, size_t idx)
            : m_scene(s), m_ids(ids), m_count(count), m_index(idx)
        {
            Advance();
        }

        void Advance() {
            while (m_index < m_count && !HasAll(m_ids[m_index]))
                ++m_index;
        }

        bool HasAll(EntityID id) const {
            return (... && HasOne<Ts>(*m_scene, id));
        }

        // operator* は tuple<Ts&...> を返す。range-for では auto [a,b] = *it で使う
        std::tuple<Ts&...> operator*() const {
            EntityID id = m_ids[m_index];
            return std::tuple<Ts&...>(GetRef<Ts>(*m_scene, id)...);
        }

        Iterator& operator++() { ++m_index; Advance(); return *this; }
        bool operator!=(const Iterator& o) const { return m_index != o.m_index; }

    private:
        template<typename T>
        static bool HasOne(Scene& scene, EntityID id) {
            if constexpr (std::is_same_v<T, Transform>)
                return scene.GetGameObject(id) != nullptr;
            else
                return scene.HasComponent<T>(id);
        }

        template<typename T>
        static T& GetRef(Scene& scene, EntityID id) {
            if constexpr (std::is_same_v<T, Transform>)
                return scene.GetGameObject(id)->transform;
            else
                return *scene.GetComponent<T>(id);
        }
    };

    Iterator begin() const {
        auto [ptr, count] = GetBase();
        return Iterator(m_scene, ptr, count, 0);
    }

    Iterator end() const {
        auto [ptr, count] = GetBase();
        return Iterator(m_scene, ptr, count, count);
    }

private:
    Scene* m_scene;

    // 最小の非 Transform ComponentArray の entity span を返す
    std::pair<const EntityID*, size_t> GetBase() const {
        const EntityID* bestPtr   = nullptr;
        size_t          bestCount = SIZE_MAX;
        UpdateBase<Ts...>(bestPtr, bestCount);
        if (bestPtr == nullptr) return { nullptr, 0 };
        return { bestPtr, bestCount };
    }

    template<typename Head, typename... Tail>
    void UpdateBase(const EntityID*& ptr, size_t& count) const {
        if constexpr (!std::is_same_v<Head, Transform>) {
            auto span = m_scene->GetEntities<Head>();
            if (span.size() < count) {
                count = span.size();
                ptr   = span.data();
            }
        }
        if constexpr (sizeof...(Tail) > 0)
            UpdateBase<Tail...>(ptr, count);
    }
};

// -----------------------------------------------------------------------
// Scene template 本体
// -----------------------------------------------------------------------

template<typename T>
std::vector<GameObject*> Scene::FindObjectsOfType() const {
    std::vector<GameObject*> result;
    auto span = GetEntities<T>();
    result.reserve(span.size());
    for (EntityID id : span)
        if (auto* go = GetGameObject(id))
            result.push_back(go);
    return result;
}

template<typename... Ts>
SceneView<Ts...> Scene::View() {
    return SceneView<Ts...>(*this);
}

template<typename T>
T& Scene::AddComponent(EntityID id, T component) {
    assert(IsValid(id));
    GetArray<T>().Add(id, std::move(component));
    return GetArray<T>().Get(id);
}

template<typename T>
T* Scene::GetComponent(EntityID id) {
    if (!IsValid(id)) return nullptr;
    auto& arr = GetArray<T>();
    return arr.Has(id) ? &arr.Get(id) : nullptr;
}

template<typename T>
bool Scene::HasComponent(EntityID id) const {
    if (!IsValid(id)) return false;
    return GetArray<T>().Has(id);
}

template<typename T>
void Scene::RemoveComponent(EntityID id) {
    GetArray<T>().Remove(id);
}

template<typename T>
std::vector<T*> Scene::GetComponents() {
    auto data = GetArray<T>().Data();
    std::vector<T*> result;
    result.reserve(data.size());
    for (auto& component : data)
        result.push_back(&component);
    return result;
}

template<typename T>
std::vector<const T*> Scene::GetComponents() const {
    auto data = GetArray<T>().Data();
    std::vector<const T*> result;
    result.reserve(data.size());
    for (const auto& component : data)
        result.push_back(&component);
    return result;
}

template<typename T>
std::span<const EntityID> Scene::GetEntities() const {
    return GetArray<T>().Entities();
}

// -----------------------------------------------------------------------
// GameObject template 本体 (Scene が完全型になったあとに定義)
// -----------------------------------------------------------------------

template<typename T>
T& GameObject::AddComponent(T component) {
    assert(m_scene && "GameObject is not attached to a Scene");
    return m_scene->AddComponent<T>(m_id, std::move(component));
}

template<typename T>
T* GameObject::GetComponent() {
    if (!m_scene) return nullptr;
    return m_scene->GetComponent<T>(m_id);
}

template<typename T>
void GameObject::RemoveComponent() {
    if (m_scene) m_scene->RemoveComponent<T>(m_id);
}

template<typename T, typename... Args>
T& GameObject::AddScript(Args&&... args) {
    auto* sc = GetComponent<ScriptComponent>();
    if (!sc)
        sc = &AddComponent<ScriptComponent>();
    ScriptEntry& entry = sc->scripts.emplace_back();
    entry.script = std::make_unique<T>(std::forward<Args>(args)...);
    return static_cast<T&>(*entry.script);
}

template<typename T>
T* GameObject::GetScript() {
    auto* sc = GetComponent<ScriptComponent>();
    if (!sc) return nullptr;
    for (auto& entry : sc->scripts) {
        if (!entry.script) continue;
        if (std::string_view(entry.script->GetTypeName()) == T::TYPE_NAME)
            return static_cast<T*>(entry.script.get());
    }
    return nullptr;
}

template<typename T>
std::vector<GameObject*> GameObject::FindObjectsOfType() {
    return {};  // Application::Get().GetSceneManager().GetActive() 追加後に実装
}

// -----------------------------------------------------------------------
// Script::GetComponent<T> template 本体
// WHY: Script.hpp は Scene.hpp をインクルードできない (循環依存) ため、
//      GameObject が完全型になるこのタイミングで定義する。
//      同様のパターンは GameObject::GetComponent<T>() でも採用している。
// -----------------------------------------------------------------------
template<typename T>
T* Script::GetComponent() const
{
    return m_gameObject ? m_gameObject->GetComponent<T>() : nullptr;
}

template<typename T>
GameObject* ScriptSceneProxy::FindObjectOfType() const
{
    if (!script || !script->m_scene) return nullptr;
    auto objects = script->m_scene->FindObjectsOfType<T>();
    return objects.empty() ? nullptr : objects.front();
}

template<typename T>
std::vector<GameObject*> ScriptSceneProxy::FindObjectsOfType() const
{
    return script && script->m_scene ? script->m_scene->FindObjectsOfType<T>() : std::vector<GameObject*>{};
}

template<typename T>
T* ScriptSceneProxy::GetScript() const
{
    return script && script->m_gameObject ? script->m_gameObject->GetScript<T>() : nullptr;
}

template<typename T>
T* ScriptSceneProxy::GetScript(GameObject& go) const
{
    return go.GetScript<T>();
}

template<typename T>
T* ScriptSceneProxy::GetComponent() const
{
    return script ? script->GetComponent<T>() : nullptr;
}

template<typename T>
T& ScriptSceneProxy::GetOrAddComponent() const
{
    assert(script && script->m_gameObject && "Script context is not set");
    if (auto* component = script->m_gameObject->GetComponent<T>())
        return *component;
    return script->m_gameObject->AddComponent<T>();
}

template<typename T>
T& ScriptSceneProxy::RequireComponent() const
{
    auto* component = GetComponent<T>();
    assert(component && "Required component is missing");
    return *component;
}

} // namespace fbzz::scene
