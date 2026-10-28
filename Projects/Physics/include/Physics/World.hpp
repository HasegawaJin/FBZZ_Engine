/// @file    World.hpp
/// @brief   物理シミュレーション世界の管理と Step 実行。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Physics/RigidBody.hpp>
#include <Physics/BodyHandle.hpp>
#include <Physics/BroadPhaseDebugSnapshot.hpp>
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
#include <cstddef>
#include <cstdint>
#include <atomic>

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
        /// @note 書き換えているため bodyA/bodyB から「ぶつかった勢い」を復元できない。ゲーム側
        /// @note (衝突ダメージ・ヒットストップ・SE) が必要とする解決前の値を、Resolve の前後で
        /// @note 拾ってここへ持ち越す。substep を増やすと 2 回目以降は既に減速しているため、
        /// @note フレーム内で観測した最大値を保持する (substep 数を変えても値がぶれない)。
        /// @{

        math::Vector3 relativeVelocity = math::Vector3::ZERO;  ///< @note 接触点での相対速度 (A から見た B との差、角速度の寄与を含む)。解決前の値。
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
        /// @note World はフレーム中の参照を非所有ポインタとして受け取るだけ。
        BodyHandle SyncBody(BodyHandle handle, RigidBody* body);
        ColliderHandle SyncCollider(ColliderHandle handle, ColliderInstance collider);
        /// @note Volume は PhysicsSystem が毎フレーム生成する使い捨てオブジェクト。
        /// @note World が unique_ptr で所有し、EndSceneSync で未参照のものを自動破棄する。
        VolumeHandle SyncVolume(VolumeHandle handle, std::unique_ptr<Volume> volume);
        void EndSceneSync();
        /// @note Constraint は AddConstraint 呼び出し側が生成して World へ移譲する。
        /// @note World が唯一の所有者となり、World 破棄時に一括解放される。
        void AddConstraint(std::unique_ptr<Constraint> constraint);

        /// @name シーン同期で張る制約
        /// @note BeginSceneSync / EndSceneSync の窓の中で申告する。申告が途切れた制約は
        /// @note EndSceneSync が破棄するので、Scene 側は «外す» 処理を書かなくてよい。
        /// @note AddConstraint (置いたら消えない・Physics 単体利用の入口) とは別枠にしないと、
        /// @note BeginSceneSync を呼んだ瞬間にそれらも消えてしまう。HingeConstraint /
        /// @note FixedConstraint は構築時の相対姿勢を基準に抱えるため、Volume と違い
        /// @note 毎フレーム作り直させると基準が «今の姿勢» へ書き換わり続けてしまう。
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

        /// @pre Owner thread only, outside Step. UI publishes requests through its owning system.
        void SetBroadPhaseDebugRequest(const BroadPhaseDebugRequest& request);
        [[nodiscard]] uint64_t GetBroadPhaseDebugWorldEpoch() const { return m_debugEpoch.value; }
        /// @note Owner-thread read; the immutable result can be retained across removal, reset and World destruction.
        [[nodiscard]] std::shared_ptr<const BroadPhaseDebugSnapshot> GetBroadPhaseDebugSnapshot() const
        { return m_broadPhaseDebugSnapshot; }

        /// @brief この 2 レイヤーがぶつかるレイヤー行列を設定する。設定しなければ全部ぶつかる。
        /// @note Step の引数と別に持つ理由: 衝突行列はプロジェクト設定でフレームごとに変わらない。
        /// @note 毎フレーム組み立てて渡す形だと、渡し忘れた経路 (エディタのプレビュー等) だけ
        /// @note 黙ってフィルタが外れる。設定の適用は毎フレーム走る (ProjectRuntime::Update) ので、
        /// @note 行列 (1KB) を都度 std::function に包むヒープ確保を避けるため、中身が同じなら
        /// @note 何もしない。
        void SetCollisionMatrix(const LayerCollisionMatrix& matrix)
        {
            if (m_hasCollisionMatrix &&
                std::memcmp(&m_collisionMatrix, &matrix, sizeof(matrix)) == 0) return;
            m_collisionMatrix    = matrix;
            m_hasCollisionMatrix = true;
            /// @note 値で captures する。this を掴むと World の代入 (ProjectRuntime::ResetPhysics が
            /// @note `world = World{}` で作り直す) で消えたオブジェクトを指し続ける。
            const LayerCollisionMatrix copy = matrix;
            m_defaultLayerFilter = [copy](int a, int b) { return copy.CanCollide(a, b); };
        }

        /// @brief この 2 つのレイヤーがぶつかるか。行列を設定していなければ常に true。
        /// @note «自分の当たりを自分の剛体へ当てない» ような形は行列が正しく組まれて初めて
        /// @note 成立する。設定が外れると «押され続けて勝手に動く» という原因の見えない
        /// @note 壊れ方をするため、組み立てる側が前提を確かめられるよう公開する。
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
        /// @see Docs/design/runtime-workset-optimization.md
        /// @{

        /// @brief レイキャストの結果。hit = false のときフィールドは未定義。
        struct RaycastHit {
            math::Vector3 point;        ///< @note ヒット点 (ワールド空間)
            math::Vector3 normal;       ///< @note ヒット面の外向き法線
            float         distance = 0; ///< @note origin からの距離
            const Collider* collider = nullptr; ///< @note ヒットしたコライダー
            RigidBody*      body     = nullptr; ///< @note 紐づく剛体 (static なら nullptr)
        };

        /// @brief コライダーの絞り込み。nullptr のとき全コライダーを対象にする。渡すと false を
        /// @note 返したコライダーをスキップする。
        /// @note 例: 静的かつ非トリガーのみ →
        /// @note `[](const ColliderInstance& i){ return !i.isTrigger && (!i.body || i.body->IsStatic()); }`
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
        static uint64_t NextDebugWorldEpoch();
        struct DebugEpoch
        {
            uint64_t value = NextDebugWorldEpoch();
            DebugEpoch() = default;
            DebugEpoch(DebugEpoch&& other) noexcept : value(other.value) { other.value = NextDebugWorldEpoch(); }
            DebugEpoch& operator=(DebugEpoch&& other) noexcept
            {
                if (this != &other) { value = other.value; other.value = NextDebugWorldEpoch(); }
                return *this;
            }
        };
        void CaptureBroadPhaseDebugSnapshot();
        DebugEpoch m_debugEpoch;
        uint64_t m_broadPhaseDebugSequence = 0;
        BroadPhaseDebugRequest m_broadPhaseDebugRequest;
        std::shared_ptr<const BroadPhaseDebugSnapshot> m_broadPhaseDebugSnapshot;
        std::shared_ptr<const BroadPhaseDebugSnapshot> m_pendingBroadPhaseDebugSnapshot;
        struct QueryNode {
            AABB bounds;
            size_t colliderIndex = SIZE_MAX;
            size_t escape = 0;
        };
        void RefreshQueryIndex();
        size_t BuildQueryNode(size_t begin, size_t end);
        [[nodiscard]] bool QueryIndexMatchesColliders() const;
        /// @note Collider は外部所有で直接変更可能。境界が同期時点と一致しない問い合わせは全候補へ戻す。
        /// @note 作業状態を書き換えず、filter 内の再入問い合わせと複数スレッドの読み取りを許す。
        template<typename Callback>
        void ForEachQueryCandidate(const AABB& bounds, Callback&& callback) const
        {
            if (!QueryIndexMatchesColliders()) {
                for (size_t index = 0; index < m_colliders.size(); ++index)
                    callback(index, m_colliders[index]);
                return;
            }
            for (size_t index = 0; index < m_queryNodes.size();) {
                const auto& node = m_queryNodes[index];
                if (!node.bounds.Overlaps(bounds)) {
                    index = node.escape;
                    continue;
                }
                if (node.colliderIndex != SIZE_MAX)
                    callback(node.colliderIndex, m_colliders[node.colliderIndex]);
                ++index;
            }
        }
        std::vector<QueryNode> m_queryNodes;
        std::vector<AABB> m_queryBounds;
        std::vector<size_t> m_queryOrder;
        std::vector<const Collider*> m_queryColliders;
        /// @note World の move を保ちつつ、並行 read query から次の安全点での refit 要求だけを記録する。
        alignas(std::atomic_ref<bool>::required_alignment) mutable bool m_queryUsed = false;

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
        void CCDPhase(float dt);    ///< @note 高速物体のトンネリング防止 (IntegrateBodies の前)
        bool HasActiveSimulationBodies() const;
        /// @brief Resolve の直前に呼ぶ。接触点の相対速度 (= 衝突直前の勢い) を記録する。
        void RecordApproachVelocities();
        /// @brief Resolve の直後に呼ぶ。実際に加わった法線インパルスを記録する。
        void RecordContactImpulses();

        math::Vector3 m_gravity = { 0.0f, -9.81f, 0.0f };
        int m_substeps = 1;
        std::function<bool(int, int)> m_layerFilter;         ///< @note BroadPhase 中に使う一時フィルタ。Scene の LayerCollisionMatrix から渡される。
        std::function<bool(int, int)> m_defaultLayerFilter;  ///< @note Step に何も渡されなかったときに使う既定。プロジェクト設定の衝突行列が入る。
        LayerCollisionMatrix          m_collisionMatrix;
        bool                          m_hasCollisionMatrix = false;

        /// @name 現フレームの Step() に渡す非所有ビュー (EndSceneSync で再構築)
        /// @{
        std::vector<RigidBody*>                 m_bodies;
        std::vector<ColliderInstance>           m_colliders;
        std::vector<Volume*>                    m_volumes;
        std::vector<std::unique_ptr<Constraint>> m_ownedConstraints;  ///< @note World が唯一の所有者。AddConstraint で置かれた «消えない» ぶん。
        std::vector<Constraint*>                 m_activeConstraints; ///< @note Step() が走らせる非所有ビュー (所有分 + 同期分。RebuildConstraintViews で再構築)
        /// @}
        struct BodySlot {
            RigidBody* body = nullptr;  ///< @note 所有権は RigidBodyComponent が持つ。World は非所有参照のみ保持する。
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
            std::unique_ptr<Volume> volume;  ///< @note Volume は PhysicsSystem が毎フレーム生成する使い捨て。World が所有する。
            uint32_t generation = 1;
            bool touched = false;
        };
        struct ConstraintSlot {
            /// @brief SyncConstraint で渡された Constraint。World が所有し、申告が途切れたら
            /// @note EndSceneSync がここで破棄する。
            std::unique_ptr<Constraint> constraint;
            uint32_t generation = 1;
            bool touched = false;
        };
        std::vector<BodySlot> m_bodyPool;
        std::vector<ColliderSlot> m_colliderPool;
        std::vector<VolumeSlot> m_volumePool;
        std::vector<ConstraintSlot> m_constraintPool;
        bool m_sceneSyncChanged = false; ///< @note 追加・削除など接触集合が変わり得る同期変更があったか
        std::vector<CollisionPair>              m_collisionPairs;
        std::vector<ContactPoint>               m_contacts;
        std::vector<float>                      m_effectiveDts;  ///< @note Step() 内で毎サブステップ使う有効 dt。ローカル vector だと物理更新ごとに確保が走るため再利用する。
        PhysicsSolver                           m_solver;
        ContactCache                            m_contactCache;

        using ColliderPair = std::pair<const Collider*, const Collider*>;
        std::map<ColliderPair, CollisionEvent> m_prevEvents;
        std::vector<CollisionEvent> m_enterEvents;
        std::vector<CollisionEvent> m_stayEvents;
        std::vector<CollisionEvent> m_exitEvents;

        /// @brief このフレームで観測した衝突の強さ。Step() の先頭で clear し、各サブステップの
        /// @note Resolve 前後で最大値を更新して ClassifyCollisions が読む。
        /// @note 1 フレームに複数サブステップがあると 2 回目以降は既に減速しているため、
        /// @note substep 数を変えてもダメージ量が変わらないよう最初の (最大の) 勢いで代表させる。
        struct ContactImpact {
            math::Vector3 relativeVelocity = math::Vector3::ZERO;
            float approachSpeed = 0.0f;
            float normalImpulse = 0.0f;
        };
        std::map<ColliderPair, ContactImpact> m_frameImpacts;
    };

} /// @note namespace fbzz::physics
