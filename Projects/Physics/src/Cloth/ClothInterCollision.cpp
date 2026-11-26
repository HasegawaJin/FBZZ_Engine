/// @file    ClothInterCollision.cpp
/// @brief   複数の布の衝突マスク、候補探索、厚み付き CCD と状態の確定。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/Cloth/ClothSolver.hpp>
#include "ClothSolverDetail.hpp"
#include "ClothContactGeometry.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace fbzz::physics {

namespace {

using math::Vector3;

using cloth_detail::ToDouble;
using cloth_detail::Dot;
using cloth_detail::IsFiniteVector;
using cloth_detail::IsFiniteNonnegative;

using cloth_detail::BoxesOverlapYZ;
using cloth_detail::ContactWeights;
using cloth_detail::OrientContactNormal;
using cloth_detail::PairWeights;
using cloth_detail::ParticleQuad;
using cloth_detail::ProjectSeparation;
using cloth_detail::WeightedOffset;
using cloth_detail::MAX_COLLISION_CANDIDATES;
using cloth_detail::MAX_COLLISION_DISTANCE;
using cloth_detail::MIN_COLLISION_DISTANCE;

enum class InterPrimitiveType { POINT, TRIANGLE, EDGE };
struct InterPrimitive {
    ParticleQuad vertices{}; ///< 面または辺を構成する布の質点番号。
    size_t cloth = 0; ///< 相互接触に参加する布の配列での所有側の添字。
    InterPrimitiveType type = InterPrimitiveType::POINT; ///< 相互接触で詳しく判定するプリミティブの種類。
};

/// @note CCD の距離下界を壊さないよう、ほぼ平行な辺も double で最近点を解く。
/// @see https://realtimecollisiondetection.net/ Ericson, 5.1.9 Closest Points of Two Line Segments。
std::pair<float,float> InterSegmentParameters(const Vector3& p0, const Vector3& p1, const Vector3& q0, const Vector3& q1)
{
    const auto d1 = ToDouble(p1)-ToDouble(p0), d2 = ToDouble(q1)-ToDouble(q0), r = ToDouble(p0)-ToDouble(q0);
    const double a = Dot(d1,d1), e = Dot(d2,d2), f = Dot(d2,r);
    if (a <= 1.0e-30 && e <= 1.0e-30) {
        return {0.0f,0.0f};
    }
    if (a <= 1.0e-30) {
        return {0.0f,static_cast<float>(std::clamp(f/e,0.0,1.0))};
    }
    const double c = Dot(d1,r);
    if (e <= 1.0e-30) {
        return {static_cast<float>(std::clamp(-c/a,0.0,1.0)),0.0f};
    }
    const double b = Dot(d1,d2), denominator = a*e-b*b;
    double s = denominator > 0 ? std::clamp((b*f-c*e)/denominator,0.0,1.0) : 0;
    double t = (b*s+f)/e;
    if (t < 0) {
        t = 0;
        s = std::clamp(-c/a,0.0,1.0);
    }
    else if (t > 1) {
        t = 1;
        s = std::clamp((b-c)/a,0.0,1.0);
    }
    return {static_cast<float>(s),static_cast<float>(t)};
}

/// @note 境界・端点も含む最近点。潰れた三角形は 3 辺との最短距離に退化させる。
/// @see https://realtimecollisiondetection.net/ Ericson, 5.1.5 / 5.1.9 Closest Points。
ContactWeights InterWeights(const std::vector<Vector3>& p, bool edgeEdge)
{
    if (edgeEdge) {
        const auto [s,t] = InterSegmentParameters(p[0],p[1],p[2],p[3]);
        return {1-s,s,-(1-t),-t};
    }
    bool interior = false;
    constexpr ParticleQuad LOCAL{0,1,2,3};
    auto result = PairWeights(p,LOCAL,edgeEdge,interior);
    float best = WeightedOffset(p,LOCAL,result).LengthSq();
    for (uint32_t a = 1; a <= 3; ++a) {
        const uint32_t b = a == 3 ? 1 : a+1;
        const float t = InterSegmentParameters(p[0],p[0],p[a],p[b]).second;
        ContactWeights candidate{1,0,0,0};
        candidate[a] = -(1-t); candidate[b] = -t;
        const float distance = WeightedOffset(p,LOCAL,candidate).LengthSq();
        if (distance < best) {
            result = candidate;
            best = distance;
        }
    }
    return result;
}

/// @brief 2 つの布のプリミティブが離隔距離へ達する最初の位置を求める。
/// @param[in] start 4 質点の更新開始時のワールド位置 [m]。辺対辺では 2 点ずつ、点対三角形では点を先頭に並べる。
/// @param[in] end start と同順の更新終了時のワールド位置 [m]。
/// @param[in,out] distanceEvaluations この呼び出しまでに消費した距離計算回数に、今回の回数を加える。
/// @param[out] hit 厚みへの到達と有効な法線の両方が確認できた場合に true を書く。
/// @param[out] weights 接触時に使う 4 質点の重みを格納する。成功して hit が true の場合に有効。
/// @param[out] normal 接触時に離す方向を示すワールド空間の単位法線。成功して hit が true の場合に有効。
/// @return 計算上限・非有限値なら false。true のときは hit で接触の有無を確認する。
bool FindInterClothImpact(const std::vector<Vector3>& start, const std::vector<Vector3>& end, bool edgeEdge,
                 float thickness, size_t& distanceEvaluations, bool& hit, ContactWeights& weights, Vector3& normal)
{
    constexpr ParticleQuad LOCAL{0,1,2,3};
    // 2 つの形状にまたがる頂点対の最大相対変位を、距離が縮む速さの上限に使う。
    const int split = edgeEdge ? 2 : 1;
    float speed = 0;
    for (int a = 0; a < split; ++a) {
        for (int b = split; b < 4; ++b) {
            speed = std::max(speed,((end[a]-start[a])-(end[b]-start[b])).Length());
        }
    }
    if (!std::isfinite(speed)) {
        return false;
    }

    // 開始から終了を 0～1 として進む。離隔距離への到達は 1 μm または離隔距離の 0.01% の大きい方まで近づけば認める。
    const float tolerance = std::max(1.0e-6f,thickness*1.0e-4f);
    std::vector<Vector3> p(4);
    double time = 0;
    hit = false;
    for (int iteration = 0; iteration < 128; ++iteration) {
        if (++distanceEvaluations > MAX_COLLISION_CANDIDATES) {
            return false;
        }
        for (size_t i = 0; i < 4; ++i) {
            p[i] = Vector3::Lerp(start[i],end[i],static_cast<float>(time));
        }
        weights = InterWeights(p,edgeEdge);
        const Vector3 delta = WeightedOffset(p,LOCAL,weights);
        const float distance = delta.Length();
        if (!std::isfinite(distance)) {
            return false;
        }
        if (distance <= thickness+tolerance) {
            if (distance > 1.0e-8f) {
                normal = delta/distance;
            }
            else if (!OrientContactNormal(p,start,LOCAL,edgeEdge,weights,normal)) {
                return true;
            }
            hit = true;
            return true;
        }
        if (speed <= 1.0e-12f || time >= 1.0) {
            return true;
        }

        // 上限の速さで近づいても厚みを飛び越えない時間だけ進む。
        /// @see https://ipc-sim.github.io/C-IPC/file/paper.pdf 5.3 / 5.4、有限厚みと conservative advancement の距離下界。
        const double step = 0.9*static_cast<double>(distance-thickness)/speed;
        if (step > 1.0-time) {
            return true;
        }
        time += step;
    }
    return false;
}

/// @see https://graphics.stanford.edu/papers/cloth-sig02/cloth.pdf 6.2、質点–三角形 / 辺–辺による布の連続衝突。
bool SolveInterPrimitives(std::vector<Vector3>& x, const std::vector<Vector3>& start,
                          const std::vector<float>& masses, const std::vector<InterPrimitive>& primitives,
                          std::span<const ClothInteraction> cloths, float inflation, size_t& distanceEvaluations)
{
    struct Box {
        Vector3 lower; ///< 掃引 AABB のワールド座標の最小点 [m]。
        Vector3 upper; ///< 掃引 AABB のワールド座標の最大点 [m]。
        size_t primitive; ///< 掃引境界の対象を指す primitives の添字。
    };
    // 開始・終了の両方を包む境界を作って X 軸で整列し、厚みの範囲で交差する候補だけを調べる。
    std::vector<Box> boxes;
    boxes.reserve(primitives.size());
    for (size_t i = 0; i < primitives.size(); ++i) {
        const auto& primitive = primitives[i];
        Box box{x[primitive.vertices[0]],x[primitive.vertices[0]],i};
        for (uint32_t v : primitive.vertices) {
            for (const auto* source : std::array<const std::vector<Vector3>*,2>{&x,&start}) {
            const auto& p = (*source)[v];
            box.lower = {std::min(box.lower.x,p.x),std::min(box.lower.y,p.y),std::min(box.lower.z,p.z)};
            box.upper = {std::max(box.upper.x,p.x),std::max(box.upper.y,p.y),std::max(box.upper.z,p.z)};
        }
        }
        box.lower -= Vector3{inflation,inflation,inflation};
        box.upper += Vector3{inflation,inflation,inflation};
        if (!IsFiniteVector(box.lower) || !IsFiniteVector(box.upper)) {
            return false;
        }
        boxes.push_back(box);
    }
    std::sort(boxes.begin(),boxes.end(),[](const Box& a,const Box& b) {
        return a.lower.x != b.lower.x ? a.lower.x < b.lower.x : a.primitive < b.primitive;
    });
    size_t scanned = 0;
    std::vector<Vector3> previous(4),current(4);
    constexpr ParticleQuad LOCAL{0,1,2,3};

    // 異なる布でマスクが両立する組について、掃引または終端位置の近接を解く。
    for (size_t i = 0; i < boxes.size(); ++i) {
        for (size_t j = i+1; j < boxes.size() && boxes[j].lower.x <= boxes[i].upper.x; ++j) {
        if (++scanned > MAX_COLLISION_CANDIDATES) {
            return false;
        }
        const auto& a = primitives[boxes[i].primitive]; const auto& b = primitives[boxes[j].primitive];
        if (a.cloth == b.cloth || !BoxesOverlapYZ(boxes[i].lower,boxes[i].upper,boxes[j].lower,boxes[j].upper)) {
            continue;
        }
        const auto& ca = cloths[a.cloth]; const auto& cb = cloths[b.cloth];
        if (!(ca.mask & (1u<<cb.layer)) || !(cb.mask & (1u<<ca.layer))) {
            continue;
        }
        const bool continuous = ca.continuous || cb.continuous;
        if (!(ca.faces || cb.faces || continuous)) {
            continue;
        }
        const bool edgeEdge = a.type == InterPrimitiveType::EDGE && b.type == InterPrimitiveType::EDGE;
        ParticleQuad v;
        if (edgeEdge) {
            v = {a.vertices[0],a.vertices[1],b.vertices[0],b.vertices[1]};
        }
        else {
            const auto& point = a.type == InterPrimitiveType::POINT ? a : b;
            const auto& face = a.type == InterPrimitiveType::POINT ? b : a;
            if (point.type != InterPrimitiveType::POINT || face.type != InterPrimitiveType::TRIANGLE) {
                continue;
            }
            v = {point.vertices[0],face.vertices[0],face.vertices[1],face.vertices[2]};
        }
        if (masses[v[0]]+masses[v[1]]+masses[v[2]]+masses[v[3]] == 0) {
            continue;
        }
        for (size_t k = 0; k < 4; ++k) {
            previous[k] = start[v[k]];
            current[k] = x[v[k]];
        }
        const float distance = std::max(ca.distance,cb.distance);
        ContactWeights weights;
        Vector3 normal;
        bool hit = false;
        if (continuous && !FindInterClothImpact(previous,current,edgeEdge,distance,distanceEvaluations,hit,weights,normal)) {
            return false;
        }
        if (!hit) {
            weights = InterWeights(current,edgeEdge);
            const Vector3 delta = WeightedOffset(current,LOCAL,weights);
            const float length = delta.Length();
            if (!std::isfinite(length)) {
                return false;
            }
            if (length >= distance) {
                continue;
            }
            if (length > 1.0e-8f) {
                normal = delta/length;
            }
            else if (!OrientContactNormal(current,previous,LOCAL,edgeEdge,weights,normal)) {
                continue;
            }
        }
        const float separation = Vector3::Dot(WeightedOffset(x,v,weights),normal);
        if (separation < distance) {
            ProjectSeparation(x,masses,v,weights,normal,distance-separation);
        }
    }
    }
    return true;
}

}

