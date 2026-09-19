/// @file    World.hpp
/// @brief   物理シミュレーション世界の管理と Step 実行。
/// @author  Hasegawa Jin
/// @date    2026-05-21
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
#include <Physics/Layer.hpp>
#include <cstring>
#include <functional>
#include <vector>
#include <map>
#include <memory>

namespace fbzz::physics 
{

    /// @brief Step() の最後に前フレームとの差分から生成するイベント。
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

        /// @name 衝突の強さ
        /// @note このイベントは Step() の末尾で作られるが、その時点では Resolve が既に速度を
        ///       書き換えているため bodyA/bodyB から「ぶつかった勢い」を復元できない。ゲーム側
        ///       (衝突ダメージ・ヒットストップ・SE) が必要とする解決前の値を、Resolve の前後で
        ///       拾ってここへ持ち越す。substep を増やすと 2 回目以降は既に減速しているため、
        ///       フレーム内で観測した最大値を保持する (substep 数を変えても値がぶれない)。
        /// @{

        math::Vector3 relativeVelocity = math::Vector3::ZERO;  ///< 接触点での相対速度 (A から見た B との差、角速度の寄与を含む)。解決前の値。
        /// @brief 法線方向の接近速度。正 = 近づいている = 実際にぶつかった強さ。
        /// @note 「一定速度以上で衝突したときだけダメージ」の判定はこの値を使う。
        float approachSpeed = 0.0f;
        /// @brief 解決で実際に加わった法線インパルス (質量込みの強さ)。
        /// @note 軽い敵と重い敵で手応えを変えたい場合は approachSpeed ではなくこちらを使う。
        float normalImpulse = 0.0f;
        /// @}
    };

    /// @brief Physics モジュールの統合点。剛体・コライダー・制約を受け取り、1 フレーム分の物理を進める。
    class World
    {
    public:
        void BeginSceneSync();
        /// @note RigidBody の所有権は RigidBodyComponent (unique_ptr) が持つ。
        ///       World はフレーム中の参照を非所有ポインタとして受け取るだけ。
        BodyHandle SyncBody(BodyHandle handle, RigidBody* body);
        ColliderHandle SyncCollider(ColliderHandle handle, ColliderInstance collider);
        /// @note Volume は PhysicsSystem が毎フレーム生成する使い捨てオブジェクト。
        ///       World が unique_ptr で所有し、EndSceneSync で未参照のものを自動破棄する。
        VolumeHandle SyncVolume(VolumeHandle handle, std::unique_ptr<Volume> volume);
        void EndSceneSync();
        /// @note Constraint は AddConstraint 呼び出し側が生成して World へ移譲する。
        ///       World が唯一の所有者となり、World 破棄時に一括解放される。
        void AddConstraint(std::unique_ptr<Constraint> constraint);

        /// @name シーン同期で張る制約
        /// @note BeginSceneSync / EndSceneSync の窓の中で申告する。申告が途切れた制約は
        ///       EndSceneSync が破棄するので、Scene 側は «外す» 処理を書かなくてよい。
        ///       AddConstraint (置いたら消えない・Physics 単体利用の入口) とは別枠にしないと、
        ///       BeginSceneSync を呼んだ瞬間にそれらも消えてしまう。HingeConstraint /
        ///       FixedConstraint は構築時の相対姿勢を基準に抱えるため、Volume と違い
        ///       毎フレーム作り直させると基準が «今の姿勢» へ書き換わり続けてしまう。
        /// @{
        ConstraintHandle SyncConstraint(ConstraintHandle handle, std::unique_ptr<Constraint> constraint);
        /// @brief 既存の制約をそのまま今フレームも生かす申告。作り直しが要らないときに使う。
        /// @return false ならそのハンドルは既に無効 (作り直しが必要)。
        bool KeepConstraint(ConstraintHandle handle);
        /// @brief 同期で張った制約への非所有参照。パラメーターだけを書き換えるために使う。
        [[nodiscard]] Constraint* FindConstraint(ConstraintHandle handle) const;
        void RemoveConstraint(ConstraintHandle handle);
        /// @}

        /// @brief 今フレーム有効な制約すべて (AddConstraint 分 + 同期分)。
        const std::vector<Constraint*>& GetConstraints() const;

        /// @brief サブステップ、Volume、制約、衝突検出、衝突解決、イベント分類をこの順で実行する。
        /// @param layerFilter 渡すとその 1 回だけ使い、渡さなければ SetCollisionMatrix の値が効く。
        void Step(float dt, std::function<bool(int, int)> layerFilter = nullptr);

        /// @brief この 2 レイヤーがぶつかるレイヤー行列を設定する。設定しなければ全部ぶつかる。
        /// @note Step の引数と別に持つ理由: 衝突行列はプロジェクト設定でフレームごとに変わらない。
        ///       毎フレーム組み立てて渡す形だと、渡し忘れた経路 (エディタのプレビュー等) だけ
        ///       黙ってフィルタが外れる。設定の適用は毎フレーム走る (ProjectRuntime::Update) ので、
        ///       行列 (1KB) を都度 std::function に包むヒープ確保を避けるため、中身が同じなら
        ///       何もしない。
        void SetCollisionMatrix(const LayerCollisionMatrix& matrix)
        {
            if (m_hasCollisionMatrix &&
                std::memcmp(&m_collisionMatrix, &matrix, sizeof(matrix)) == 0) return;
            m_collisionMatrix    = matrix;
            m_hasCollisionMatrix = true;
            /// @note 値で captures する。this を掴むと World の代入 (ProjectRuntime::ResetPhysics が
            ///       `world = World{}` で作り直す) で消えたオブジェクトを指し続ける。
            const LayerCollisionMatrix copy = matrix;
            m_defaultLayerFilter = [copy](int a, int b) { return copy.CanCollide(a, b); };
        }

        /// @brief この 2 つのレイヤーがぶつかるか。行列を設定していなければ常に true。
        /// @note «自分の当たりを自分の剛体へ当てない» ような形は行列が正しく組まれて初めて
        ///       成立する。設定が外れると «押され続けて勝手に動く» という原因の見えない
        ///       壊れ方をするため、組み立てる側が前提を確かめられるよう公開する。
        [[nodiscard]] bool LayersCollide(int a, int b) const
        {
            return !m_hasCollisionMatrix || m_collisionMatrix.CanCollide(a, b);
        }

        void          SetGravity(const math::Vector3& gravity);
        math::Vector3 GetGravity() const { return m_gravity; }
        void SetSubsteps(int substeps);
        int  GetSubsteps() const { return m_substeps; }

        /// @name Step() 後に参照する衝突イベント
        /// @{
        const std::vector<CollisionEvent>& GetEnterEvents() const;
        const std::vector<CollisionEvent>& GetStayEvents()  const;
        const std::vector<CollisionEvent>& GetExitEvents()  const;
        /// @}

        /// @name レイキャスト / 形状クエリ
        /// @note Step() の前後どちらでも呼べる。コライダーは UpdateColliders() 後の状態を使う。
        /// @{

        /// @brief レイキャストの結果。hit = false のときフィールドは未定義。
        struct RaycastHit {
            math::Vector3 point;        ///< ヒット点 (ワールド空間)
            math::Vector3 normal;       ///< ヒット面の外向き法線
            float         distance = 0; ///< origin からの距離
            const Collider* collider = nullptr; ///< ヒットしたコライダー
            RigidBody*      body     = nullptr; ///< 紐づく剛体 (static なら nullptr)
        };

        /// @brief コライダーの絞り込み。nullptr のとき全コライダーを対象にする。渡すと false を
        ///        返したコライダーをスキップする。
        /// @note 例: 静的かつ非トリガーのみ →
        ///       `[](const ColliderInstance& i){ return !i.isTrigger && (!i.body || i.body->IsStatic()); }`
        using ColliderFilter = std::function<bool(const ColliderInstance&)>;

        /// @brief 最も近い 1 件のみ返す。
        /// @return hit の有無。
        bool Raycast(const math::Vector3& origin,
                     const math::Vector3& direction,
                     float                maxDistance,
                     RaycastHit&          hit,
                     ColliderFilter        filter = nullptr) const;

        /// @brief 全ヒットを距離昇順で返す。
        std::vector<RaycastHit> RaycastAll(const math::Vector3& origin,
                                           const math::Vector3& direction,
                                           float                maxDistance,
                                           ColliderFilter        filter = nullptr) const;

        /// @brief 球形スイープ (Sphere Cast)。最も近い 1 件のみ返す。
        bool SphereCast(const math::Vector3& origin,
                        float                radius,
                        const math::Vector3& direction,
                        float                maxDistance,
                        RaycastHit&          hit,
                        ColliderFilter        filter = nullptr) const;

        /// @brief 球と重なるコライダーを全て返す (順序未定義)。
        std::vector<const ColliderInstance*> OverlapSphere(
                        const math::Vector3& center,
                        float                radius,
                        ColliderFilter        filter = nullptr) const;
        /// @}

    private:
        void RemoveExpiredVolumes();
        void ApplyForcesAndVolumes(float dt, std::vector<float>& effectiveDts);
        void ApplyConstraintForces(float dt);
        void ApplyGravitationalAttraction();
        void IntegrateBodies(const std::vector<float>& effectiveDts);
        void SolveConstraintPositions(float dt);
        /// @brief m_ownedConstraints と制約プールから m_activeConstraints を組み直す。
        void RebuildConstraintViews();
        void UpdateColliders();
        void BroadPhase();
        void NarrowPhase(bool doWarmStart = false);
        void Resolve();
        void WakeSleepingContacts();
        void UpdateSleepStates(float dt);
        void ClassifyCollisions();
        void CCDPhase(float dt);    ///< 高速物体のトンネリング防止 (IntegrateBodies の前)
        bool HasActiveSimulationBodies() const;
        /// @brief Resolve の直前に呼ぶ。接触点の相対速度 (= 衝突直前の勢い) を記録する。
        void RecordApproachVelocities();
        /// @brief Resolve の直後に呼ぶ。実際に加わった法線インパルスを記録する。
        void RecordContactImpulses();

        math::Vector3 m_gravity = { 0.0f, -9.81f, 0.0f };
        int m_substeps = 1;
        std::function<bool(int, int)> m_layerFilter;         ///< BroadPhase 中に使う一時フィルタ。Scene の LayerCollisionMatrix から渡される。
        std::function<bool(int, int)> m_defaultLayerFilter;  ///< Step に何も渡されなかったときに使う既定。プロジェクト設定の衝突行列が入る。
        LayerCollisionMatrix          m_collisionMatrix;
        bool                          m_hasCollisionMatrix = false;

        /// @name 現フレームの Step() に渡す非所有ビュー (EndSceneSync で再構築)
        /// @{
        std::vector<RigidBody*>                 m_bodies;
        std::vector<ColliderInstance>           m_colliders;
        std::vector<Volume*>                    m_volumes;
        std::vector<std::unique_ptr<Constraint>> m_ownedConstraints;  ///< World が唯一の所有者。AddConstraint で置かれた «消えない» ぶん。
        std::vector<Constraint*>                 m_activeConstraints; ///< Step() が走らせる非所有ビュー (所有分 + 同期分。RebuildConstraintViews で再構築)
        /// @}
        struct BodySlot {
            RigidBody* body = nullptr;  ///< 所有権は RigidBodyComponent が持つ。World は非所有参照のみ保持する。
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
            std::unique_ptr<Volume> volume;  ///< Volume は PhysicsSystem が毎フレーム生成する使い捨て。World が所有する。
            uint32_t generation = 1;
            bool touched = false;
        };
        struct ConstraintSlot {
            /// @brief SyncConstraint で渡された Constraint。World が所有し、申告が途切れたら
            ///        EndSceneSync がここで破棄する。
            std::unique_ptr<Constraint> constraint;
            uint32_t generation = 1;
            bool touched = false;
        };
        std::vector<BodySlot> m_bodyPool;
        std::vector<ColliderSlot> m_colliderPool;
        std::vector<VolumeSlot> m_volumePool;
        std::vector<ConstraintSlot> m_constraintPool;
        bool m_sceneSyncChanged = false; ///< 追加・削除など接触集合が変わり得る同期変更があったか
        std::vector<CollisionPair>              m_collisionPairs;
        std::vector<ContactPoint>               m_contacts;
        std::vector<float>                      m_effectiveDts;  ///< Step() 内で毎サブステップ使う有効 dt。ローカル vector だと物理更新ごとに確保が走るため再利用する。
        PhysicsSolver                           m_solver;
        ContactCache                            m_contactCache;

        using ColliderPair = std::pair<const Collider*, const Collider*>;
        std::map<ColliderPair, CollisionEvent> m_prevEvents;
        std::vector<CollisionEvent> m_enterEvents;
        std::vector<CollisionEvent> m_stayEvents;
        std::vector<CollisionEvent> m_exitEvents;

        /// @brief このフレームで観測した衝突の強さ。Step() の先頭で clear し、各サブステップの
        ///        Resolve 前後で最大値を更新して ClassifyCollisions が読む。
        /// @note 1 フレームに複数サブステップがあると 2 回目以降は既に減速しているため、
        ///       substep 数を変えてもダメージ量が変わらないよう最初の (最大の) 勢いで代表させる。
        struct ContactImpact {
            math::Vector3 relativeVelocity = math::Vector3::ZERO;
            float approachSpeed = 0.0f;
            float normalImpulse = 0.0f;
        };
        std::map<ColliderPair, ContactImpact> m_frameImpacts;
    };

} // namespace fbzz::physics
