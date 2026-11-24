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

enum class InterPrimitiveType { POINT, TRIANGLE, EDGE };
struct InterPrimitive {
    ParticleQuad vertices{};
    size_t cloth = 0;
    InterPrimitiveType type = InterPrimitiveType::POINT;
};

/// @note CCD の距離下界を壊さないよう、ほぼ平行な辺も double で最近点を解く。
/// @see https://realtimecollisiondetection.net/ Ericson, 5.1.9 Closest Points of Two Line Segments。
std::pair<float,float> InterSegmentParameters(const Vector3& p0, const Vector3& p1, const Vector3& q0, const Vector3& q1)
{
    const auto d1 = ToDouble(p1)-ToDouble(p0), d2 = ToDouble(q1)-ToDouble(q0), r = ToDouble(p0)-ToDouble(q0);
    const double a = Dot(d1,d1), e = Dot(d2,d2), f = Dot(d2,r);
    if (a <= 1.0e-30 && e <= 1.0e-30) return {0.0f,0.0f};
    if (a <= 1.0e-30) return {0.0f,static_cast<float>(std::clamp(f/e,0.0,1.0))};
    const double c = Dot(d1,r);
    if (e <= 1.0e-30) return {static_cast<float>(std::clamp(-c/a,0.0,1.0)),0.0f};
    const double b = Dot(d1,d2), denominator = a*e-b*b;
    double s = denominator > 0 ? std::clamp((b*f-c*e)/denominator,0.0,1.0) : 0;
    double t = (b*s+f)/e;
    if (t < 0) { t = 0; s = std::clamp(-c/a,0.0,1.0); }
    else if (t > 1) { t = 1; s = std::clamp((b-c)/a,0.0,1.0); }
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
        if (distance < best) { result = candidate; best = distance; }
    }
    return result;
}

/// @return 計算上限・非有限値なら false。hit=false は厚みへの到達なし。
/// @note 距離減少速度の上限は両 primitive の頂点間の最大相対変位。安全な時間増分を積み、厚みから 1 μm または厚みの 0.01% 以内で止める。
/// @see https://ipc-sim.github.io/C-IPC/file/paper.pdf 5.3 / 5.4、有限厚みと conservative advancement の距離下界。
bool FindInterClothImpact(const std::vector<Vector3>& start, const std::vector<Vector3>& end, bool edgeEdge,
                 float thickness, size_t& distanceEvaluations, bool& hit, ContactWeights& weights, Vector3& normal)
{
    constexpr ParticleQuad LOCAL{0,1,2,3};
    const int split = edgeEdge ? 2 : 1;
    float speed = 0;
    for (int a = 0; a < split; ++a) for (int b = split; b < 4; ++b)
        speed = std::max(speed,((end[a]-start[a])-(end[b]-start[b])).Length());
    if (!std::isfinite(speed)) return false;
    const float tolerance = std::max(1.0e-6f,thickness*1.0e-4f);
    std::vector<Vector3> p(4);
    double time = 0;
    hit = false;
    for (int iteration = 0; iteration < 128; ++iteration) {
        if (++distanceEvaluations > MAX_COLLISION_CANDIDATES) return false;
        for (size_t i = 0; i < 4; ++i) p[i] = Vector3::Lerp(start[i],end[i],static_cast<float>(time));
        weights = InterWeights(p,edgeEdge);
        const Vector3 delta = WeightedOffset(p,LOCAL,weights);
        const float distance = delta.Length();
        if (!std::isfinite(distance)) return false;
        if (distance <= thickness+tolerance) {
            if (distance > 1.0e-8f) normal = delta/distance;
            else if (!OrientContactNormal(p,start,LOCAL,edgeEdge,weights,normal)) return true;
            hit = true;
            return true;
        }
        if (speed <= 1.0e-12f || time >= 1.0) return true;
        const double step = 0.9*static_cast<double>(distance-thickness)/speed;
        if (step > 1.0-time) return true;
        time += step;
    }
    return false;
}