bool ClothSolver::SolveInterCollision(std::span<const ClothInteraction> cloths, float dt)
{
    if (!std::isfinite(dt) || dt <= 0 || dt > 0.1f) {
        return false;
    }

    // 各布の Step が用意した質量を確認し、補正に失敗したときの位置と速度を退避する。
    float cellSize = 0;
    std::vector<std::vector<Vector3>> savedPositions;
    std::vector<std::vector<Vector3>> savedVelocities;
    for (size_t clothIndex = 0; clothIndex < cloths.size(); ++clothIndex) {
        const auto& entry = cloths[clothIndex];
        if (!entry.solver) {
            return false;
        }
        if (entry.layer >= std::numeric_limits<uint32_t>::digits) {
            return false;
        }
        if (!IsFiniteNonnegative(entry.distance) || entry.distance > MAX_COLLISION_DISTANCE) {
            return false;
        }
        if (entry.distance > 0.0f && entry.distance < MIN_COLLISION_DISTANCE) {
            return false;
        }
        if (entry.solver->m_positions.size() != entry.solver->m_stepInverseMasses.size()) {
            return false;
        }
        for (size_t earlierClothIndex = 0; earlierClothIndex < clothIndex; ++earlierClothIndex) {
            if (cloths[earlierClothIndex].solver == entry.solver) {
                return false;
            }
        }
        for (const auto& motion : entry.motion) {
            if (motion.particle >= entry.solver->m_positions.size() || !IsFiniteVector(motion.center)
                || !IsFiniteVector(motion.previousCenter) || !IsFiniteNonnegative(motion.radius)) {
                return false;
            }
        }
        cellSize = std::max(cellSize,entry.distance);
        savedPositions.push_back(entry.solver->m_positions);
        savedVelocities.push_back(entry.solver->m_velocities);
    }
    if (cellSize == 0) {
        return true;
    }

    // 正の離隔距離を持ち、互いの層を許可し、少なくとも片方が面・辺接触を指定した組だけを選ぶ。
    bool solveSurfaceContacts = false;
    std::vector<unsigned char> primitiveParticipants(cloths.size(), 0);
    for (size_t firstClothIndex = 0; firstClothIndex < cloths.size(); ++firstClothIndex) {
        const auto& first = cloths[firstClothIndex];
        for (size_t secondClothIndex = firstClothIndex + 1; secondClothIndex < cloths.size(); ++secondClothIndex) {
            const auto& second = cloths[secondClothIndex];
            if (!(first.distance > 0.0f && second.distance > 0.0f)) {
                continue;
            }
            const bool layersMatch = (first.mask & (1u << second.layer)) && (second.mask & (1u << first.layer));
            if (!layersMatch) {
                continue;
            }
            const bool requiresPrimitives = first.faces || second.faces || first.continuous || second.continuous;
            if (!requiresPrimitives) {
                continue;
            }

            solveSurfaceContacts = true;
            primitiveParticipants[secondClothIndex] = 1;
            primitiveParticipants[firstClothIndex] = 1;
        }
    }

    // 異なる布の点・面・辺へ共通の質点番号でアクセスできるよう、開始位置と予測位置を平坦化する。
    std::vector<InterPrimitive> primitives;
    std::vector<Vector3> flatPositions;
    std::vector<Vector3> flatStart;
    std::vector<float> flatMasses;
    std::vector<size_t> offsets;
    if (solveSurfaceContacts) {
        for (size_t clothIndex = 0; clothIndex < cloths.size(); ++clothIndex) {
            const auto& solver = *cloths[clothIndex].solver;
            if (solver.m_stepPositions.size() != solver.m_positions.size()) {
                return false;
            }
            if (flatPositions.size() + solver.m_positions.size() > std::numeric_limits<uint32_t>::max()) {
                return false;
            }
            const uint32_t offset = static_cast<uint32_t>(flatPositions.size());
            offsets.push_back(offset);
            flatPositions.insert(flatPositions.end(), solver.m_positions.begin(), solver.m_positions.end());
            flatStart.insert(flatStart.end(), solver.m_stepPositions.begin(), solver.m_stepPositions.end());
            flatMasses.insert(flatMasses.end(), solver.m_stepInverseMasses.begin(), solver.m_stepInverseMasses.end());
            if (!primitiveParticipants[clothIndex]) {
                continue;
            }
            for (uint32_t particleIndex = 0; particleIndex < solver.m_positions.size(); ++particleIndex) {
                const uint32_t particle = offset + particleIndex;
                primitives.push_back({{particle, particle, particle, particle}, clothIndex, InterPrimitiveType::POINT});
            }
            for (size_t triangleStart = 0; triangleStart < solver.m_indices.size(); triangleStart += 3) {
                const uint32_t first = offset + solver.m_indices[triangleStart];
                const uint32_t second = offset + solver.m_indices[triangleStart + 1];
                const uint32_t third = offset + solver.m_indices[triangleStart + 2];
                primitives.push_back({{first, second, third, first}, clothIndex, InterPrimitiveType::TRIANGLE});
            }
            for (const auto& edge : solver.m_edges) {
                const uint32_t first = offset + edge[0];
                const uint32_t second = offset + edge[1];
                primitives.push_back({{first, second, first, second}, clothIndex, InterPrimitiveType::EDGE});
            }
        }
    }

    const auto rollback = [&] {
        for (size_t i = 0; i < cloths.size(); ++i) {
            cloths[i].solver->m_positions = savedPositions[i];
            cloths[i].solver->m_velocities = savedVelocities[i];
        }
        return false;
    };
    struct ParticleCell {
        std::array<int64_t, 3> cell; ///< 布の相互接触用の近傍格子の整数セル座標。
        size_t cloth; ///< 相互接触に参加する cloths の添字。
        uint32_t particle; ///< このセルに入った、布の中での質点番号。
    };
    std::vector<ParticleCell> particleCells;
    size_t distanceEvaluations = 0;
    // 相互衝突は固定更新の最後に 4 反復。座標順・参加者順・質点順で再現性を保つ。
    for (int iteration = 0; iteration < 4; ++iteration) {
        particleCells.clear();
        for (size_t c = 0; c < cloths.size(); ++c) {
            if (cloths[c].distance == 0) {
                continue;
            }
            const auto& solver = *cloths[c].solver;
            for (uint32_t p = 0; p < solver.m_positions.size(); ++p) {
                ParticleCell point{{}, c, p};
                if (!cloth_detail::TryGetCollisionCell(solver.m_positions[p], cellSize, point.cell)) {
                    return rollback();
                }
                particleCells.push_back(point);
            }
        }
        std::sort(particleCells.begin(),particleCells.end(),[](const ParticleCell& a,const ParticleCell& b) {
            if (a.cell != b.cell) {
                return a.cell < b.cell;
            }
            return a.cloth != b.cloth ? a.cloth < b.cloth : a.particle < b.particle;
        });
        size_t candidates = 0;
        for (const auto& point : particleCells) {
            for (int x = -1; x <= 1; ++x) {
                for (int y = -1; y <= 1; ++y) {
                    for (int z = -1; z <= 1; ++z) {
                const std::array<int64_t,3> cell{point.cell[0]+x,point.cell[1]+y,point.cell[2]+z};
                auto it = std::lower_bound(particleCells.begin(),particleCells.end(),cell,[](const ParticleCell& p,const auto& key) { return p.cell < key; });
                for (; it != particleCells.end() && it->cell == cell; ++it) {
                    if (it->cloth <= point.cloth) {
                        continue;
                    }
                    if (++candidates > MAX_COLLISION_CANDIDATES) {
                        return rollback();
                    }
                    const auto& a = cloths[point.cloth]; const auto& b = cloths[it->cloth];
                    if (!(a.mask & (1u<<b.layer)) || !(b.mask & (1u<<a.layer))) {
                        continue;
                    }
                    auto& sa = *a.solver; auto& sb = *b.solver;
                    const float wa = sa.m_stepInverseMasses[point.particle], wb = sb.m_stepInverseMasses[it->particle];
                    if (wa+wb == 0) {
                        continue;
                    }
                    auto& pa = sa.m_positions[point.particle]; auto& pb = sb.m_positions[it->particle];
                    const Vector3 delta = pa-pb;
                    const float length = delta.Length(), distance = std::max(a.distance,b.distance);
                    if (length >= distance) {
                        continue;
                    }
                    const Vector3 normal = delta.NormalizedOr((savedPositions[point.cloth][point.particle]-savedPositions[it->cloth][it->particle]).NormalizedOr(Vector3::UP));
                    const Vector3 correction = normal*((distance-length)/(wa+wb));
                    pa += correction*wa; pb -= correction*wb;
                }
            }
                }
            }
        }
        if (solveSurfaceContacts) {
            for (size_t c = 0; c < cloths.size(); ++c) {
                std::copy(cloths[c].solver->m_positions.begin(),cloths[c].solver->m_positions.end(),flatPositions.begin()+offsets[c]);
            }
            if (!SolveInterPrimitives(flatPositions,flatStart,flatMasses,primitives,cloths,cellSize,distanceEvaluations)) {
                return rollback();
            }
            for (size_t c = 0; c < cloths.size(); ++c) {
                std::copy_n(flatPositions.begin()+offsets[c],cloths[c].solver->m_positions.size(),cloths[c].solver->m_positions.begin());
            }
        }
        for (const auto& entry : cloths) {
            entry.solver->SolveMotion(entry.motion,1.0f,dt,false);
        }
    }
    for (size_t c = 0; c < cloths.size(); ++c) {
        auto& solver = *cloths[c].solver;
        for (size_t p = 0; p < solver.m_positions.size(); ++p) {
            solver.m_velocities[p] += (solver.m_positions[p]-savedPositions[c][p])/dt;
            if (!IsFiniteVector(solver.m_positions[p]) || !IsFiniteVector(solver.m_velocities[p])) {
                return rollback();
            }
        }
    }
    return true;
}

}
