// FBZZ Engine
// World.hpp | fbzz::physics
// 物理シミュレーション世界の管理と Step 実行
#pragma once
#include <Physics/RigidBody.hpp>
#include <Physics/BodyHandle.hpp>
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
#include <Physics/FixedConstraint.hpp>
#include <Physics/SliderConstraint.hpp>
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
        void BeginSceneSync();
        // WHY: RigidBody の所有権は RigidBodyComponent (unique_ptr) が持つ。
        //      World はフレーム中の参照を非所有ポインタとして受け取るだけ。
        BodyHandle SyncBody(BodyHandle handle, RigidBody* body);
        ColliderHandle SyncCollider(ColliderHandle handle, ColliderInstance collider);
        // WHY: Volume は PhysicsSystem が毎フレーム生成する使い捨てオブジェクト。
        //      World が unique_ptr で所有し、EndSceneSync で未参照のものを自動破棄する。
        VolumeHandle SyncVolume(VolumeHandle handle, std::unique_ptr<Volume> volume);
        void EndSceneSync();
        // WHY: Constraint は AddConstraint 呼び出し側が生成して World へ移譲する。
        //      World が唯一の所有者となり、World 破棄時に一括解放される。
        void AddConstraint(std::unique_ptr<Constraint> constraint);
        const std::vector<std::unique_ptr<Constraint>>& GetConstraints() const;

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

        // ── レイキャスト / 形状クエリ ────────────────────────────────────────────
        // Step() の前後どちらでも呼べる。コライダーは UpdateColliders() 後の状態を使う。

        // レイキャストの結果。hit = false のときフィールドは未定義。
        struct RaycastHit {
            math::Vector3 point;        // ヒット点 (ワールド空間)
            math::Vector3 normal;       // ヒット面の外向き法線
            float         distance = 0; // origin からの距離
            const Collider* collider = nullptr; // ヒットしたコライダー
            RigidBody*      body     = nullptr; // 紐づく剛体 (static なら nullptr)
        };

        // filter: nullptr のとき全コライダーを対象にする。
        //         渡すと false を返したコライダーをスキップする。
        //         例: 静的かつ非トリガーのみ → [](const ColliderInstance& i){ return !i.isTrigger && (!i.body || i.body->IsStatic()); }
        using ColliderFilter = std::function<bool(const ColliderInstance&)>;

        // 最も近い 1 件のみ返す。戻り値は hit の有無。
        bool Raycast(const math::Vector3& origin,
                     const math::Vector3& direction,
                     float                maxDistance,
                     RaycastHit&          hit,
                     ColliderFilter        filter = nullptr) const;

        // 全ヒットを距離昇順で返す。
        std::vector<RaycastHit> RaycastAll(const math::Vector3& origin,
                                           const math::Vector3& direction,
                                           float                maxDistance,
                                           ColliderFilter        filter = nullptr) const;

        // 球形スイープ (Sphere Cast)。最も近い 1 件のみ返す。
        bool SphereCast(const math::Vector3& origin,
                        float                radius,
                        const math::Vector3& direction,
                        float                maxDistance,
                        RaycastHit&          hit,
                        ColliderFilter        filter = nullptr) const;

        // 球と重なるコライダーを全て返す (順序未定義)。
        std::vector<const ColliderInstance*> OverlapSphere(
                        const math::Vector3& center,
                        float                radius,
                        ColliderFilter        filter = nullptr) const;

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
        void WakeSleepingContacts();
        void UpdateSleepStates(float dt);
        void ClassifyCollisions();
        void CCDPhase(float dt);    // 高速物体のトンネリング防止 (IntegrateBodies の前)
        bool HasActiveSimulationBodies() const;

        math::Vector3 m_gravity = { 0.0f, -9.81f, 0.0f };
        int m_substeps = 1;
        // BroadPhase 中に使う一時フィルタ。Scene の LayerCollisionMatrix から渡される。
        std::function<bool(int, int)> m_layerFilter;

        // 現フレームの Step() に渡す非所有ビュー (EndSceneSync で再構築)
        std::vector<RigidBody*>                 m_bodies;
        std::vector<ColliderInstance>           m_colliders;
        std::vector<Volume*>                    m_volumes;
        // Constraint は World が唯一の所有者
        std::vector<std::unique_ptr<Constraint>> m_constraints;
        struct BodySlot {
            // WHY: 所有権は RigidBodyComponent が持つ。World は非所有参照のみ保持する。
            RigidBody* body = nullptr;
            uint32_t generation = 1;
            bool touched = false;
        };
        struct ColliderSlot {
            ColliderInstance collider;
            uint32_t generation = 1;
            bool touched = false;
            bool occupied = false;
        };
        struct VolumeSlot {
            // WHY: Volume は PhysicsSystem が毎フレーム生成する使い捨て。World が所有する。
            std::unique_ptr<Volume> volume;
            uint32_t generation = 1;
            bool touched = false;
        };
        std::vector<BodySlot> m_bodyPool;
        std::vector<ColliderSlot> m_colliderPool;
        std::vector<VolumeSlot> m_volumePool;
        bool m_sceneSyncChanged = false; // 追加・削除など接触集合が変わり得る同期変更があったか
        std::vector<CollisionPair>              m_collisionPairs;
        std::vector<ContactPoint>               m_contacts;
        // Step() 内で毎サブステップ使う有効 dt。ローカル vector にすると物理更新ごとに確保が走るため再利用する。
        std::vector<float>                      m_effectiveDts;
        PhysicsSolver                           m_solver;
        ContactCache                            m_contactCache;

        using ColliderPair = std::pair<const Collider*, const Collider*>;
        std::map<ColliderPair, CollisionEvent> m_prevEvents;
        std::vector<CollisionEvent> m_enterEvents;
        std::vector<CollisionEvent> m_stayEvents;
        std::vector<CollisionEvent> m_exitEvents;
    };

} // namespace fbzz::physics
