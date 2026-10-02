/// @file    Scene.hpp
/// @brief   GameObject 所有と ComponentArray 管理。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note Destroy は遅延キューを通し、フレーム中の参照破壊を避ける。
#pragma once
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include "Entity.hpp"
#include "ComponentArray.hpp"
#include "Transform.hpp"
#include "GameObject.hpp"
#include "ComponentRegistry.hpp"
#include "ScriptFactory.hpp"
#include "SceneRenderResources.hpp"
#include <Engine/Scene/Environment/SceneEnvironment.hpp>
#include <Engine/Scene/Fields/FlowFieldFrame.hpp>
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

/// @brief Script から要求されたデバッグ描画の形状種別。
/// @note 値の並びは DLL 境界を越える。末尾へ足し、kScriptVtableAbiVersion を上げる。
enum class ScriptDebugDrawType {
    Line,
    Sphere,
    Box,
    Ray,
    Arrow,       ///< @note from=a, to=b, headLength=radius, headRadius=halfExtents.x
    Cone,        ///< @note apex=a, direction=b, height=halfExtents.x, baseRadius=radius
    OrientedBox, ///< @note center=a, halfExtents, rotation
    Capsule,     ///< @note center=a, radius, halfHeight=halfExtents.x, rotation (軸はローカル Y)
    Circle,      ///< @note center=a, normal=b, radius
    Arc,         ///< @note center=a, normal=b, fromDirection=halfExtents, radius, angle [rad]
};

/// @brief OnUpdate など任意のタイミングで発行されたデバッグ描画要求。
/// @note DebugDraw は描画パスの区間でしか使えないので、Script は要求を積むだけにする。
struct ScriptDebugDrawCommand {
    ScriptDebugDrawType type = ScriptDebugDrawType::Line;
    math::Vector3 a = math::Vector3::ZERO;
    math::Vector3 b = math::Vector3::ZERO;
    math::Vector3 halfExtents = math::Vector3::ZERO;
    math::Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
    float radius = 0.0f;
    float duration = 0.0f;
    uint64_t frameCreated = 0;
    math::Quaternion rotation = math::Quaternion::Identity();
    float angle = 0.0f;
    bool depthTest = false; ///< @note true ならシーン深度で遮蔽される (HDR 段で描く)。
};

/// @note ComponentList を `tuple<ComponentArray<Ts>...>` へ展開する。
namespace detail {

template<typename Tuple> struct ArrayTupleHelper;
template<typename... Ts>
struct ArrayTupleHelper<std::tuple<Ts...>> {
    using type = std::tuple<ComponentArray<Ts>...>;
};
template<typename Tuple>
using ArrayTuple = typename ArrayTupleHelper<Tuple>::type;

/// @note ComponentList に未登録の型を compile-time で拒否する。
template<typename T, typename Tuple> struct IsInList;
template<typename T, typename... Ts>
struct IsInList<T, std::tuple<Ts...>>
    : std::disjunction<std::is_same<T, Ts>...> {};

} /// @note namespace detail

/// @note Scene が所有する GameObject を非所有参照で走査する。
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

class Scene {
public:
    static constexpr uint32_t MAX_ENTITIES = ComponentArray<uint8_t>::MAX;

