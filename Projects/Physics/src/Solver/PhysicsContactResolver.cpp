/// @file    PhysicsContactResolver.cpp
/// @brief   接触島の編成と PGS による衝突応答。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/Collision/NarrowPhase/PhysicsSolver.hpp>
#include <Physics/Dynamics/PhysicsMaterial.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace fbzz::physics
{
    void PhysicsSolver::PrepareContactTangents(std::vector<ContactPoint>& contacts)
    {
        for (auto& contact : contacts)
        {
            if (contact.isTrigger)
                continue;

            math::Vector3 firstTangent = math::Vector3::Cross(contact.normal, math::Vector3::RIGHT);
            if (firstTangent.LengthSq() < 1e-6f)
                firstTangent = math::Vector3::Cross(contact.normal, math::Vector3::UP);

            /// @note 法線が退化した場合は正規化の契約違反を避け、接線に既定軸を使う。
            firstTangent = firstTangent.NormalizedOr(math::Vector3::RIGHT);
            contact.tangent[0] = firstTangent;
            contact.tangent[1] = math::Vector3::Cross(contact.normal, firstTangent).NormalizedOr(math::Vector3::FORWARD);
        }
    }

    namespace
    {
        bool IsActiveContactBody(const RigidBody* body)
        {
            return body && !body->IsStatic() && !body->IsSleeping();
        }

        /// @note 接触点へ軸方向の単位インパルスを加えたとき、回転に由来する同軸方向の速度変化。
        float AngularInverseMassAt(const RigidBody* body, const math::Vector3& offset, const math::Vector3& axis)
        {
            if (!body)
                return 0.0f;

            const math::Vector3 angularImpulse = math::Vector3::Cross(offset, axis);
            const math::Vector3 angularVelocity = body->ApplyInvInertia(angularImpulse);
            return math::Vector3::Dot(math::Vector3::Cross(angularVelocity, offset), axis);
        }
    }

    void PhysicsSolver::Resolve(std::vector<ContactPoint>& contacts)
    {
        if (contacts.empty())
            return;

        PrepareContactTangents(contacts);

        if (contacts.size() == 1)
        {
            ContactPoint& contact = contacts.front();
            if (contact.isTrigger)
                return;
            const bool activeA = IsActiveContactBody(contact.bodyA);
            const bool activeB = IsActiveContactBody(contact.bodyB);
            if (!activeA && !activeB)
                return;

            /// @note 単一接触では接触島の構築とバッファ確保を省ける。
            for (int iteration = 0; iteration < VELOCITY_ITER; ++iteration)
            {
                ResolveVelocity(contact);
                ResolveFriction(contact);
            }
            ResolvePosition(contact);
            return;
        }

        /// @note サブステップごとの再確保を減らすため、接触島の配列とマップの容量をスレッド内で再利用する。
        static thread_local std::vector<std::vector<size_t>> islands;
        static thread_local std::unordered_map<RigidBody*, size_t> bodyToIsland;
        islands.clear();
        islands.reserve(contacts.size());
        bodyToIsland.clear();
        bodyToIsland.reserve(contacts.size() * 2);

        for (size_t contactIndex = 0; contactIndex < contacts.size(); ++contactIndex)
        {
            const ContactPoint& contact = contacts[contactIndex];
            if (contact.isTrigger)
                continue;

            /// @note 静的・休止中の剛体は接触島の接続に使わない。たとえば共通の床だけでは島はつながらない。
            RigidBody* activeA = IsActiveContactBody(contact.bodyA) ? contact.bodyA : nullptr;
            RigidBody* activeB = IsActiveContactBody(contact.bodyB) ? contact.bodyB : nullptr;
            if (!activeA && !activeB)
                continue;

            const auto islandA = activeA ? bodyToIsland.find(activeA) : bodyToIsland.end();
            const auto islandB = activeB ? bodyToIsland.find(activeB) : bodyToIsland.end();
            size_t islandIndex = islands.size();
            if (islandA != bodyToIsland.end())
                islandIndex = islandA->second;
            else if (islandB != bodyToIsland.end())
                islandIndex = islandB->second;
            else
                islands.emplace_back();

            if (islandA != bodyToIsland.end() && islandB != bodyToIsland.end() && islandIndex != islandB->second)
            {
                /// @note PGS の解決順を保つため、B の島を A の末尾に追加し、元の島番号は空けておく。
                const size_t mergedIndex = islandB->second;
                auto& island = islands[islandIndex];
                auto& merged = islands[mergedIndex];
                island.insert(island.end(), merged.begin(), merged.end());
                for (auto& entry : bodyToIsland)
                {
                    if (entry.second == mergedIndex)
                        entry.second = islandIndex;
                }
                merged.clear();
            }

            if (activeA)
                bodyToIsland[activeA] = islandIndex;
            if (activeB)
                bodyToIsland[activeB] = islandIndex;
            islands[islandIndex].push_back(contactIndex);
        }

        for (const auto& island : islands)
        {
            if (island.empty())
                continue;

            for (int iteration = 0; iteration < VELOCITY_ITER; ++iteration)
            {
                for (size_t contactIndex : island)
                {
                    ContactPoint& contact = contacts[contactIndex];
                    ResolveVelocity(contact);
                    ResolveFriction(contact);
                }
            }

            for (size_t contactIndex : island)
                ResolvePosition(contacts[contactIndex]);
        }
    }

    /// @name ResolveVelocity (PGS)
    math::Vector3 PhysicsSolver::RelativeVelocityAt(const ContactPoint& contact)
    {
        const RigidBody* bodyA = contact.bodyA;
        const RigidBody* bodyB = contact.bodyB;

        const math::Vector3 vA = bodyA ? bodyA->GetVelocity()        : math::Vector3::ZERO;
        const math::Vector3 vB = bodyB ? bodyB->GetVelocity()        : math::Vector3::ZERO;
        const math::Vector3 wA = bodyA ? bodyA->GetAngularVelocity() : math::Vector3::ZERO;
        const math::Vector3 wB = bodyB ? bodyB->GetAngularVelocity() : math::Vector3::ZERO;

        const math::Vector3 offsetA = bodyA ? contact.point - bodyA->GetPosition() : math::Vector3::ZERO;
        const math::Vector3 offsetB = bodyB ? contact.point - bodyB->GetPosition() : math::Vector3::ZERO;

        /// @note 角速度による接触点の速度も含める。回転しながらぶつかる物体では
        /// @note 重心速度だけを見ると実際の当たりの強さと合わない。
        const math::Vector3 vAContact = vA + math::Vector3::Cross(wA, offsetA);
        const math::Vector3 vBContact = vB + math::Vector3::Cross(wB, offsetB);
        return vAContact - vBContact;
    }

    void PhysicsSolver::ResolveVelocity(ContactPoint& contact)
    {
        if (contact.isTrigger) return;

        RigidBody* bodyA = contact.bodyA;
        RigidBody* bodyB = contact.bodyB;

        const float invMassA = bodyA ? bodyA->GetInvMass() : 0.0f;
        const float invMassB = bodyB ? bodyB->GetInvMass() : 0.0f;

        const math::Vector3 offsetA = bodyA ? contact.point - bodyA->GetPosition() : math::Vector3::ZERO;
        const math::Vector3 offsetB = bodyB ? contact.point - bodyB->GetPosition() : math::Vector3::ZERO;

        const math::Vector3 relativeVelocity  = RelativeVelocityAt(contact);
        const float         normalSpeed = math::Vector3::Dot(relativeVelocity, contact.normal);

        /// @note Warm Start の過去インパルスが強すぎると小さい Collider が大きい床上で微小な
        /// @note 上向き速度を持つ。cachedNormalImpulse が残る接触は即 return せず、下の PGS
        /// @note 累積クランプで過剰分を戻す。
        if (normalSpeed > 0.0f && (!contact.cacheImpulse || contact.cachedNormalImpulse <= 0.0f)) return;

        float restitution = 0.3f;
        if (contact.materialA && contact.materialB)
            restitution = PhysicsMaterial::CombineRestitution(*contact.materialA, *contact.materialB);

        /// @note 静止接触の微小反発を消し、床上の物体が跳ね続けるのを防ぐ。
        constexpr float REST_THRESHOLD = 0.5f;
        if (normalSpeed >= 0.0f || std::abs(normalSpeed) < REST_THRESHOLD) restitution = 0.0f;
        const bool usesRestitution = restitution > 0.0f;

        const float angularInvMassA = AngularInverseMassAt(bodyA, offsetA, contact.normal);
        const float angularInvMassB = AngularInverseMassAt(bodyB, offsetB, contact.normal);
        const float effectiveInvMass = invMassA + invMassB + angularInvMassA + angularInvMassB;
        if (effectiveInvMass == 0.0f)
            return;

        /// @note deltaJ は今回追加すべき法線インパルス。蓄積値は 0 未満にしない。
        /// @note 接触面に向かう速度を打ち消し、反発係数 e の分だけ逆向きに戻すのに必要な力積を求める。回転でも接触点が動くので、その分も分母に含める。
        const float deltaJ = -(1.0f + restitution) * normalSpeed / effectiveInvMass;
        float applyJ = 0.0f;
        if (usesRestitution)
        {
            /// @note 反発インパルスは瞬間的な効果なので Warm Start へ持ち越さない。
            applyJ = std::max(0.0f, deltaJ);
            contact.cachedNormalImpulse = applyJ;
            contact.cacheImpulse = false;
        }
        else
        {
            const float oldAccum = contact.cachedNormalImpulse;
            const float newAccum = std::max(0.0f, oldAccum + deltaJ);
            applyJ = newAccum - oldAccum;
            contact.cachedNormalImpulse = newAccum;
        }

        const math::Vector3 impulse = contact.normal * applyJ;
        if (bodyA)
        {
            bodyA->SetVelocity(bodyA->GetVelocity() + impulse * invMassA);
            bodyA->ApplyAngularImpulse(math::Vector3::Cross(offsetA, impulse));
        }
        if (bodyB)
        {
            bodyB->SetVelocity(bodyB->GetVelocity() - impulse * invMassB);
            bodyB->ApplyAngularImpulse(-math::Vector3::Cross(offsetB, impulse));
        }
    }

    /// @name ResolveFriction (PGS)
    void PhysicsSolver::ResolveFriction(ContactPoint& contact)
    {
        if (contact.isTrigger) return;

        RigidBody* bodyA = contact.bodyA;
        RigidBody* bodyB = contact.bodyB;

        const float invMassA = bodyA ? bodyA->GetInvMass() : 0.0f;
        const float invMassB = bodyB ? bodyB->GetInvMass() : 0.0f;
        if (invMassA + invMassB == 0.0f) return;

        float dynamicMu = 0.5f;
        float staticMu = 0.7f;
        if (contact.materialA && contact.materialB)
        {
            dynamicMu = PhysicsMaterial::CombineFriction(*contact.materialA, *contact.materialB);
            staticMu = PhysicsMaterial::CombineStaticFriction(*contact.materialA, *contact.materialB);
        }

        /// @note 各接線の蓄積摩擦を、法線インパルスに比例する範囲へ収める。
        const float maxDynamicFriction = dynamicMu * contact.cachedNormalImpulse;
        const float maxStaticFriction = staticMu * contact.cachedNormalImpulse;

        const math::Vector3 offsetA = bodyA ? contact.point - bodyA->GetPosition() : math::Vector3::ZERO;
        const math::Vector3 offsetB = bodyB ? contact.point - bodyB->GetPosition() : math::Vector3::ZERO;

        for (int tangentIndex = 0; tangentIndex < 2; ++tangentIndex)
        {
            /// @note 先の接線で適用したインパルスを次の接線の PGS 計算が読むため、軸ごとに速度を取得する。
            const math::Vector3 vA = bodyA ? bodyA->GetVelocity()        : math::Vector3::ZERO;
            const math::Vector3 vB = bodyB ? bodyB->GetVelocity()        : math::Vector3::ZERO;
            const math::Vector3 wA = bodyA ? bodyA->GetAngularVelocity() : math::Vector3::ZERO;
            const math::Vector3 wB = bodyB ? bodyB->GetAngularVelocity() : math::Vector3::ZERO;
            const math::Vector3 relativeVelocity = (vA + math::Vector3::Cross(wA, offsetA))
                                     - (vB + math::Vector3::Cross(wB, offsetB));
            const math::Vector3& tangent = contact.tangent[tangentIndex];
            const float tangentSpeed = math::Vector3::Dot(relativeVelocity, tangent);

            const float angularInvMassA = AngularInverseMassAt(bodyA, offsetA, tangent);
            const float angularInvMassB = AngularInverseMassAt(bodyB, offsetB, tangent);
            const float effectiveInvMass = invMassA + invMassB + angularInvMassA + angularInvMassB;
            if (effectiveInvMass == 0.0f)
                continue;

            const float deltaJt = -tangentSpeed / effectiveInvMass;
            const float oldAccum = contact.cachedTangentImpulse[tangentIndex];
            /// @note 滑りを止めるのに必要な蓄積値が静止摩擦の上限を超えた軸では、動摩擦の上限を使う。
            const float targetAccum = oldAccum + deltaJt;
            const float newAccum = std::abs(targetAccum) <= maxStaticFriction
                ? targetAccum
                : std::clamp(targetAccum, -maxDynamicFriction, maxDynamicFriction);
            const float applyJt  = newAccum - oldAccum;
            contact.cachedTangentImpulse[tangentIndex] = newAccum;

            const math::Vector3 impulse = tangent * applyJt;
            if (bodyA)
            {
                bodyA->SetVelocity(bodyA->GetVelocity() + impulse * invMassA);
                bodyA->ApplyAngularImpulse(math::Vector3::Cross(offsetA, impulse));
            }
            if (bodyB)
            {
                bodyB->SetVelocity(bodyB->GetVelocity() - impulse * invMassB);
                bodyB->ApplyAngularImpulse(-math::Vector3::Cross(offsetB, impulse));
            }
        }
    }

    /// @name ResolvePosition
    void PhysicsSolver::ResolvePosition(ContactPoint& contact)
    {
        if (contact.isTrigger) return;

        const float invMassA = contact.bodyA ? contact.bodyA->GetInvMass() : 0.0f;
        const float invMassB = contact.bodyB ? contact.bodyB->GetInvMass() : 0.0f;
        const float invMassSum = invMassA + invMassB;
        if (invMassSum == 0.0f) return;

        /// @note SLOP 分の浅い貫通は許容し、接触面の小さな振動を抑える。
        const float penetration = std::max(contact.depth - SLOP, 0.0f);
        const float scalar      = penetration / invMassSum * BAUMGARTE * contact.positionCorrectionWeight;
        math::Vector3 correction = contact.normal * scalar;

        if (contact.bodyA)
            contact.bodyA->SetPosition(contact.bodyA->GetPosition() + correction * invMassA);
        if (contact.bodyB)
            contact.bodyB->SetPosition(contact.bodyB->GetPosition() - correction * invMassB);
    }
}
