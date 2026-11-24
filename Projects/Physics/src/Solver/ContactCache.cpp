/// @file    ContactCache.cpp
/// @brief   フレーム間接触インパルスのキャッシュ (Warm Starting 用)。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#include <Physics/Solver/ContactCache.hpp>
#include <Physics/Dynamics/RigidBody.hpp>

#include <algorithm>
#include <utility>

namespace fbzz::physics
{
    namespace
    {
        ContactKey MakeContactKey(const ContactPoint& contact)
        {
            ContactKey key{contact.colliderA, contact.colliderB};
            if (key.a > key.b)
                std::swap(key.a, key.b);
            return key;
        }
    }

    void ContactCache::WarmStart(std::vector<ContactPoint>& contacts)
    {
        for (auto& cp : contacts)
        {
            if (cp.isTrigger || !cp.colliderA || !cp.colliderB)
                continue;

            const auto cachedPair = m_cache.find(MakeContactKey(cp));
            if (cachedPair == m_cache.end())
                continue;

            /// @note 最初の近傍エントリだけを使う。法線が合わない場合も次の候補には進まない。
            for (const auto& entry : cachedPair->second)
            {
                const float distanceSq = (cp.point - entry.point).LengthSq();
                if (distanceSq > MATCH_RADIUS_SQ)
                    continue;

                /// @note 三角形の継ぎ目などで法線が大きく変わると、前の接触基底は再利用できない。
                if (entry.normal.LengthSq() <= 1e-6f ||
                    math::Vector3::Dot(entry.normal, cp.normal) < 0.98f)
                {
                    break;
                }

                /// @note 前回の基底からワールド空間へ戻し、今回の法線・接線に射影し直す。
                const math::Vector3 cachedImpulse =
                    entry.normal * entry.normalImpulse +
                    entry.tangent[0] * entry.tangentImpulse[0] +
                    entry.tangent[1] * entry.tangentImpulse[1];

                cp.cachedNormalImpulse = std::max(0.0f, math::Vector3::Dot(cachedImpulse, cp.normal));
                cp.cachedTangentImpulse[0] = math::Vector3::Dot(cachedImpulse, cp.tangent[0]);
                cp.cachedTangentImpulse[1] = math::Vector3::Dot(cachedImpulse, cp.tangent[1]);

                const math::Vector3 normalImpulse = cp.normal * cp.cachedNormalImpulse;
                const math::Vector3 tangentImpulse = cp.tangent[0] * cp.cachedTangentImpulse[0] +
                    cp.tangent[1] * cp.cachedTangentImpulse[1];
                const math::Vector3 totalImpulse = normalImpulse + tangentImpulse;
                if (totalImpulse.LengthSq() <= 1e-12f)
                    break;

                if (cp.bodyA)
                {
                    const float invMassA = cp.bodyA->GetInvMass();
                    cp.bodyA->SetVelocity(cp.bodyA->GetVelocity() + totalImpulse * invMassA);
                    const math::Vector3 offsetA = cp.point - cp.bodyA->GetPosition();
                    cp.bodyA->ApplyAngularImpulse(math::Vector3::Cross(offsetA, totalImpulse));
                }
                if (cp.bodyB)
                {
                    const float invMassB = cp.bodyB->GetInvMass();
                    cp.bodyB->SetVelocity(cp.bodyB->GetVelocity() - totalImpulse * invMassB);
                    const math::Vector3 offsetB = cp.point - cp.bodyB->GetPosition();
                    cp.bodyB->ApplyAngularImpulse(-math::Vector3::Cross(offsetB, totalImpulse));
                }
                break;
            }

            /// @note 同じペアの接触点を調べるたびに、採用しなかったエントリも含めて age を進める。
            for (auto& entry : cachedPair->second)
                ++entry.age;
        }
    }

    void ContactCache::UpdateCache(const std::vector<ContactPoint>& contacts)
    {
        for (const auto& cp : contacts)
        {
            if (cp.isTrigger || !cp.colliderA || !cp.colliderB)
                continue;

            const ContactKey key = MakeContactKey(cp);
            if (!cp.cacheImpulse)
            {
                /// @note 反発が起きたペアは以前の接触点を含めて消し、次の Warm Start に持ち越さない。
                m_cache.erase(key);
                continue;
            }

            const CacheEntry refreshed{
                .point = cp.point,
                .normal = cp.normal,
                .tangent = {cp.tangent[0], cp.tangent[1]},
                .normalImpulse = cp.cachedNormalImpulse,
                .tangentImpulse = {cp.cachedTangentImpulse[0], cp.cachedTangentImpulse[1]},
                .age = 0
            };
            auto& entries = m_cache[key];
            const auto nearby = std::find_if(entries.begin(), entries.end(), [&](const CacheEntry& entry) {
                return (cp.point - entry.point).LengthSq() <= MATCH_RADIUS_SQ;
            });
            if (nearby == entries.end())
                entries.push_back(refreshed);
            else
                *nearby = refreshed;
        }
    }

    void ContactCache::PurgeStale()
    {
        constexpr int MAX_AGE = 2;

        for (auto it = m_cache.begin(); it != m_cache.end(); )
        {
            auto& entries = it->second;
            entries.erase(
                std::remove_if(entries.begin(), entries.end(),
                    [](const CacheEntry& e) { return e.age > MAX_AGE; }),
                entries.end());

            if (entries.empty())
                it = m_cache.erase(it);
            else
                ++it;
        }
    }

}
