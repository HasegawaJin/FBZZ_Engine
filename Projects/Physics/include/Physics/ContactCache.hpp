/// @file    ContactCache.hpp
/// @brief   フレーム間接触インパルスのキャッシュ (Warm Starting 用)。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#pragma once
#include <map>
#include <utility>
#include <vector>
#include <Physics/ContactPoint.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::physics
{

    /// キー: コライダーポインタペアを a < b に正規化
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
        /// 接触点が前フレームの同一ペアと近傍と見なす距離の閾値。
        /// 位置が少し揺れても同じ接触として扱い、静止接触のジッターを抑える。
        static constexpr float MATCH_RADIUS_SQ = 0.05f * 0.05f;

        /// NarrowPhase 後に呼ぶ。前フレームの蓄積インパルスを cp に書き戻す
        void WarmStart(std::vector<ContactPoint>& contacts);

        /// Resolve 後に呼ぶ。今フレームの蓄積インパルスをキャッシュに保存する
        void UpdateCache(const std::vector<ContactPoint>& contacts);

        /// フレーム末尾で呼ぶ。一定フレーム以上接触がないエントリを削除する。
        /// Collider のポインタをキーにするため、古いエントリを残し続けない。
        void PurgeStale();

    private:
        struct CacheEntry
        {
            math::Vector3 point;
            /// @note 前フレームの接触基底。法線が変わったときに摩擦インパルスを
            ///       現フレームの接線へ変換するために保持する。
            math::Vector3 normal            = math::Vector3::ZERO;
            math::Vector3 tangent[2]        = {
                math::Vector3::ZERO,
                math::Vector3::ZERO
            };
            float         normalImpulse       = 0.0f;
            float         tangentImpulse[2]   = {0.0f, 0.0f};
            /// @note UpdateCache が呼ばれるたびにリセット
            int           age                 = 0;
                                                     /// @note WarmStart のみ呼ばれると +1
        };

        std::map<ContactKey, std::vector<CacheEntry>> m_cache;
    };

} // namespace fbzz::physics
