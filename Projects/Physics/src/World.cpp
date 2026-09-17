/// @file    World.cpp
/// @brief   物理シミュレーション世界の管理と Step 実行。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include <Physics/World.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/CylinderCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/HeightFieldCollider.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <utility>

/// @name レイキャスト交差判定ヘルパー (匿名 namespace)
namespace {

using namespace fbzz::math;
using namespace fbzz::physics;

/// 符号を返す (+1 or -1、0 のときは +1)
static inline float Sign(float v) { return v >= 0.0f ? 1.0f : -1.0f; }

/// @brief Ray vs Sphere。
/// @note oc = origin - center とし、二次方程式 |oc + t*d|^2 = r^2 を解く。
static bool RaySphere(const Vector3& o, const Vector3& d, float maxDist,
                      const Vector3& center, float radius,
                      float& tOut, Vector3& normalOut)
{
    const Vector3 oc = o - center;
    const float   b  = Vector3::Dot(oc, d);
    const float   c  = Vector3::Dot(oc, oc) - radius * radius;
    const float   disc = b * b - c;
    if (disc < 0.0f) return false;
    const float sqrtDisc = std::sqrt(disc);
    float t = -b - sqrtDisc;
    if (t < 0.0f) t = -b + sqrtDisc;
    if (t < 0.0f || t > maxDist) return false;
    tOut      = t;
    normalOut = (o + d * t - center).Normalized();
    return true;
}

/// @brief Ray vs AABB (スラブ法)。
/// @note 各軸のスラブ [min, max] にレイが入る/出る時刻 tMin/tMax を計算し、全軸の tMin の
///       最大と tMax の最小が [0, maxDist] に収まるか確認する。
static bool RayAABB(const Vector3& o, const Vector3& d, float maxDist,
                    const Vector3& aabbMin, const Vector3& aabbMax,
                    float& tOut, Vector3& normalOut)
{
    float tNear = 0.0f;
    float tFar  = maxDist;
    int   hitAxis = 0;
    bool  hitNeg  = false;

    const float* oArr    = &o.x;
    const float* dArr    = &d.x;
    const float* minArr  = &aabbMin.x;
    const float* maxArr  = &aabbMax.x;

    for (int i = 0; i < 3; ++i) {
        if (std::abs(dArr[i]) < 1e-8f) {
            /// @note レイが軸に平行: スラブ外なら miss
            if (oArr[i] < minArr[i] || oArr[i] > maxArr[i]) return false;
        } else {
            float t1 = (minArr[i] - oArr[i]) / dArr[i];
            float t2 = (maxArr[i] - oArr[i]) / dArr[i];
            bool  neg = t1 > t2;
            if (neg) std::swap(t1, t2);
            if (t1 > tNear) { tNear = t1; hitAxis = i; hitNeg = neg; }
            if (t2 < tFar)    tFar = t2;
            if (tNear > tFar) return false;
        }
    }
    if (tNear < 0.0f || tNear > maxDist) return false;

    tOut = tNear;
    /// @note 法線: ヒット面の外向き (hitAxis 軸、レイが +→- なら外側は + 方向)
    float normArr[3] = { 0.0f, 0.0f, 0.0f };
    normArr[hitAxis] = hitNeg ? 1.0f : -1.0f;
    normalOut = Vector3(normArr[0], normArr[1], normArr[2]);
    return true;
}

/// @brief Ray vs OBB。
/// @note レイを OBB ローカル空間へ変換して AABB 判定に帰着させ、法線をワールドへ戻す。
static bool RayOBB(const Vector3& o, const Vector3& d, float maxDist,
                   const OBBCollider& obb,
                   float& tOut, Vector3& normalOut)
{
    const Vector3    center = obb.GetCenter();
    const Quaternion rot    = obb.GetRotation();
    const Quaternion invRot = rot.Inverse();

    /// @note レイをローカル空間へ
    const Vector3 lo = invRot * (o - center);
    const Vector3 ld = invRot * d;
    const Vector3& he = obb.m_halfExtents;

    float     t;
    Vector3   ln;
    if (!RayAABB(lo, ld, maxDist, -he, he, t, ln)) return false;

    tOut      = t;
    normalOut = (rot * ln).Normalized();
    return true;
}

/// @brief Ray vs Capsule (無限円柱 + 端球)。
/// @note カプセルの中心軸を線分 [start, end] とし、まず無限円柱との交差を求め、
///       次に両端の半球を確認して最近点を返す。
static bool RayCapsule(const Vector3& o, const Vector3& d, float maxDist,
                       const CapsuleCollider& cap,
                       float& tOut, Vector3& normalOut)
{
    const Vector3 pa  = cap.GetSegmentStart();
    const Vector3 pb  = cap.GetSegmentEnd();
    const float   r   = cap.m_radius;
    const Vector3 ab  = pb - pa;
    const Vector3 ao  = o - pa;
    const float   abLen2 = Vector3::Dot(ab, ab);

    /// @note 無限円柱テスト: d と ab の平面に投影して 2D のレイ vs 円問題に帰着させる。
    const float m  = Vector3::Dot(d, ab) / abLen2;
    const float n  = Vector3::Dot(ao, ab) / abLen2;
    /// @note 投影後の方向
    const Vector3 q = d  - ab * m;
    /// @note 投影後の原点
    const Vector3 s = ao - ab * n;
    const float a2 = Vector3::Dot(q, q);
    const float b2 = 2.0f * Vector3::Dot(q, s);
    const float c2 = Vector3::Dot(s, s) - r * r;

    float bestT = maxDist + 1.0f;
    Vector3 bestN;

    if (std::abs(a2) > 1e-8f) {
        const float disc = b2 * b2 - 4.0f * a2 * c2;
        if (disc >= 0.0f) {
            float t = (-b2 - std::sqrt(disc)) / (2.0f * a2);
            if (t < 0.0f) t = (-b2 + std::sqrt(disc)) / (2.0f * a2);
            if (t >= 0.0f && t < maxDist) {
                /// @note カプセル軸上のパラメータ: 端球の外側なら棄却
                const float tAxis = m * t + n;
                if (tAxis >= 0.0f && tAxis <= 1.0f) {
                    const Vector3 hitPt = o + d * t;
                    const Vector3 axisPt = pa + ab * tAxis;
                    bestT = t;
                    bestN = (hitPt - axisPt).Normalized();
                }
            }
        }
    }

    /// @note 端球テスト
    float ts, ts2;
    Vector3 ns, ns2;
    if (RaySphere(o, d, maxDist, pa, r, ts, ns) && ts < bestT) { bestT = ts; bestN = ns; }
    if (RaySphere(o, d, maxDist, pb, r, ts2, ns2) && ts2 < bestT) { bestT = ts2; bestN = ns2; }

    if (bestT > maxDist) return false;
    tOut = bestT;
    normalOut = bestN;
    return true;
}

/// @brief Ray vs Cylinder (側面 + 上下の円板)。
/// @note 軸方向成分を抜いて 2D のレイ vs 円に帰着させ側面の交点を求め、軸に垂直な 2 枚の円板は
///       平面交点が半径内かで判定して最も手前を返す。
static bool RayCylinder(const Vector3& o, const Vector3& d, float maxDist,
                        const CylinderCollider& cyl,
                        float& tOut, Vector3& normalOut)
{
    const Vector3 center = cyl.GetCenter();
    const Vector3 axis   = cyl.GetAxis();
    const float   r      = cyl.m_radius;
    const float   h      = cyl.m_halfHeight;

    const Vector3 oc      = o - center;
    const float   dAxial  = Vector3::Dot(d, axis);
    const float   ocAxial = Vector3::Dot(oc, axis);
    const Vector3 dPerp   = d  - axis * dAxial;
    const Vector3 ocPerp  = oc - axis * ocAxial;

    float   bestT = maxDist + 1.0f;
    Vector3 bestN;

    /// @note 側面: 無限円柱との交点のうち、円板の間に収まるものだけ採用する
    const float a = Vector3::Dot(dPerp, dPerp);
    if (a > 1e-8f) {
        const float b    = 2.0f * Vector3::Dot(dPerp, ocPerp);
        const float c    = Vector3::Dot(ocPerp, ocPerp) - r * r;
        const float disc = b * b - 4.0f * a * c;
        if (disc >= 0.0f) {
            const float sqrtDisc = std::sqrt(disc);
            /// @note a > 0 なので 2 根は昇順。手前から見て最初に条件を満たしたものが最近点。
            const float roots[2] = { (-b - sqrtDisc) / (2.0f * a),
                                     (-b + sqrtDisc) / (2.0f * a) };
            for (const float t : roots) {
                if (t < 0.0f || t >= bestT) continue;
                const float axial = ocAxial + dAxial * t;
                if (std::abs(axial) > h) continue;
                bestT = t;
                bestN = (o + d * t - (center + axis * axial)).Normalized();
                break;
            }
        }
    }

    /// @note 上下の円板
    if (std::abs(dAxial) > 1e-8f) {
        for (int sign = -1; sign <= 1; sign += 2) {
            const float capAxial = h * static_cast<float>(sign);
            const float t = (capAxial - ocAxial) / dAxial;
            if (t < 0.0f || t >= bestT) continue;
            if ((o + d * t - (center + axis * capAxial)).LengthSq() > r * r) continue;
            bestT = t;
            bestN = axis * static_cast<float>(sign);
        }
    }

    if (bestT > maxDist) return false;
    tOut = bestT;
    normalOut = bestN;
    return true;
}

/// Ray vs Triangle (Möller–Trumbore)
/// 戻り値: ヒットした t (負なら miss)
static float RayTriangle(const Vector3& o, const Vector3& d,
                         const Vector3& v0, const Vector3& v1, const Vector3& v2)
{
    constexpr float EPS = 1e-7f;
    const Vector3 e1 = v1 - v0;
    const Vector3 e2 = v2 - v0;
    const Vector3 h  = Vector3::Cross(d, e2);
    const float   a  = Vector3::Dot(e1, h);
    if (std::abs(a) < EPS) return -1.0f;
    const float   f  = 1.0f / a;
    const Vector3 s  = o - v0;
    const float   u  = f * Vector3::Dot(s, h);
    if (u < 0.0f || u > 1.0f) return -1.0f;
    const Vector3 q  = Vector3::Cross(s, e1);
    const float   v  = f * Vector3::Dot(d, q);
    if (v < 0.0f || u + v > 1.0f) return -1.0f;
    return f * Vector3::Dot(e2, q);
}

/// Ray vs TriangleMesh (BVH トラバーサル)
static bool RayTriangleMesh(const Vector3& o, const Vector3& d, float maxDist,
                             const TriangleMeshCollider& mesh,
                             float& tOut, Vector3& normalOut)
{
    /// @note BVH クエリ用 AABB: レイを包む細長いボックスで粗いカリングをかける
    const Vector3 end = o + d * maxDist;
    AABB queryBox;
    queryBox.min = Vector3(std::min(o.x, end.x), std::min(o.y, end.y), std::min(o.z, end.z));
    queryBox.max = Vector3(std::max(o.x, end.x), std::max(o.y, end.y), std::max(o.z, end.z));

    float   bestT = maxDist + 1.0f;
    Vector3 bestN;

    mesh.GetBVH().Query(queryBox, [&](const Triangle& tri) {
        const float t = RayTriangle(o, d, tri.v[0], tri.v[1], tri.v[2]);
        if (t > 0.0f && t < bestT) {
            bestT = t;
            /// @note フロント/バック 両面対応: 法線がレイと逆向きなら反転
            bestN = Vector3::Dot(tri.normal, d) < 0.0f ? tri.normal : -tri.normal;
        }
    });

    if (bestT > maxDist) return false;
    tOut = bestT;
    normalOut = bestN;
    return true;
}

/// Ray vs ConvexHull (面ごとに三角形テスト)
static bool RayConvexHull(const Vector3& o, const Vector3& d, float maxDist,
                           const ConvexHullCollider& hull,
                           float& tOut, Vector3& normalOut)
{
    const auto& verts = hull.GetWorldVertices();
    const auto& faces = hull.GetFaces();
    float   bestT = maxDist + 1.0f;
    Vector3 bestN;

    for (const auto& face : faces) {
        const Vector3& v0 = verts[face[0]];
        const Vector3& v1 = verts[face[1]];
        const Vector3& v2 = verts[face[2]];
        const float t = RayTriangle(o, d, v0, v1, v2);
        if (t > 0.0f && t < bestT) {
            bestT = t;
            const Vector3 e1 = v1 - v0;
            const Vector3 e2 = v2 - v0;
            Vector3 n = Vector3::Cross(e1, e2);
            if (n.LengthSq() > 1e-12f) {
                n = n.Normalized();
                bestN = Vector3::Dot(n, d) < 0.0f ? n : -n;
            }
        }
    }

    if (bestT > maxDist) return false;
    tOut = bestT;
    normalOut = bestN;
    return true;
}

/// ColliderInstance に対してレイキャストを行い、ヒット結果を出力する
static bool RaycastInstance(const Vector3& o, const Vector3& d, float maxDist,
                             const ColliderInstance& inst,
                             float& tOut, Vector3& normalOut)
{
    if (!inst.collider) return false;

    switch (inst.collider->GetType()) {
    case ColliderType::SPHERE: {
        const auto* s = static_cast<const SphereCollider*>(inst.collider);
        return RaySphere(o, d, maxDist, s->GetAABB().Center(), s->m_radius, tOut, normalOut);
    }
    case ColliderType::AABB: {
        const AABB aabb = inst.collider->GetAABB();
        return RayAABB(o, d, maxDist, aabb.min, aabb.max, tOut, normalOut);
    }
    case ColliderType::OBB: {
        const auto* obb = static_cast<const OBBCollider*>(inst.collider);
        return RayOBB(o, d, maxDist, *obb, tOut, normalOut);
    }
    case ColliderType::CAPSULE: {
        const auto* cap = static_cast<const CapsuleCollider*>(inst.collider);
        return RayCapsule(o, d, maxDist, *cap, tOut, normalOut);
    }
    case ColliderType::CYLINDER: {
        const auto* cyl = static_cast<const CylinderCollider*>(inst.collider);
        return RayCylinder(o, d, maxDist, *cyl, tOut, normalOut);
    }
    case ColliderType::TRIANGLE_MESH: {
        const auto* mesh = static_cast<const TriangleMeshCollider*>(inst.collider);
        return RayTriangleMesh(o, d, maxDist, *mesh, tOut, normalOut);
    }
    case ColliderType::CONVEX_HULL: {
        const auto* hull = static_cast<const ConvexHullCollider*>(inst.collider);
        return RayConvexHull(o, d, maxDist, *hull, tOut, normalOut);
    }
    case ColliderType::HEIGHT_FIELD: {
        /// @note HeightFieldCollider の BVH は TriangleMeshCollider と同一の `BVHTree<Triangle>` 構造を
        ///       持つ。RayTriangleMesh は TriangleMeshCollider を受け取るため直接呼べないが、
        ///       GetBVH() で同じクエリを実行できる。
        const auto* hf = static_cast<const HeightFieldCollider*>(inst.collider);
        const Vector3 hfEnd = o + d * maxDist;
        AABB hfQueryBox;
        hfQueryBox.min = Vector3(std::min(o.x, hfEnd.x), std::min(o.y, hfEnd.y), std::min(o.z, hfEnd.z));
        hfQueryBox.max = Vector3(std::max(o.x, hfEnd.x), std::max(o.y, hfEnd.y), std::max(o.z, hfEnd.z));
        float   hfBestT = maxDist + 1.0f;
        Vector3 hfBestN;
        hf->GetBVH().Query(hfQueryBox, [&](const Triangle& tri) {
            const float t = RayTriangle(o, d, tri.v[0], tri.v[1], tri.v[2]);
            if (t > 0.0f && t < hfBestT) {
                hfBestT = t;
                hfBestN = Vector3::Dot(tri.normal, d) < 0.0f ? tri.normal : -tri.normal;
            }
        });
        if (hfBestT > maxDist) return false;
        tOut = hfBestT;
        normalOut = hfBestN;
        return true;
    }
    default:
        return false;
    }
}

/// @brief Sphere vs ColliderInstance (OverlapSphere 用)。
/// @note 球の中心からコライダーへの最近点距離を求め、半径以内かを確認する。
static bool SphereOverlapsInstance(const Vector3& center, float radius,
                                   const ColliderInstance& inst)
{
    if (!inst.collider) return false;

    switch (inst.collider->GetType()) {
    case ColliderType::SPHERE: {
        const auto* s = static_cast<const SphereCollider*>(inst.collider);
        const float dist2 = (center - s->GetAABB().Center()).LengthSq();
        const float r = radius + s->m_radius;
        return dist2 <= r * r;
    }
    case ColliderType::AABB:
    case ColliderType::OBB:
    case ColliderType::TRIANGLE_MESH:
    case ColliderType::CONVEX_HULL: {
        /// @note AABB での保守的判定: 球の中心から AABB 上の最近点への距離
        const AABB aabb = inst.collider->GetAABB();
        float dist2 = 0.0f;
        const float* cArr   = &center.x;
        const float* minArr = &aabb.min.x;
        const float* maxArr = &aabb.max.x;
        for (int i = 0; i < 3; ++i) {
            const float v = cArr[i];
            if (v < minArr[i]) dist2 += (minArr[i] - v) * (minArr[i] - v);
            else if (v > maxArr[i]) dist2 += (v - maxArr[i]) * (v - maxArr[i]);
        }
        return dist2 <= radius * radius;
    }
    case ColliderType::CAPSULE: {
        const auto* cap = static_cast<const CapsuleCollider*>(inst.collider);
        const Vector3 ab = cap->GetSegmentEnd() - cap->GetSegmentStart();
        const Vector3 ac = center - cap->GetSegmentStart();
        const float len2 = Vector3::Dot(ab, ab);
        const float proj  = (len2 > 1e-8f)
                            ? std::clamp(Vector3::Dot(ac, ab) / len2, 0.0f, 1.0f)
                            : 0.0f;
        const Vector3 closest = cap->GetSegmentStart() + ab * proj;
        const float dist2 = (center - closest).LengthSq();
        const float r = radius + cap->m_radius;
        return dist2 <= r * r;
    }
    case ColliderType::CYLINDER: {
        const auto* cyl = static_cast<const CylinderCollider*>(inst.collider);
        const Vector3 closest = cyl->ClosestPoint(center);
        return (center - closest).LengthSq() <= radius * radius;
    }
    default:
        return false;
    }
}

} // namespace

