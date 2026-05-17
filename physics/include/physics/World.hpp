// FBZZ Engine
// World.hpp | fbzz::physics
// 物理シミュレーション世界の管理と Step 実行
#pragma once
#include <physics/RigidBody.hpp>
#include <physics/ContactPoint.hpp>
#include <physics/CollisionPair.hpp>
#include <physics/PhysicsSolver.hpp>
#include <vector>
#include <set>
#include <memory>

namespace fbzz::physics 
{

    struct CollisionEvent 
    {
        RigidBody* bodyA;
        RigidBody* bodyB;
    };

    class World 
    {
    public:
        void AddBody(std::shared_ptr<RigidBody> body);
        void RemoveBody(const std::shared_ptr<RigidBody>& body);
        const std::vector<std::shared_ptr<RigidBody>>& GetBodies() const;

        void Step(float dt);

        void          SetGravity(const math::Vector3& gravity);
        math::Vector3 GetGravity() const { return m_gravity; }

        // Step() 後に参照する衝突イベント
        const std::vector<CollisionEvent>& GetEnterEvents() const;
        const std::vector<CollisionEvent>& GetStayEvents()  const;
        const std::vector<CollisionEvent>& GetExitEvents()  const;

    private:
        void ApplyGlobalGravity();
        void IntegrateBodies(float dt);
        void UpdateColliders();
        void BroadPhase();
        void NarrowPhase();
        void Resolve();
        void ClassifyCollisions();

        math::Vector3 m_gravity = { 0.0f, -9.81f, 0.0f };

        std::vector<std::shared_ptr<RigidBody>> m_bodies;
        std::vector<CollisionPair>              m_collisionPairs;
        std::vector<ContactPoint>               m_contacts;
        PhysicsSolver                           m_solver;

        using BodyPair = std::pair<RigidBody*, RigidBody*>;
        std::set<BodyPair>          m_prevPairs;
        std::vector<CollisionEvent> m_enterEvents;
        std::vector<CollisionEvent> m_stayEvents;
        std::vector<CollisionEvent> m_exitEvents;
    };

} // namespace fbzz::physics
