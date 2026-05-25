// FBZZ Engine
// World.cpp | fbzz::physics
// 物理シミュレーション世界の管理と Step 実行
#include <Physics/World.hpp>
#include <Physics/SphereCollider.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <utility>

namespace fbzz::physics 
{
    void World::SetBodies(std::vector<std::shared_ptr<RigidBody>> bodies)
    {
        m_bodies = std::move(bodies);
    }

    const std::vector<std::shared_ptr<RigidBody>>& World::GetBodies() const
    {
        return m_bodies;
    }

    void World::SetColliders(std::vector<ColliderInstance> colliders)
    {
        m_colliders = std::move(colliders);
    }

    void World::SetVolumes(std::vector<std::shared_ptr<Volume>> volumes)
    {
        m_volumes = std::move(volumes);
    }

    void World::AddConstraint(std::shared_ptr<Constraint> constraint)
    {
        m_constraints.push_back(std::move(constraint));
    }

    const std::vector<std::shared_ptr<Constraint>>& World::GetConstraints() const
    {
        return m_constraints;
    }

    void World::SetGravity(const math::Vector3& gravity)
    {
        m_gravity = gravity;
    }

    void World::SetSubsteps(int substeps)
    {
        m_substeps = std::clamp(substeps, 1, 32);
    }

    void World::Step(float dt, std::function<bool(int, int)> layerFilter)
    {
        m_layerFilter = std::move(layerFilter);
        const int substeps = std::max(m_substeps, 1);
        const float subDt = dt / static_cast<float>(substeps);
        std::vector<float> effectiveDts;

        for (int s = 0; s < substeps; ++s)
        {
            RemoveExpiredVolumes();
            for (auto& volume : m_volumes)
                if (volume) volume->Tick(subDt);
            ApplyForcesAndVolumes(subDt, effectiveDts);
            ApplyConstraintForces(subDt);
            ApplyGravitationalAttraction();
            CCDPhase(subDt);
            IntegrateBodies(effectiveDts);
            SolveConstraintPositions(subDt);
            UpdateColliders();
            BroadPhase();
            NarrowPhase(s == 0); // WarmStart は最初のサブステップのみ
            Resolve();
        }

        // フレーム末尾に蓄積インパルスを保存し古いキャッシュを削除する
        m_contactCache.UpdateCache(m_contacts);
        m_contactCache.PurgeStale();

        ClassifyCollisions();
    }

    void World::RemoveExpiredVolumes()
    {
        m_volumes.erase(std::remove_if(m_volumes.begin(), m_volumes.end(),
            [](const std::shared_ptr<Volume>& volume) {
                return !volume || volume->IsExpired();
            }),
            m_volumes.end());
    }

    void World::ApplyForcesAndVolumes(float dt, std::vector<float>& effectiveDts)
    {
        effectiveDts.assign(m_bodies.size(), dt);

        for (size_t i = 0; i < m_bodies.size(); ++i)
        {
            auto& body = m_bodies[i];
            bool gravityOverridden = false;

            for (auto& volume : m_volumes)
            {
                if (!volume || !volume->Contains(body->GetPosition())) continue;

                effectiveDts[i] = effectiveDts[i] * volume->GetTimeScale();
                gravityOverridden = gravityOverridden || volume->OverridesGravity();
                volume->Apply(*body, effectiveDts[i]);
            }

            if (!body->IsStatic() && !gravityOverridden)
                body->ApplyForce(m_gravity * body->GetMass());
        }
    }

    void World::ApplyConstraintForces(float dt)
    {
        for (auto& constraint : m_constraints)
        {
            if (constraint) constraint->ApplyForce(dt);
        }
    }

    void World::ApplyGravitationalAttraction()
    {
        constexpr float G = 6.674e-11f;

        for (size_t i = 0; i < m_bodies.size(); ++i)
        {
            RigidBody& a = *m_bodies[i];
            if (!a.m_isGravitationalSource) continue;

            for (size_t j = i + 1; j < m_bodies.size(); ++j)
            {
                RigidBody& b = *m_bodies[j];
                if (!b.m_isGravitationalSource) continue;

                math::Vector3 delta = b.GetPosition() - a.GetPosition();
                const float distSq = std::max(delta.LengthSq(), 0.0001f);
                const float dist = std::sqrt(distSq);
                const math::Vector3 dir = delta * (1.0f / dist);
                const float forceScale = G * a.m_gravitationalMass * b.m_gravitationalMass / distSq;
                const math::Vector3 force = dir * forceScale;

                if (!a.IsStatic()) a.ApplyForce(force);
                if (!b.IsStatic()) b.ApplyForce(-force);
            }
        }
    }

    void World::IntegrateBodies(const std::vector<float>& effectiveDts)
    {
        for (size_t i = 0; i < m_bodies.size(); ++i)
            m_bodies[i]->Integrate(effectiveDts[i]);
    }

    void World::SolveConstraintPositions(float dt)
    {
        for (auto& constraint : m_constraints)
        {
            if (constraint) constraint->SolvePosition(dt);
        }
    }

    void World::UpdateColliders()
    {
        for (auto& instance : m_colliders)
        {
            if (instance.body && instance.collider)
                instance.collider->Update(instance.body->GetPosition(), instance.body->GetRotation());
        }
    }