namespace fbzz::physics
{
    namespace
    {
        uint32_t NextGeneration(uint32_t generation)
        {
            return generation == 0xFFFFFFFFu ? 1u : generation + 1u;
        }
    }

    void World::BeginSceneSync()
    {
        m_sceneSyncChanged = false;
        for (auto& slot : m_bodyPool)
            slot.touched = false;
        for (auto& slot : m_colliderPool)
            slot.touched = false;
        for (auto& slot : m_volumePool)
            slot.touched = false;
        for (auto& slot : m_constraintPool)
            slot.touched = false;
    }

    BodyHandle World::SyncBody(BodyHandle handle, RigidBody* body)
    {
        if (!body) return {};

        if (handle.IsValid()) {
            const size_t index = static_cast<size_t>(handle.slot - 1u);
            if (index < m_bodyPool.size() &&
                m_bodyPool[index].generation == handle.generation &&
                m_bodyPool[index].body &&
                !m_bodyPool[index].touched)
            {
                m_bodyPool[index].body = body;
                m_bodyPool[index].touched = true;
                return handle;
            }
        }

        for (size_t i = 0; i < m_bodyPool.size(); ++i) {
            if (m_bodyPool[i].body == body && !m_bodyPool[i].touched) {
                /// @note Handle は Scene 側 Component に保持されるが、コピー・再生成・初期化順で
                ///       stale/invalid になりうる。同じ RigidBody* が既に World にあるなら新規
                ///       slot を作らず既存 slot を再接続し、pool の肥大化を防ぐ。
                m_bodyPool[i].touched = true;
                return { static_cast<uint32_t>(i + 1u), m_bodyPool[i].generation };
            }
        }

        for (size_t i = 0; i < m_bodyPool.size(); ++i) {
            if (!m_bodyPool[i].body) {
                m_bodyPool[i].body = body;
                m_bodyPool[i].touched = true;
                m_sceneSyncChanged = true;
                return { static_cast<uint32_t>(i + 1u), m_bodyPool[i].generation };
            }
        }

        BodySlot slot;
        slot.body = body;
        slot.touched = true;
        m_bodyPool.push_back(std::move(slot));
        m_sceneSyncChanged = true;
        return { static_cast<uint32_t>(m_bodyPool.size()), m_bodyPool.back().generation };
    }

