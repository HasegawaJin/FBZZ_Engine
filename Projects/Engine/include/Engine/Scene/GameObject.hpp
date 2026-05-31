// FBZZ Engine
// GameObject.hpp | fbzz::scene
// Unity ライクな OOP ラッパー
// Scene が unique_ptr で所有し、ComponentArray への入口を提供する。
// 親子関係は EntityID で持ち、TransformSystem が world 値を更新する。
#pragma once
#include "Entity.hpp"
#include "Transform.hpp"
#include <string>
#include <vector>

namespace fbzz::scene {

class Scene;

class GameObject {
public:
    // Unity: gameObject.name / .tag (直接変数)
    std::string name       = "GameObject";
    std::string tag        = "Untagged";
    // クロスオブジェクト参照 (IK Pole/Target 等) はリネームに耐えられるよう
    // UUID v4 を永続識別子として持つ。Scene::CreateGameObject で自動生成される。
    std::string instanceId;
    int layer = 0;

    // Unity: gameObject.transform (常に存在。ComponentArray には入れない)
    Transform transform;

    // Unity: SetActive / activeSelf
    void SetActive(bool active);
    bool activeSelf() const;

    // Unity: CompareTag
    bool CompareTag(const std::string& t) const;

    // Unity: AddComponent<T> / GetComponent<T>
    // template 本体は Scene.hpp の末尾で定義する (Scene が完全型である必要があるため)
    template<typename T> T& AddComponent(T component = {});
    template<typename T> T* GetComponent();
    template<typename T> void RemoveComponent();
    template<typename T, typename... Args> T& AddScript(Args&&... args);
    template<typename T> T* GetScript();

    // Unity: transform.SetParent / childCount / GetChild
    void        SetParent(GameObject& parent);
    bool        SetParent(GameObject* parent);
    bool        ClearParent();
    bool        IsDescendantOf(const GameObject& ancestor) const;
    GameObject* GetParent()         const;
    int         GetChildCount()     const;
    GameObject* GetChild(int index) const;

    // Unity: GameObject.Find / FindWithTag / FindObjectsOfType (static)
    static GameObject*              Find(const std::string& n);
    static GameObject*              FindByGuid(const std::string& guid);
    static GameObject*              FindWithTag(const std::string& t);
    template<typename T>
    static std::vector<GameObject*> FindObjectsOfType();

    // Unity: Object.Destroy(go, delay)
    // delay=0 → 次フレーム末尾で削除 / delay>0 → 毎フレーム減算後に削除
    static void Destroy(GameObject& go, float delay = 0.0f);

    bool     IsValid() const;
    EntityID GetID()   const { return m_id; }

private:
    EntityID              m_id       = EntityID::INVALID;
    bool                  m_isActive = true;
    EntityID              m_parent   = EntityID::INVALID;
    std::vector<EntityID> m_children;
    Scene*                m_scene    = nullptr; // 非所有参照

    friend class Scene;
};

} // namespace fbzz::scene
