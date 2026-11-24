/// @file    World.cpp
/// @brief   物理ステップ・接触の解決と衝突イベント。
/// @author  Hasegawa Jin
/// @date    2026-05-21

#include <Physics/World.hpp>
#include <Physics/SphereCollider.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

namespace fbzz::physics {

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
    RetireExpiredRegistrations();
    m_pendingBroadPhaseDebugSnapshot.reset();
    m_layerFilter = layerFilter ? std::move(layerFilter) : m_defaultLayerFilter;
    /// @note 休止中のフレームにも前回の衝撃値を持ち越さない。
    m_frameImpacts.clear();

    /// @note Scene 同期に差分がなく動的剛体が休止中なら、前回の接触集合でイベントを分類できる。
    if (!m_sceneSyncChanged && !HasActiveSimulationBodies()) {
        RefreshQueryIndex();
        ClassifyCollisions();
        if (m_broadPhaseDebugRequest.enabled) {
            auto snapshot = std::make_shared<BroadPhaseDebugSnapshot>();
            snapshot->worldEpoch = m_debugEpoch.value;
            snapshot->sequence = ++m_broadPhaseDebugSequence;
            snapshot->request = m_broadPhaseDebugRequest;
            m_broadPhaseDebugSnapshot = std::move(snapshot);
        }
        return;
    }

    const int substeps = std::max(m_substeps, 1);
    const float substepDt = dt / static_cast<float>(substeps);

    for (int substep = 0; substep < substeps; ++substep)
    {
        RemoveExpiredVolumes();
        for (Volume* volume : m_volumes)
        {
            if (volume)
                volume->Tick(substepDt);
        }
        ApplyForcesAndVolumes(substepDt, m_effectiveDts);
        ApplyConstraintForces(substepDt);
        ApplyGravitationalAttraction();
        CCDPhase(substepDt);
        IntegrateBodies(m_effectiveDts);

        /// @note 拘束が姿勢を書き換えた後にコライダーを同期し、その形状で衝突を検出する。
        SolveConstraintPositions(substepDt);
        UpdateColliders();
        BroadPhase();
        if (substep + 1 == substeps && m_broadPhaseDebugRequest.enabled)
            CaptureBroadPhaseDebugSnapshot();

        /// @note 前フレームのインパルスを再適用するのは先頭サブステップの 1 回だけ。
        NarrowPhase(substep == 0);
        RecordApproachVelocities();
        WakeSleepingContacts();
        Resolve();
        RecordContactImpulses();
        UpdateSleepStates(substepDt);
    }

    RefreshQueryIndex();
    /// @note 次のフレームへ渡すのは最終サブステップで解決した接触。
    m_contactCache.UpdateCache(m_contacts);
    m_contactCache.PurgeStale();

    ClassifyCollisions();
    m_broadPhaseDebugSnapshot = std::move(m_pendingBroadPhaseDebugSnapshot);
}

bool World::HasActiveSimulationBodies() const
{
    for (const auto& body : m_bodies)
    {
        if (body && !body->IsStatic() && !body->IsSleeping())
            return true;
    }
    return false;
}