    ColliderHandle World::SyncCollider(ColliderHandle handle, ColliderInstance collider)
    {
        if (!collider.collider) return {};

        if (collider.body)
        {
            /// @note ColliderInstance は Physics 単体利用時にも Body と形状の対応を持つ入口になる。
            ///       Engine 側の PhysicsSystem だけに慣性設定を任せると、Tests のように World を直接使う経路で
            ///       AABB の「軸整合なので回転させない」という制約が抜けるため、同期時に必ず形状から慣性を更新する。
            collider.body->SetInertiaFromCollider(collider.collider);
        }

        if (handle.IsValid()) {
            const size_t index = static_cast<size_t>(handle.slot - 1u);
            if (index < m_colliderPool.size() &&
                m_colliderPool[index].generation == handle.generation &&
                m_colliderPool[index].occupied &&
                !m_colliderPool[index].touched)
            {
                m_colliderPool[index].collider = std::move(collider);
                m_colliderPool[index].touched = true;
                return handle;
            }
        }

        for (size_t i = 0; i < m_colliderPool.size(); ++i) {
            if (m_colliderPool[i].occupied &&
                m_colliderPool[i].collider.collider == collider.collider &&
                !m_colliderPool[i].touched)
            {
                /// @note ColliderHandle が stale になっても Collider* が同じなら Scene 上は
                ///       同じ ColliderComponent。既存 slot を再接続し、handle 不整合が毎フレーム
                ///       の重複登録へ発展するのを防ぐ。
                m_colliderPool[i].collider = std::move(collider);
                m_colliderPool[i].touched = true;
                return { static_cast<uint32_t>(i + 1u), m_colliderPool[i].generation };
            }
        }

        for (size_t i = 0; i < m_colliderPool.size(); ++i) {
            if (!m_colliderPool[i].occupied) {
                m_colliderPool[i].collider = std::move(collider);
                m_colliderPool[i].occupied = true;
                m_colliderPool[i].touched = true;
                m_sceneSyncChanged = true;
                return { static_cast<uint32_t>(i + 1u), m_colliderPool[i].generation };
            }
        }

        ColliderSlot slot;
        slot.collider = std::move(collider);
        slot.occupied = true;
        slot.touched = true;
        m_colliderPool.push_back(std::move(slot));
        m_sceneSyncChanged = true;
        return { static_cast<uint32_t>(m_colliderPool.size()), m_colliderPool.back().generation };
    }

