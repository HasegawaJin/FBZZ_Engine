/// @file    ClothContactGeometry.cpp
/// @brief   布の近接判定に使う最近点、接触法線、逆質量による位置補正。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "ClothContactGeometry.hpp"
#include "ClothSolverDetail.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace fbzz::physics::cloth_detail {

namespace {

/// @note 近接判定で三角形の内部とみなす重心座標の下限。辺・頂点付近は辺–辺と質点球に任せる。
constexpr float INTERIOR_WEIGHT = 1.0e-4f;

float SafeRatio(float numerator, float denominator)
{
    return std::abs(denominator) > 1.0e-20f ? numerator / denominator : 0.0f;
}

/// @return p に最も近い三角形 abc 上の点の重心座標 (wa, wb, wc)。
/// @see https://realtimecollisiondetection.net/ Ericson, Real-Time Collision Detection 5.1.5 Closest Point on Triangle to Point。
Vector3 TriangleWeights(const Vector3& p, const Vector3& a, const Vector3& b, const Vector3& c)
{
    const Vector3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = Vector3::Dot(ab, ap), d2 = Vector3::Dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return {1.0f, 0.0f, 0.0f};
    }
    const Vector3 bp = p - b;
    const float d3 = Vector3::Dot(ab, bp), d4 = Vector3::Dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        return {0.0f, 1.0f, 0.0f};
    }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = SafeRatio(d1, d1 - d3);
        return {1.0f - v, v, 0.0f};
    }
    const Vector3 cp = p - c;
    const float d5 = Vector3::Dot(ab, cp), d6 = Vector3::Dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        return {0.0f, 0.0f, 1.0f};
    }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = SafeRatio(d2, d2 - d6);
        return {1.0f - w, 0.0f, w};
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && d4 - d3 >= 0.0f && d5 - d6 >= 0.0f) {
        const float w = SafeRatio(d4 - d3, (d4 - d3) + (d5 - d6));
        return {0.0f, 1.0f - w, w};
    }
    const float sum = va + vb + vc;
    if (sum <= 1.0e-20f) {
        return {1.0f, 0.0f, 0.0f};
    }
    const float v = vb / sum, w = vc / sum;
    return {1.0f - v - w, v, w};
}

/// @return 線分 p0p1 と q0q1 の最近点の媒介変数 (s, t)。いずれも [0,1]。
/// @see https://realtimecollisiondetection.net/ Ericson, Real-Time Collision Detection 5.1.9 Closest Points of Two Line Segments。
std::pair<float, float> SegmentParameters(const Vector3& p0, const Vector3& p1, const Vector3& q0, const Vector3& q1)
{
    const Vector3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
    const float a = Vector3::Dot(d1, d1), e = Vector3::Dot(d2, d2), f = Vector3::Dot(d2, r);
    constexpr float TINY = 1.0e-20f;
    if (a <= TINY && e <= TINY) {
        return {0.0f, 0.0f};
    }
    if (a <= TINY) {
        return {0.0f, std::clamp(f / e, 0.0f, 1.0f)};
    }
    const float c = Vector3::Dot(d1, r);
    if (e <= TINY) {
        return {std::clamp(-c / a, 0.0f, 1.0f), 0.0f};
    }
    const float b = Vector3::Dot(d1, d2);
    const float denominator = a * e - b * b;
    float s = denominator > a * e * 1.0e-6f ? std::clamp((b * f - c * e) / denominator, 0.0f, 1.0f) : 0.0f;
    float t = (b * s + f) / e;
    if (t < 0.0f) {
        t = 0.0f;
        s = std::clamp(-c / a, 0.0f, 1.0f);
    } else if (t > 1.0f) {
        t = 1.0f;
        s = std::clamp((b - c) / a, 0.0f, 1.0f);
    }
    return {s, t};
}

}