void World::RemoveExpiredVolumes()
{
    m_volumes.erase(std::remove_if(m_volumes.begin(), m_volumes.end(),
        [](const Volume* volume) {
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
        if (!body || body->IsSleeping()) continue;
        effectiveDts[i] *= std::clamp(body->m_timeScale, 0.0f, 8.0f);
        bool gravityOverridden = false;

        for (auto& volume : m_volumes)
        {
            if (!volume || !volume->Contains(body->GetPosition())) continue;

            effectiveDts[i] = effectiveDts[i] * volume->GetTimeScale();
            gravityOverridden = gravityOverridden || volume->OverridesGravity();
            volume->Apply(*body, effectiveDts[i]);
        }

        if (!body->IsStatic() && !gravityOverridden && body->m_useGravity)
            body->ApplyForceNoWake(m_gravity * body->GetMass() * body->m_gravityScale);
    }
}

void World::ApplyConstraintForces(float dt)
{
    for (Constraint* constraint : m_constraints.Active())
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

            if (!a.IsStatic()) a.ApplyForceNoWake(force);
            if (!b.IsStatic()) b.ApplyForceNoWake(-force);
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
    for (Constraint* constraint : m_constraints.Active())
    {
        if (constraint) constraint->SolvePosition(dt);
    }
}

void World::UpdateColliders()
{
    for (auto& instance : m_colliders)
    {
        if (instance.body && instance.collider)
        {
            const math::Quaternion bodyRotation = instance.body->GetRotation();
            const math::Vector3 worldCenter = instance.body->GetPosition()
                + bodyRotation * instance.centerOffset;
            instance.collider->Update(worldCenter, bodyRotation);
        }
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
    PhysicsSolver::PrepareContactTangents(m_contacts);
    if (doWarmStart)
        m_contactCache.WarmStart(m_contacts);
}

void World::Resolve()
{
    m_solver.Resolve(m_contacts);
}

void World::WakeSleepingContacts()
{
    constexpr float WAKE_SPEED_SQ = 0.02f * 0.02f;
    for (const auto& cp : m_contacts)
    {
        if (cp.isTrigger) continue;
        RigidBody* a = cp.bodyA;
        RigidBody* b = cp.bodyB;
        const bool movingA = a && !a->IsSleeping() &&
            (a->GetVelocity().LengthSq() + a->GetAngularVelocity().LengthSq()) > WAKE_SPEED_SQ;
        const bool movingB = b && !b->IsSleeping() &&
            (b->GetVelocity().LengthSq() + b->GetAngularVelocity().LengthSq()) > WAKE_SPEED_SQ;
        if (movingA && b && b->IsSleeping()) b->WakeUp();
        if (movingB && a && a->IsSleeping()) a->WakeUp();
    }
}

void World::UpdateSleepStates(float dt)
{
    constexpr float LINEAR_SLEEP_THRESHOLD = 0.03f;
    constexpr float ANGULAR_SLEEP_THRESHOLD = 0.03f;
    constexpr float SLEEP_TIME = 0.75f;
    for (auto& body : m_bodies)
    {
        if (body)
            body->UpdateSleepState(dt, LINEAR_SLEEP_THRESHOLD, ANGULAR_SLEEP_THRESHOLD, SLEEP_TIME);
    }
}

void World::CCDPhase(float dt)
{
    if (dt <= 0.0f)
        return;

    struct SphereEntry {
        size_t bodyIndex;
        const SphereCollider* sphere;
    };
    std::unordered_map<const RigidBody*, const SphereCollider*> firstSpheres;
    std::vector<SphereEntry> spheres;
    bool indexed = false;
    for (size_t bodyIndexA = 0; bodyIndexA < m_bodies.size(); ++bodyIndexA)
    {
        RigidBody* bodyA = m_bodies[bodyIndexA];
        if (!bodyA->m_useCCD || bodyA->IsStatic())
            continue;
        const float localDtA = m_effectiveDts[bodyIndexA];
        if (!CCDSolver::NeedsCCD(*bodyA, bodyA->m_ccdRadius, localDtA))
            continue;

        if (!indexed) {
            indexed = true;
            /// @note 最初の CCD 対象が見つかった時点で、登録順で最初の有効な球を剛体ごとに選ぶ。
            for (size_t colliderIndex = 0; colliderIndex < m_colliders.size(); ++colliderIndex) {
                const auto& instance = m_colliders[colliderIndex];
                if (instance.body && IsLiveCollider(colliderIndex)
                    && instance.collider->GetType() == ColliderType::SPHERE) {
                    firstSpheres.try_emplace(instance.body,
                        static_cast<const SphereCollider*>(instance.collider));
                }
            }
            spheres.reserve(firstSpheres.size());
            for (size_t bodyIndex = 0; bodyIndex < m_bodies.size(); ++bodyIndex) {
                const auto sphere = firstSpheres.find(m_bodies[bodyIndex]);
                if (sphere != firstSpheres.end())
                    spheres.push_back({bodyIndex, sphere->second});
            }
        }
        const auto firstSphere = firstSpheres.find(bodyA);
        if (firstSphere == firstSpheres.end())
            continue;
        const auto* sphereA = firstSphere->second;

        const math::Vector3 centerA = sphereA->GetAABB().Center();
        const float radiusA = sphereA->m_radius;
        float earliestToi = 1.0f;

        for (const SphereEntry& entry : spheres)
        {
            const size_t bodyIndexB = entry.bodyIndex;
            if (bodyIndexA == bodyIndexB)
                continue;
            const RigidBody* bodyB = m_bodies[bodyIndexB];
            const auto* sphereB = entry.sphere;

            const math::Vector3 centerB = sphereB->GetAABB().Center();
            const float radiusB = sphereB->m_radius;

            /// @note 剛体ごとの時間倍率を速度に掛け、同じサブステップ幅で両者の変位を比較する。
            const math::Vector3 scaledVelocityA = bodyA->GetVelocity() * (localDtA / dt);
            const math::Vector3 scaledVelocityB = bodyB->IsStatic() ? math::Vector3::ZERO
                : bodyB->GetVelocity() * (m_effectiveDts[bodyIndexB] / dt);
            const CCDResult hit = CCDSolver::SweptSphereSphere(
                centerA, radiusA, scaledVelocityA - scaledVelocityB, centerB, radiusB, dt);

            if (hit.hit && hit.toi < earliestToi)
                earliestToi = hit.toi;
        }

        /// @note 衝突前の移動量を制限し、応答は通常の接触解決に任せる。後続の TOI はここで更新した速度を読む。
        if (earliestToi < 1.0f)
            bodyA->SetVelocity(bodyA->GetVelocity() * earliestToi);
    }
}

void World::RecordApproachVelocities()
{
    for (const auto& cp : m_contacts)
    {
        if (cp.isTrigger) continue;

        const math::Vector3 vRel = PhysicsSolver::RelativeVelocityAt(cp);
        /// @note normal は b → a 向きなので、近づいているとき Dot は負になる。
        /// @note ゲーム側が扱いやすいよう「正 = 接近」へ符号を反転する。
        const float approach = -math::Vector3::Dot(vRel, cp.normal);
        /// @note 離れていく接触は衝突ではない
        if (approach <= 0.0f) continue;

        const Collider* a = cp.colliderA;
        const Collider* b = cp.colliderB;
        if (a > b) std::swap(a, b);

        ContactImpact& impact = m_frameImpacts[ColliderPair{ a, b }];
        if (approach > impact.approachSpeed) {
            impact.approachSpeed    = approach;
            impact.relativeVelocity = vRel;
        }
    }
}

void World::RecordContactImpulses()
{
    for (const auto& cp : m_contacts)
    {
        if (cp.isTrigger) continue;

        const Collider* a = cp.colliderA;
        const Collider* b = cp.colliderB;
        if (a > b) std::swap(a, b);

        /// @note 接近していない接触 (床に載っているだけ等) は RecordApproachVelocities が
        /// @note 積んでいない。そこへインパルスだけを入れると «速度 0 なのに強い衝突» に
        /// @note 見えるエントリができるため find で確認してから加える。
        const auto it = m_frameImpacts.find(ColliderPair{ a, b });
        if (it == m_frameImpacts.end()) continue;

        it->second.normalImpulse = std::max(it->second.normalImpulse, cp.cachedNormalImpulse);
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
        CollisionEvent event{
            cp.colliderA, cp.colliderB, cp.bodyA, cp.bodyB,
            cp.point, cp.normal, cp.depth, cp.isTrigger
        };
        /// @note このフレーム中に観測した衝突の強さを載せる。
        /// @note 接触が継続しているだけ (Stay) なら 0 のままになる。
        if (const auto impact = m_frameImpacts.find(pair); impact != m_frameImpacts.end()) {
            event.relativeVelocity = impact->second.relativeVelocity;
            event.approachSpeed    = impact->second.approachSpeed;
            event.normalImpulse    = impact->second.normalImpulse;
        }
        currentEvents.insert({ pair, event });
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
        if (currentEvents.count(pair) != 0) continue;

        /// @note 離れた瞬間のイベントに「ぶつかった強さ」は無い。前フレームの値を
        /// @note そのまま残すと、Exit を見ているスクリプトが古い衝突速度を読んでしまう。
        CollisionEvent exitEvent = event;
        exitEvent.relativeVelocity = math::Vector3::ZERO;
        exitEvent.approachSpeed    = 0.0f;
        exitEvent.normalImpulse    = 0.0f;
        m_exitEvents.push_back(exitEvent);
    }

    m_prevEvents = std::move(currentEvents);
}

const std::vector<CollisionEvent>& World::GetEnterEvents() const { return m_enterEvents; }
const std::vector<CollisionEvent>& World::GetStayEvents()  const { return m_stayEvents;  }
const std::vector<CollisionEvent>& World::GetExitEvents()  const { return m_exitEvents;  }

} /// @note namespace fbzz::physics
