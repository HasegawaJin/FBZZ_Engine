// FBZZ Engine
// Scene.hpp | fbzz::scene
// GameObject の所有・ComponentArray の管理・GameObjectRange / SceneView の提供
#pragma once
#include "Entity.hpp"
#include "ComponentArray.hpp"
#include "Transform.hpp"
#include "GameObject.hpp"
#include "Components/MeshRenderer.hpp"
#include "Components/ParticleEmitter.hpp"
#include "Components/RigidBodyComponent.hpp"
#include "Components/SkyRenderer.hpp"
#include "Components/LightComponent.hpp"
#include "Components/CameraComponent.hpp"
#include "Components/AudioSourceComponent.hpp"
#include "ScriptComponent.hpp"
#include <vector>
#include <memory>
#include <string>
#include <string_view>
#include <span>
#include <cassert>
#include <type_traits>
#include <tuple>
#include <utility>

namespace fbzz::scene {

// 前方宣言 (Scene.hpp 内で定義するが、クラス宣言より前に使う場面はない)
template<typename... Ts> class SceneView;

// -----------------------------------------------------------------------
// GameObjectRange  —  scene.GameObjects() が返す Unity ライク foreach 用 range
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

    // Unity: new GameObject("name")
    GameObject& CreateGameObject(const std::string& name = "GameObject");

    // Unity: GameObject.Find 系の実体
    GameObject*              Find(const std::string& name)    const;
    GameObject*              FindWithTag(const std::string& t) const;
    template<typename T>
    std::vector<GameObject*> FindObjectsOfType()              const;

    // Unity: scene.GetRootGameObjects()
    std::vector<GameObject*> GetRootGameObjects() const;

    // Unity ライク foreach (ゲームロジック向け)
    GameObjectRange GameObjects();

    // System 向け高速マルチ Component イテレータ
    template<typename... Ts>
    SceneView<Ts...> View();

    // Entity の生死を確認する
    bool IsValid(EntityID id) const;

    // フレーム末尾で呼ぶ。delay 付き Destroy を処理し、期限切れを削除する
    void FlushDestroyQueue(float dt);

    // --- GameObject / SceneView の template 本体から呼ばれる内部 API ---

    template<typename T> T&   AddComponent(EntityID id, T component);
    template<typename T> T*   GetComponent(EntityID id);
    template<typename T> bool HasComponent(EntityID id) const;
    template<typename T> void RemoveComponent(EntityID id);

    // SceneView が entity span を取得するために使う
    template<typename T>
    std::span<const EntityID> GetEntities() const;

    // EntityID → GameObject* の O(1) 逆引き
    GameObject* GetGameObject(EntityID id) const;

private:
    // Entity 管理
    uint32_t             m_generations[MAX_ENTITIES] = {};
    uint32_t             m_nextIndex  = 0;
    std::vector<uint32_t> m_freeIndices;

    // GameObjects 所有
    std::vector<std::unique_ptr<GameObject>> m_gameObjects;

    // EntityID.index → GameObject* (非所有)
    GameObject* m_entityToGameObject[MAX_ENTITIES] = {};

    // Component 配列
    ComponentArray<MeshRenderer>       m_meshRenderers;
    ComponentArray<ParticleEmitter>    m_particleEmitters;
    ComponentArray<RigidBodyComponent> m_rigidBodies;
    ComponentArray<SkyRenderer>          m_skyRenderers;
    ComponentArray<LightComponent>       m_lightComponents;
    ComponentArray<CameraComponent>      m_cameraComponents;
    ComponentArray<AudioSourceComponent> m_audioSources;
    ComponentArray<ScriptComponent>      m_scriptComponents;

    // delay 付き Destroy キュー
    struct DestroyEntry { EntityID id; float delay; };
    std::vector<DestroyEntry> m_destroyQueue;