    VolumeHandle World::SyncVolume(VolumeHandle handle, std::unique_ptr<Volume> volume)
    {
        if (!volume) return {};

        if (handle.IsValid()) {
            const size_t index = static_cast<size_t>(handle.slot - 1u);
            if (index < m_volumePool.size() &&
                m_volumePool[index].generation == handle.generation &&
                m_volumePool[index].volume &&
                !m_volumePool[index].touched)
            {
                m_volumePool[index].volume = std::move(volume);
                m_volumePool[index].touched = true;
                return handle;
            }
        }

        for (size_t i = 0; i < m_volumePool.size(); ++i) {
            if (!m_volumePool[i].volume) {
                m_volumePool[i].volume = std::move(volume);
                m_volumePool[i].touched = true;
                m_sceneSyncChanged = true;
                return { static_cast<uint32_t>(i + 1u), m_volumePool[i].generation };
            }
        }

        VolumeSlot slot;
        slot.volume = std::move(volume);
        slot.touched = true;
        m_volumePool.push_back(std::move(slot));
        m_sceneSyncChanged = true;
        return { static_cast<uint32_t>(m_volumePool.size()), m_volumePool.back().generation };
    }

    void World::EndSceneSync()
    {
        m_bodies.clear();
        for (auto& slot : m_bodyPool) {
            if (slot.touched && slot.body) {
                m_bodies.push_back(slot.body);
            } else if (slot.body) {
                /// @note 所有権は Component 側にあるため World はポインタを null するだけ。
                ///       Component が破棄されれば unique_ptr により自動解放される。
                slot.body = nullptr;
                slot.generation = NextGeneration(slot.generation);
                m_sceneSyncChanged = true;
            }
        }

        m_colliders.clear();
        for (auto& slot : m_colliderPool) {
            if (slot.touched && slot.occupied && slot.collider.collider) {
                m_colliders.push_back(slot.collider);
            } else if (slot.occupied) {
                slot.collider = {};
                slot.occupied = false;
                slot.generation = NextGeneration(slot.generation);
                m_sceneSyncChanged = true;
            }
        }

        m_volumes.clear();
        for (auto& slot : m_volumePool) {
            if (slot.touched && slot.volume) {
                m_volumes.push_back(slot.volume.get());
            } else if (slot.volume) {
                /// @note World が所有しているので直接破棄する
                slot.volume.reset();
                slot.generation = NextGeneration(slot.generation);
                m_sceneSyncChanged = true;
            }
        }

        for (auto& slot : m_constraintPool) {
            if (slot.touched || !slot.constraint) continue;
            /// @note World が所有しているので直接破棄する
            slot.constraint.reset();
            slot.generation = NextGeneration(slot.generation);
            m_sceneSyncChanged = true;
        }
        RebuildConstraintViews();
    }