    void World::BroadPhase()
    {
        m_collisionPairs.clear();
        m_solver.BroadPhase(m_colliders, m_collisionPairs, m_layerFilter);
    }

    void World::NarrowPhase(bool doWarmStart)
    {
        m_contacts.clear();
        m_solver.NarrowPhase(m_collisionPairs, m_contacts);

        for (auto& cp : m_contacts)
        {
            if (cp.isTrigger) continue;
            math::Vector3 t0 = math::Vector3::Cross(cp.normal, math::Vector3::RIGHT);
            if (t0.LengthSq() < 1e-6f)
                t0 = math::Vector3::Cross(cp.normal, math::Vector3::UP);
            t0            = t0.Normalized();
            cp.tangent[0] = t0;
            cp.tangent[1] = math::Vector3::Cross(cp.normal, t0).Normalized();
        }

        if (doWarmStart)
            m_contactCache.WarmStart(m_contacts);
    }

    void World::Resolve()
    {
        m_solver.Resolve(m_contacts);
    }

    void World::CCDPhase(float dt)
    {
        // m_useCCD が true かつ速度が十分に速い物体について、
        // 他の球コライダー持ち物体との TOI を計算し速度をクランプする。
        // この処理は IntegrateBodies の前に呼ぶことで貫通を防ぐ。
        for (size_t i = 0; i < m_bodies.size(); ++i)
        {
            auto& bodyA = m_bodies[i];
            if (!bodyA->m_useCCD) continue;
            if (bodyA->IsStatic()) continue;
            if (!CCDSolver::NeedsCCD(*bodyA, bodyA->m_ccdRadius, dt)) continue;

            // bodyA に紐づくコライダーを探す (SphereCollider のみ対応)
            const SphereCollider* sphereA = nullptr;
            for (const auto& inst : m_colliders)
            {
                if (inst.body == bodyA.get() &&
                    inst.collider &&
                    inst.collider->GetType() == ColliderType::SPHERE)
                {
                    sphereA = static_cast<const SphereCollider*>(inst.collider.get());
                    break;
                }
            }
            if (!sphereA) continue;

            const math::Vector3 centerA = sphereA->GetAABB().Center();
            const float         radiusA = sphereA->m_radius;
            float minToi = 1.0f;

            // 全ボディと TOI を計算し最小値を採用する
            for (size_t j = 0; j < m_bodies.size(); ++j)
            {
                if (i == j) continue;
                auto& bodyB = m_bodies[j];

                // bodyB の SphereCollider を探す
                const SphereCollider* sphereB = nullptr;
                for (const auto& inst : m_colliders)
                {
                    if (inst.body == bodyB.get() &&
                        inst.collider &&
                        inst.collider->GetType() == ColliderType::SPHERE)
                    {
                        sphereB = static_cast<const SphereCollider*>(inst.collider.get());
                        break;
                    }
                }
                if (!sphereB) continue;

                const math::Vector3 centerB = sphereB->GetAABB().Center();
                const float         radiusB = sphereB->m_radius;

                // 相対速度を使った Swept Sphere テスト
                const math::Vector3 relVel = bodyA->GetVelocity()
                                           - (bodyB->IsStatic() ? math::Vector3::ZERO
                                                                 : bodyB->GetVelocity());
                const CCDResult res = CCDSolver::SweptSphereSphere(
                    centerA, radiusA, relVel, centerB, radiusB, dt);

                if (res.hit && res.toi < minToi)
                    minToi = res.toi;
            }

            // 速度を TOI でスケールし、衝突時点までしか進まないようにする
            // 残りの速度解決は通常の NarrowPhase/Resolve が担う
            if (minToi < 1.0f)
                bodyA->SetVelocity(bodyA->GetVelocity() * minToi);
        }
    }

    void World::ClassifyCollisions()
    {
        m_enterEvents.clear();
        m_stayEvents.clear();
        m_exitEvents.clear();

        std::map<ColliderPair, CollisionEvent> currentEvents;
        for (auto& cp : m_contacts)
        {
            const Collider* a = cp.colliderA;
            const Collider* b = cp.colliderB;
            if (a > b) std::swap(a, b);

            const ColliderPair pair{ a, b };
            currentEvents.insert({
                pair,
                { cp.colliderA, cp.colliderB, cp.bodyA, cp.bodyB, cp.isTrigger }
            });
        }

        for (auto& [pair, event] : currentEvents)
        {
            if (m_prevEvents.count(pair) == 0)
                m_enterEvents.push_back(event);
            else
                m_stayEvents.push_back(event);
        }

        for (auto& [pair, event] : m_prevEvents)
        {
            if (currentEvents.count(pair) == 0)
                m_exitEvents.push_back(event);
        }

        m_prevEvents = std::move(currentEvents);
    }

    const std::vector<CollisionEvent>& World::GetEnterEvents() const { return m_enterEvents; }
    const std::vector<CollisionEvent>& World::GetStayEvents()  const { return m_stayEvents;  }
    const std::vector<CollisionEvent>& World::GetExitEvents()  const { return m_exitEvents;  }

} // namespace fbzz::physics
