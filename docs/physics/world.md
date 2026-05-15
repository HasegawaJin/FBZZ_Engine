# Physics / World

`fbzz::physics::World`。物理シミュレーション全体を管理するトップレベルクラス。

---

## クラス定義

```cpp
namespace fbzz::physics {

class World {
public:
    void AddBody(std::shared_ptr<RigidBody> body);
    void RemoveBody(const std::shared_ptr<RigidBody>& body);

    // 1 ステップ進める。dt = 経過秒 (例: 1/60)
    void Step(float dt);

    void SetGravity(const math::Vector3& gravity);
    math::Vector3 GetGravity() const { return m_gravity; }

    const std::vector<std::shared_ptr<RigidBody>>& GetBodies() const;

private:
    void BroadPhase();      // AABB で衝突候補ペアを収集
    void NarrowPhase();     // 詳細判定 → ContactPoint 生成
    void Resolve();         // インパルスで速度を修正
    void Integrate(float dt);

    math::Vector3 m_gravity = { 0.0f, -9.81f, 0.0f };
    std::vector<std::shared_ptr<RigidBody>> m_bodies;
    std::vector<CollisionPair> m_collisionPairs;
};

} // namespace fbzz::physics
```

---

## ゲームループとの統合

`Application::Run()` の中で毎フレーム `Step(dt)` を呼ぶ。

```cpp
// Application::Run() 内
float dt = CalcDeltaTime();
Input::Update();
m_scene->Update(dt);
m_physicsWorld->Step(dt);         // 物理ステップ
m_renderer->BeginFrame();
m_scene->Render(*m_renderer);
m_renderer->EndFrame();
```

---

## シミュレーションループの順序

```
World::Step(dt)
│
├─ Integrate(dt)
│     各 RigidBody に重力を加え、速度・位置を更新
│
├─ BroadPhase()
│     全ボディの AABB を更新
│     重なりがある組み合わせを m_collisionPairs に追加
│
├─ NarrowPhase()
│     各ペアに対して詳細判定
│     ContactPoint (位置, 法線, 貫通深度) を生成
│
└─ Resolve()
      ContactPoint を元にインパルスを計算し速度を修正
      摩擦・反発係数を考慮する
```

---

## ファイル構成

```
physics/
├── include/physics/
│   └── World.hpp
└── src/
    └── World.cpp
```
