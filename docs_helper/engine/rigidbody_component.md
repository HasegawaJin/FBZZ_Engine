# Engine / RigidBodyComponent & ColliderComponent

物理システムと GameObject を橋渡しする Component 群。
`RigidBodyComponent` が `World` に剛体を登録し、物理演算の結果を毎フレーム `Transform` に反映する。

---

## RigidBodyComponent

```cpp
namespace fbzz::scene {

class RigidBodyComponent : public Component {
public:
    void OnAwake()          override;   // World に RigidBody を登録
    void OnDestroy()        override;   // World から RigidBody を除去
    void OnUpdate(float dt) override;   // 物理位置 → Transform に反映

    physics::RigidBody&                    GetBody()       { return *m_body; }
    const physics::RigidBody&              GetBody() const { return *m_body; }
    std::shared_ptr<physics::RigidBody>    GetBodyShared() { return m_body; }

    // 初期質量・静的フラグ (OnAwake 前に設定すること)
    float m_mass     = 1.0f;
    bool  m_isStatic = false;

private:
    std::shared_ptr<physics::RigidBody> m_body;
};

} // namespace fbzz::scene
```

### OnAwake / OnDestroy

```cpp
void RigidBodyComponent::OnAwake() {
    m_body = std::make_shared<physics::RigidBody>();
    m_body->SetMass(m_mass);
    m_body->m_isStatic  = m_isStatic;
    m_body->m_position  = GetOwner().GetTransform().GetWorldPosition();
    m_body->m_rotation  = GetOwner().GetTransform().GetWorldRotation();
    m_body->m_userData  = this;   // 衝突コールバック用 (collision_callback.md 参照)

    Application::Get().GetPhysicsWorld().AddBody(m_body);
}

void RigidBodyComponent::OnDestroy() {
    Application::Get().GetPhysicsWorld().RemoveBody(m_body);
}
```

### OnUpdate — 物理結果を Transform に反映

```cpp
void RigidBodyComponent::OnUpdate(float dt) {
    if (m_isStatic) return;

    Transform& t     = GetOwner().GetTransform();
    t.m_position     = m_body->GetPosition();
    t.m_rotation     = m_body->GetRotation();
}
```

---

## ColliderComponent

```cpp
namespace fbzz::scene {

class ColliderComponent : public Component {
public:
    void OnAwake()   override;   // Collider を生成して RigidBody に紐付ける
    void OnDestroy() override;

    // OnAwake 前に形状を設定すること
    enum class Shape { SPHERE, AABB };
    Shape          m_shape       = Shape::SPHERE;
    float          m_radius      = 0.5f;       // Sphere 用
    math::Vector3  m_halfExtents = { 0.5f, 0.5f, 0.5f };  // AABB 用

private:
    std::shared_ptr<physics::Collider> m_collider;
};

} // namespace fbzz::scene
```

### OnAwake — 形状に応じた Collider を生成

```cpp
void ColliderComponent::OnAwake() {
    auto* rb = GetOwner().GetComponent<RigidBodyComponent>();
    assert(rb && "ColliderComponent requires RigidBodyComponent on the same GameObject");

    switch (m_shape) {
        case Shape::SPHERE:
            m_collider = std::make_shared<physics::SphereCollider>(m_radius);
            break;
        case Shape::AABB:
            m_collider = std::make_shared<physics::AABBCollider>(m_halfExtents);
            break;
    }

    m_collider->m_body = rb->GetBodyShared();
    rb->GetBody().SetCollider(m_collider);
}
```

---

## Component::GetOwner()

`RigidBodyComponent` / `ColliderComponent` の OnAwake から `GetOwner()` を呼ぶため、
`Component` に以下を追加する。

```cpp
// Component.hpp に追加 (protected)
protected:
    GameObject& GetOwner() {
        assert(m_owner && "Component is not attached to a GameObject");
        return *m_owner;
    }
```

---

## 使用例

```cpp
// AddComponent は順番が重要: RigidBody → Collider
auto& obj = m_scene->CreateObject("Ball");
obj.GetTransform().m_position = { 0.0f, 10.0f, 0.0f };

auto& rb  = obj.AddComponent<RigidBodyComponent>();
rb.m_mass = 1.0f;

auto& col = obj.AddComponent<ColliderComponent>();
col.m_shape  = ColliderComponent::Shape::SPHERE;
col.m_radius = 0.5f;

// 地面 (静的)
auto& ground = m_scene->CreateObject("Ground");
auto& grb    = ground.AddComponent<RigidBodyComponent>();
grb.m_isStatic = true;
auto& gcol     = ground.AddComponent<ColliderComponent>();
gcol.m_shape   = ColliderComponent::Shape::AABB;
gcol.m_halfExtents = { 10.0f, 0.5f, 10.0f };
```

---

## ファイル構成

```
engine/
├── include/engine/
│   └── Scene/
│       ├── RigidBodyComponent.hpp
│       └── ColliderComponent.hpp
└── src/
    └── Scene/
        ├── RigidBodyComponent.cpp
        └── ColliderComponent.cpp
```
