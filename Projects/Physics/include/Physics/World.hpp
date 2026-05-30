// FBZZ Engine
// World.hpp | fbzz::physics
// 物理シミュレーション世界の管理と Step 実行
#pragma once
#include <Physics/RigidBody.hpp>
#include <Physics/ContactPoint.hpp>
#include <Physics/CollisionPair.hpp>
#include <Physics/PhysicsSolver.hpp>
#include <Physics/ContactCache.hpp>
#include <Physics/CCDSolver.hpp>
#include <Physics/Volume.hpp>
#include <Physics/Constraint.hpp>
#include <Physics/SpringConstraint.hpp>
#include <Physics/RopeConstraint.hpp>
#include <Physics/DistanceConstraint.hpp>
#include <Physics/ChainConstraint.hpp>
#include <Physics/HingeConstraint.hpp>
#include <functional>
#include <vector>
#include <map>
#include <memory>

namespace fbzz::physics 
{

    // Step() の最後に前フレームとの差分から生成するイベント。
    struct CollisionEvent 
    {
        const Collider* colliderA;
        const Collider* colliderB;
        RigidBody* bodyA;
        RigidBody* bodyB;
        math::Vector3 point = math::Vector3::ZERO;
        math::Vector3 normal = math::Vector3::UP;
        float depth = 0.0f;
        bool isTrigger = false;
    };

    // Physics モジュールの統合点。剛体・コライダー・制約を受け取り、1 フレーム分の物理を進める。
    class World 
    {
    public:
        void SetBodies(std::vector<std::shared_ptr<RigidBody>> bodies);
        const std::vector<std::shared_ptr<RigidBody>>& GetBodies() const;
        void SetColliders(std::vector<ColliderInstance> colliders);
        void SetVolumes(std::vector<std::shared_ptr<Volume>> volumes);
        void AddConstraint(std::shared_ptr<Constraint> constraint);
        const std::vector<std::shared_ptr<Constraint>>& GetConstraints() const;

        // サブステップ、Volume、制約、衝突検出、衝突解決、イベント分類をこの順で実行する。
        void Step(float dt, std::function<bool(int, int)> layerFilter = nullptr);

        void          SetGravity(const math::Vector3& gravity);
        math::Vector3 GetGravity() const { return m_gravity; }
        void SetSubsteps(int substeps);
        int  GetSubsteps() const { return m_substeps; }

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
        void NarrowPhase(bool doWarmStart = false);
        void Resolve();
        void ClassifyCollisions();
        void CCDPhase(float dt);    // 高速物体のトンネリング防止 (IntegrateBodies の前)

        math::Vector3 m_gravity = { 0.0f, -9.81f, 0.0f };
        int m_substeps = 4;
        // BroadPhase 中に使う一時フィルタ。Scene の LayerCollisionMatrix から渡される。
        std::function<bool(int, int)> m_layerFilter;

        std::vector<std::shared_ptr<RigidBody>> m_bodies;
        std::vector<ColliderInstance>           m_colliders;
        std::vector<std::shared_ptr<Volume>>    m_volumes;
        std::vector<std::shared_ptr<Constraint>> m_constraints;
        std::vector<CollisionPair>              m_collisionPairs;
        std::vector<ContactPoint>               m_contacts;
        PhysicsSolver                           m_solver;
        ContactCache                            m_contactCache;

        using ColliderPair = std::pair<const Collider*, const Collider*>;
        std::map<ColliderPair, CollisionEvent> m_prevEvents;
        std::vector<CollisionEvent> m_enterEvents;
        std::vector<CollisionEvent> m_stayEvents;
        std::vector<CollisionEvent> m_exitEvents;
    };

} // namespace fbzz::physics
