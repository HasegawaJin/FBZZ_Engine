# Physics / World

`fbzz::physics::World` — シミュレーション全体の管理クラス。

---

## クラス定義

```cpp
namespace fbzz::physics {

class World {
public:
    // ボディ管理
    void AddBody(std::shared_ptr<RigidBody> body);
    void RemoveBody(const std::shared_ptr<RigidBody>& body);
    const std::vector<std::shared_ptr<RigidBody>>& GetBodies() const;

    // Volume 管理
    void AddVolume(std::shared_ptr<Volume> volume);
    void RemoveVolume(const std::shared_ptr<Volume>& volume);

    // Constraint 管理
    void AddConstraint(std::shared_ptr<Constraint> constraint);
    void RemoveConstraint(const std::shared_ptr<Constraint>& constraint);

    // シミュレーション
    void Step(float dt);

    // グローバル重力 (GravityVolume が存在しない領域に適用)
    void          SetGravity(const math::Vector3& gravity);
    math::Vector3 GetGravity() const { return m_gravity; }

    // 衝突イベント (Step() 後に参照する)
    const std::vector<CollisionEvent>& GetEnterEvents() const;
    const std::vector<CollisionEvent>& GetStayEvents()  const;
    const std::vector<CollisionEvent>& GetExitEvents()  const;

private:
    void RemoveExpiredVolumes();
    void ApplyVolumes(float dt, std::vector<float>& outEffectiveDts,
                      std::vector<bool>& outInGravityVolume);
    void ApplyGlobalGravity(const std::vector<bool>& inGravityVolume);
    void ApplyConstraintForces(float dt);
    void ApplyGravitationalAttraction(float dt);
    void IntegrateBodies(const std::vector<float>& effectiveDts);
    void SolveConstraintPositions(float dt);
    void UpdateColliders();
    void BroadPhase();
    void NarrowPhase();
    void Resolve();
    void ClassifyCollisions();

    math::Vector3 m_gravity = { 0.0f, -9.81f, 0.0f };

    std::vector<std::shared_ptr<RigidBody>>  m_bodies;
    std::vector<std::shared_ptr<Volume>>     m_volumes;
    std::vector<std::shared_ptr<Constraint>> m_constraints;
    std::vector<CollisionPair>               m_collisionPairs;
    std::vector<ContactPoint>                m_contacts;

    using BodyPair = std::pair<RigidBody*, RigidBody*>;
    std::set<BodyPair>           m_prevPairs;
    std::vector<CollisionEvent>  m_enterEvents;
    std::vector<CollisionEvent>  m_stayEvents;
    std::vector<CollisionEvent>  m_exitEvents;
};

} // namespace fbzz::physics
```

---

## Step 実装イメージ

```cpp
void World::Step(float dt) {
    RemoveExpiredVolumes();

    // per-body の有効 dt を計算しつつ Volume を適用
    // inGravityVolume[i] == true のボディはグローバル重力をスキップする
    std::vector<float> effectiveDts(m_bodies.size(), dt);
    std::vector<bool>  inGravityVolume(m_bodies.size(), false);
    for (size_t i = 0; i < m_bodies.size(); ++i) {
        float timeScale = 1.0f;
        for (auto& vol : m_volumes) {
            if (!vol->m_enabled || !vol->Contains(m_bodies[i]->GetPosition())) continue;
            timeScale *= vol->GetTimeScale();
            if (vol->GetType() == VolumeType::GRAVITY)  // GravityVolume は重力を上書き
                inGravityVolume[i] = true;
        }
        effectiveDts[i] = dt * timeScale;
        for (auto& vol : m_volumes) {
            if (vol->m_enabled && vol->Contains(m_bodies[i]->GetPosition()))
                vol->Apply(*m_bodies[i], effectiveDts[i]);
        }
    }

    // GravityVolume 外のボディにだけグローバル重力を加算
    for (size_t i = 0; i < m_bodies.size(); ++i) {
        if (!m_bodies[i]->IsStatic() && !inGravityVolume[i])
            m_bodies[i]->ApplyForce(m_gravity * m_bodies[i]->GetMass());
    }
    ApplyConstraintForces(dt);
    ApplyGravitationalAttraction(dt);
    IntegrateBodies(effectiveDts);
    SolveConstraintPositions(dt);
    UpdateColliders();
    BroadPhase();
    NarrowPhase();
    Resolve();
    ClassifyCollisions();
}
```

---

## N 体重力引力

```cpp
void World::ApplyGravitationalAttraction(float dt) {
    constexpr float G = 6.674e-4f; // ゲームスケール用定数

    for (size_t i = 0; i < m_bodies.size(); ++i) {
        if (!m_bodies[i]->m_isGravitationalSource) continue;
        for (size_t j = i + 1; j < m_bodies.size(); ++j) {
            auto& a = *m_bodies[i];
            auto& b = *m_bodies[j];
            math::Vector3 dir = b.GetPosition() - a.GetPosition();
            float dist2 = math::Vector3::Dot(dir, dir);
            if (dist2 < 1e-4f) continue;
            float f = G * a.m_gravitationalMass * b.m_gravitationalMass / dist2;
            math::Vector3 force = dir.Normalized() * f;
            a.ApplyForce( force);
            b.ApplyForce(-force);
        }
    }
}
```

---

## CollisionEvent

```cpp
struct CollisionEvent {
    RigidBody* bodyA;
    RigidBody* bodyB;
};
```

`GetEnterEvents()` / `GetStayEvents()` / `GetExitEvents()` は Step() 後に参照する。
engine 側の Component (RigidBodyComponent 等) が毎フレームポーリングして
コールバックを起動する想定 (Step 5 で実装)。
