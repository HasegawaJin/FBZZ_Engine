/// @file    Scene.hpp
/// @brief   GameObject 所有と ComponentArray 管理。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note Destroy は遅延キューを通し、フレーム中の参照破壊を避ける。
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/UserRenderPass.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include "Entity.hpp"
#include "ComponentArray.hpp"
#include "Transform.hpp"
#include "GameObject.hpp"
#include "ComponentTypes.hpp"
#include "ScriptComponent.hpp"
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

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {

template<typename... Ts> class SceneView;

/// @brief Script から要求されたデバッグ描画の形状種別。
/// @note Managed bridge と Scene scheduler の間で共有する値。既存の数値はシリアライズ契約のため変更しない。
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

class GameObjectRange {
    using VecT = std::vector<std::unique_ptr<GameObject>>;
    VecT::iterator m_begin, m_end;
public:
    GameObjectRange(VecT::iterator b, VecT::iterator e) : m_begin(b), m_end(e) {}
    struct Iterator {
        VecT::iterator it;
        VecT::iterator end;
        Iterator(VecT::iterator current, VecT::iterator last) : it(current), end(last) { Advance(); }
        void Advance() { while (it != end && !*it) ++it; }
        GameObject& operator*() const { return **it; }
        Iterator& operator++() { ++it; Advance(); return *this; }
        bool operator!=(const Iterator& other) const { return it != other.it; }
    };
    Iterator begin() { return {m_begin, m_end}; }
    Iterator end() { return {m_end, m_end}; }
};

class Scene {
public:
    /// @note EntityID の数値表現限界。保存領域は実際の slot 数に応じて拡張する。
    static constexpr uint32_t MAX_ENTITIES = EntityID::INVALID_INDEX;

    FBZZ_ENGINE_API Scene();
    /// @note Component が個体ごとに確保した GPU リソースは ResourceManager 側の実体のため、Component 破棄だけでは返らない。畳むときに必ず返す口をここに置く。
    FBZZ_ENGINE_API ~Scene();
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    /// @pre 移動元と移動先で個体破棄 callback を実行していないこと。
    FBZZ_ENGINE_API Scene(Scene&& other) noexcept;
    /// @pre 移動元と移動先で個体破棄 callback を実行していないこと。
    FBZZ_ENGINE_API Scene& operator=(Scene&& other) noexcept;

    /// @note 保存しない寿命識別値。Clear / 移動代入で更新し、再利用された EntityID と区別する。
    [[nodiscard]] uint64_t GetRenderSceneGeneration() const { return m_renderSceneGeneration; }
    /// @note ホストの実行資源を寿命 ID で借用する。保存・個体複製の対象外で、Clear 後も同じホストへ接続する。
    FBZZ_ENGINE_API void SetResourceManager(renderer::ResourceManager* resources);
    [[nodiscard]] FBZZ_ENGINE_API renderer::ResourceManager* GetResourceManager() const;
    /// @note Parking suspends owned event delivery without changing authoring active/enabled fields or destroying cached runtime objects.
    void SetExecutionParked(bool parked) { m_executionParked = parked; }
    [[nodiscard]] bool IsExecutionParked() const { return m_executionParked; }

    /// @return EntityID の数値表現限界なら nullptr。既存の割り当て状態は変更しない。
    /// @see Docs/design/scene-capacity.md
    [[nodiscard]] FBZZ_ENGINE_API GameObject* TryCreateGameObject(const std::string& name = "GameObject");

    /// @pre EntityID の数値表現内で作成できること。
    /// @note 外部入力や実行中の生成には TryCreateGameObject を使う。
    FBZZ_ENGINE_API GameObject& CreateGameObject(const std::string& name = "GameObject");

    [[nodiscard]] FBZZ_ENGINE_API size_t RemainingEntityCapacity() const;
    /// @note 削除済みを含む index 領域の実際の長さ。Entity index を添字にする作業領域へ使う。
    [[nodiscard]] size_t EntitySlotCount() const { return m_nextIndex; }
    /// @note 単一スレッドでの事前確認。枠を予約しないため、生成前の別処理による消費は防がない。
    [[nodiscard]] FBZZ_ENGINE_API bool CanCreateGameObjects(size_t count = 1) const;

    /// @note ここは参照解決 (保存・プレハブ・エディター) 用で、無効な GameObject も返す。ゲームスクリプトは ScriptSceneProxy の Find 系 (既定で有効な物だけ) を使う。
    FBZZ_ENGINE_API GameObject*              Find(const std::string& name)     const;
    FBZZ_ENGINE_API GameObject*              FindByGuid(const std::string& guid) const;
    FBZZ_ENGINE_API GameObject*              FindWithTag(const std::string& t)  const;
    FBZZ_ENGINE_API GameObject*              FindWithLayer(int layer)            const;
    FBZZ_ENGINE_API std::vector<GameObject*> FindAllWithTag(const std::string& t) const;
    FBZZ_ENGINE_API std::vector<GameObject*> FindAllWithLayer(int layer)          const;
    template<typename T>
    std::vector<GameObject*> FindObjectsOfType()               const;

    FBZZ_ENGINE_API std::vector<GameObject*> GetRootGameObjects() const;

    FBZZ_ENGINE_API GameObjectRange GameObjects();
    size_t GameObjectCount() const { return m_gameObjectCount; }

    /// @note 保存・Undo のフラット順を維持する階層操作。
    FBZZ_ENGINE_API bool DestroyGameObject(EntityID id);
    /// @note 入力順に子孫から退役し、所有配列は一度だけ整理する。失効 ID と重複 ID は無視する。
    /// @note callback が Scene 世代を変えた場合は旧要求の続きを実行しない。
    FBZZ_ENGINE_API void DestroyGameObjects(std::span<const EntityID> ids);
    /// @note Stale IDs are ignored. Every surviving child must belong to the supplied ownership set.
    [[nodiscard]] FBZZ_ENGINE_API bool CanDestroyOwnedGameObjects(std::span<const EntityID> ids) const;
    /// @note Retires owned children before parents and refuses foreign descendants, including those attached by destruction callbacks.
    /// @return False preserves every foreign object; callbacks may already have retired some owned children.
    [[nodiscard]] FBZZ_ENGINE_API bool DestroyOwnedGameObjects(std::span<const EntityID> ids);
    FBZZ_ENGINE_API bool MoveGameObject(EntityID id, int offset);
    FBZZ_ENGINE_API bool MoveGameObjectToIndex(EntityID id, size_t newIndex);
    /// @brief ルート GO をルート同士の並び順で newRootIndex 位置へ移動する。Hierarchy のドラッグ並べ替え用。
    /// @note ルートの表示順はフラット配列の出現順で決まるため、子 GO の SetSiblingIndex とは別に Scene 側で並べ替える。
    FBZZ_ENGINE_API bool SetRootSiblingIndex(EntityID id, int newRootIndex);
    /// @brief 親の m_children 並び替え後に、フラット配列上の兄弟順序を同期させる。
    /// @note シリアライザ (保存 / Undo スナップショット / Play 復元) は flat 順で SetParent を再生して子リストを再構築するため、flat 順の兄弟順序が正本になる。
    FBZZ_ENGINE_API bool SyncSiblingFlatOrder(EntityID id);

    template<typename... Ts>
    SceneView<Ts...> View();

    FBZZ_ENGINE_API bool IsValid(EntityID id) const;

    /// @note フレーム末尾で呼び、走査中の Component 参照を無効化しない。
    FBZZ_ENGINE_API void FlushDestroyQueue(float dt);

    FBZZ_ENGINE_API void Clear();

    /// @note SceneSerializer の [environment] に保存する。同一フレーム内の変更は InvalidateFlowFrame が必要。
    [[nodiscard]] SceneEnvironment&       Environment()       { return m_environment; }
    [[nodiscard]] const SceneEnvironment& Environment() const { return m_environment; }

    /// @note スケジューラを回さない preview でも使えるよう、古いフレームは遅延収集する。
    [[nodiscard]] FBZZ_ENGINE_API FlowFieldFrame& FlowFrame();
    FBZZ_ENGINE_API void InvalidateFlowFrame();

    FBZZ_ENGINE_API renderer::PostProcessSettings& GetRuntimePostProcessSettings();
    FBZZ_ENGINE_API const renderer::PostProcessSettings* TryGetRuntimePostProcessSettings() const;
    FBZZ_ENGINE_API void SetRuntimePostProcessSettings(const renderer::PostProcessSettings& settings);
    FBZZ_ENGINE_API void ClearRuntimePostProcessSettings();

    /// @brief Script から RenderGraph へ追加するパスを 1 フレーム分キューに積む。
    /// @note Script が RenderSystem 内部の登録順に直接依存せず、意図した挿入点だけを宣言できるようにする。
    FBZZ_ENGINE_API void QueueUserRenderPass(UserRenderPassDesc desc);
    FBZZ_ENGINE_API void ClearUserRenderPasses();
    FBZZ_ENGINE_API const std::vector<UserRenderPassDesc>& GetUserRenderPasses() const;

    /// @brief ScriptDebugProxy から来た描画要求を RenderSystem まで保持する。
    /// @note DebugDraw は BeginFrame/Flush の間でしか使えないため、OnUpdate から即時描画せずキューに積む。
    FBZZ_ENGINE_API void QueueScriptDebugDraw(ScriptDebugDrawCommand command);
    FBZZ_ENGINE_API void TickScriptDebugDrawCommands(float dt);
    FBZZ_ENGINE_API const std::vector<ScriptDebugDrawCommand>& GetScriptDebugDrawCommands() const;

    /// @name GameObject / SceneView の template 本体から呼ばれる内部 API
    /// @{

    /// @note 追加は既存 Component のアドレスを保つ。Entity 走査 Span は追加で失効し得る。
    template<typename T> T&   AddComponent(EntityID id, T component);
    template<typename T> T*   GetComponent(EntityID id);
    /// @return Component row の追加時 identity。未存在・失効 Entity は 0。mutable array を公開しない。
    template<typename T> [[nodiscard]] uint64_t GetComponentLifetime(EntityID id) const { return GetArray<T>().GetLifetime(id); }
    template<typename T> bool HasComponent(EntityID id) const;
    /// @note 未存在の Component と失効・無効な Entity の除去は何も変更しない。
    template<typename T> void RemoveComponent(EntityID id);
    template<typename T> std::vector<T*> GetComponents();
    template<typename T> std::vector<const T*> GetComponents() const;

    FBZZ_ENGINE_API void DuplicateComponents(EntityID src, EntityID dst);
    /// @brief Component を受動的に複製する。Script は保存値だけを作り、forest 後段まで実体化しない。
    FBZZ_ENGINE_API void DuplicateComponentsPassive(EntityID src, EntityID dst);
    /// @brief 別 Scene 上の src から dst へ全 Component をコピーする (Prefab / Clipboard 用)。
    /// @note Prefab は一時 Scene に通常ロードしてから現在の Scene へ追加するため、Scene 内複製だけでは全 Component 対応を共有できない。
    FBZZ_ENGINE_API void CopyComponentsFrom(const Scene& srcScene, EntityID src, EntityID dst);
    /// @brief 別 Scene から受動的に Component を複製する。Script callback は後段へ遅延する。
    FBZZ_ENGINE_API void CopyComponentsFromPassive(const Scene& srcScene, EntityID src, EntityID dst);
    /// @brief 複製 forest の参照再対応後に Script 実体と保存値を準備する。
    FBZZ_ENGINE_API void PreparePassiveCopiedScripts(std::span<const EntityID> owners);
    /// @brief 全参照再対応後に managed cache 解決と Script callback を実行する。
    FBZZ_ENGINE_API void FinalizePassiveCopiedScripts(std::span<const EntityID> owners);

    template<typename T>
    std::span<const EntityID> GetEntities() const;

    /// @pre callback 中に Component の追加、削除、Clear を行わないこと。
    /// @note 同じ添字の EntityID と Component を chunk ごとの連続 span で渡す。
    template<typename T, typename Fn>
    void ForEachComponentChunk(Fn&& callback) { GetArray<T>().ForEachChunk(std::forward<Fn>(callback)); }
    template<typename T, typename Fn>
    void ForEachComponentChunk(Fn&& callback) const { GetArray<T>().ForEachChunk(std::forward<Fn>(callback)); }

    FBZZ_ENGINE_API GameObject* GetGameObject(EntityID id) const;
    /// @note GameObjects と同じ相対順に並べ、重複は保持する。失効 ID は末尾へ index・generation 順に置く。
    /// @pre 並べ替え中に Scene の生成・破棄・所有順の変更を行わないこと。
    FBZZ_ENGINE_API void SortEntitiesBySceneOrder(std::span<EntityID> entities) const;
    enum class RendererEntitySet : uint8_t { GEOMETRY, SKINNED, GEOMETRY_AND_TERRAIN };
    /// @brief Renderer を持つ Entity を GameObjects と同じ相対順で重複なくコピーする。
    /// @note 非アクティブ・無効 Renderer も含む。所属と所有位置の構造識別値だけを再利用し、Component の値や借用 span を保持しない。
    /// @note 呼出し後の Scene 変更で output は変わらない。利用時は現在の Entity と Component を再解決する。
    /// @pre コピー中に Component の所属や Scene の所有順を並行変更しないこと。
    FBZZ_ENGINE_API void CopyRendererEntities(std::vector<EntityID>& output, RendererEntitySet set = RendererEntitySet::GEOMETRY) const;
    /// @}

private:
    uint64_t m_renderSceneGeneration = 0;
    uint64_t m_resourceOwner = 0;
    /// @note 既存個体の所有位置を変える整理・並べ替え・move で更新する。末尾生成と穴化は既存位置を変えない。
    uint64_t m_rendererOwnerRevision = 0;
    bool m_executionParked = false;
    std::vector<uint32_t> m_generations;
    uint32_t              m_nextIndex   = 0;
    std::vector<uint32_t> m_freeIndices;

    std::vector<std::unique_ptr<GameObject>> m_gameObjects;
    size_t m_gameObjectCount = 0;
    size_t m_destroyBatchDepth = 0;

    /// @note EntityID.index から非所有 GameObject 参照を引く。
    std::vector<GameObject*> m_entityToGameObject;

    struct ComponentStorage;
    std::unique_ptr<ComponentStorage> m_components;
    template<typename T> friend struct detail::ComponentStorageAccess;

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

    FBZZ_ENGINE_API EntityID TryAllocateEntity();
    FBZZ_ENGINE_API bool     DestroyImmediate(EntityID id, const std::vector<EntityID>* ownership = nullptr);
    /// @note 生存個体の相対順を保って空位置を詰め、所有位置を更新する。
    /// @see Docs/design/ecs-bulk-processing.md
    FBZZ_ENGINE_API void     CompactGameObjects();
    FBZZ_ENGINE_API void     FixupOwnership();
    FBZZ_ENGINE_API void     RemoveAllComponents(EntityID id);

    template<typename T>
    static constexpr bool IsRegistered = detail::IS_REGISTERED_COMPONENT<T>;

    template<typename T>
    ComponentArray<T>& GetArray() {
        static_assert(IsRegistered<T>,
            "T is not in ComponentList — add it to ComponentRegistry.hpp");
        return detail::ComponentStorageAccess<T>::Get(*this);
    }

    template<typename T>
    const ComponentArray<T>& GetArray() const {
        static_assert(IsRegistered<T>,
            "T is not in ComponentList — add it to ComponentRegistry.hpp");
        return detail::ComponentStorageAccess<T>::Get(*this);
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

    FBZZ_ENGINE_API void CopyScriptComponentFrom(const Scene& srcScene, EntityID src, EntityID dst);
    FBZZ_ENGINE_API void CopyScriptComponentPassive(const Scene& srcScene, EntityID src, EntityID dst);

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
    T* component = GetComponent<T>(id);
    if (!component) return;
    ReleaseComponentGpuResources(*this, id, *component);
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
    /// @note Script 派生型とその型情報を公開する基底は `FbzzAsType` で引ける型として扱う。
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
