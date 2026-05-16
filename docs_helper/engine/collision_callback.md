# Engine / 衝突コールバック

物理システムの衝突検出結果を Component に通知する仕組み。
`World::Step()` の後に `Scene` が衝突イベントを収集し、該当 Component のコールバックを呼ぶ。

---

## Component への仮想関数追加

```cpp
// Component.hpp に追加
namespace fbzz::scene {

class Component {
public:
    // 既存のライフサイクル関数 ...

    // 衝突コールバック (RigidBodyComponent を持つ GameObject のみ呼ばれる)
    virtual void OnCollisionEnter(const physics::ContactPoint& contact) {}  // 衝突開始
    virtual void OnCollisionStay (const physics::ContactPoint& contact) {}  // 衝突継続
    virtual void OnCollisionExit (const physics::ContactPoint& contact) {}  // 衝突終了
};

} // namespace fbzz::scene
```

---

## CollisionEvent 構造体

```cpp
// World.hpp に追加
namespace fbzz::physics {

struct CollisionEvent {
    std::shared_ptr<RigidBody> bodyA;
    std::shared_ptr<RigidBody> bodyB;
    ContactPoint               contact;
};

} // namespace fbzz::physics
```

---

## World の変更点

衝突ペアを前フレームと比較して Enter / Stay / Exit を判定する。

```cpp
// World.hpp に追加
class World {
public:
    // 現フレームの衝突イベントを返す (Step() 後に呼ぶ)
    const std::vector<CollisionEvent>& GetEnterEvents() const;
    const std::vector<CollisionEvent>& GetStayEvents()  const;
    const std::vector<CollisionEvent>& GetExitEvents()  const;

private:
    // 衝突ペアの識別キー (ポインタ値の小さい方を first にして順序を固定)
    using BodyPair = std::pair<RigidBody*, RigidBody*>;

    std::set<BodyPair>           m_prevPairs;    // 前フレームの衝突ペア
    std::vector<CollisionEvent>  m_enterEvents;
    std::vector<CollisionEvent>  m_stayEvents;
    std::vector<CollisionEvent>  m_exitEvents;

    void ClassifyCollisions();   // NarrowPhase 後に呼ぶ
};
```

### ClassifyCollisions の実装イメージ

```
現フレーム衝突ペア: current
前フレーム衝突ペア: prev

Enter = current - prev   (今フレーム新たに衝突したペア)
Stay  = current ∩ prev   (前フレームから継続しているペア)
Exit  = prev - current   (今フレーム離れたペア)
```

---

## Scene::DispatchCollisionEvents()

`Application::Run()` で `World::Step()` の直後に呼ぶ。

```cpp
// Scene.hpp に追加
void DispatchCollisionEvents(const physics::World& world);
```

```cpp
void Scene::DispatchCollisionEvents(const physics::World& world) {
    auto dispatch = [&](const std::vector<physics::CollisionEvent>& events,
                        auto callback) {
        for (const auto& ev : events) {
            // RigidBody::m_userData から RigidBodyComponent* を取得
            auto* compA = static_cast<RigidBodyComponent*>(ev.bodyA->m_userData);
            auto* compB = static_cast<RigidBodyComponent*>(ev.bodyB->m_userData);
            if (!compA || !compB) continue;

            // 同じ GameObject の全 Component に通知
            for (auto& comp : compA->GetOwnerComponents()) { callback(*comp, ev.contact); }
            for (auto& comp : compB->GetOwnerComponents()) { callback(*comp, ev.contact); }
        }
    };

    dispatch(world.GetEnterEvents(), [](Component& c, const physics::ContactPoint& cp) { c.OnCollisionEnter(cp); });
    dispatch(world.GetStayEvents(),  [](Component& c, const physics::ContactPoint& cp) { c.OnCollisionStay(cp); });
    dispatch(world.GetExitEvents(),  [](Component& c, const physics::ContactPoint& cp) { c.OnCollisionExit(cp); });
}
```

### Application::Run() の更新

```cpp
m_physicsWorld->Step(dt);
m_scene->DispatchCollisionEvents(*m_physicsWorld);   // ← ここで通知
m_renderer->BeginFrame();
```

---

## RigidBody::m_userData

`RigidBodyComponent::OnAwake()` で設定する (rigidbody_component.md 参照)。

```cpp
// physics/RigidBody.hpp に追加
void* m_userData = nullptr;
```

---

## 使用例

```cpp
class BallComponent : public Component {
public:
    void OnCollisionEnter(const physics::ContactPoint& contact) override {
        FBZZ_LOG_INFO("衝突! 貫通深度: %.3f", contact.depth);
        // 反発エフェクト等の処理
    }
};
```

---

## ファイル構成

衝突コールバック専用のファイルは不要。以下の既存ファイルを変更する。

```
engine/include/engine/Scene/Component.hpp    OnCollisionEnter/Stay/Exit を追加
physics/include/physics/World.hpp            CollisionEvent, GetXxxEvents() を追加
physics/include/physics/RigidBody.hpp        m_userData を追加
engine/src/Scene/Scene.cpp                   DispatchCollisionEvents() を追加
```

---

## 参考ドキュメント

- [std::set (cppreference)](https://en.cppreference.com/w/cpp/container/set) — 前フレーム衝突ペアの管理