/// @note 開始/終了の swept AABB を x 軸で整列する。同じ布の組とマスク除外は狭域判定へ渡さない。
/// @see https://graphics.stanford.edu/papers/cloth-sig02/cloth.pdf 6.2、質点–三角形 / 辺–辺による布の連続衝突。
bool SolveInterPrimitives(std::vector<Vector3>& x, const std::vector<Vector3>& start,
                          const std::vector<float>& masses, const std::vector<InterPrimitive>& primitives,
                          std::span<const ClothInteraction> cloths, float inflation, size_t& distanceEvaluations)
{
    struct Box { Vector3 lower,upper; size_t primitive; };
    std::vector<Box> boxes;
    boxes.reserve(primitives.size());
    for (size_t i = 0; i < primitives.size(); ++i) {
        const auto& primitive = primitives[i];
        Box box{x[primitive.vertices[0]],x[primitive.vertices[0]],i};
        for (uint32_t v : primitive.vertices) for (const auto* source : std::array<const std::vector<Vector3>*,2>{&x,&start}) {
            const auto& p = (*source)[v];
            box.lower = {std::min(box.lower.x,p.x),std::min(box.lower.y,p.y),std::min(box.lower.z,p.z)};
            box.upper = {std::max(box.upper.x,p.x),std::max(box.upper.y,p.y),std::max(box.upper.z,p.z)};
        }
        box.lower -= Vector3{inflation,inflation,inflation};
        box.upper += Vector3{inflation,inflation,inflation};
        if (!IsFiniteVector(box.lower) || !IsFiniteVector(box.upper)) return false;
        boxes.push_back(box);
    }
    std::sort(boxes.begin(),boxes.end(),[](const Box& a,const Box& b) {
        return a.lower.x != b.lower.x ? a.lower.x < b.lower.x : a.primitive < b.primitive;
    });
    size_t scanned = 0;
    std::vector<Vector3> previous(4),current(4);
    constexpr ParticleQuad LOCAL{0,1,2,3};
    for (size_t i = 0; i < boxes.size(); ++i) for (size_t j = i+1; j < boxes.size() && boxes[j].lower.x <= boxes[i].upper.x; ++j) {
        if (++scanned > MAX_COLLISION_CANDIDATES) return false;
        const auto& a = primitives[boxes[i].primitive]; const auto& b = primitives[boxes[j].primitive];
        if (a.cloth == b.cloth || !BoxesOverlapYZ(boxes[i].lower,boxes[i].upper,boxes[j].lower,boxes[j].upper)) continue;
        const auto& ca = cloths[a.cloth]; const auto& cb = cloths[b.cloth];
        if (!(ca.mask & (1u<<cb.layer)) || !(cb.mask & (1u<<ca.layer))) continue;
        const bool continuous = ca.continuous || cb.continuous;
        if (!(ca.faces || cb.faces || continuous)) continue;
        const bool edgeEdge = a.type == InterPrimitiveType::EDGE && b.type == InterPrimitiveType::EDGE;
        ParticleQuad v;
        if (edgeEdge) v = {a.vertices[0],a.vertices[1],b.vertices[0],b.vertices[1]};
        else {
            const auto& point = a.type == InterPrimitiveType::POINT ? a : b;
            const auto& face = a.type == InterPrimitiveType::POINT ? b : a;
            if (point.type != InterPrimitiveType::POINT || face.type != InterPrimitiveType::TRIANGLE) continue;
            v = {point.vertices[0],face.vertices[0],face.vertices[1],face.vertices[2]};
        }
        if (masses[v[0]]+masses[v[1]]+masses[v[2]]+masses[v[3]] == 0) continue;
        for (size_t k = 0; k < 4; ++k) { previous[k] = start[v[k]]; current[k] = x[v[k]]; }
        const float distance = std::max(ca.distance,cb.distance);
        ContactWeights weights;
        Vector3 normal;
        bool hit = false;
        if (continuous && !FindInterClothImpact(previous,current,edgeEdge,distance,distanceEvaluations,hit,weights,normal)) return false;
        if (!hit) {
            weights = InterWeights(current,edgeEdge);
            const Vector3 delta = WeightedOffset(current,LOCAL,weights);
            const float length = delta.Length();
            if (!std::isfinite(length)) return false;
            if (length >= distance) continue;
            if (length > 1.0e-8f) normal = delta/length;
            else if (!OrientContactNormal(current,previous,LOCAL,edgeEdge,weights,normal)) continue;
        }
        const float separation = Vector3::Dot(WeightedOffset(x,v,weights),normal);
        if (separation < distance) ProjectSeparation(x,masses,v,weights,normal,distance-separation);
    }
    return true;
}

}

