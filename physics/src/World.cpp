// FBZZ Engine
// World.cpp | fbzz::physics
// 物理シミュレーション世界の管理と Step 実行
#include <physics/World.hpp>
#include <algorithm>
#include <utility>

namespace fbzz::physics 
{
    void World::AddBody(std::shared_ptr<RigidBody> body)
    {
        m_bodies.push_back(std::move(body));
    }

    void World::RemoveBody(const std::shared_ptr<RigidBody>& body)
    {
        m_bodies.erase(std::remove(m_bodies.begin(), m_bodies.end(), body),
                    m_bodies.end());
    }

    const std::vector<std::shared_ptr<RigidBody>>& World::GetBodies() const
    {
        return m_bodies;
    }

    void World::SetGravity(const math::Vector3& gravity)
    {
        m_gravity = gravity;
    }

    void World::Step(float dt)
    {
        constexpr int SUBSTEPS = 4;
        const float subDt = dt / SUBSTEPS;

        for (int s = 0; s < SUBSTEPS; ++s)
        {
            ApplyGlobalGravity();
            IntegrateBodies(subDt);
            UpdateColliders();
            BroadPhase();
            NarrowPhase();
            Resolve();
        }

        ClassifyCollisions();
    }

    void World::ApplyGlobalGravity()
    {
        for (auto& body : m_bodies)
        {
            if (!body->IsStatic())
                body->ApplyForce(m_gravity * body->GetMass());
        }
    }

    void World::IntegrateBodies(float dt)
    {
        for (auto& body : m_bodies)
            body->Integrate(dt);
    }

    void World::UpdateColliders()
    {
        for (auto& body : m_bodies)
        {
            auto col = body->GetCollider();
            if (col) col->Update(body->GetPosition(), body->GetRotation());
        }
    }

    void World::BroadPhase()
    {
        m_collisionPairs.clear();
        m_solver.BroadPhase(m_bodies, m_collisionPairs);
    }

    void World::NarrowPhase()
    {
        m_contacts.clear();
        m_solver.NarrowPhase(m_collisionPairs, m_contacts);
    }

    void World::Resolve()
    {
        m_solver.Resolve(m_contacts);
    }

    void World::ClassifyCollisions()
    {
        m_enterEvents.clear();
        m_stayEvents.clear();
        m_exitEvents.clear();

        std::set<BodyPair> currentPairs;
        for (auto& cp : m_contacts)
        {
            RigidBody* a = cp.bodyA;
            RigidBody* b = cp.bodyB;
            if (a > b) std::swap(a, b);
            currentPairs.insert({ a, b });
        }

        for (auto& pair : currentPairs)
        {
            if (m_prevPairs.count(pair) == 0)
                m_enterEvents.push_back({ pair.first, pair.second });
            else
                m_stayEvents.push_back({ pair.first, pair.second });
        }

        for (auto& pair : m_prevPairs)
        {
            if (currentPairs.count(pair) == 0)
                m_exitEvents.push_back({ pair.first, pair.second });
        }

        m_prevPairs = std::move(currentPairs);
    }

    const std::vector<CollisionEvent>& World::GetEnterEvents() const { return m_enterEvents; }
    const std::vector<CollisionEvent>& World::GetStayEvents()  const { return m_stayEvents;  }
    const std::vector<CollisionEvent>& World::GetExitEvents()  const { return m_exitEvents;  }

} // namespace fbzz::physics
