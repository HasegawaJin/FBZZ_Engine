// FBZZ Engine
// ContactCache.cpp | fbzz::physics
// フレーム間接触インパルスのキャッシュ (Warm Starting 用)
#include <Physics/ContactCache.hpp>
#include <Physics/RigidBody.hpp>
#include <algorithm>

namespace fbzz::physics
{

    void ContactCache::WarmStart(std::vector<ContactPoint>& contacts)
    {
        for (auto& cp : contacts)
        {
            if (cp.isTrigger) continue;
            if (!cp.colliderA || !cp.colliderB) continue;

            const Collider* a = cp.colliderA;
            const Collider* b = cp.colliderB;
            if (a > b) std::swap(a, b);

            const ContactKey key{ a, b };
            auto it = m_cache.find(key);
            if (it == m_cache.end()) continue;

            // 接触点の近傍にある前フレームのエントリを探す
            for (auto& entry : it->second)
            {
                const float distSq = (cp.point - entry.point).LengthSq();
                if (distSq > MATCH_RADIUS_SQ) continue;

                cp.cachedNormalImpulse      = entry.normalImpulse;
                cp.cachedTangentImpulse[0]  = entry.tangentImpulse[0];
                cp.cachedTangentImpulse[1]  = entry.tangentImpulse[1];

                // Warm Start: 蓄積済みインパルスを即時適用
                if (!cp.isTrigger)
                {
                    const math::Vector3 normalImpulseVec = cp.normal * cp.cachedNormalImpulse;
                    const math::Vector3 tangImpulseVec   =
                        cp.tangent[0] * cp.cachedTangentImpulse[0] +
                        cp.tangent[1] * cp.cachedTangentImpulse[1];
                    const math::Vector3 totalImpulse = normalImpulseVec + tangImpulseVec;
                    if (totalImpulse.LengthSq() <= 1e-12f)
                        break;

                    if (cp.bodyA)
                    {
                        const float invMassA = cp.bodyA->GetInvMass();
                        cp.bodyA->SetVelocity(cp.bodyA->GetVelocity() + totalImpulse * invMassA);
                        const math::Vector3 rA = cp.point - cp.bodyA->GetPosition();
                        cp.bodyA->ApplyAngularImpulse(math::Vector3::Cross(rA, totalImpulse));
                    }
                    if (cp.bodyB)
                    {
                        const float invMassB = cp.bodyB->GetInvMass();
                        cp.bodyB->SetVelocity(cp.bodyB->GetVelocity() - totalImpulse * invMassB);
                        const math::Vector3 rB = cp.point - cp.bodyB->GetPosition();
                        cp.bodyB->ApplyAngularImpulse(-math::Vector3::Cross(rB, totalImpulse));
                    }
                }
                break;
            }

            // age をインクリメント (UpdateCache が呼ばれるまで増え続ける)
            for (auto& entry : it->second)
                entry.age++;
        }
    }

    void ContactCache::UpdateCache(const std::vector<ContactPoint>& contacts)
    {
        // 今フレームの接触結果でキャッシュを更新する
        // 既存エントリを上書き、なければ新規追加
        for (const auto& cp : contacts)
        {
            if (cp.isTrigger) continue;
            if (!cp.colliderA || !cp.colliderB) continue;

            const Collider* a = cp.colliderA;
            const Collider* b = cp.colliderB;
            if (a > b) std::swap(a, b);

            const ContactKey key{ a, b };
            if (!cp.cacheImpulse)
            {
                m_cache.erase(key);
                continue;
            }

            auto& entries = m_cache[key];

            // 近傍エントリを検索して更新
            bool found = false;
            for (auto& entry : entries)
            {
                if ((cp.point - entry.point).LengthSq() <= MATCH_RADIUS_SQ)
                {
                    entry.point              = cp.point;
                    entry.normalImpulse      = cp.cachedNormalImpulse;
                    entry.tangentImpulse[0]  = cp.cachedTangentImpulse[0];
                    entry.tangentImpulse[1]  = cp.cachedTangentImpulse[1];
                    entry.age                = 0;
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                CacheEntry e;
                e.point             = cp.point;
                e.normalImpulse     = cp.cachedNormalImpulse;
                e.tangentImpulse[0] = cp.cachedTangentImpulse[0];
                e.tangentImpulse[1] = cp.cachedTangentImpulse[1];
                e.age               = 0;
                entries.push_back(e);
            }
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

} // namespace fbzz::physics