    void World::AddConstraint(std::unique_ptr<Constraint> constraint)
    {
        m_ownedConstraints.push_back(std::move(constraint));
        RebuildConstraintViews();
    }

    ConstraintHandle World::SyncConstraint(ConstraintHandle handle,
                                           std::unique_ptr<Constraint> constraint)
    {
        if (!constraint) return {};

        if (handle.IsValid()) {
            const size_t index = static_cast<size_t>(handle.slot - 1u);
            if (index < m_constraintPool.size() &&
                m_constraintPool[index].generation == handle.generation &&
                !m_constraintPool[index].touched)
            {
                m_constraintPool[index].constraint = std::move(constraint);
                m_constraintPool[index].touched = true;
                m_sceneSyncChanged = true;
                RebuildConstraintViews();
                return handle;
            }
        }

        for (size_t i = 0; i < m_constraintPool.size(); ++i) {
            if (m_constraintPool[i].constraint || m_constraintPool[i].touched) continue;
            m_constraintPool[i].constraint = std::move(constraint);
            m_constraintPool[i].touched = true;
            m_sceneSyncChanged = true;
            RebuildConstraintViews();
            return { static_cast<uint32_t>(i + 1u), m_constraintPool[i].generation };
        }

        ConstraintSlot slot;
        slot.constraint = std::move(constraint);
        slot.touched = true;
        m_constraintPool.push_back(std::move(slot));
        m_sceneSyncChanged = true;
        RebuildConstraintViews();
        return { static_cast<uint32_t>(m_constraintPool.size()),
                 m_constraintPool.back().generation };
    }

    bool World::KeepConstraint(ConstraintHandle handle)
    {
        if (!handle.IsValid()) return false;
        const size_t index = static_cast<size_t>(handle.slot - 1u);
        if (index >= m_constraintPool.size()) return false;
        ConstraintSlot& slot = m_constraintPool[index];
        if (slot.generation != handle.generation || !slot.constraint) return false;
        slot.touched = true;
        return true;
    }

    Constraint* World::FindConstraint(ConstraintHandle handle) const
    {
        if (!handle.IsValid()) return nullptr;
        const size_t index = static_cast<size_t>(handle.slot - 1u);
        if (index >= m_constraintPool.size()) return nullptr;
        const ConstraintSlot& slot = m_constraintPool[index];
        if (slot.generation != handle.generation) return nullptr;
        return slot.constraint.get();
    }

    void World::RemoveConstraint(ConstraintHandle handle)
    {
        if (!handle.IsValid()) return;
        const size_t index = static_cast<size_t>(handle.slot - 1u);
        if (index >= m_constraintPool.size()) return;
        ConstraintSlot& slot = m_constraintPool[index];
        if (slot.generation != handle.generation || !slot.constraint) return;
        slot.constraint.reset();
        slot.touched = false;
        slot.generation = NextGeneration(slot.generation);
        m_sceneSyncChanged = true;
        RebuildConstraintViews();
    }

    void World::RebuildConstraintViews()
    {
        m_activeConstraints.clear();
        m_activeConstraints.reserve(m_ownedConstraints.size() + m_constraintPool.size());
        for (const auto& constraint : m_ownedConstraints)
            if (constraint) m_activeConstraints.push_back(constraint.get());
        for (const auto& slot : m_constraintPool)
            if (slot.constraint) m_activeConstraints.push_back(slot.constraint.get());
    }

    const std::vector<Constraint*>& World::GetConstraints() const
    {
        return m_activeConstraints;
    }

    void World::SetGravity(const math::Vector3& gravity)
    {
        m_gravity = gravity;
    }

    void World::SetSubsteps(int substeps)
    {
        m_substeps = std::clamp(substeps, 1, 32);
    }

    void World::Step(float dt, std::function<bool(int, int)> layerFilter)
    {
        m_layerFilter = layerFilter ? std::move(layerFilter) : m_defaultLayerFilter;
        /// @note Sleep 済みのシーンでは接触集合が変わらず、毎 substep の
        ///       UpdateColliders/BroadPhase/NarrowPhase/Resolve を再実行しても結果は変わらない
        ///       (Terrain/TriangleMesh がある resting scene での主な CPU 負荷)。Scene 同期で
        ///       追加・削除がなく動いている非 Static body も無ければ、前回 contacts から
        ///       Stay/Exit 分類だけ更新して collision pipeline を省略する。
        /// @note m_frameImpacts のクリアは分岐より前に置く。早期 return でも前フレームの値が
        ///       残らないようにするため。
        m_frameImpacts.clear();

        if (!m_sceneSyncChanged && !HasActiveSimulationBodies()) {
            ClassifyCollisions();
            return;
        }

        const int substeps = std::max(m_substeps, 1);
        const float subDt = dt / static_cast<float>(substeps);

        for (int s = 0; s < substeps; ++s)
        {
            RemoveExpiredVolumes();
            for (auto& volume : m_volumes)
                if (volume) volume->Tick(subDt);
            ApplyForcesAndVolumes(subDt, m_effectiveDts);
            ApplyConstraintForces(subDt);
            ApplyGravitationalAttraction();
            CCDPhase(subDt);
            IntegrateBodies(m_effectiveDts);
            SolveConstraintPositions(subDt);
            UpdateColliders();
            BroadPhase();
            /// @note WarmStart は最初のサブステップのみ
            NarrowPhase(s == 0);
            /// @note Resolve は速度を書き換えるため、「ぶつかった勢い」はこの時点でしか取れない。
            RecordApproachVelocities();
            WakeSleepingContacts();
            Resolve();
            /// @note 実際に加わったインパルスは解決後に確定する。
            RecordContactImpulses();
            UpdateSleepStates(subDt);
        }

        /// @note フレーム末尾に蓄積インパルスを保存し古いキャッシュを削除する
        m_contactCache.UpdateCache(m_contacts);
        m_contactCache.PurgeStale();

        ClassifyCollisions();
    }