    Scene();
    /// @note Component が個体ごとに確保した GPU リソースは ResourceManager 側の実体のため、Component 破棄だけでは返らない。畳むときに必ず返す口をここに置く。
    ~Scene();
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&& other) noexcept;
    Scene& operator=(Scene&& other) noexcept;

    /// @note 保存しない寿命識別値。Clear / 移動代入で更新し、再利用された EntityID と区別する。
    [[nodiscard]] uint64_t GetRenderSceneGeneration() const { return m_renderSceneGeneration; }

    GameObject& CreateGameObject(const std::string& name = "GameObject");

    /// @note ここは参照解決 (保存・プレハブ・エディター) 用で、無効な GameObject も返す。ゲームスクリプトは ScriptSceneProxy の Find 系 (既定で有効な物だけ) を使う。
    GameObject*              Find(const std::string& name)     const;
    GameObject*              FindByGuid(const std::string& guid) const;
    GameObject*              FindWithTag(const std::string& t)  const;
    GameObject*              FindWithLayer(int layer)            const;
    std::vector<GameObject*> FindAllWithTag(const std::string& t) const;
    std::vector<GameObject*> FindAllWithLayer(int layer)          const;
    template<typename T>
    std::vector<GameObject*> FindObjectsOfType()               const;

    std::vector<GameObject*> GetRootGameObjects() const;

    GameObjectRange GameObjects();
    size_t GameObjectCount() const { return m_gameObjects.size(); }

    /// @note 保存・Undo のフラット順を維持する階層操作。
    bool DestroyGameObject(EntityID id);
    bool MoveGameObject(EntityID id, int offset);
    bool MoveGameObjectToIndex(EntityID id, size_t newIndex);
    /// @brief ルート GO をルート同士の並び順で newRootIndex 位置へ移動する。Hierarchy のドラッグ並べ替え用。
    /// @note ルートの表示順はフラット配列の出現順で決まるため、子 GO の SetSiblingIndex とは別に Scene 側で並べ替える。
    bool SetRootSiblingIndex(EntityID id, int newRootIndex);
    /// @brief 親の m_children 並び替え後に、フラット配列上の兄弟順序を同期させる。
    /// @note シリアライザ (保存 / Undo スナップショット / Play 復元) は flat 順で SetParent を再生して子リストを再構築するため、flat 順の兄弟順序が正本になる。
    bool SyncSiblingFlatOrder(EntityID id);

    template<typename... Ts>
    SceneView<Ts...> View();

    bool IsValid(EntityID id) const;

    /// @note フレーム末尾で呼び、走査中の Component 参照を無効化しない。
    void FlushDestroyQueue(float dt);

    void Clear();

    /// @note SceneSerializer の [environment] に保存する。同一フレーム内の変更は InvalidateFlowFrame が必要。
    [[nodiscard]] SceneEnvironment&       Environment()       { return m_environment; }
    [[nodiscard]] const SceneEnvironment& Environment() const { return m_environment; }

    /// @note スケジューラを回さない preview でも使えるよう、古いフレームは遅延収集する。
    [[nodiscard]] FlowFieldFrame& FlowFrame();
    void InvalidateFlowFrame();

    renderer::PostProcessSettings& GetRuntimePostProcessSettings();
    const renderer::PostProcessSettings* TryGetRuntimePostProcessSettings() const;
    void SetRuntimePostProcessSettings(const renderer::PostProcessSettings& settings);
    void ClearRuntimePostProcessSettings();

    /// @brief Script から RenderGraph へ追加するパスを 1 フレーム分キューに積む。
    /// @note Script が RenderSystem 内部の登録順に直接依存せず、意図した挿入点だけを宣言できるようにする。
    void QueueUserRenderPass(UserRenderPassDesc desc);
    void ClearUserRenderPasses();
    const std::vector<UserRenderPassDesc>& GetUserRenderPasses() const;

    /// @brief ScriptDebugProxy から来た描画要求を RenderSystem まで保持する。
    /// @note DebugDraw は BeginFrame/Flush の間でしか使えないため、OnUpdate から即時描画せずキューに積む。
    void QueueScriptDebugDraw(ScriptDebugDrawCommand command);
    void TickScriptDebugDrawCommands(float dt);
    const std::vector<ScriptDebugDrawCommand>& GetScriptDebugDrawCommands() const;

    /// @name GameObject / SceneView の template 本体から呼ばれる内部 API
    /// @{

    template<typename T> T&   AddComponent(EntityID id, T component);
    template<typename T> T*   GetComponent(EntityID id);
    template<typename T> bool HasComponent(EntityID id) const;
    template<typename T> void RemoveComponent(EntityID id);
    template<typename T> std::vector<T*> GetComponents();
    template<typename T> std::vector<const T*> GetComponents() const;

    void DuplicateComponents(EntityID src, EntityID dst);
    /// @brief 別 Scene 上の src から dst へ全 Component をコピーする (Prefab / Clipboard 用)。
    /// @note Prefab は一時 Scene に通常ロードしてから現在の Scene へ追加するため、Scene 内複製だけでは全 Component 対応を共有できない。
    void CopyComponentsFrom(const Scene& srcScene, EntityID src, EntityID dst);

    template<typename T>
    std::span<const EntityID> GetEntities() const;

    GameObject* GetGameObject(EntityID id) const;
    /// @}

private:
    uint64_t m_renderSceneGeneration = 0;
    uint32_t              m_generations[MAX_ENTITIES] = {};
    uint32_t              m_nextIndex   = 0;
    std::vector<uint32_t> m_freeIndices;

    std::vector<std::unique_ptr<GameObject>> m_gameObjects;

    /// @note EntityID.index から非所有 GameObject 参照を引く。
    GameObject* m_entityToGameObject[MAX_ENTITIES] = {};

    detail::ArrayTuple<ComponentList> m_arrays;

    struct DestroyEntry { EntityID id; float delay; };
    std::vector<DestroyEntry> m_destroyQueue;

    /// @brief runtime PostProcess は Script から一時的に上書きされる optional な状態。
    /// @note 値メンバにすると Clear 時の全体代入で std::vector を破棄/再構築し、レイアウト変更時のクラッシュ地点になりやすい。
    std::unique_ptr<renderer::PostProcessSettings> m_runtimePostProcessSettings;
    std::vector<UserRenderPassDesc> m_userRenderPasses;
    std::vector<ScriptDebugDrawCommand> m_scriptDebugDrawCommands;
    uint64_t m_lastScriptDebugDrawTickFrame = 0;

    /// @note environment は保存対象、flowFrame は未保存のフレームキャッシュ。
    SceneEnvironment m_environment;
    FlowFieldFrame   m_flowFrame;

    EntityID AllocateEntity();
    void     DestroyImmediate(EntityID id);
    void     FixupOwnership();
    void     RemoveAllComponents(EntityID id);

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

    template<typename T>
    static void RemoveIfHas(ComponentArray<T>& arr, EntityID id) {
        if (arr.Has(id)) arr.Remove(id);
    }

    /// @note unique_ptr を持つ ScriptComponent の複製は CopyScriptComponentFrom が担う。
    template<typename T>
    static void CopyIfHas(ComponentArray<T>& arr, EntityID src, EntityID dst) {
        if constexpr (std::is_copy_constructible_v<T>) {
            if (arr.Has(src) && !arr.Has(dst)) arr.Add(dst, arr.Get(src));
        }
    }

    template<typename T>
    static void CopyFromOtherIfHas(const ComponentArray<T>& srcArr, ComponentArray<T>& dstArr,
                                   EntityID src, EntityID dst) {
        if constexpr (std::is_copy_constructible_v<T>) {
            if (srcArr.Has(src) && !dstArr.Has(dst)) dstArr.Add(dst, srcArr.Get(src));
        }
    }

    void CopyScriptComponentFrom(const Scene& srcScene, EntityID src, EntityID dst);

    template<typename... Ts> friend class SceneView;
    friend class GameObject;
};