    EntityID AllocateEntity();
    void     DestroyImmediate(EntityID id);

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
            else if constexpr (std::is_same_v<T, MeshRenderer>)
                return scene.m_meshRenderers.Has(id);
            else if constexpr (std::is_same_v<T, ParticleEmitter>)
                return scene.m_particleEmitters.Has(id);
            else if constexpr (std::is_same_v<T, RigidBodyComponent>)
                return scene.m_rigidBodies.Has(id);
            else if constexpr (std::is_same_v<T, SkyRenderer>)
                return scene.m_skyRenderers.Has(id);
            else if constexpr (std::is_same_v<T, LightComponent>)
                return scene.m_lightComponents.Has(id);
            else if constexpr (std::is_same_v<T, CameraComponent>)
                return scene.m_cameraComponents.Has(id);
            else if constexpr (std::is_same_v<T, AudioSourceComponent>)
                return scene.m_audioSources.Has(id);
            else if constexpr (std::is_same_v<T, ScriptComponent>)
                return scene.m_scriptComponents.Has(id);
            else
                static_assert(AlwaysFalse<T>, "Component type not registered in SceneView");
        }

        template<typename T>
        static T& GetRef(Scene& scene, EntityID id) {
            if constexpr (std::is_same_v<T, Transform>)
                return scene.GetGameObject(id)->transform;
            else if constexpr (std::is_same_v<T, MeshRenderer>)
                return scene.m_meshRenderers.Get(id);
            else if constexpr (std::is_same_v<T, ParticleEmitter>)
                return scene.m_particleEmitters.Get(id);
            else if constexpr (std::is_same_v<T, RigidBodyComponent>)
                return scene.m_rigidBodies.Get(id);
            else if constexpr (std::is_same_v<T, SkyRenderer>)
                return scene.m_skyRenderers.Get(id);
            else if constexpr (std::is_same_v<T, LightComponent>)
                return scene.m_lightComponents.Get(id);
            else if constexpr (std::is_same_v<T, CameraComponent>)
                return scene.m_cameraComponents.Get(id);
            else if constexpr (std::is_same_v<T, AudioSourceComponent>)
                return scene.m_audioSources.Get(id);
            else if constexpr (std::is_same_v<T, ScriptComponent>)
                return scene.m_scriptComponents.Get(id);
            else
                static_assert(AlwaysFalse<T>, "Component type not registered in SceneView");
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
    if constexpr (std::is_same_v<T, MeshRenderer>) {
        m_meshRenderers.Add(id, std::move(component));
        return m_meshRenderers.Get(id);
    } else if constexpr (std::is_same_v<T, ParticleEmitter>) {
        m_particleEmitters.Add(id, std::move(component));
        return m_particleEmitters.Get(id);
    } else if constexpr (std::is_same_v<T, RigidBodyComponent>) {
        m_rigidBodies.Add(id, std::move(component));
        return m_rigidBodies.Get(id);
    } else if constexpr (std::is_same_v<T, SkyRenderer>) {
        m_skyRenderers.Add(id, std::move(component));
        return m_skyRenderers.Get(id);
    } else if constexpr (std::is_same_v<T, LightComponent>) {
        m_lightComponents.Add(id, std::move(component));
        return m_lightComponents.Get(id);
    } else if constexpr (std::is_same_v<T, CameraComponent>) {
        m_cameraComponents.Add(id, std::move(component));
        return m_cameraComponents.Get(id);
    } else if constexpr (std::is_same_v<T, AudioSourceComponent>) {
        m_audioSources.Add(id, std::move(component));
        return m_audioSources.Get(id);
    } else if constexpr (std::is_same_v<T, ScriptComponent>) {
        m_scriptComponents.Add(id, std::move(component));
        return m_scriptComponents.Get(id);
    } else {
        static_assert(AlwaysFalse<T>, "Component type not registered in Scene");
    }
}

template<typename T>
T* Scene::GetComponent(EntityID id) {
    if (!IsValid(id)) return nullptr;
    if constexpr (std::is_same_v<T, MeshRenderer>)
        return m_meshRenderers.Has(id) ? &m_meshRenderers.Get(id) : nullptr;
    else if constexpr (std::is_same_v<T, ParticleEmitter>)
        return m_particleEmitters.Has(id) ? &m_particleEmitters.Get(id) : nullptr;
    else if constexpr (std::is_same_v<T, RigidBodyComponent>)
        return m_rigidBodies.Has(id) ? &m_rigidBodies.Get(id) : nullptr;
    else if constexpr (std::is_same_v<T, SkyRenderer>)
        return m_skyRenderers.Has(id) ? &m_skyRenderers.Get(id) : nullptr;
    else if constexpr (std::is_same_v<T, LightComponent>)
        return m_lightComponents.Has(id) ? &m_lightComponents.Get(id) : nullptr;
    else if constexpr (std::is_same_v<T, CameraComponent>)
        return m_cameraComponents.Has(id) ? &m_cameraComponents.Get(id) : nullptr;
    else if constexpr (std::is_same_v<T, AudioSourceComponent>)
        return m_audioSources.Has(id) ? &m_audioSources.Get(id) : nullptr;
    else if constexpr (std::is_same_v<T, ScriptComponent>)
        return m_scriptComponents.Has(id) ? &m_scriptComponents.Get(id) : nullptr;
    else
        static_assert(AlwaysFalse<T>, "Component type not registered in Scene");
}