    bool World::HasActiveSimulationBodies() const
    {
        for (const auto& body : m_bodies)
        {
            if (body && !body->IsStatic() && !body->IsSleeping())
                return true;
        }
        return false;
    }

    void World::RemoveExpiredVolumes()
    {
        m_volumes.erase(std::remove_if(m_volumes.begin(), m_volumes.end(),
            [](const Volume* volume) {
                return !volume || volume->IsExpired();
            }),
            m_volumes.end());
    }

    void World::ApplyForcesAndVolumes(float dt, std::vector<float>& effectiveDts)
    {
        effectiveDts.assign(m_bodies.size(), dt);

        for (size_t i = 0; i < m_bodies.size(); ++i)
        {
            auto& body = m_bodies[i];
            if (!body || body->IsSleeping()) continue;
            effectiveDts[i] *= std::clamp(body->m_timeScale, 0.0f, 8.0f);
            bool gravityOverridden = false;

            for (auto& volume : m_volumes)
            {
                if (!volume || !volume->Contains(body->GetPosition())) continue;

                effectiveDts[i] = effectiveDts[i] * volume->GetTimeScale();
                gravityOverridden = gravityOverridden || volume->OverridesGravity();
                volume->Apply(*body, effectiveDts[i]);
            }

            if (!body->IsStatic() && !gravityOverridden && body->m_useGravity)
                body->ApplyForceNoWake(m_gravity * body->GetMass() * body->m_gravityScale);
        }
    }

    void World::ApplyConstraintForces(float dt)
    {
        for (Constraint* constraint : m_activeConstraints)
        {
            if (constraint) constraint->ApplyForce(dt);
        }
    }

    void World::ApplyGravitationalAttraction()
    {
        constexpr float G = 6.674e-11f;

        for (size_t i = 0; i < m_bodies.size(); ++i)
        {
            RigidBody& a = *m_bodies[i];
            if (!a.m_isGravitationalSource) continue;

            for (size_t j = i + 1; j < m_bodies.size(); ++j)
            {
                RigidBody& b = *m_bodies[j];
                if (!b.m_isGravitationalSource) continue;

                math::Vector3 delta = b.GetPosition() - a.GetPosition();
                const float distSq = std::max(delta.LengthSq(), 0.0001f);
                const float dist = std::sqrt(distSq);
                const math::Vector3 dir = delta * (1.0f / dist);
                const float forceScale = G * a.m_gravitationalMass * b.m_gravitationalMass / distSq;
                const math::Vector3 force = dir * forceScale;

                if (!a.IsStatic()) a.ApplyForceNoWake(force);
                if (!b.IsStatic()) b.ApplyForceNoWake(-force);
            }
        }
    }

    void World::IntegrateBodies(const std::vector<float>& effectiveDts)
    {
        for (size_t i = 0; i < m_bodies.size(); ++i)
            m_bodies[i]->Integrate(effectiveDts[i]);
    }

    void World::SolveConstraintPositions(float dt)
    {
        for (Constraint* constraint : m_activeConstraints)
        {
            if (constraint) constraint->SolvePosition(dt);
        }
    }

    void World::UpdateColliders()
    {
        for (auto& instance : m_colliders)
        {
            if (instance.body && instance.collider)
            {
                const math::Quaternion bodyRotation = instance.body->GetRotation();
                const math::Vector3 worldCenter = instance.body->GetPosition()
                    + bodyRotation * instance.centerOffset;
                instance.collider->Update(worldCenter, bodyRotation);
            }
        }
    }

    void World::BroadPhase()
    {
        m_collisionPairs.clear();
        m_solver.BroadPhase(m_colliders, m_collisionPairs, m_layerFilter);
    }

    void World::NarrowPhase(bool doWarmStart)
    {
        m_contacts.clear();
        m_solver.NarrowPhase(m_collisionPairs, m_contacts);

        for (auto& cp : m_contacts)
        {
            if (cp.isTrigger) continue;
            /// @note 法線が潰れた接触では直交基底そのものが作れない。摩擦だけ切って
            ///       法線インパルス側の処理は続けられるよう、既定軸を入れておく。
            math::Vector3 t0 = math::Vector3::Cross(cp.normal, math::Vector3::RIGHT);
            if (t0.LengthSq() < 1e-6f)
                t0 = math::Vector3::Cross(cp.normal, math::Vector3::UP);
            t0            = t0.NormalizedOr(math::Vector3::RIGHT);
            cp.tangent[0] = t0;
            cp.tangent[1] = math::Vector3::Cross(cp.normal, t0).NormalizedOr(math::Vector3::FORWARD);
        }

        if (doWarmStart)
            m_contactCache.WarmStart(m_contacts);
    }

    void World::Resolve()
    {
        m_solver.Resolve(m_contacts);
    }

    void World::WakeSleepingContacts()
    {
        constexpr float WAKE_SPEED_SQ = 0.02f * 0.02f;
        for (const auto& cp : m_contacts)
        {
            if (cp.isTrigger) continue;
            RigidBody* a = cp.bodyA;
            RigidBody* b = cp.bodyB;
            const bool movingA = a && !a->IsSleeping() &&
                (a->GetVelocity().LengthSq() + a->GetAngularVelocity().LengthSq()) > WAKE_SPEED_SQ;
            const bool movingB = b && !b->IsSleeping() &&
                (b->GetVelocity().LengthSq() + b->GetAngularVelocity().LengthSq()) > WAKE_SPEED_SQ;
            if (movingA && b && b->IsSleeping()) b->WakeUp();
            if (movingB && a && a->IsSleeping()) a->WakeUp();
        }
    }

    void World::UpdateSleepStates(float dt)
    {
        constexpr float LINEAR_SLEEP_THRESHOLD = 0.03f;
        constexpr float ANGULAR_SLEEP_THRESHOLD = 0.03f;
        constexpr float SLEEP_TIME = 0.75f;
        for (auto& body : m_bodies)
        {
            if (body)
                body->UpdateSleepState(dt, LINEAR_SLEEP_THRESHOLD, ANGULAR_SLEEP_THRESHOLD, SLEEP_TIME);
        }
    }

