/// @file    ClothSelfCollision.cpp
/// @brief   同じ布の質点・面・辺の候補探索と自己接触の補正。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/Cloth/ClothSolver.hpp>
#include "ClothSolverDetail.hpp"
#include "ClothContactGeometry.hpp"
#include <algorithm>
#include <cmath>

namespace fbzz::physics {

namespace {

using math::Vector3;

using cloth_detail::Vector3d;
using cloth_detail::ToDouble;

using cloth_detail::Cross;
using cloth_detail::Dot;
using cloth_detail::IsFiniteVector;
using cloth_detail::ParticlePairKey;

using cloth_detail::BoxesOverlapYZ;
using cloth_detail::ContactWeights;
using cloth_detail::OrientContactNormal;
using cloth_detail::PairWeights;
using cloth_detail::ParticleQuad;
using cloth_detail::ProjectSeparation;
using cloth_detail::WeightedOffset;
using cloth_detail::MAX_COLLISION_CANDIDATES;

/// @return 4 点が同一平面になる (0,1] の最初の時刻。符号が変わらなければ負。
/// @note 各点は開始から終了まで等速直線運動とみなす。f(t)=((x1-x0)×(x2-x0))·(x3-x0) の三次式を単調区間ごとに二分する。
/// @see https://graphics.stanford.edu/papers/cloth-sig02/cloth.pdf Bridson et al. 2002, 6.2 Collision Detection の同一平面三次方程式。
double EarliestCoplanarTime(const std::array<Vector3d, 4>& start, const std::array<Vector3d, 4>& end)
{
    const Vector3d a1 = start[1] - start[0], a2 = start[2] - start[0], a3 = start[3] - start[0];
    const Vector3d b1 = (end[1] - end[0]) - a1, b2 = (end[2] - end[0]) - a2, b3 = (end[3] - end[0]) - a3;
    const Vector3d c0 = Cross(a1, a2);
    const Vector3d c1 = Cross(a1, b2) + Cross(b1, a2);
    const Vector3d c2 = Cross(b1, b2);
    const double k[]{Dot(c0, a3), Dot(c0, b3) + Dot(c1, a3), Dot(c1, b3) + Dot(c2, a3), Dot(c2, b3)};
    const double motion = std::max({std::abs(k[1]), std::abs(k[2]), std::abs(k[3])});
    if (!(motion > 1.0e-30) || k[0] == 0.0) return -1.0;
    const auto f = [&k](double t) { return ((k[3] * t + k[2]) * t + k[1]) * t + k[0]; };
    std::array<double, 4> breaks{0.0, 1.0, 1.0, 1.0};
    size_t count = 1;
    /// @note f' = k1 + 2 k2 t + 3 k3 t² の根で単調区間へ分ける。
    const double qa = 3.0 * k[3], qb = 2.0 * k[2], qc = k[1];
    if (std::abs(qa) > 1.0e-30) {
        const double discriminant = qb * qb - 4.0 * qa * qc;
        if (discriminant > 0.0) {
            const double root = std::sqrt(discriminant);
            double r0 = (-qb - root) / (2.0 * qa), r1 = (-qb + root) / (2.0 * qa);
            if (r0 > r1) std::swap(r0, r1);
            for (double r : {r0, r1}) if (r > 0.0 && r < 1.0) breaks[count++] = r;
        }
    } else if (std::abs(qb) > 1.0e-30) {
        const double r = -qc / qb;
        if (r > 0.0 && r < 1.0) breaks[count++] = r;
    }
    breaks[count++] = 1.0;
    for (size_t i = 0; i + 1 < count; ++i) {
        double lo = breaks[i], hi = breaks[i + 1];
        double flo = f(lo);
        const double fhi = f(hi);
        if (flo == 0.0) continue;
        if (fhi == 0.0) return hi;
        if ((flo < 0.0) == (fhi < 0.0)) continue;
        for (int iteration = 0; iteration < 48; ++iteration) {
            const double mid = 0.5 * (lo + hi);
            const double fmid = f(mid);
            if ((fmid < 0.0) == (flo < 0.0)) { lo = mid; flo = fmid; }
            else hi = mid;
        }
        return hi;
    }
    return -1.0;
}

}

/// @see https://nvidiagameworks.github.io/PhysX/3.3/PhysXGuide/Manual/Cloth.html#self-collision-of-a-single-cloth-actor 質点球と rest-position による近傍除外。
bool ClothSolver::SolveSelfContacts()
{
    const float distance = m_settings.selfCollisionDistance;
    if (distance == 0.0f || m_settings.selfCollisionStiffness == 0.0f) return true;
    m_selfCells.clear();
    for (uint32_t particleIndex = 0; particleIndex < m_positions.size(); ++particleIndex) {
        SelfCell entry;
        entry.particle = particleIndex;
        if (!cloth_detail::TryGetCollisionCell(m_positions[particleIndex], distance, entry.cell)) return false;
        m_selfCells.push_back(entry);
    }
    std::sort(m_selfCells.begin(), m_selfCells.end(), [](const SelfCell& a, const SelfCell& b) {
        return a.cell != b.cell ? a.cell < b.cell : a.particle < b.particle;
    });
    size_t candidates = 0;
    for (const auto& entry : m_selfCells) {
        const uint32_t a = entry.particle;
        for (int x = -1; x <= 1; ++x) for (int y = -1; y <= 1; ++y) for (int z = -1; z <= 1; ++z) {
            const std::array<int64_t, 3> cell{entry.cell[0] + x, entry.cell[1] + y, entry.cell[2] + z};
            auto it = std::lower_bound(m_selfCells.begin(), m_selfCells.end(), cell,
                [](const SelfCell& item, const auto& key) { return item.cell < key; });
            for (; it != m_selfCells.end() && it->cell == cell; ++it) {
                const uint32_t b = it->particle;
                if (b <= a) continue;
                /// @note 極端に密集した入力で固定更新を無制限に占有しない。失敗時は Step 全体を巻き戻す。
                if (++candidates > MAX_COLLISION_CANDIDATES) return false;
                const float inverseMassA = m_stepInverseMasses[a], inverseMassB = m_stepInverseMasses[b];
                if (inverseMassA + inverseMassB == 0.0f || std::binary_search(m_selfExcluded.begin(), m_selfExcluded.end(), ParticlePairKey(a, b))) continue;
                const Vector3 rest = m_restPositions[a] - m_restPositions[b];
                if (rest.LengthSq() < distance * distance) continue;
                const Vector3 delta = m_positions[a] - m_positions[b];
                const float length = delta.Length();
                if (length >= distance) continue;
                const Vector3 normal = delta.NormalizedOr(rest.NormalizedOr(Vector3::UP));
                const Vector3 correction = normal * ((distance - length) * m_settings.selfCollisionStiffness / (inverseMassA + inverseMassB));
                m_positions[a] += correction * inverseMassA;
                m_positions[b] -= correction * inverseMassB;
            }
        }
    }
    return true;
}

/// @note 広域判定は x 軸の sort-and-sweep。セル分割と違い高速な質点でも候補数が掃引長に比例して爆発しない。
/// @see https://graphics.stanford.edu/papers/cloth-sig02/cloth.pdf Bridson et al. 2002, 4 Proximity と 6 Collisions の質点–三角形・辺–辺の組。
bool ClothSolver::BuildPrimitivePairs(float inflation)
{
    m_primitivePairs.clear();
    const auto makeBox = [&](std::initializer_list<uint32_t> vertices, uint32_t index, SweepBox& box) {
        box.index = index;
        box.lower = box.upper = m_positions[*vertices.begin()];
        for (uint32_t vertex : vertices) for (const auto* source : {&m_previous, &m_positions}) {
            const Vector3& p = (*source)[vertex];
            box.lower = {std::min(box.lower.x, p.x), std::min(box.lower.y, p.y), std::min(box.lower.z, p.z)};
            box.upper = {std::max(box.upper.x, p.x), std::max(box.upper.y, p.y), std::max(box.upper.z, p.z)};
        }
        box.lower -= Vector3{inflation, inflation, inflation};
        box.upper += Vector3{inflation, inflation, inflation};
        return IsFiniteVector(box.lower) && IsFiniteVector(box.upper);
    };
    const auto byLowerX = [](const SweepBox& a, const SweepBox& b) {
        return a.lower.x != b.lower.x ? a.lower.x < b.lower.x : a.index < b.index;
    };
    m_pointBoxes.resize(m_positions.size());
    for (uint32_t i = 0; i < m_positions.size(); ++i) if (!makeBox({i}, i, m_pointBoxes[i])) return false;
    m_triangleBoxes.resize(m_indices.size() / 3);
    for (uint32_t t = 0; t < m_triangleBoxes.size(); ++t)
        if (!makeBox({m_indices[t * 3], m_indices[t * 3 + 1], m_indices[t * 3 + 2]}, t, m_triangleBoxes[t])) return false;
    m_edgeBoxes.resize(m_edges.size());
    for (uint32_t e = 0; e < m_edges.size(); ++e)
        if (!makeBox({m_edges[e][0], m_edges[e][1]}, e, m_edgeBoxes[e])) return false;
    std::sort(m_pointBoxes.begin(), m_pointBoxes.end(), byLowerX);
    std::sort(m_triangleBoxes.begin(), m_triangleBoxes.end(), byLowerX);
    std::sort(m_edgeBoxes.begin(), m_edgeBoxes.end(), byLowerX);

    const float distance = m_settings.selfCollisionDistance;
    const auto movable = [&](const ParticleQuad& v) {
        return m_stepInverseMasses[v[0]] + m_stepInverseMasses[v[1]] + m_stepInverseMasses[v[2]] + m_stepInverseMasses[v[3]] > 0.0f;
    };
    /// @note 静止形状で既に接触距離より近い組は、細かいメッシュの隣接として除外する。
    const auto addPair = [&](const ParticleQuad& v, bool edgeEdge) {
        if (!movable(v)) return;
        bool interior = false;
        const ContactWeights weights = PairWeights(m_restPositions, v, edgeEdge, interior);
        if (WeightedOffset(m_restPositions, v, weights).LengthSq() < distance * distance) return;
        m_primitivePairs.push_back({v, edgeEdge});
    };
    const auto visitPointTriangle = [&](uint32_t point, uint32_t triangle) {
        const ParticleQuad v{point, m_indices[triangle * 3], m_indices[triangle * 3 + 1], m_indices[triangle * 3 + 2]};
        if (point == v[1] || point == v[2] || point == v[3]) return;
        addPair(v, false);
    };
    size_t scannedPairCount = 0;
    for (const auto& point : m_pointBoxes) {
        auto it = std::lower_bound(m_triangleBoxes.begin(), m_triangleBoxes.end(), point.lower.x,
            [](const SweepBox& box, float x) { return box.lower.x < x; });
        for (; it != m_triangleBoxes.end() && it->lower.x <= point.upper.x; ++it) {
            if (++scannedPairCount > MAX_COLLISION_CANDIDATES) return false;
            if (BoxesOverlapYZ(point.lower, point.upper, it->lower, it->upper)) visitPointTriangle(point.index, it->index);
        }
    }
    for (const auto& triangle : m_triangleBoxes) {
        auto it = std::upper_bound(m_pointBoxes.begin(), m_pointBoxes.end(), triangle.lower.x,
            [](float x, const SweepBox& box) { return x < box.lower.x; });
        for (; it != m_pointBoxes.end() && it->lower.x <= triangle.upper.x; ++it) {
            if (++scannedPairCount > MAX_COLLISION_CANDIDATES) return false;
            if (BoxesOverlapYZ(it->lower, it->upper, triangle.lower, triangle.upper)) visitPointTriangle(it->index, triangle.index);
        }
    }
    for (size_t i = 0; i < m_edgeBoxes.size(); ++i) {
        const auto& first = m_edgeBoxes[i];
        for (size_t j = i + 1; j < m_edgeBoxes.size() && m_edgeBoxes[j].lower.x <= first.upper.x; ++j) {
            if (++scannedPairCount > MAX_COLLISION_CANDIDATES) return false;
            const auto& second = m_edgeBoxes[j];
            if (!BoxesOverlapYZ(first.lower, first.upper, second.lower, second.upper)) continue;
            const auto& a = m_edges[std::min(first.index, second.index)];
            const auto& b = m_edges[std::max(first.index, second.index)];
            if (a[0] == b[0] || a[0] == b[1] || a[1] == b[0] || a[1] == b[1]) continue;
            addPair({a[0], a[1], b[0], b[1]}, true);
        }
    }
    return true;
}

/// @note 開始時刻の側へ selfCollisionDistance だけ離す。三角形の内部と辺の内点だけを扱い、端は質点球に任せる。
/// @see https://graphics.stanford.edu/papers/cloth-sig02/cloth.pdf Bridson et al. 2002, 4 Proximity。
void ClothSolver::SolvePrimitiveProximity()
{
    const float distance = m_settings.selfCollisionDistance;
    for (const auto& pair : m_primitivePairs) {
        bool interior = false;
        const ContactWeights weights = PairWeights(m_positions, pair.v, pair.edgeEdge, interior);
        if (!interior) continue;
        Vector3 normal;
        if (!OrientContactNormal(m_positions, m_previous, pair.v, pair.edgeEdge, weights, normal)) continue;
        const float separation = Vector3::Dot(WeightedOffset(m_positions, pair.v, weights), normal);
        /// @note 接触距離を超えて裏へ抜けた組は近接として扱わない。抜けは CCD が拾う。
        if (separation >= distance || separation <= -distance) continue;
        ProjectSeparation(m_positions, m_stepInverseMasses, pair.v, weights,
                          normal, (distance - separation) * m_settings.selfCollisionStiffness);
    }
}

/// @note 開始位置から現在位置への直線運動で同一平面になった時刻を求め、その時刻に接触距離内なら開始時の側へ戻す。
/// @see https://graphics.stanford.edu/papers/cloth-sig02/cloth.pdf Bridson et al. 2002, 6 Geometric Collisions。
bool ClothSolver::SolvePrimitiveContinuous()
{
    const float distance = m_settings.selfCollisionDistance;
    bool corrected = false;
    std::vector<Vector3> impact(4);
    for (const auto& pair : m_primitivePairs) {
        const ParticleQuad& v = pair.v;
        /// @note 三次式は x0 を基準に (x1-x0)×(x2-x0)·(x3-x0)。辺–辺は p0 を、質点–三角形は a を基準に並べる。
        const ParticleQuad order = pair.edgeEdge ? v : ParticleQuad{v[1], v[2], v[3], v[0]};
        std::array<Vector3d, 4> start, end;
        for (int i = 0; i < 4; ++i) {
            start[i] = ToDouble(m_previous[order[i]]);
            end[i] = ToDouble(m_positions[order[i]]);
        }
        const double time = EarliestCoplanarTime(start, end);
        if (time < 0.0) continue;
        const float t = static_cast<float>(time);
        for (int i = 0; i < 4; ++i) impact[i] = Vector3::Lerp(m_previous[v[i]], m_positions[v[i]], t);
        const ParticleQuad local{0, 1, 2, 3};
        bool interior = false;
        const ContactWeights weights = PairWeights(impact, local, pair.edgeEdge, interior);
        if (WeightedOffset(impact, local, weights).LengthSq() > distance * distance) continue;
        Vector3 normal;
        if (!OrientContactNormal(m_positions, m_previous, v, pair.edgeEdge, weights, normal)) continue;
        const float separation = Vector3::Dot(WeightedOffset(m_positions, v, weights), normal);
        if (separation >= distance) continue;
        ProjectSeparation(m_positions, m_stepInverseMasses, v, weights, normal, distance - separation);
        corrected = true;
    }
    return corrected;
}

}
