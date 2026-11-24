/// @file    ContactCache.hpp
/// @brief   フレーム間接触インパルスのキャッシュ (Warm Starting 用)。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#pragma once
#include <Physics/Api.hpp>
#include <map>
#include <utility>
#include <vector>
#include <Physics/Solver/ContactPoint.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>

namespace fbzz::physics
{

    /// @note キー: コライダーポインタペアを a < b に正規化
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
        /// @note 接触点が前フレームの同一ペアと近傍と見なす距離の閾値。
        /// @note 位置が少し揺れても同じ接触として扱い、静止接触のジッターを抑える。
        static constexpr float MATCH_RADIUS_SQ = 0.05f * 0.05f;

        /// @pre NarrowPhase 後に接触法線と摩擦の接線基底を設定済みであること。
        /// @note 保存したインパルスを現在の基底に射影し、蓄積値に設定して剛体の速度にも反映する。
        FBZZ_PHYSICS_API void WarmStart(std::vector<ContactPoint>& contacts);

        /// @note 最終サブステップの Resolve 後に呼ぶ。cacheImpulse が false の接触はペアごと削除する。
        FBZZ_PHYSICS_API void UpdateCache(const std::vector<ContactPoint>& contacts);

        /// @note フレーム末尾で呼ぶ。WarmStart で進んだ age が閾値を超えたエントリを削除する。
        /// @note Collider のポインタをキーにするため、古いエントリを残し続けない。
        FBZZ_PHYSICS_API void PurgeStale();

    private:
        struct CacheEntry
        {
            math::Vector3 point;
            /// @note 保存したインパルスをワールド空間で復元するため、保存時の接触基底を保持する。
            math::Vector3 normal            = math::Vector3::ZERO;
            math::Vector3 tangent[2]        = {
                math::Vector3::ZERO,
                math::Vector3::ZERO
            };
            float         normalImpulse       = 0.0f;
            float         tangentImpulse[2]   = {0.0f, 0.0f};
            /// @note WarmStart のペア照会ごとに進み、UpdateCache がこのエントリを更新すると 0 に戻る。
            int           age                 = 0;
        };

        std::map<ContactKey, std::vector<CacheEntry>> m_cache;
    };

}