    void World::CCDPhase(float dt)
    {
        if (dt <= 0.0f) return;
        /// @note m_useCCD が true かつ速度が十分に速い物体について、
        ///       他の球コライダー持ち物体との TOI を計算し速度をクランプする。
        ///       この処理は IntegrateBodies の前に呼ぶことで貫通を防ぐ。
        for (size_t i = 0; i < m_bodies.size(); ++i)
        {
            auto& bodyA = m_bodies[i];
            if (!bodyA->m_useCCD) continue;
            if (bodyA->IsStatic()) continue;
            const float localDtA = m_effectiveDts[i];
            if (!CCDSolver::NeedsCCD(*bodyA, bodyA->m_ccdRadius, localDtA)) continue;

            /// @note bodyA に紐づくコライダーを探す (SphereCollider のみ対応)
            const SphereCollider* sphereA = nullptr;
            for (const auto& inst : m_colliders)
            {
                if (inst.body == bodyA &&
                    inst.collider &&
                    inst.collider->GetType() == ColliderType::SPHERE)
                {
                    sphereA = static_cast<const SphereCollider*>(inst.collider);
                    break;
                }
            }
            if (!sphereA) continue;

            const math::Vector3 centerA = sphereA->GetAABB().Center();
            const float         radiusA = sphereA->m_radius;
            float minToi = 1.0f;

            /// @note 全ボディと TOI を計算し最小値を採用する
            for (size_t j = 0; j < m_bodies.size(); ++j)
            {
                if (i == j) continue;
                auto& bodyB = m_bodies[j];

                /// @note bodyB の SphereCollider を探す
                const SphereCollider* sphereB = nullptr;
                for (const auto& inst : m_colliders)
                {
                    if (inst.body == bodyB &&
                        inst.collider &&
                        inst.collider->GetType() == ColliderType::SPHERE)
                    {
                        sphereB = static_cast<const SphereCollider*>(inst.collider);
                        break;
                    }
                }
                if (!sphereB) continue;

                const math::Vector3 centerB = sphereB->GetAABB().Center();
                const float         radiusB = sphereB->m_radius;

                /// @note 相対速度を使った Swept Sphere テスト
                const math::Vector3 relVel = bodyA->GetVelocity() * (localDtA / dt)
                                            - (bodyB->IsStatic() ? math::Vector3::ZERO
                                                                  : bodyB->GetVelocity() * (m_effectiveDts[j] / dt));
                const CCDResult res = CCDSolver::SweptSphereSphere(
                    centerA, radiusA, relVel, centerB, radiusB, dt);

                if (res.hit && res.toi < minToi)
                    minToi = res.toi;
            }

            /// @note 速度を TOI でスケールし、衝突時点までしか進まないようにする
            ///       残りの速度解決は通常の NarrowPhase/Resolve が担う
            if (minToi < 1.0f)
                bodyA->SetVelocity(bodyA->GetVelocity() * minToi);
        }
    }

    void World::RecordApproachVelocities()
    {
        for (const auto& cp : m_contacts)
        {
            if (cp.isTrigger) continue;

            const math::Vector3 vRel = PhysicsSolver::RelativeVelocityAt(cp);
            /// @note normal は b → a 向きなので、近づいているとき Dot は負になる。
            ///       ゲーム側が扱いやすいよう「正 = 接近」へ符号を反転する。
            const float approach = -math::Vector3::Dot(vRel, cp.normal);
            /// @note 離れていく接触は衝突ではない
            if (approach <= 0.0f) continue;

            const Collider* a = cp.colliderA;
            const Collider* b = cp.colliderB;
            if (a > b) std::swap(a, b);

            ContactImpact& impact = m_frameImpacts[ColliderPair{ a, b }];
            if (approach > impact.approachSpeed) {
                impact.approachSpeed    = approach;
                impact.relativeVelocity = vRel;
            }
        }
    }

    void World::RecordContactImpulses()
    {
        for (const auto& cp : m_contacts)
        {
            if (cp.isTrigger) continue;

            const Collider* a = cp.colliderA;
            const Collider* b = cp.colliderB;
            if (a > b) std::swap(a, b);

            /// @note 接近していない接触 (床に載っているだけ等) は RecordApproachVelocities が
            ///       積んでいない。そこへインパルスだけを入れると «速度 0 なのに強い衝突» に
            ///       見えるエントリができるため find で確認してから加える。
            const auto it = m_frameImpacts.find(ColliderPair{ a, b });
            if (it == m_frameImpacts.end()) continue;

            it->second.normalImpulse = std::max(it->second.normalImpulse, cp.cachedNormalImpulse);
        }
    }

    void World::ClassifyCollisions()
    {
        m_enterEvents.clear();
        m_stayEvents.clear();
        m_exitEvents.clear();

        std::map<ColliderPair, CollisionEvent> currentEvents;
        for (auto& cp : m_contacts)
        {
            const Collider* a = cp.colliderA;
            const Collider* b = cp.colliderB;
            if (a > b) std::swap(a, b);

            const ColliderPair pair{ a, b };
            CollisionEvent event{
                cp.colliderA, cp.colliderB, cp.bodyA, cp.bodyB,
                cp.point, cp.normal, cp.depth, cp.isTrigger
            };
            /// @note このフレーム中に観測した衝突の強さを載せる。
            ///       接触が継続しているだけ (Stay) なら 0 のままになる。
            if (const auto impact = m_frameImpacts.find(pair); impact != m_frameImpacts.end()) {
                event.relativeVelocity = impact->second.relativeVelocity;
                event.approachSpeed    = impact->second.approachSpeed;
                event.normalImpulse    = impact->second.normalImpulse;
            }
            currentEvents.insert({ pair, event });
        }

        for (auto& [pair, event] : currentEvents)
        {
            if (m_prevEvents.count(pair) == 0)
                m_enterEvents.push_back(event);
            else
                m_stayEvents.push_back(event);
        }

        for (auto& [pair, event] : m_prevEvents)
        {
            if (currentEvents.count(pair) != 0) continue;

            /// @note 離れた瞬間のイベントに「ぶつかった強さ」は無い。前フレームの値を
            ///       そのまま残すと、Exit を見ているスクリプトが古い衝突速度を読んでしまう。
            CollisionEvent exitEvent = event;
            exitEvent.relativeVelocity = math::Vector3::ZERO;
            exitEvent.approachSpeed    = 0.0f;
            exitEvent.normalImpulse    = 0.0f;
            m_exitEvents.push_back(exitEvent);
        }

        m_prevEvents = std::move(currentEvents);
    }

