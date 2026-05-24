// FBZZ Engine
// ContactCache.hpp | fbzz::physics
// フレーム間接触インパルスのキャッシュ (Warm Starting 用)
#pragma once
#include <map>
#include <utility>
#include <vector>
#include <Physics/ContactPoint.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::physics
{

    // キー: コライダーポインタペアを a < b に正規化
    struct ContactKey
    {
        const Collider* a;
        const Collider* b;
        bool operator<(const ContactKey& rhs) const
        {
            if (a != rhs.a) return a < rhs.a;
            return b < rhs.b;
        }
    };

    class ContactCache
    {
    public:
        // 接触点が前フレームの同一ペアと近傍と見なす距離の閾値
        static constexpr float MATCH_RADIUS_SQ = 0.05f * 0.05f;

        // NarrowPhase 後に呼ぶ。前フレームの蓄積インパルスを cp に書き戻す
        void WarmStart(std::vector<ContactPoint>& contacts);

        // Resolve 後に呼ぶ。今フレームの蓄積インパルスをキャッシュに保存する
        void UpdateCache(const std::vector<ContactPoint>& contacts);

        // フレーム末尾で呼ぶ。一定フレーム以上接触がないエントリを削除する
        void PurgeStale();

    private:
        struct CacheEntry
        {
            math::Vector3 point;
            float         normalImpulse       = 0.0f;
            float         tangentImpulse[2]   = {0.0f, 0.0f};
            int           age                 = 0;   // UpdateCache が呼ばれるたびにリセット
                                                     // WarmStart のみ呼ばれると +1
        };

        std::map<ContactKey, std::vector<CacheEntry>> m_cache;
    };

} // namespace fbzz::physics