bool ClothSolver::SolveInterCollision(std::span<const ClothInteraction> cloths, float dt)
{
    if (!std::isfinite(dt) || dt <= 0 || dt > 0.1f) return false;
    float cellSize = 0;
    std::vector<std::vector<Vector3>> savedPositions, savedVelocities;
    for (size_t i = 0; i < cloths.size(); ++i) {
        const auto& entry = cloths[i];
        if (!entry.solver || entry.layer >= 32 || !IsFiniteNonnegative(entry.distance) || entry.distance > 100
            || (entry.distance > 0 && entry.distance < 1.0e-6f)
            || entry.solver->m_positions.size() != entry.solver->m_stepInverseMasses.size()) return false;
        for (size_t j = 0; j < i; ++j) if (cloths[j].solver == entry.solver) return false;
        for (const auto& motion : entry.motion)
            if (motion.particle >= entry.solver->m_positions.size() || !IsFiniteVector(motion.center)
                || !IsFiniteVector(motion.previousCenter) || !IsFiniteNonnegative(motion.radius)) return false;
        cellSize = std::max(cellSize,entry.distance);
        savedPositions.push_back(entry.solver->m_positions);
        savedVelocities.push_back(entry.solver->m_velocities);
    }
    if (cellSize == 0) return true;
    bool solveSurfaceContacts = false;
    std::vector<unsigned char> primitiveParticipants(cloths.size(),0);
    for (size_t a = 0; a < cloths.size(); ++a) for (size_t b = a+1; b < cloths.size(); ++b)
        if (cloths[a].distance > 0 && cloths[b].distance > 0 && (cloths[a].mask & (1u<<cloths[b].layer))
            && (cloths[b].mask & (1u<<cloths[a].layer))
            && (cloths[a].faces || cloths[b].faces || cloths[a].continuous || cloths[b].continuous)) {
            solveSurfaceContacts = true;
            primitiveParticipants[a] = primitiveParticipants[b] = 1;
        }
    std::vector<InterPrimitive> primitives;
    std::vector<Vector3> flatPositions,flatStart;
    std::vector<float> flatMasses;
    std::vector<size_t> offsets;
    if (solveSurfaceContacts) for (size_t c = 0; c < cloths.size(); ++c) {
        const auto& solver = *cloths[c].solver;
        if (solver.m_stepPositions.size() != solver.m_positions.size()
            || flatPositions.size()+solver.m_positions.size() > std::numeric_limits<uint32_t>::max()) return false;
        const uint32_t offset = static_cast<uint32_t>(flatPositions.size());
        offsets.push_back(offset);
        flatPositions.insert(flatPositions.end(),solver.m_positions.begin(),solver.m_positions.end());
        flatStart.insert(flatStart.end(),solver.m_stepPositions.begin(),solver.m_stepPositions.end());
        flatMasses.insert(flatMasses.end(),solver.m_stepInverseMasses.begin(),solver.m_stepInverseMasses.end());
        if (!primitiveParticipants[c]) continue;
        for (uint32_t p = 0; p < solver.m_positions.size(); ++p)
            primitives.push_back({{offset+p,offset+p,offset+p,offset+p},c,InterPrimitiveType::POINT});
        for (size_t t = 0; t < solver.m_indices.size(); t += 3)
            primitives.push_back({{offset+solver.m_indices[t],offset+solver.m_indices[t+1],offset+solver.m_indices[t+2],offset+solver.m_indices[t]},c,InterPrimitiveType::TRIANGLE});
        for (const auto& edge : solver.m_edges)
            primitives.push_back({{offset+edge[0],offset+edge[1],offset+edge[0],offset+edge[1]},c,InterPrimitiveType::EDGE});
    }
    const auto rollback = [&] {
        for (size_t i = 0; i < cloths.size(); ++i) {
            cloths[i].solver->m_positions = savedPositions[i];
            cloths[i].solver->m_velocities = savedVelocities[i];
        }
        return false;
    };
    struct ParticleCell { std::array<int64_t,3> cell; size_t cloth; uint32_t particle; };
    std::vector<ParticleCell> particleCells;
    size_t distanceEvaluations = 0;
    /// @note 相互衝突は固定更新の最後に 4 反復。座標順・参加者順・質点順で再現性を保つ。
    for (int iteration = 0; iteration < 4; ++iteration) {
        particleCells.clear();
        for (size_t c = 0; c < cloths.size(); ++c) {
            if (cloths[c].distance == 0) continue;
            const auto& solver = *cloths[c].solver;
            for (uint32_t p = 0; p < solver.m_positions.size(); ++p) {
                ParticleCell point{{}, c, p};
                if (!cloth_detail::TryGetCollisionCell(solver.m_positions[p], cellSize, point.cell)) return rollback();
                particleCells.push_back(point);
            }
        }
        std::sort(particleCells.begin(),particleCells.end(),[](const ParticleCell& a,const ParticleCell& b) {
            if (a.cell != b.cell) return a.cell < b.cell;
            return a.cloth != b.cloth ? a.cloth < b.cloth : a.particle < b.particle;
        });
        size_t candidates = 0;
        for (const auto& point : particleCells) {
            for (int x = -1; x <= 1; ++x) for (int y = -1; y <= 1; ++y) for (int z = -1; z <= 1; ++z) {
                const std::array<int64_t,3> cell{point.cell[0]+x,point.cell[1]+y,point.cell[2]+z};
                auto it = std::lower_bound(particleCells.begin(),particleCells.end(),cell,[](const ParticleCell& p,const auto& key) { return p.cell < key; });
                for (; it != particleCells.end() && it->cell == cell; ++it) {
                    if (it->cloth <= point.cloth) continue;
                    if (++candidates > MAX_COLLISION_CANDIDATES) return rollback();
                    const auto& a = cloths[point.cloth]; const auto& b = cloths[it->cloth];
                    if (!(a.mask & (1u<<b.layer)) || !(b.mask & (1u<<a.layer))) continue;
                    auto& sa = *a.solver; auto& sb = *b.solver;
                    const float wa = sa.m_stepInverseMasses[point.particle], wb = sb.m_stepInverseMasses[it->particle];
                    if (wa+wb == 0) continue;
                    auto& pa = sa.m_positions[point.particle]; auto& pb = sb.m_positions[it->particle];
                    const Vector3 delta = pa-pb;
                    const float length = delta.Length(), distance = std::max(a.distance,b.distance);
                    if (length >= distance) continue;
                    const Vector3 normal = delta.NormalizedOr((savedPositions[point.cloth][point.particle]-savedPositions[it->cloth][it->particle]).NormalizedOr(Vector3::UP));
                    const Vector3 correction = normal*((distance-length)/(wa+wb));
                    pa += correction*wa; pb -= correction*wb;
                }
            }
        }
        if (solveSurfaceContacts) {
            for (size_t c = 0; c < cloths.size(); ++c)
                std::copy(cloths[c].solver->m_positions.begin(),cloths[c].solver->m_positions.end(),flatPositions.begin()+offsets[c]);
            if (!SolveInterPrimitives(flatPositions,flatStart,flatMasses,primitives,cloths,cellSize,distanceEvaluations)) return rollback();
            for (size_t c = 0; c < cloths.size(); ++c)
                std::copy_n(flatPositions.begin()+offsets[c],cloths[c].solver->m_positions.size(),cloths[c].solver->m_positions.begin());
        }
        for (const auto& entry : cloths) entry.solver->SolveMotion(entry.motion,1.0f,dt,false);
    }
    for (size_t c = 0; c < cloths.size(); ++c) {
        auto& solver = *cloths[c].solver;
        for (size_t p = 0; p < solver.m_positions.size(); ++p) {
            solver.m_velocities[p] += (solver.m_positions[p]-savedPositions[c][p])/dt;
            if (!IsFiniteVector(solver.m_positions[p]) || !IsFiniteVector(solver.m_velocities[p])) return rollback();
        }
    }
    return true;
}

}
