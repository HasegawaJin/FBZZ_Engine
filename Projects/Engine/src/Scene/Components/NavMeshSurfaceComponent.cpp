// FBZZ Engine
// NavMeshSurfaceComponent.cpp | fbzz::scene
// NavMeshPolygon のメソッド実装（重心・内外判定・距離計算）
#include "Engine/Scene/Components/NavMeshSurfaceComponent.hpp"
#include <limits>

namespace fbzz::scene {

math::Vector3 NavMeshPolygon::Center() const
{
    math::Vector3 sum = math::Vector3::ZERO;
    for (const auto& v : vertices) sum += v;
    return vertices.empty() ? sum : sum / static_cast<float>(vertices.size());
}

// Crossing number 法: 点から +X 方向への半直線とポリゴン各エッジの交差回数を数え、
// 奇数なら内部・偶数なら外部と判定する。
bool NavMeshPolygon::ContainsXZ(float x, float z) const
{
    bool inside = false;
    const size_t n = vertices.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const float xi = vertices[i].x, zi = vertices[i].z;
        const float xj = vertices[j].x, zj = vertices[j].z;
        if (((zi > z) != (zj > z)) &&
            (x < (xj - xi) * (z - zi) / (zj - zi) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

// 各エッジ (線分) までの XZ 平面上の距離の 2 乗のうち最小値を返す。
float NavMeshPolygon::DistanceSqXZ(float x, float z) const
{
    float best = std::numeric_limits<float>::max();
    const size_t n = vertices.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const float xi = vertices[i].x, zi = vertices[i].z;
        const float xj = vertices[j].x, zj = vertices[j].z;

        const float ex = xj - xi, ez = zj - zi;
        const float lenSq = ex * ex + ez * ez;

        float t = lenSq > 0.0f ? ((x - xi) * ex + (z - zi) * ez) / lenSq : 0.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);

        const float px = xi + t * ex, pz = zi + t * ez;
        const float dx = x - px, dz = z - pz;
        const float distSq = dx * dx + dz * dz;
        if (distSq < best) best = distSq;
    }
    return best;
}

} // namespace fbzz::scene