bool BoxesOverlapYZ(const Vector3& lowerA, const Vector3& upperA, const Vector3& lowerB, const Vector3& upperB)
{
    return lowerA.y <= upperB.y && lowerB.y <= upperA.y && lowerA.z <= upperB.z && lowerB.z <= upperA.z;
}

/// @note 質点–三角形は p - (wa a + wb b + wc c)、辺–辺は p(s) - q(t) を表す係数。
ContactWeights PairWeights(const std::vector<Vector3>& positions, const ParticleQuad& particles,
                           bool edgeEdge, bool& outInterior)
{
    if (!edgeEdge) {
        const Vector3 barycentric = TriangleWeights(positions[particles[0]], positions[particles[1]],
                                                    positions[particles[2]], positions[particles[3]]);
        outInterior = barycentric.x > INTERIOR_WEIGHT && barycentric.y > INTERIOR_WEIGHT
            && barycentric.z > INTERIOR_WEIGHT;
        return {1.0f, -barycentric.x, -barycentric.y, -barycentric.z};
    }
    const auto [fractionA, fractionB] = SegmentParameters(
        positions[particles[0]], positions[particles[1]], positions[particles[2]], positions[particles[3]]);
    outInterior = fractionA > 0.0f && fractionA < 1.0f && fractionB > 0.0f && fractionB < 1.0f;
    return {1.0f - fractionA, fractionA, -(1.0f - fractionB), -fractionB};
}

Vector3 WeightedOffset(const std::vector<Vector3>& positions, const ParticleQuad& particles,
                       const ContactWeights& weights)
{
    return positions[particles[0]] * weights[0] + positions[particles[1]] * weights[1]
        + positions[particles[2]] * weights[2] + positions[particles[3]] * weights[3];
}

/// @brief 現在の面法線 (辺–辺は 2 辺の外積) を、開始時刻に質点が居た側へ向ける。
/// @return 法線が縮退、または開始時の側が決まらなければ false。
bool OrientContactNormal(const std::vector<Vector3>& positions, const std::vector<Vector3>& previous,
                         const ParticleQuad& particles, bool edgeEdge,
                         const ContactWeights& weights, Vector3& outNormal)
{
    const Vector3 unnormalized = edgeEdge
        ? Vector3::Cross(positions[particles[1]] - positions[particles[0]],
                         positions[particles[3]] - positions[particles[2]])
        : Vector3::Cross(positions[particles[2]] - positions[particles[1]],
                         positions[particles[3]] - positions[particles[1]]);
    const float lengthSq = unnormalized.LengthSq();
    if (!(lengthSq > 1.0e-20f)) {
        return false;
    }
    outNormal = unnormalized / std::sqrt(lengthSq);
    const float side = Vector3::Dot(WeightedOffset(previous, particles, weights), outNormal);
    if (std::abs(side) <= 1.0e-7f) {
        return false;
    }
    if (side < 0.0f) {
        outNormal = -outNormal;
    }
    return true;
}

/// @brief dot(Σ c_i x_i, n) を penetration だけ増やす逆質量重み付きの PBD 射影。
/// @see https://matthias-research.github.io/pages/publications/posBasedDyn.pdf Müller et al. 2007, 3.3 Solver と 4.4 Collisions の三角形衝突。
void ProjectSeparation(std::vector<Vector3>& positions, const std::vector<float>& inverseMasses,
                       const ParticleQuad& particles, const ContactWeights& weights, const Vector3& normal, float penetration)
{
    float weightedInverseMass = 0.0f;
    for (int i = 0; i < 4; ++i) {
        weightedInverseMass += weights[i] * weights[i] * inverseMasses[particles[i]];
    }
    if (weightedInverseMass <= GEOMETRY_EPSILON) {
        return;
    }
    const float scale = penetration / weightedInverseMass;
    for (int i = 0; i < 4; ++i) {
        positions[particles[i]] += normal * (scale * weights[i] * inverseMasses[particles[i]]);
    }
}

}