/// @note Transform は ComponentArray に入らず GameObject から取得する。
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

    /// @note 最小の ComponentArray を基準にし、該当型のない Entity の走査を抑える。
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
    /// @note Component を配列から外すと誰がハンドルを持っていたか辿れなくなるため、GPU リソースの解放はここで済ませる (放置すると定数バッファ/頂点バッファが残り続ける)。
    if (T* component = GetComponent<T>(id))
        ReleaseComponentGpuResources(*component);
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

/// @note Scene の完全型が必要な GameObject の template 定義。

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
        /// @note 名前一致でなく FbzzAsType で判定し、基底型・インターフェースでも `GetScript<T>()` で引けるようにする。多重継承でも調整済みの番地を返すため、正しい部分オブジェクトを指す。
        if (void* found = entry.script->FbzzAsType(T::TYPE_NAME))
            return static_cast<T*>(found);
    }
    return nullptr;
}

template<typename T>
std::vector<GameObject*> GameObject::FindObjectsOfType() {
    /// @note Application::Get().GetSceneManager().GetActive() 追加後に実装
    return {};
}

/// @brief `Script::GetComponent<T>` template 本体
/// @note Script.hpp は Scene.hpp を循環依存のため include できず、GameObject が完全型になるこのタイミングで定義する。`GameObject::GetComponent<T>()` も同じパターン。
template<typename T>
T* Script::GetComponent() const
{
    return m_gameObject ? m_gameObject->GetComponent<T>() : nullptr;
}

template<typename T>
GameObject* ScriptSceneProxy::FindObjectOfType(bool includeInactive) const
{
    if (!script || !script->m_scene) return nullptr;
    auto objects = FindObjectsOfType<T>(includeInactive);
    return objects.empty() ? nullptr : objects.front();
}

template<typename T>
std::vector<GameObject*> ScriptSceneProxy::FindObjectsOfType(bool includeInactive) const
{
    if (!script || !script->m_scene) return {};
    /// @note Script 派生型は ECS に登録されていないため GameObject を全走査し `GetScript<T>()` で探す。Component 型は `Scene::FindObjectsOfType<T>()` (ECS) に委譲する。
    /// @note `is_base_of<Script, T>` で判定しないのは、横断インターフェース (FBZZ_SCRIPT_INTERFACE) が Script を継承せずコンパイルが通らなくなるため。`FbzzAsType` で引ける型かで振り分ける。
    if constexpr (detail::kIsScriptQueryable<T>) {
        std::vector<GameObject*> result;
        for (auto& go : script->m_scene->GameObjects())
            if ((includeInactive || go.activeInHierarchy()) && go.template GetScript<T>())
                result.push_back(&go);
        return result;
    } else {
        auto result = script->m_scene->FindObjectsOfType<T>();
        if (!includeInactive)
            std::erase_if(result, [](const GameObject* go) { return !go->activeInHierarchy(); });
        return result;
    }
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
T* ScriptSceneProxy::GetScript(GameObject* go) const
{
    return go ? go->GetScript<T>() : nullptr;
}

template<typename T>
T* ScriptSceneProxy::GetScript(EntityID id) const
{
    if (!script || !script->m_scene) return nullptr;
    auto* go = script->m_scene->GetGameObject(id);
    return go ? go->GetScript<T>() : nullptr;
}

template<typename T>
T* ScriptSceneProxy::GetComponent() const
{
    return (script && script->m_gameObject)
        ? script->m_gameObject->GetComponent<T>() : nullptr;
}

template<typename T>
T* ScriptSceneProxy::GetComponent(GameObject& go) const
{
    return go.GetComponent<T>();
}

template<typename T>
T* ScriptSceneProxy::GetComponent(GameObject* go) const
{
    return go ? go->GetComponent<T>() : nullptr;
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

} /// @note namespace fbzz::scene