    const std::vector<CollisionEvent>& World::GetEnterEvents() const { return m_enterEvents; }
    const std::vector<CollisionEvent>& World::GetStayEvents()  const { return m_stayEvents;  }
    const std::vector<CollisionEvent>& World::GetExitEvents()  const { return m_exitEvents;  }

    /// @name Raycast

    bool World::Raycast(const math::Vector3& origin,
                        const math::Vector3& direction,
                        float                maxDistance,
                        RaycastHit&          hit,
                        ColliderFilter        filter) const
    {
        /// @note 向きの無いレイは何にも当たらない。スクリプトから 0 ベクトルが来るのは
        ///       「対象と重なっている」等で普通に起きるため、当たり無しとして返す。
        if (direction.LengthSq() < 1e-12f) return false;
        const math::Vector3 d = direction.Normalized();
        float   bestT = maxDistance + 1.0f;
        RaycastHit bestHit;

        for (const auto& inst : m_colliders) {
            if (filter && !filter(inst)) continue;
            float   t;
            math::Vector3 n;
            if (RaycastInstance(origin, d, maxDistance, inst, t, n) && t < bestT) {
                bestT          = t;
                bestHit.point    = origin + d * t;
                bestHit.normal   = n;
                bestHit.distance = t;
                bestHit.collider = inst.collider;
                bestHit.body     = inst.body;
            }
        }

        if (bestT > maxDistance) return false;
        hit = bestHit;
        return true;
    }

    std::vector<World::RaycastHit> World::RaycastAll(const math::Vector3& origin,
                                                      const math::Vector3& direction,
                                                      float                maxDistance,
                                                      ColliderFilter        filter) const
    {
        if (direction.LengthSq() < 1e-12f) return {};
        const math::Vector3 d = direction.Normalized();
        std::vector<RaycastHit> results;

        for (const auto& inst : m_colliders) {
            if (filter && !filter(inst)) continue;
            float   t;
            math::Vector3 n;
            if (RaycastInstance(origin, d, maxDistance, inst, t, n)) {
                RaycastHit h;
                h.point    = origin + d * t;
                h.normal   = n;
                h.distance = t;
                h.collider = inst.collider;
                h.body     = inst.body;
                results.push_back(h);
            }
        }

        std::sort(results.begin(), results.end(),
                  [](const RaycastHit& a, const RaycastHit& b){ return a.distance < b.distance; });
        return results;
    }

    bool World::SphereCast(const math::Vector3& origin,
                           float                radius,
                           const math::Vector3& direction,
                           float                maxDistance,
                           RaycastHit&          hit,
                           ColliderFilter        filter) const
    {
        /// @note Minkowski 和による膨張: 各コライダーを球半径分だけ広げてから通常のレイキャストを行う。
        ///       膨張した形状はコライダー種ごとに近似する — Sphere: 半径を加算 / AABB,OBB: 各辺
        ///       方向へ radius だけ広げる / Capsule: カプセル半径を加算 / Cylinder,Mesh,Convex: AABB 近似。

        if (direction.LengthSq() < 1e-12f) return false;
        const math::Vector3 d = direction.Normalized();
        float     bestT = maxDistance + 1.0f;
        RaycastHit bestHit;

        for (const auto& inst : m_colliders) {
            if (!inst.collider) continue;
            if (filter && !filter(inst)) continue;

            float t = -1.0f;
            math::Vector3 n;

            switch (inst.collider->GetType()) {
            case ColliderType::SPHERE: {
                const auto* s = static_cast<const SphereCollider*>(inst.collider);
                RaySphere(origin, d, maxDistance, s->GetAABB().Center(),
                          s->m_radius + radius, t, n);
                break;
            }
            case ColliderType::CAPSULE: {
                /// @note CapsuleCollider の半径を膨張させて再判定する。一時オブジェクトを作らず
                ///       既存関数の radius 引数を拡張して再利用する。
                const auto* cap = static_cast<const CapsuleCollider*>(inst.collider);
                /// @note 軸両端の球を膨張
                float tA; math::Vector3 nA;
                float tB; math::Vector3 nB;
                bool hitA = RaySphere(origin, d, maxDistance,
                                      cap->GetSegmentStart(), cap->m_radius + radius, tA, nA);
                bool hitB = RaySphere(origin, d, maxDistance,
                                      cap->GetSegmentEnd(),   cap->m_radius + radius, tB, nB);
                /// @note 膨張 AABB でもテスト (保守的)
                AABB aabb = inst.collider->GetAABB();
                const math::Vector3 expand(radius, radius, radius);
                aabb.min = aabb.min - expand;
                aabb.max = aabb.max + expand;
                float tBox; math::Vector3 nBox;
                bool hitBox = RayAABB(origin, d, maxDistance, aabb.min, aabb.max, tBox, nBox);
                if (hitA && (t < 0.0f || tA < t)) { t = tA; n = nA; }
                if (hitB && (t < 0.0f || tB < t)) { t = tB; n = nB; }
                if (hitBox && (t < 0.0f || tBox < t)) { t = tBox; n = nBox; }
                break;
            }
            default: {
                /// @note AABB を radius 分だけ膨張させてレイテスト
                AABB aabb = inst.collider->GetAABB();
                const math::Vector3 expand(radius, radius, radius);
                aabb.min = aabb.min - expand;
                aabb.max = aabb.max + expand;
                RayAABB(origin, d, maxDistance, aabb.min, aabb.max, t, n);
                break;
            }
            }

            if (t >= 0.0f && t < bestT) {
                bestT            = t;
                bestHit.point    = origin + d * t;
                bestHit.normal   = n;
                bestHit.distance = t;
                bestHit.collider = inst.collider;
                bestHit.body     = inst.body;
            }
        }

        if (bestT > maxDistance) return false;
        hit = bestHit;
        return true;
    }

    std::vector<const ColliderInstance*> World::OverlapSphere(
                        const math::Vector3& center,
                        float                radius,
                        ColliderFilter        filter) const
    {
        std::vector<const ColliderInstance*> results;
        for (const auto& inst : m_colliders) {
            if (filter && !filter(inst)) continue;
            if (SphereOverlapsInstance(center, radius, inst))
                results.push_back(&inst);
        }
        return results;
    }

} // namespace fbzz::physics