template<typename T>
bool Scene::HasComponent(EntityID id) const {
    if (!IsValid(id)) return false;
    if constexpr (std::is_same_v<T, MeshRenderer>)
        return m_meshRenderers.Has(id);
    else if constexpr (std::is_same_v<T, ParticleEmitter>)
        return m_particleEmitters.Has(id);
    else if constexpr (std::is_same_v<T, RigidBodyComponent>)
        return m_rigidBodies.Has(id);
    else if constexpr (std::is_same_v<T, SkyRenderer>)
        return m_skyRenderers.Has(id);
    else if constexpr (std::is_same_v<T, LightComponent>)
        return m_lightComponents.Has(id);
    else if constexpr (std::is_same_v<T, CameraComponent>)
        return m_cameraComponents.Has(id);
    else if constexpr (std::is_same_v<T, AudioSourceComponent>)
        return m_audioSources.Has(id);
    else if constexpr (std::is_same_v<T, ScriptComponent>)
        return m_scriptComponents.Has(id);
    else
        static_assert(AlwaysFalse<T>, "Component type not registered in Scene");
}

template<typename T>
void Scene::RemoveComponent(EntityID id) {
    if constexpr (std::is_same_v<T, MeshRenderer>)
        m_meshRenderers.Remove(id);
    else if constexpr (std::is_same_v<T, ParticleEmitter>)
        m_particleEmitters.Remove(id);
    else if constexpr (std::is_same_v<T, RigidBodyComponent>)
        m_rigidBodies.Remove(id);
    else if constexpr (std::is_same_v<T, SkyRenderer>)
        m_skyRenderers.Remove(id);
    else if constexpr (std::is_same_v<T, LightComponent>)
        m_lightComponents.Remove(id);
    else if constexpr (std::is_same_v<T, CameraComponent>)
        m_cameraComponents.Remove(id);
    else if constexpr (std::is_same_v<T, AudioSourceComponent>)
        m_audioSources.Remove(id);
    else if constexpr (std::is_same_v<T, ScriptComponent>)
        m_scriptComponents.Remove(id);
    else
        static_assert(AlwaysFalse<T>, "Component type not registered in Scene");
}

template<typename T>
std::span<const EntityID> Scene::GetEntities() const {
    if constexpr (std::is_same_v<T, MeshRenderer>)
        return m_meshRenderers.Entities();
    else if constexpr (std::is_same_v<T, ParticleEmitter>)
        return m_particleEmitters.Entities();
    else if constexpr (std::is_same_v<T, RigidBodyComponent>)
        return m_rigidBodies.Entities();
    else if constexpr (std::is_same_v<T, SkyRenderer>)
        return m_skyRenderers.Entities();
    else if constexpr (std::is_same_v<T, LightComponent>)
        return m_lightComponents.Entities();
    else if constexpr (std::is_same_v<T, CameraComponent>)
        return m_cameraComponents.Entities();
    else if constexpr (std::is_same_v<T, AudioSourceComponent>)
        return m_audioSources.Entities();
    else if constexpr (std::is_same_v<T, ScriptComponent>)
        return m_scriptComponents.Entities();
    else
        static_assert(AlwaysFalse<T>, "Component type not registered in Scene");
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

template<typename T, typename... Args>
T& GameObject::AddScript(Args&&... args) {
    auto& sc = AddComponent<ScriptComponent>();
    sc.script = std::make_unique<T>(std::forward<Args>(args)...);
    return static_cast<T&>(*sc.script);
}

template<typename T>
T* GameObject::GetScript() {
    auto* sc = GetComponent<ScriptComponent>();
    if (!sc || !sc->script) return nullptr;
    if (std::string_view(sc->script->GetTypeName()) != T::TYPE_NAME) return nullptr;
    return static_cast<T*>(sc->script.get());
}

template<typename T>
std::vector<GameObject*> GameObject::FindObjectsOfType() {
    return {};  // Application::Get().GetSceneManager().GetActive() 追加後に実装
}

} // namespace fbzz::scene
