/// @file    WorldQueryCast.hpp
/// @brief   レイキャストと球キャストで共有する経路、ヒット生成、最近接ヒットの選択。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Physics/World/World.hpp>
#include <cmath>
#include <limits>
#include <optional>

namespace fbzz::physics::world_query_detail {

/// @note origin は呼び出し側が所有し、同期的なキャストの間だけ参照する。
struct CastPath {
    const math::Vector3& origin;
    math::Vector3 unitDirection;
    float maxDistance;
};

/// @return スクリプトが渡すゼロ方向など、長さが閾値未満なら空。
[[nodiscard]] inline std::optional<CastPath> MakeCastPath(
    const math::Vector3& origin, const math::Vector3& direction, float maxDistance)
{
    constexpr float MIN_DIRECTION_LENGTH_SQUARED = 1e-12f;
    if (direction.LengthSq() < MIN_DIRECTION_LENGTH_SQUARED) {
        return std::nullopt;
    }
    return CastPath{origin, direction.Normalized(), maxDistance};
}

/// @note 球キャストの point は接触時点の球の中心を表す。
[[nodiscard]] inline World::RaycastHit MakeCastHit(const CastPath& path,
    const ColliderInstance& candidate, float hitDistance, const math::Vector3& hitNormal)
{
    return {
        .point = path.origin + path.unitDirection * hitDistance,
        .normal = hitNormal,
        .distance = hitDistance,
        .collider = candidate.collider,
        .body = candidate.body,
    };
}

class NearestCastHit {
public:
    /// @note BVH の走査順に依存せず、同距離では先に登録されたコライダーを選ぶ。
    void Consider(const CastPath& path, size_t colliderIndex, const ColliderInstance& candidate,
        float hitDistance, const math::Vector3& hitNormal)
    {
        if (hitDistance < m_hit.distance || (hitDistance == m_hit.distance && colliderIndex < m_colliderIndex)) {
            m_hit = MakeCastHit(path, candidate, hitDistance, hitNormal);
            m_colliderIndex = colliderIndex;
        }
    }

    [[nodiscard]] bool WriteResult(float maxDistance, World::RaycastHit& outHit) const
    {
        if (!std::isfinite(m_hit.distance) || m_hit.distance > maxDistance) {
            return false;
        }
        outHit = m_hit;
        return true;
    }

private:
    World::RaycastHit m_hit{.distance = std::numeric_limits<float>::infinity()};
    size_t m_colliderIndex = SIZE_MAX;
};

} /// @note namespace fbzz::physics::world_query_detail
