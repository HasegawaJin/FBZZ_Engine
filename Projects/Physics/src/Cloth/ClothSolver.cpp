/// @file    ClothSolver.cpp
/// @brief   布の距離拘束・近似曲げ・風・離散/連続接触の実装。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <Physics/Cloth/ClothSolver.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace fbzz::physics {
namespace {
using math::Vector3;
constexpr float EPSILON = 1.0e-8f;
/// @note 1 回の広域判定で走査する候補の上限。超えたら Step 全体を巻き戻す。
constexpr size_t MAX_SELF_CANDIDATES = 2000000;
/// @note 近接判定で三角形の内部とみなす重心座標の下限。辺・頂点付近は辺–辺と質点球に任せる。
constexpr float INTERIOR_WEIGHT = 1.0e-4f;

struct Vector3d {
    double x = 0.0, y = 0.0, z = 0.0;
};
Vector3d ToDouble(const Vector3& v) { return {v.x, v.y, v.z}; }
Vector3d operator-(const Vector3d& a, const Vector3d& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vector3d operator+(const Vector3d& a, const Vector3d& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vector3d operator*(const Vector3d& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double Dot(const Vector3d& a, const Vector3d& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vector3d Cross(const Vector3d& a, const Vector3d& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

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
    if (d1 <= 0.0f && d2 <= 0.0f) return {1.0f, 0.0f, 0.0f};
    const Vector3 bp = p - b;
    const float d3 = Vector3::Dot(ab, bp), d4 = Vector3::Dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return {0.0f, 1.0f, 0.0f};
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = SafeRatio(d1, d1 - d3);
        return {1.0f - v, v, 0.0f};
    }
    const Vector3 cp = p - c;
    const float d5 = Vector3::Dot(ab, cp), d6 = Vector3::Dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return {0.0f, 0.0f, 1.0f};
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
    if (sum <= 1.0e-20f) return {1.0f, 0.0f, 0.0f};
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
    if (a <= TINY && e <= TINY) return {0.0f, 0.0f};
    if (a <= TINY) return {0.0f, std::clamp(f / e, 0.0f, 1.0f)};
    const float c = Vector3::Dot(d1, r);
    if (e <= TINY) return {std::clamp(-c / a, 0.0f, 1.0f), 0.0f};
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

/// @return start が外側にあり、線分が半径 radius のカプセル (a==b は球) に入る割合 [0,1]。入らなければ負。
/// @see https://iquilezles.org/articles/intersectors/ Inigo Quilez, Intersectors の Sphere / Capsule。
double SweepCapsule(const Vector3& start, const Vector3& end, const Vector3& a, const Vector3& b, float radius)
{
    const Vector3 axis = b - a;
    const float axisSq = axis.LengthSq();
    const Vector3 nearest = axisSq > EPSILON
        ? a + axis * std::clamp(Vector3::Dot(start - a, axis) / axisSq, 0.0f, 1.0f) : a;
    if ((start - nearest).LengthSq() <= radius * radius) return -1.0;
    const Vector3d ro = ToDouble(start), delta = ToDouble(end) - ro;
    const double length = std::sqrt(Dot(delta, delta));
    if (!(length > 1.0e-12)) return -1.0;
    const Vector3d rd = delta * (1.0 / length);
    const double r2 = static_cast<double>(radius) * radius;
    double best = std::numeric_limits<double>::infinity();
    const auto sphere = [&](const Vector3d& center) {
        const Vector3d oc = ro - center;
        const double half = Dot(oc, rd);
        const double h = half * half - (Dot(oc, oc) - r2);
        if (h < 0.0) return;
        const double t = -half - std::sqrt(h);
        if (t >= 0.0) best = std::min(best, t);
    };
    const Vector3d pa = ToDouble(a), ba = ToDouble(b) - pa;
    const double baba = Dot(ba, ba);
    if (baba > 1.0e-24) {
        const Vector3d oa = ro - pa;
        const double bard = Dot(ba, rd), baoa = Dot(ba, oa);
        const double qa = baba - bard * bard;
        const double qb = baba * Dot(rd, oa) - baoa * bard;
        const double qc = baba * Dot(oa, oa) - baoa * baoa - r2 * baba;
        if (qa > 1.0e-12 * baba) {
            const double h = qb * qb - qa * qc;
            if (h >= 0.0) {
                const double t = (-qb - std::sqrt(h)) / qa;
                const double y = baoa + t * bard;
                if (t >= 0.0 && y > 0.0 && y < baba) best = std::min(best, t);
            }
        }
        sphere(ToDouble(b));
    }
    sphere(pa);
    return best <= length ? best / length : -1.0;
}

bool BoxesOverlapYZ(const Vector3& lowerA, const Vector3& upperA, const Vector3& lowerB, const Vector3& upperB)
{
    return lowerA.y <= upperB.y && lowerB.y <= upperA.y && lowerA.z <= upperB.z && lowerB.z <= upperA.z;
}
bool Finite(const Vector3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool Nonnegative(float v) { return std::isfinite(v) && v >= 0.0f; }
uint64_t PairKey(uint32_t a, uint32_t b)
{
    return (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
}
bool ValidContact(const ClothContact& c)
{
    if (!Finite(c.a) || !Finite(c.b) || !Finite(c.normal) || !Finite(c.velocity)
        || !Nonnegative(c.radius) || !std::isfinite(c.offset)) return false;
    switch (c.type) {
    case ClothContactType::SPHERE: return true;
    case ClothContactType::CAPSULE: return std::isfinite((c.b - c.a).LengthSq());
    case ClothContactType::PLANE:
        return std::abs(c.normal.LengthSq() - 1.0f) < 0.001f;
    default: return false;
    }
}
}

bool ClothSolver::Initialize(std::span<const math::Vector3> positions,
                             std::span<const uint32_t> indices,
                             std::span<const float> inverseMasses)
{
    if (positions.empty() || indices.empty() || indices.size() % 3 != 0
        || inverseMasses.size() != positions.size()) return false;
    for (size_t i = 0; i < positions.size(); ++i)
        if (!Finite(positions[i]) || !Nonnegative(inverseMasses[i])) return false;
    struct Edge { uint32_t opposite; bool paired = false; };
    std::map<std::pair<uint32_t, uint32_t>, Edge> edges;
    std::vector<DistanceConstraint> constraints;
    std::vector<std::array<uint32_t, 2>> meshEdges;
    for (size_t i = 0; i < indices.size(); i += 3) {
        const uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= positions.size() || b >= positions.size() || c >= positions.size()) return false;
        const float areaSquared = Vector3::Cross(positions[b] - positions[a], positions[c] - positions[a]).LengthSq();
        if (!std::isfinite(areaSquared) || areaSquared < EPSILON * EPSILON) return false;
        const uint32_t triangle[]{a, b, c};
        for (int j = 0; j < 3; ++j) {
            const uint32_t x = triangle[j], y = triangle[(j + 1) % 3], opposite = triangle[(j + 2) % 3];
            const auto key = std::make_pair(std::min(x, y), std::max(x, y));
            auto [it, inserted] = edges.emplace(key, Edge{opposite});
            if (inserted) {
                const float length = (positions[x] - positions[y]).Length();
                if (!std::isfinite(length) || length <= EPSILON) return false;
                constraints.push_back({x, y, length, 0.0f, false});
                meshEdges.push_back({key.first, key.second});
            } else {
                if (it->second.paired || it->second.opposite == opposite) return false;
                it->second.paired = true;
                const float length = (positions[opposite] - positions[it->second.opposite]).Length();
                if (!std::isfinite(length) || length <= EPSILON) return false;
                /// @note 初期版の曲げは対頂点間距離による近似。二面角拘束とは異なる。
                constraints.push_back({opposite, it->second.opposite, length, 0.0f, true});
            }
        }
    }
    m_positions.assign(positions.begin(), positions.end());
    m_restPositions = m_positions;
    m_pinTargets = m_positions;
    m_previous = m_positions;
    m_stepPositions = m_positions;
    m_velocities.assign(positions.size(), {});
    m_stepVelocities = m_velocities;
    m_inverseMasses.assign(inverseMasses.begin(), inverseMasses.end());
    m_indices.assign(indices.begin(), indices.end());
    m_constraints = std::move(constraints);
    m_edges = std::move(meshEdges);
    m_primitivePairs.clear();
    m_selfExcluded.clear();
    for (const auto& constraint : m_constraints) m_selfExcluded.push_back(PairKey(constraint.a, constraint.b));
    std::sort(m_selfExcluded.begin(), m_selfExcluded.end());
    return true;
}

bool ClothSolver::SetSettings(const ClothSettings& settings)
{
    if (!Finite(settings.gravity) || !Finite(settings.windVelocity)
        || !Nonnegative(settings.stretchCompliance) || !Nonnegative(settings.bendCompliance)
        || !Nonnegative(settings.damping) || !Nonnegative(settings.airDensity)
        || !Nonnegative(settings.dragCoefficient) || !Nonnegative(settings.thickness)
        || !Nonnegative(settings.friction) || settings.friction > 1.0f
        || !Nonnegative(settings.selfCollisionDistance)
        || (settings.selfCollisionDistance > 0.0f && settings.selfCollisionDistance < 1.0e-6f)
        || settings.selfCollisionDistance > 100.0f
        || !Nonnegative(settings.selfCollisionStiffness) || settings.selfCollisionStiffness > 1.0f
        || settings.substeps < 1 || settings.substeps > 64
        || settings.iterations < 1 || settings.iterations > 32) return false;
    m_settings = settings;
    return true;
}

bool ClothSolver::SetPinTarget(uint32_t vertex, const math::Vector3& position)
{
    if (vertex >= m_positions.size() || m_inverseMasses[vertex] != 0.0f || !Finite(position)) return false;
    m_pinTargets[vertex] = position;
    return true;
}

void ClothSolver::Reset()
{
    m_positions = m_restPositions;
    m_pinTargets = m_restPositions;
    std::fill(m_velocities.begin(), m_velocities.end(), Vector3{});
}

/// @see https://www1.grc.nasa.gov/beginners-guide-to-aeronautics/dynamic-pressure/ 動圧 q=ρv²/2。法線抗力への適用は初期版の近似。
void ClothSolver::ApplyWind(float h)
{
    for (size_t i = 0; i < m_indices.size(); i += 3) {
        const uint32_t a = m_indices[i], b = m_indices[i + 1], c = m_indices[i + 2];
        const Vector3 cross = Vector3::Cross(m_positions[b] - m_positions[a], m_positions[c] - m_positions[a]);
        const float twiceArea = cross.Length();
        if (twiceArea <= EPSILON) continue;
        const Vector3 normal = cross / twiceArea;
        const Vector3 relativeWind = m_settings.windVelocity - (m_velocities[a] + m_velocities[b] + m_velocities[c]) / 3.0f;
        const float speed = Vector3::Dot(relativeWind, normal);
        /// @note q=ρv²/2 の法線抗力を面積で積分し、3 頂点へ等分する。
        const Vector3 impulse = normal * (m_settings.airDensity * m_settings.dragCoefficient
            * twiceArea * speed * std::abs(speed) * h / 12.0f);
        for (uint32_t vertex : {a, b, c})
            if (m_stepInverseMasses[vertex] > 0.0f)
                m_velocities[vertex] += impulse * m_stepInverseMasses[vertex];
    }
}

/// @see https://matthias-research.github.io/pages/publications/XPBD.pdf 式 (18)–(19)、compliance/h² と累積 lambda。
void ClothSolver::SolveDistances(float h)
{
    for (auto& constraint : m_constraints) {
        const float wa = m_stepInverseMasses[constraint.a], wb = m_stepInverseMasses[constraint.b];
        if (wa + wb == 0.0f) continue;
        const Vector3 delta = m_positions[constraint.a] - m_positions[constraint.b];
        const float length = delta.Length();
        if (length <= EPSILON) continue;
        const float alpha = (constraint.bending ? m_settings.bendCompliance : m_settings.stretchCompliance) / (h * h);
        const float change = (-(length - constraint.restLength) - alpha * constraint.lambda) / (wa + wb + alpha);
        constraint.lambda += change;
        const Vector3 correction = delta * (change / length);
        m_positions[constraint.a] += correction * wa;
        m_positions[constraint.b] -= correction * wb;
    }
}

void ClothSolver::SolveContacts(std::span<const ClothContact> contacts, float h)
{
    for (size_t i = 0; i < m_positions.size(); ++i) {
        if (m_stepInverseMasses[i] == 0.0f) continue;
        for (const auto& contact : contacts) {
            Vector3 normal;
            float depth;
            bool swept = false;
            /// @note 形状は現在位置で与えられるため、形状に固定した座標で開始点を velocity*h だけ戻す。平面は半空間なので掃引しない。
            if (m_settings.continuousCollision && contact.type != ClothContactType::PLANE) {
                const Vector3 start = m_previous[i] + contact.velocity * h;
                const Vector3 end = contact.type == ClothContactType::CAPSULE ? contact.b : contact.a;
                const double hit = SweepCapsule(start, m_positions[i], contact.a, end, contact.radius + m_settings.thickness);
                if (hit >= 0.0) {
                    m_positions[i] = start + (m_positions[i] - start) * static_cast<float>(hit);
                    swept = true;
                }
            }
            if (contact.type == ClothContactType::PLANE) {
                normal = contact.normal;
                depth = contact.offset + m_settings.thickness - Vector3::Dot(m_positions[i], normal);
            } else {
                Vector3 center = contact.a;
                if (contact.type == ClothContactType::CAPSULE) {
                    const Vector3 axis = contact.b - contact.a;
                    if (axis.LengthSq() > EPSILON)
                        center += axis * std::clamp(Vector3::Dot(m_positions[i] - contact.a, axis) / axis.LengthSq(), 0.0f, 1.0f);
                }
                const Vector3 delta = m_positions[i] - center;
                const float length = delta.Length();
                normal = delta.NormalizedOr((m_previous[i] - center).NormalizedOr(Vector3::UP));
                depth = contact.radius + m_settings.thickness - length;
            }
            if (depth < 0.0f && !swept) continue;
            m_positions[i] += normal * std::max(depth, 0.0f);
            /// @note 摩擦は相対接線速度を減衰させる近似。位置射影の後に評価し、法線方向の侵入速度を除く。
            Vector3 relative = m_velocities[i] - contact.velocity;
            const float normalSpeed = Vector3::Dot(relative, normal);
            const Vector3 tangent = relative - normal * normalSpeed;
            relative = tangent * (1.0f - m_settings.friction) + normal * std::max(0.0f, normalSpeed);
            m_velocities[i] = relative + contact.velocity;
            m_previous[i] = m_positions[i] - m_velocities[i] * h;
        }
    }
}

/// @see https://nvidiagameworks.github.io/PhysX/3.3/PhysXGuide/Manual/Cloth.html#motion-constraints 中心から半径を超えた位置を球面へ射影する。
void ClothSolver::SolveMotion(std::span<const ClothMotionConstraint> motion, float fraction, float h, bool updateVelocity)
{
    for (const auto& constraint : motion) {
        const Vector3 center = Vector3::Lerp(constraint.previousCenter, constraint.center, fraction);
        auto& position = m_positions[constraint.particle];
        const Vector3 before = position;
        const Vector3 delta = position - center;
        const float length = delta.Length();
        if (constraint.radius == 0.0f) position = center;
        else if (length > constraint.radius) position = center + delta * (constraint.radius / length);
        /// @note 接触の接線摩擦を保持したまま、最終射影の位置補正だけ速度へ加える。
        if (updateVelocity) m_velocities[constraint.particle] += (position - before) / h;
    }
}

/// @see https://nvidiagameworks.github.io/PhysX/3.3/PhysXGuide/Manual/Cloth.html#self-collision-of-a-single-cloth-actor 質点球と rest-position による近傍除外。
bool ClothSolver::SolveSelfContacts()
{
    const float distance = m_settings.selfCollisionDistance;
    if (distance == 0.0f || m_settings.selfCollisionStiffness == 0.0f) return true;
    m_selfCells.clear();
    for (uint32_t i = 0; i < m_positions.size(); ++i) {
        const auto& p = m_positions[i];
        const double coords[]{std::floor(static_cast<double>(p.x) / distance),
            std::floor(static_cast<double>(p.y) / distance), std::floor(static_cast<double>(p.z) / distance)};
        SelfCell entry;
        entry.particle = i;
        for (int axis = 0; axis < 3; ++axis) {
            /// @note 整数変換と隣接セルの加算を安全な範囲に限定する。
            if (!std::isfinite(coords[axis]) || std::abs(coords[axis]) > 1.0e15) return false;
            entry.cell[axis] = static_cast<int64_t>(coords[axis]);
        }
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
                if (++candidates > 2000000) return false;
                const float wa = m_stepInverseMasses[a], wb = m_stepInverseMasses[b];
                if (wa + wb == 0.0f || std::binary_search(m_selfExcluded.begin(), m_selfExcluded.end(), PairKey(a, b))) continue;
                const Vector3 rest = m_restPositions[a] - m_restPositions[b];
                if (rest.LengthSq() < distance * distance) continue;
                const Vector3 delta = m_positions[a] - m_positions[b];
                const float length = delta.Length();
                if (length >= distance) continue;
                const Vector3 normal = delta.NormalizedOr(rest.NormalizedOr(Vector3::UP));
                const Vector3 correction = normal * ((distance - length) * m_settings.selfCollisionStiffness / (wa + wb));
                m_positions[a] += correction * wa;
                m_positions[b] -= correction * wb;
            }
        }
    }
    return true;
}

namespace {
using Quad = std::array<uint32_t, 4>;
using Weights = std::array<float, 4>;

/// @note 質点–三角形は p - (wa a + wb b + wc c)、辺–辺は p(s) - q(t) を表す係数。
Weights PairWeights(const std::vector<Vector3>& x, const Quad& v, bool edgeEdge, bool& interior)
{
    if (!edgeEdge) {
        const Vector3 w = TriangleWeights(x[v[0]], x[v[1]], x[v[2]], x[v[3]]);
        interior = w.x > INTERIOR_WEIGHT && w.y > INTERIOR_WEIGHT && w.z > INTERIOR_WEIGHT;
        return {1.0f, -w.x, -w.y, -w.z};
    }
    const auto [s, t] = SegmentParameters(x[v[0]], x[v[1]], x[v[2]], x[v[3]]);
    interior = s > 0.0f && s < 1.0f && t > 0.0f && t < 1.0f;
    return {1.0f - s, s, -(1.0f - t), -t};
}

Vector3 Combine(const std::vector<Vector3>& x, const Quad& v, const Weights& c)
{
    return x[v[0]] * c[0] + x[v[1]] * c[1] + x[v[2]] * c[2] + x[v[3]] * c[3];
}

/// @brief 現在の面法線 (辺–辺は 2 辺の外積) を、開始時刻に質点が居た側へ向ける。
/// @return 法線が縮退、または開始時の側が決まらなければ false。
bool OrientedNormal(const std::vector<Vector3>& x, const std::vector<Vector3>& previous,
                    const Quad& v, bool edgeEdge, const Weights& c, Vector3& normal)
{
    const Vector3 raw = edgeEdge ? Vector3::Cross(x[v[1]] - x[v[0]], x[v[3]] - x[v[2]])
                                 : Vector3::Cross(x[v[2]] - x[v[1]], x[v[3]] - x[v[1]]);
    const float lengthSq = raw.LengthSq();
    if (!(lengthSq > 1.0e-20f)) return false;
    normal = raw / std::sqrt(lengthSq);
    const float side = Vector3::Dot(Combine(previous, v, c), normal);
    if (std::abs(side) <= 1.0e-7f) return false;
    if (side < 0.0f) normal = -normal;
    return true;
}

/// @brief dot(Σ c_i x_i, n) を penetration だけ増やす逆質量重み付きの PBD 射影。
/// @see https://matthias-research.github.io/pages/publications/posBasedDyn.pdf Müller et al. 2007, 3.3 Solver と 4.4 Collisions の三角形衝突。
void ProjectSeparation(std::vector<Vector3>& x, const std::vector<float>& inverseMasses,
                       const Quad& v, const Weights& c, const Vector3& normal, float penetration)
{
    float denominator = 0.0f;
    for (int i = 0; i < 4; ++i) denominator += c[i] * c[i] * inverseMasses[v[i]];
    if (denominator <= EPSILON) return;
    const float scale = penetration / denominator;
    for (int i = 0; i < 4; ++i) x[v[i]] += normal * (scale * c[i] * inverseMasses[v[i]]);
}
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
        return Finite(box.lower) && Finite(box.upper);
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
    const auto movable = [&](const Quad& v) {
        return m_stepInverseMasses[v[0]] + m_stepInverseMasses[v[1]] + m_stepInverseMasses[v[2]] + m_stepInverseMasses[v[3]] > 0.0f;
    };
    /// @note 静止形状で既に接触距離より近い組は、細かいメッシュの隣接として除外する。
    const auto addPair = [&](const Quad& v, bool edgeEdge) {
        if (!movable(v)) return;
        bool interior = false;
        const Weights c = PairWeights(m_restPositions, v, edgeEdge, interior);
        if (Combine(m_restPositions, v, c).LengthSq() < distance * distance) return;
        m_primitivePairs.push_back({v, edgeEdge});
    };
    const auto visitPointTriangle = [&](uint32_t point, uint32_t triangle) {
        const Quad v{point, m_indices[triangle * 3], m_indices[triangle * 3 + 1], m_indices[triangle * 3 + 2]};
        if (point == v[1] || point == v[2] || point == v[3]) return;
        addPair(v, false);
    };
    size_t scanned = 0;
    for (const auto& point : m_pointBoxes) {
        auto it = std::lower_bound(m_triangleBoxes.begin(), m_triangleBoxes.end(), point.lower.x,
            [](const SweepBox& box, float x) { return box.lower.x < x; });
        for (; it != m_triangleBoxes.end() && it->lower.x <= point.upper.x; ++it) {
            if (++scanned > MAX_SELF_CANDIDATES) return false;
            if (BoxesOverlapYZ(point.lower, point.upper, it->lower, it->upper)) visitPointTriangle(point.index, it->index);
        }
    }
    for (const auto& triangle : m_triangleBoxes) {
        auto it = std::upper_bound(m_pointBoxes.begin(), m_pointBoxes.end(), triangle.lower.x,
            [](float x, const SweepBox& box) { return x < box.lower.x; });
        for (; it != m_pointBoxes.end() && it->lower.x <= triangle.upper.x; ++it) {
            if (++scanned > MAX_SELF_CANDIDATES) return false;
            if (BoxesOverlapYZ(it->lower, it->upper, triangle.lower, triangle.upper)) visitPointTriangle(it->index, triangle.index);
        }
    }
    for (size_t i = 0; i < m_edgeBoxes.size(); ++i) {
        const auto& first = m_edgeBoxes[i];
        for (size_t j = i + 1; j < m_edgeBoxes.size() && m_edgeBoxes[j].lower.x <= first.upper.x; ++j) {
            if (++scanned > MAX_SELF_CANDIDATES) return false;
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
        const Weights c = PairWeights(m_positions, pair.v, pair.edgeEdge, interior);
        if (!interior) continue;
        Vector3 normal;
        if (!OrientedNormal(m_positions, m_previous, pair.v, pair.edgeEdge, c, normal)) continue;
        const float separation = Vector3::Dot(Combine(m_positions, pair.v, c), normal);
        /// @note 接触距離を超えて裏へ抜けた組は近接として扱わない。抜けは CCD が拾う。
        if (separation >= distance || separation <= -distance) continue;
        ProjectSeparation(m_positions, m_stepInverseMasses, pair.v, c,
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
        const Quad& v = pair.v;
        /// @note 三次式は x0 を基準に (x1-x0)×(x2-x0)·(x3-x0)。辺–辺は p0 を、質点–三角形は a を基準に並べる。
        const Quad order = pair.edgeEdge ? v : Quad{v[1], v[2], v[3], v[0]};
        std::array<Vector3d, 4> start, end;
        for (int i = 0; i < 4; ++i) {
            start[i] = ToDouble(m_previous[order[i]]);
            end[i] = ToDouble(m_positions[order[i]]);
        }
        const double time = EarliestCoplanarTime(start, end);
        if (time < 0.0) continue;
        const float t = static_cast<float>(time);
        for (int i = 0; i < 4; ++i) impact[i] = Vector3::Lerp(m_previous[v[i]], m_positions[v[i]], t);
        const Quad local{0, 1, 2, 3};
        bool interior = false;
        const Weights c = PairWeights(impact, local, pair.edgeEdge, interior);
        if (Combine(impact, local, c).LengthSq() > distance * distance) continue;
        Vector3 normal;
        if (!OrientedNormal(m_positions, m_previous, v, pair.edgeEdge, c, normal)) continue;
        const float separation = Vector3::Dot(Combine(m_positions, v, c), normal);
        if (separation >= distance) continue;
        ProjectSeparation(m_positions, m_stepInverseMasses, v, c, normal, distance - separation);
        corrected = true;
    }
    return corrected;
}

bool ClothSolver::Step(float dt, std::span<const ClothContact> contacts, std::span<const ClothMotionConstraint> motion)
{
    if (m_positions.empty() || !std::isfinite(dt) || dt <= 0.0f || dt > 0.1f) return false;
    for (const auto& contact : contacts) if (!ValidContact(contact)) return false;
    m_stepInverseMasses = m_inverseMasses;
    m_motionUsed.assign(m_positions.size(), 0);
    for (const auto& constraint : motion) {
        if (constraint.particle >= m_positions.size() || !Finite(constraint.previousCenter)
            || !Finite(constraint.center) || !Nonnegative(constraint.radius)) return false;
        const uint32_t particle = constraint.particle;
        if (m_motionUsed[particle] || (m_inverseMasses[particle] == 0.0f && constraint.radius > 0.0f)) return false;
        m_motionUsed[particle] = 1;
        if (constraint.radius == 0.0f) m_stepInverseMasses[particle] = 0.0f;
    }
    const float h = dt / static_cast<float>(m_settings.substeps);
    if (h * h < 1.0e-16f) return false;
    m_stepPositions = m_positions;
    m_stepVelocities = m_velocities;
    const auto rollback = [this] {
        m_positions = m_stepPositions;
        m_velocities = m_stepVelocities;
        return false;
    };
    const bool primitives = m_settings.selfCollisionFaces && m_settings.selfCollisionDistance > 0.0f
        && m_settings.selfCollisionStiffness > 0.0f;
    const float damping = std::exp(-m_settings.damping * h);
    for (int substep = 0; substep < m_settings.substeps; ++substep) {
        m_previous = m_positions;
        ApplyWind(h);
        const float fraction = static_cast<float>(substep + 1) / static_cast<float>(m_settings.substeps);
        for (size_t i = 0; i < m_positions.size(); ++i) {
            if (m_stepInverseMasses[i] == 0.0f) {
                m_positions[i] = Vector3::Lerp(m_stepPositions[i], m_pinTargets[i], fraction);
            } else {
                m_velocities[i] = (m_velocities[i] + m_settings.gravity * h) * damping;
                m_positions[i] += m_velocities[i] * h;
            }
        }
        SolveMotion(motion, fraction, h, false);
        for (auto& constraint : m_constraints) constraint.lambda = 0.0f;
        /// @note 反復中の移動を見込み、予測位置の掃引を接触距離の 2 倍だけ広げて候補を固定する。
        if (primitives && !BuildPrimitivePairs(2.0f * m_settings.selfCollisionDistance)) return rollback();
        for (int iteration = 0; iteration < m_settings.iterations; ++iteration) {
            SolveDistances(h);
            if (!SolveSelfContacts()) return rollback();
            if (primitives) SolvePrimitiveProximity();
            SolveMotion(motion, fraction, h, false);
        }
        /// @note CCD は反復後の最終位置で候補を取り直す。補正が別の交差を生む場合に備えて数回まで繰り返す。
        if (primitives && m_settings.continuousCollision) {
            if (!BuildPrimitivePairs(m_settings.selfCollisionDistance)) return rollback();
            for (int pass = 0; pass < 4 && SolvePrimitiveContinuous(); ++pass) {}
        }
        for (size_t i = 0; i < m_positions.size(); ++i)
            m_velocities[i] = (m_positions[i] - m_previous[i]) / h;
        SolveContacts(contacts, h);
        SolveMotion(motion, fraction, h, true);
        for (size_t i = 0; i < m_positions.size(); ++i)
            if (!Finite(m_positions[i]) || !Finite(m_velocities[i])) return rollback();
    }
    return true;
}

}
