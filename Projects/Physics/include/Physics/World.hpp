// FBZZ Engine
// World.hpp | fbzz::physics
// 物理シミュレーション世界の管理と Step 実行
#pragma once
#include <Physics/RigidBody.hpp>
#include <Physics/ContactPoint.hpp>
#include <Physics/CollisionPair.hpp>
#include <Physics/PhysicsSolver.hpp>
#include <Physics/Volume.hpp>
#include <Physics/Constraint.hpp>
#include <Physics/SpringConstraint.hpp>
#include <Physics/RopeConstraint.hpp>
#include <Physics/DistanceConstraint.hpp>
#include <Physics/ChainConstraint.hpp>
#include <Physics/HingeConstraint.hpp>
#include <vector>
#include <map>
#include <memory>

namespace fbzz::physics 
{

    struct CollisionEvent 
    {
        const Collider* colliderA;
        const Collider* colliderB;
        RigidBody* bodyA;
        RigidBody* bodyB;
        bool isTrigger = false;
    };

    class World 
    {
    public:
        void SetBodies(std::vector<std::shared_ptr<RigidBody>> bodies);
        const std::vector<std::shared_ptr<RigidBody>>& GetBodies() const;
        void SetColliders(std::vector<ColliderInstance> colliders);
        void SetVolumes(std::vector<std::shared_ptr<Volume>> volumes);
        void AddConstraint(std::shared_ptr<Constraint> constraint);

        void Step(float dt);

        void          SetGravity(const math::Vector3& gravity);
        math::Vector3 GetGravity() const { return m_gravity; }

        // Step() 後に参照する衝突イベント
        const std::vector<CollisionEvent>& GetEnterEvents() const;
        const std::vector<CollisionEvent>& GetStayEvents()  const;
        const std::vector<CollisionEvent>& GetExitEvents()  const;

    private:
        void RemoveExpiredVolumes();
        void ApplyForcesAndVolumes(float dt, std::vector<float>& effectiveDts);
        void ApplyConstraintForces(float dt);
        void ApplyGravitationalAttraction();
        void IntegrateBodies(const std::vector<float>& effectiveDts);
        void SolveConstraintPositions(float dt);
        void UpdateColliders();
        void BroadPhase();
        void NarrowPhase();
        void Resolve();
        void ClassifyCollisions();

        math::Vector3 m_gravity = { 0.0f, -9.81f, 0.0f };

        std::vector<std::shared_ptr<RigidBody>> m_bodies;
        std::vector<ColliderInstance>           m_colliders;
        std::vector<std::shared_ptr<Volume>>    m_volumes;
        std::vector<std::shared_ptr<Constraint>> m_constraints;
        std::vector<CollisionPair>              m_collisionPairs;
        std::vector<ContactPoint>               m_contacts;
        PhysicsSolver                           m_solver;

        using ColliderPair = std::pair<const Collider*, const Collider*>;
        std::map<ColliderPair, CollisionEvent> m_prevEvents;
        std::vector<CollisionEvent> m_enterEvents;
        std::vector<CollisionEvent> m_stayEvents;
        std::vector<CollisionEvent> m_exitEvents;
    };

} // namespace fbzz::physics
