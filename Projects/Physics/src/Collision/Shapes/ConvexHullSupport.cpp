/// @file    ConvexHullSupport.cpp
/// @brief   凸包の支持点探索と頂点上限に合わせた外周点の選択。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/Collision/Shapes/ConvexHullCollider.hpp>
#include "ConvexHullSupport.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace fbzz::physics
{
    std::size_t convex_hull_detail::FindFarthestPointIndex(std::span<const math::Vector3> points,
                                                           const math::Vector3& direction)
    {
        std::size_t farthestPointIndex = 0;
        float maxProjection = -std::numeric_limits<float>::max();
        for (std::size_t pointIndex = 0; pointIndex < points.size(); ++pointIndex)
        {
            const float projection = math::Vector3::Dot(points[pointIndex], direction);
            if (projection > maxProjection)
            {
                maxProjection = projection;
                farthestPointIndex = pointIndex;
            }
        }
        return farthestPointIndex;
    }

    std::vector<math::Vector3> convex_hull_detail::SelectExtremePoints(std::span<const math::Vector3> points,
                                                                     int pointBudget)
    {
        std::vector<std::size_t> selectedIndices;
        selectedIndices.reserve(static_cast<std::size_t>(pointBudget));

        // フィボナッチ球の方向ごとに外周点を拾い、入力点の並びだけで間引かれるのを防ぐ。
        constexpr float GOLDEN_ANGLE = 2.39996322972865332f;
        for (int directionIndex = 0; directionIndex < pointBudget; ++directionIndex)
        {
            const float height = 1.0f - (static_cast<float>(directionIndex) + 0.5f)
                * 2.0f / static_cast<float>(pointBudget);
            const float ringRadius = std::sqrt(std::max(0.0f, 1.0f - height * height));
            const float azimuth = GOLDEN_ANGLE * static_cast<float>(directionIndex);
            const math::Vector3 direction = {std::cos(azimuth) * ringRadius, height, std::sin(azimuth) * ringRadius};

            const std::size_t pointIndex = FindFarthestPointIndex(points, direction);
            if (std::find(selectedIndices.begin(), selectedIndices.end(), pointIndex) == selectedIndices.end()) {
                selectedIndices.push_back(pointIndex);
            }
        }

        std::vector<math::Vector3> selectedPoints;
        selectedPoints.reserve(selectedIndices.size());
        for (const std::size_t pointIndex : selectedIndices) {
            selectedPoints.push_back(points[pointIndex]);
        }
        return selectedPoints;
    }

    math::Vector3 ConvexHullCollider::SupportPoint(const math::Vector3& direction) const
    {
        if (m_worldVerts.empty()) {
            return m_worldCenter;
        }
        const std::size_t pointIndex = convex_hull_detail::FindFarthestPointIndex(m_worldVerts, direction);
        return m_worldVerts[pointIndex];
    }

    math::Vector3 ConvexHullCollider::SupportFnImpl(const void* shape, const math::Vector3& direction)
    {
        return static_cast<const ConvexHullCollider*>(shape)->SupportPoint(direction);
    }
}
