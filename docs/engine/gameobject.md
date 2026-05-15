# Engine / GameObject & Component

Unity 同等の OOP ベース設計。`GameObject` がエンティティ、`Component` が振る舞いを担う。

---

## GameObject

```cpp
namespace fbzz::scene {

class GameObject {
public:
    explicit GameObject(std::string name);

    // Component 管理
    template<typename T, typename... Args>
    T& AddComponent(Args&&... args);

    template<typename T>
    T* GetComponent();

    template<typename T>
    const T* GetComponent() const;

    template<typename T>
    bool HasComponent() const;

    template<typename T>
    void RemoveComponent();

    // 毎フレーム
    void Update(float dt);

    // アクセサ
    const std::string& GetName()      const { return m_name; }
    Transform&         GetTransform()       { return m_transform; }
    const Transform&   GetTransform() const { return m_transform; }

private:
    std::string m_name;
    Transform   m_transform;
    std::vector<std::unique_ptr<Component>> m_components;
};

} // namespace fbzz::scene
```

### AddComponent の実装イメージ

```cpp
template<typename T, typename... Args>
T& GameObject::AddComponent(Args&&... args) {
    static_assert(std::is_base_of_v<Component, T>,
                  "T must derive from Component");
    auto& comp = m_components.emplace_back(std::make_unique<T>(std::forward<Args>(args)...));
    comp->m_owner = this;
    comp->OnAwake();
    return static_cast<T&>(*comp);
}
```

---

## Component

```cpp
namespace fbzz::scene {

class Component {
public:
    virtual ~Component() = default;

    virtual void OnAwake()          {}  // AddComponent 直後に 1 回
    virtual void OnStart()          {}  // 最初の Update フレーム前に 1 回
    virtual void OnUpdate(float dt) {}
    virtual void OnDestroy()        {}

protected:
    bool m_started = false;  // OnStart 呼び出し済みフラグ

private:
    friend class GameObject;
    GameObject* m_owner = nullptr;  // 非所有参照。所有は GameObject 側の unique_ptr
};

} // namespace fbzz::scene
```

---

## Transform

```cpp
namespace fbzz::scene {

class Transform {
public:
    // ワールド行列 (親の変換を含む)
    math::Matrix4 GetWorldMatrix() const;

    // ローカル → ワールド変換
    math::Vector3 GetWorldPosition() const;
    math::Quaternion GetWorldRotation() const;

    // 親子関係
    void SetParent(Transform* parent);
    void AddChild(Transform* child);
    void RemoveChild(Transform* child);

    math::Vector3       m_position = { 0.0f, 0.0f, 0.0f };
    math::Quaternion m_rotation;
    math::Vector3       m_scale    = { 1.0f, 1.0f, 1.0f };

    Transform*              m_parent   = nullptr;
    std::vector<Transform*> m_children;
};

} // namespace fbzz::scene
```

---

## Component ライフサイクル

```
AddComponent<T>()
  └─ OnAwake()        生成直後

最初の Update フレーム
  └─ OnStart()        1 回のみ

毎フレーム
  └─ OnUpdate(dt)

DestroyObject()
  └─ OnDestroy()      削除前
```

---

## 組み込み Component 一覧

| Component | ヘッダ | 説明 |
|-----------|--------|------|
| `MeshRenderer` | `Scene/MeshRenderer.hpp` | メッシュを描画する |
| `RigidBodyComponent` | `Scene/RigidBodyComponent.hpp` | 物理剛体を管理する |
| `ColliderComponent` | `Scene/ColliderComponent.hpp` | 衝突形状を保持する |
| `Camera` (Component 版) | `Scene/CameraComponent.hpp` | シーン内のカメラ |

---

## ファイル構成

```
engine/
├── include/engine/
│   └── Scene/
│       ├── GameObject.hpp
│       ├── Component.hpp
│       └── Transform.hpp
└── src/
    └── Scene/
        ├── GameObject.cpp
        └── Transform.cpp
```
