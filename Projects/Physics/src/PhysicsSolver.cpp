// FBZZ Engine
// PhysicsSolver.cpp | fbzz::physics
// 衝突検出 (Broad/Narrow フェーズ) と衝突解決 (インパルスベース)
#include <Physics/PhysicsSolver.hpp>
#include <Physics/PhysicsMaterial.hpp>
#include <Physics/GJK.hpp>
#include <Physics/EPA.hpp>
#include <Physics/HeightFieldCollider.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace fbzz::physics 
{
    namespace
    {
        struct ContactManifold
        {
            ContactPoint points[4];
            int count = 0;
        };

        void AddManifoldPoint(ContactManifold& manifold, const ContactPoint& base,
                              const math::Vector3& point)
        {
            ContactPoint cp = base;
            cp.point = point;
            manifold.points[manifold.count++] = cp;
        }

        ContactManifold BuildAABBManifold(const AABB& aabbA, const AABB& aabbB,
                                          const ContactPoint& base)
        {
            constexpr float EPS = 1e-5f;

            ContactManifold manifold;
            const float minX = std::max(aabbA.min.x, aabbB.min.x);
            const float maxX = std::min(aabbA.max.x, aabbB.max.x);
            const float minY = std::max(aabbA.min.y, aabbB.min.y);
            const float maxY = std::min(aabbA.max.y, aabbB.max.y);
            const float minZ = std::max(aabbA.min.z, aabbB.min.z);
            const float maxZ = std::min(aabbA.max.z, aabbB.max.z);

            auto AddRect = [&](float fixed, int axis)
            {
                float u0 = minX;
                float u1 = maxX;
                float v0 = minZ;
                float v1 = maxZ;

                if (axis == 0)
                {
                    u0 = minY;
                    u1 = maxY;
                }
                else if (axis == 2)
                {
                    v0 = minY;
                    v1 = maxY;
                }

                if ((u1 - u0) <= EPS || (v1 - v0) <= EPS)
                {
                    AddManifoldPoint(manifold, base, base.point);
                    return;
                }

                const float us[2] = { u0, u1 };
                const float vs[2] = { v0, v1 };
                for (float u : us)
                {
                    for (float v : vs)
                    {
                        if (axis == 0)
                            AddManifoldPoint(manifold, base, { fixed, u, v });
                        else if (axis == 1)
                            AddManifoldPoint(manifold, base, { u, fixed, v });
                        else
                            AddManifoldPoint(manifold, base, { u, v, fixed });
                    }
                }
            };

            if (std::abs(base.normal.x) > 0.5f)
                AddRect(base.point.x, 0);
            else if (std::abs(base.normal.y) > 0.5f)
                AddRect(base.point.y, 1);
            else
                AddRect(base.point.z, 2);

            if (manifold.count == 0)
                AddManifoldPoint(manifold, base, base.point);

            const float weight = 1.0f / static_cast<float>(manifold.count);
            for (int i = 0; i < manifold.count; ++i)
                manifold.points[i].positionCorrectionWeight = weight;

            return manifold;
        }

        bool IsPointInsideOBB(const math::Vector3& point, const OBBCollider& box, float tolerance)
        {
            const math::Vector3 delta = point - box.GetCenter();
            for (int i = 0; i < 3; ++i)
            {
                const float extent = i == 0 ? box.m_halfExtents.x : (i == 1 ? box.m_halfExtents.y : box.m_halfExtents.z);
                if (std::abs(math::Vector3::Dot(delta, box.GetAxis(i))) > extent + tolerance)
                    return false;
            }
            return true;
        }

        void AddUniqueManifoldPoint(ContactManifold& manifold, const ContactPoint& base,
                                    const math::Vector3& point)
        {
            constexpr float MATCH_RADIUS_SQ = 1e-4f;
            for (int i = 0; i < manifold.count; ++i)
            {
                if ((manifold.points[i].point - point).LengthSq() <= MATCH_RADIUS_SQ)
                    return;
            }

            if (manifold.count < 4)
                AddManifoldPoint(manifold, base, point);
        }

        ContactManifold BuildOBBManifold(const OBBCollider& a, const OBBCollider& b,
                                         const ContactPoint& base)
        {
            ContactManifold manifold;
            const float tolerance = std::max(base.depth, 0.01f) + 1e-4f;
            const auto cornersA = a.GetCorners();
            const auto cornersB = b.GetCorners();

            for (const math::Vector3& point : cornersA)
            {
                if (IsPointInsideOBB(point, b, tolerance))
                    AddUniqueManifoldPoint(manifold, base, point);
            }
            for (const math::Vector3& point : cornersB)
            {
                if (IsPointInsideOBB(point, a, tolerance))
                    AddUniqueManifoldPoint(manifold, base, point);
            }

            if (manifold.count == 0)
                AddManifoldPoint(manifold, base, base.point);

            const float weight = 1.0f / static_cast<float>(manifold.count);
            for (int i = 0; i < manifold.count; ++i)
                manifold.points[i].positionCorrectionWeight = weight;

            return manifold;
        }

        struct BroadPhaseProxy
        {
            size_t index = 0;
            AABB bounds;
            math::Vector3 center = math::Vector3::ZERO;
        };

        struct BroadPhaseNode
        {
            AABB bounds;
            int left = -1;
            int right = -1;
            int start = 0;
            int count = 0;

            bool IsLeaf() const { return left < 0 && right < 0; }
        };

        float AxisValue(const math::Vector3& v, int axis)
        {
            if (axis == 0) return v.x;
            if (axis == 1) return v.y;
            return v.z;
        }

        int LongestAxis(const math::Vector3& size)
        {
            if (size.x >= size.y && size.x >= size.z) return 0;
            if (size.y >= size.z) return 1;
            return 2;
        }

        AABB MergeBounds(const AABB& a, const AABB& b)
        {
            AABB out;
            out.min.x = std::min(a.min.x, b.min.x);
            out.min.y = std::min(a.min.y, b.min.y);
            out.min.z = std::min(a.min.z, b.min.z);
            out.max.x = std::max(a.max.x, b.max.x);
            out.max.y = std::max(a.max.y, b.max.y);
            out.max.z = std::max(a.max.z, b.max.z);
            return out;
        }

        AABB ComputeBounds(const std::vector<BroadPhaseProxy>& proxies,
                           const std::vector<int>& order,
                           int start,
                           int count)
        {
            AABB bounds = proxies[static_cast<size_t>(order[static_cast<size_t>(start)])].bounds;
            for (int i = 1; i < count; ++i)
            {
                const int proxyIndex = order[static_cast<size_t>(start + i)];
                bounds = MergeBounds(bounds, proxies[static_cast<size_t>(proxyIndex)].bounds);
            }
            return bounds;
        }

        AABB ComputeCenterBounds(const std::vector<BroadPhaseProxy>& proxies,
                                 const std::vector<int>& order,
                                 int start,
                                 int count)
        {
            const math::Vector3 first = proxies[static_cast<size_t>(order[static_cast<size_t>(start)])].center;
            AABB bounds{ first, first };
            for (int i = 1; i < count; ++i)
            {
                const math::Vector3 center = proxies[static_cast<size_t>(order[static_cast<size_t>(start + i)])].center;
                bounds.min.x = std::min(bounds.min.x, center.x);
                bounds.min.y = std::min(bounds.min.y, center.y);
                bounds.min.z = std::min(bounds.min.z, center.z);
                bounds.max.x = std::max(bounds.max.x, center.x);
                bounds.max.y = std::max(bounds.max.y, center.y);
                bounds.max.z = std::max(bounds.max.z, center.z);
            }
            return bounds;
        }

        int BuildBroadPhaseBVH(const std::vector<BroadPhaseProxy>& proxies,
                               std::vector<int>& order,
                               std::vector<BroadPhaseNode>& nodes,
                               int start,
                               int count)
        {
            constexpr int LEAF_SIZE = 4;

            const int nodeIndex = static_cast<int>(nodes.size());
            nodes.push_back({});

            BroadPhaseNode& node = nodes[static_cast<size_t>(nodeIndex)];
            node.bounds = ComputeBounds(proxies, order, start, count);
            node.start = start;
            node.count = count;

            if (count <= LEAF_SIZE)
                return nodeIndex;

            const AABB centerBounds = ComputeCenterBounds(proxies, order, start, count);
            const math::Vector3 centerSize = centerBounds.max - centerBounds.min;
            const int splitAxis = LongestAxis(centerSize);
            const int split = start + count / 2;

            // WHY: 毎フレーム再構築する一時 BVH なので、SAH ではなく中央値分割を使う。
            //      構築を O(n log n) に抑えつつ、総当たりより候補ペア数を安定して削減する。
            std::nth_element(order.begin() + start,
                             order.begin() + split,
                             order.begin() + start + count,
                             [&](int a, int b) {
                                 return AxisValue(proxies[static_cast<size_t>(a)].center, splitAxis) <
                                        AxisValue(proxies[static_cast<size_t>(b)].center, splitAxis);
                             });

            // WHAT: 全中心が同一点に潰れても split は count/2 で進むため、再帰は必ず収束する。
            node.left = BuildBroadPhaseBVH(proxies, order, nodes, start, split - start);
            node.right = BuildBroadPhaseBVH(proxies, order, nodes, split, start + count - split);
            return nodeIndex;
        }

        bool ShouldSkipBroadPhasePair(const ColliderInstance& a,
                                       const ColliderInstance& b,
                                       const std::function<bool(int, int)>& layerFilter)
        {
            if (layerFilter && !layerFilter(a.layer, b.layer)) return true;
            if (a.isTrigger || b.isTrigger) return false;

            // WHY: 非 Trigger の Static / Sleeping 同士は解決しても状態が変わらない。
            //      特に「Sleeping dynamic vs static TriangleMesh」は、接触維持のためだけに
            //      Terrain BVH クエリを毎 substep 実行して 1 フレーム数十 ms の原因になる。
            const bool inactiveA = !a.body || a.body->IsStatic() || a.body->IsSleeping();
            const bool inactiveB = !b.body || b.body->IsStatic() || b.body->IsSleeping();
            return inactiveA && inactiveB;
        }

        void AddBroadPhasePair(const std::vector<ColliderInstance>& colliders,
                               const BroadPhaseProxy& a,
                               const BroadPhaseProxy& b,
                               std::vector<CollisionPair>& outPairs,
                               const std::function<bool(int, int)>& layerFilter)
        {
            const ColliderInstance& colliderA = colliders[a.index];
            const ColliderInstance& colliderB = colliders[b.index];
            if (ShouldSkipBroadPhasePair(colliderA, colliderB, layerFilter)) return;
            if (!a.bounds.Overlaps(b.bounds)) return;

            outPairs.push_back({ &colliderA, &colliderB });
        }

        void CollectLeafPairs(const std::vector<ColliderInstance>& colliders,
                              const std::vector<BroadPhaseProxy>& proxies,
                              const std::vector<int>& order,
                              const BroadPhaseNode& node,
                              std::vector<CollisionPair>& outPairs,
                              const std::function<bool(int, int)>& layerFilter)
        {
            for (int i = 0; i < node.count; ++i)
            {
                const BroadPhaseProxy& a = proxies[static_cast<size_t>(order[static_cast<size_t>(node.start + i)])];
                for (int j = i + 1; j < node.count; ++j)
                {
                    const BroadPhaseProxy& b = proxies[static_cast<size_t>(order[static_cast<size_t>(node.start + j)])];
                    AddBroadPhasePair(colliders, a, b, outPairs, layerFilter);
                }
            }
        }

        void CollectNodePairs(const std::vector<ColliderInstance>& colliders,
                              const std::vector<BroadPhaseProxy>& proxies,
                              const std::vector<int>& order,
                              const std::vector<BroadPhaseNode>& nodes,
                              int nodeAIndex,
                              int nodeBIndex,
                              std::vector<CollisionPair>& outPairs,
                              const std::function<bool(int, int)>& layerFilter)
        {
            const BroadPhaseNode& nodeA = nodes[static_cast<size_t>(nodeAIndex)];
            const BroadPhaseNode& nodeB = nodes[static_cast<size_t>(nodeBIndex)];
            if (!nodeA.bounds.Overlaps(nodeB.bounds)) return;

            if (nodeA.IsLeaf() && nodeB.IsLeaf())
            {
                for (int i = 0; i < nodeA.count; ++i)
                {
                    const BroadPhaseProxy& a = proxies[static_cast<size_t>(order[static_cast<size_t>(nodeA.start + i)])];
                    for (int j = 0; j < nodeB.count; ++j)
                    {
                        const BroadPhaseProxy& b = proxies[static_cast<size_t>(order[static_cast<size_t>(nodeB.start + j)])];
                        AddBroadPhasePair(colliders, a, b, outPairs, layerFilter);
                    }
                }
                return;
            }

            if (nodeB.IsLeaf() || (!nodeA.IsLeaf() && nodeA.count >= nodeB.count))
            {
                CollectNodePairs(colliders, proxies, order, nodes, nodeA.left, nodeBIndex, outPairs, layerFilter);
                CollectNodePairs(colliders, proxies, order, nodes, nodeA.right, nodeBIndex, outPairs, layerFilter);
                return;
            }

            CollectNodePairs(colliders, proxies, order, nodes, nodeAIndex, nodeB.left, outPairs, layerFilter);
            CollectNodePairs(colliders, proxies, order, nodes, nodeAIndex, nodeB.right, outPairs, layerFilter);
        }

        void CollectSelfPairs(const std::vector<ColliderInstance>& colliders,
                              const std::vector<BroadPhaseProxy>& proxies,
                              const std::vector<int>& order,
                              const std::vector<BroadPhaseNode>& nodes,
                              int nodeIndex,
                              std::vector<CollisionPair>& outPairs,
                              const std::function<bool(int, int)>& layerFilter)
        {
            const BroadPhaseNode& node = nodes[static_cast<size_t>(nodeIndex)];
            if (node.IsLeaf())
            {
                CollectLeafPairs(colliders, proxies, order, node, outPairs, layerFilter);
                return;
            }

            CollectSelfPairs(colliders, proxies, order, nodes, node.left, outPairs, layerFilter);
            CollectNodePairs(colliders, proxies, order, nodes, node.left, node.right, outPairs, layerFilter);
            CollectSelfPairs(colliders, proxies, order, nodes, node.right, outPairs, layerFilter);
        }

        void ClosestPointsOnSegments(const math::Vector3& p1,
                                     const math::Vector3& q1,
                                     const math::Vector3& p2,
                                     const math::Vector3& q2,
                                     math::Vector3& outC1,
                                     math::Vector3& outC2)
        {
            constexpr float EPS = 1e-6f;

            const math::Vector3 d1 = q1 - p1;
            const math::Vector3 d2 = q2 - p2;
            const math::Vector3 r = p1 - p2;
            const float a = math::Vector3::Dot(d1, d1);
            const float e = math::Vector3::Dot(d2, d2);
            const float f = math::Vector3::Dot(d2, r);

            float s = 0.0f;
            float t = 0.0f;

            if (a <= EPS && e <= EPS)
            {
                // どちらも点に縮退している。
                outC1 = p1;
                outC2 = p2;
                return;
            }

            if (a <= EPS)
            {
                t = std::clamp(f / e, 0.0f, 1.0f);
            }
            else
            {
                const float c = math::Vector3::Dot(d1, r);
                if (e <= EPS)
                {
                    s = std::clamp(-c / a, 0.0f, 1.0f);
                }
                else
                {
                    const float b = math::Vector3::Dot(d1, d2);
                    const float denom = a * e - b * b;
                    if (denom > EPS)
                    {
                        s = std::clamp((b * f - c * e) / denom, 0.0f, 1.0f);
                    }
                    else
                    {
                        // ほぼ平行な線分は、重なっている区間の中央を代表点にする。
                        const float s0 = math::Vector3::Dot(p2 - p1, d1) / a;
                        const float s1 = math::Vector3::Dot(q2 - p1, d1) / a;
                        const float overlapMin = std::max(0.0f, std::min(s0, s1));
                        const float overlapMax = std::min(1.0f, std::max(s0, s1));
                        s = overlapMin <= overlapMax
                            ? (overlapMin + overlapMax) * 0.5f
                            : std::clamp((s0 + s1) * 0.5f, 0.0f, 1.0f);
                    }

                    t = (b * s + f) / e;
                    if (t < 0.0f)
                    {
                        t = 0.0f;
                        s = std::clamp(-c / a, 0.0f, 1.0f);
                    }
                    else if (t > 1.0f)
                    {
                        t = 1.0f;
                        s = std::clamp((b - c) / a, 0.0f, 1.0f);
                    }
                }
            }

            outC1 = p1 + d1 * s;
            outC2 = p2 + d2 * t;
        }
    } // namespace

    // ------------------------------------------------------------------ BroadPhase
    void PhysicsSolver::BroadPhase(const std::vector<ColliderInstance>& colliders,
                                    std::vector<CollisionPair>& outPairs,
                                    const std::function<bool(int, int)>& layerFilter)
    {
        // WHY: World::Step は substep ごとに BroadPhase を呼ぶ。ここで毎回 vector を新規確保すると、
        //      コライダー数が多いシーンほど衝突判定そのもの以外の allocator コストが目立つ。
        // WHAT: 一時 BVH 用バッファをスレッドローカルに保持し、容量をフレーム間で再利用する。
        static thread_local std::vector<BroadPhaseProxy> proxies;
        static thread_local std::vector<int> order;
        static thread_local std::vector<BroadPhaseNode> nodes;

        proxies.clear();
        proxies.reserve(colliders.size());

        for (size_t i = 0; i < colliders.size(); ++i)
        {
            if (!colliders[i].collider) continue;

            const AABB bounds = colliders[i].collider->GetAABB();
            BroadPhaseProxy proxy;
            proxy.index = i;
            proxy.bounds = bounds;
            proxy.center = bounds.Center();
            proxies.push_back(proxy);
        }

        if (proxies.size() < 2) return;

        order.resize(proxies.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = static_cast<int>(i);

        nodes.clear();
        nodes.reserve(proxies.size() * 2);
        // WHAT: コライダー AABB から毎ステップ一時 BVH を構築し、重なり得るノード同士だけを走査する。
        // WHY: 全ペア比較 O(n^2) は、非接触の遠いオブジェクトが増えるほど NarrowPhase 前に詰まるため。
        const int root = BuildBroadPhaseBVH(proxies, order, nodes, 0, static_cast<int>(proxies.size()));
        CollectSelfPairs(colliders, proxies, order, nodes, root, outPairs, layerFilter);
        return;

    }

    // ----------------------------------------------------------------- NarrowPhase
    void PhysicsSolver::NarrowPhase(const std::vector<CollisionPair>& pairs,
                                    std::vector<ContactPoint>& outContacts)
    {
        for (auto& pair : pairs)
        {
            ColliderType tA = pair.colliderA->collider->GetType();
            ColliderType tB = pair.colliderB->collider->GetType();
            ContactPoint cp;
            bool hit = false;
            bool pushedContacts = false;

            auto PushContact = [&](ContactPoint contact)
            {
                contact.bodyA = pair.colliderA->body;
                contact.bodyB = pair.colliderB->body;
                contact.colliderA = pair.colliderA->collider;
                contact.colliderB = pair.colliderB->collider;
                contact.materialA = pair.colliderA->material;
                contact.materialB = pair.colliderB->material;
                contact.isTrigger = pair.colliderA->isTrigger || pair.colliderB->isTrigger;
                if (contact.bodyA && contact.bodyB)
                {
                    const math::Vector3 bodyDelta = contact.bodyA->GetPosition() - contact.bodyB->GetPosition();
                    if (bodyDelta.LengthSq() > 1e-8f &&
                        math::Vector3::Dot(contact.normal, bodyDelta) < 0.0f)
                    {
                        contact.normal = -contact.normal;
                    }
                }
                outContacts.push_back(contact);
            };

            if (tA == ColliderType::SPHERE && tB == ColliderType::SPHERE)
            {
                hit = TestSphereSphere(
                    *static_cast<SphereCollider*>(pair.colliderA->collider),
                    *static_cast<SphereCollider*>(pair.colliderB->collider), cp);
            }
            else if (tA == ColliderType::AABB && tB == ColliderType::AABB)
            {
                hit = TestAABBAABB(
                    *static_cast<AABBCollider*>(pair.colliderA->collider),
                    *static_cast<AABBCollider*>(pair.colliderB->collider), cp);
                if (hit)
                {
                    const ContactManifold manifold = BuildAABBManifold(
                        static_cast<AABBCollider*>(pair.colliderA->collider)->GetAABB(),
                        static_cast<AABBCollider*>(pair.colliderB->collider)->GetAABB(),
                        cp);
                    for (int i = 0; i < manifold.count; ++i)
                        PushContact(manifold.points[i]);
                    pushedContacts = true;
                }
            }
            else if (tA == ColliderType::OBB && tB == ColliderType::OBB)
            {
                hit = TestOBBOBB(
                    *static_cast<OBBCollider*>(pair.colliderA->collider),
                    *static_cast<OBBCollider*>(pair.colliderB->collider), cp);
                if (hit)
                {
                    const ContactManifold manifold = BuildOBBManifold(
                        *static_cast<OBBCollider*>(pair.colliderA->collider),
                        *static_cast<OBBCollider*>(pair.colliderB->collider),
                        cp);
                    for (int i = 0; i < manifold.count; ++i)
                        PushContact(manifold.points[i]);
                    pushedContacts = true;
                }
            }
            else if (tA == ColliderType::SPHERE && tB == ColliderType::AABB)
            {
                hit = TestSphereAABB(
                    *static_cast<SphereCollider*>(pair.colliderA->collider),
                    *static_cast<AABBCollider*> (pair.colliderB->collider), cp);
            }
            else if (tA == ColliderType::AABB && tB == ColliderType::SPHERE)
            {
                // 引数順を正規化して呼び、法線を反転する
                hit = TestSphereAABB(
                    *static_cast<SphereCollider*>(pair.colliderB->collider),
                    *static_cast<AABBCollider*> (pair.colliderA->collider), cp);
                if (hit)
                {
                    cp.normal  = -cp.normal;
                }
            }
            else if (tA == ColliderType::SPHERE && tB == ColliderType::OBB)
            {
                hit = TestSphereOBB(
                    *static_cast<SphereCollider*>(pair.colliderA->collider),
                    *static_cast<OBBCollider*> (pair.colliderB->collider), cp);
            }
            else if (tA == ColliderType::OBB && tB == ColliderType::SPHERE)
            {
                hit = TestSphereOBB(
                    *static_cast<SphereCollider*>(pair.colliderB->collider),
                    *static_cast<OBBCollider*> (pair.colliderA->collider), cp);
                if (hit)
                {
                    cp.normal = -cp.normal;
                }
            }
            else if (tA == ColliderType::AABB && tB == ColliderType::OBB)
            {
                hit = TestAABBOBB(
                    *static_cast<AABBCollider*>(pair.colliderA->collider),
                    *static_cast<OBBCollider*> (pair.colliderB->collider), cp);
            }
            else if (tA == ColliderType::OBB && tB == ColliderType::AABB)
            {
                hit = TestAABBOBB(
                    *static_cast<AABBCollider*>(pair.colliderB->collider),
                    *static_cast<OBBCollider*> (pair.colliderA->collider), cp);
                if (hit)
                {
                    cp.normal = -cp.normal;
                }
            }
            else if (tA == ColliderType::SPHERE && tB == ColliderType::CAPSULE)
            {
                hit = TestSphereCapsule(
                    *static_cast<SphereCollider*>(pair.colliderA->collider),
                    *static_cast<CapsuleCollider*>(pair.colliderB->collider), cp);
            }
            else if (tA == ColliderType::CAPSULE && tB == ColliderType::SPHERE)
            {
                hit = TestSphereCapsule(
                    *static_cast<SphereCollider*>(pair.colliderB->collider),
                    *static_cast<CapsuleCollider*>(pair.colliderA->collider), cp);
                if (hit)
                {
                    cp.normal = -cp.normal;
                }
            }
            else if (tA == ColliderType::AABB && tB == ColliderType::CAPSULE)
            {
                hit = TestAABBCapsule(
                    *static_cast<AABBCollider*>(pair.colliderA->collider),
                    *static_cast<CapsuleCollider*>(pair.colliderB->collider), cp);
            }
            else if (tA == ColliderType::CAPSULE && tB == ColliderType::AABB)
            {
                hit = TestAABBCapsule(
                    *static_cast<AABBCollider*>(pair.colliderB->collider),
                    *static_cast<CapsuleCollider*>(pair.colliderA->collider), cp);
                if (hit)
                {
                    cp.normal = -cp.normal;
                }
            }
            else if (tA == ColliderType::OBB && tB == ColliderType::CAPSULE)
            {
                hit = TestOBBCapsule(
                    *static_cast<OBBCollider*>(pair.colliderA->collider),
                    *static_cast<CapsuleCollider*>(pair.colliderB->collider), cp);
            }
            else if (tA == ColliderType::CAPSULE && tB == ColliderType::OBB)
            {
                hit = TestOBBCapsule(
                    *static_cast<OBBCollider*>(pair.colliderB->collider),
                    *static_cast<CapsuleCollider*>(pair.colliderA->collider), cp);
                if (hit)
                {
                    cp.normal = -cp.normal;
                }
            }
            else if (tA == ColliderType::CAPSULE && tB == ColliderType::CAPSULE)
            {
                hit = TestCapsuleCapsule(
                    *static_cast<CapsuleCollider*>(pair.colliderA->collider),
                    *static_cast<CapsuleCollider*>(pair.colliderB->collider), cp);
            }

            else if (tA == ColliderType::CONVEX_HULL || tB == ColliderType::CONVEX_HULL)
            {
                // CONVEX_HULL を含むペアは GJK + EPA で処理する
                // CONVEX_HULL vs CONVEX_HULL
                if (tA == ColliderType::CONVEX_HULL && tB == ColliderType::CONVEX_HULL)
                {
                    hit = TestConvexConvex(
                        *static_cast<ConvexHullCollider*>(pair.colliderA->collider),
                        *static_cast<ConvexHullCollider*>(pair.colliderB->collider), cp);
                }
                else if ((tA == ColliderType::CONVEX_HULL && tB == ColliderType::TRIANGLE_MESH) ||
                         (tA == ColliderType::TRIANGLE_MESH && tB == ColliderType::CONVEX_HULL))
                {
                    const bool swapped = (tA == ColliderType::TRIANGLE_MESH);
                    const auto& hull = *static_cast<ConvexHullCollider*>(
                        (swapped ? pair.colliderB : pair.colliderA)->collider);
                    const auto& mesh = *static_cast<TriangleMeshCollider*>(
                        (swapped ? pair.colliderA : pair.colliderB)->collider);
                    hit = TestConvexHullTriangleMesh(hull, mesh, cp);
                    if (hit && swapped)
                        cp.normal = -cp.normal;
                }
                else
                {
                    // ConvexHull を常に B 側に正規化する
                    const bool swapped = (tA == ColliderType::CONVEX_HULL);
                    const ColliderInstance& dynInst  = *(swapped ? pair.colliderB : pair.colliderA);
                    const ColliderInstance& convInst = *(swapped ? pair.colliderA : pair.colliderB);
                    const ColliderType dynType = dynInst.collider->GetType();
                    const auto& hull = *static_cast<ConvexHullCollider*>(convInst.collider);

                    if (dynType == ColliderType::SPHERE)
                        hit = TestSphereConvex(
                            *static_cast<SphereCollider*>(dynInst.collider), hull, cp);
                    else if (dynType == ColliderType::AABB)
                        hit = TestAABBConvex(
                            *static_cast<AABBCollider*>(dynInst.collider), hull, cp);
                    else if (dynType == ColliderType::OBB)
                        hit = TestOBBConvex(
                            *static_cast<OBBCollider*>(dynInst.collider), hull, cp);
                    else if (dynType == ColliderType::CAPSULE)
                        hit = TestCapsuleConvex(
                            *static_cast<CapsuleCollider*>(dynInst.collider), hull, cp);
                    else if (dynType == ColliderType::CYLINDER)
                        hit = TestCylinderConvex(
                            *static_cast<CylinderCollider*>(dynInst.collider), hull, cp);

                    if (hit && swapped)
                        cp.normal = -cp.normal;
                }
            }
            else if (tA == ColliderType::TRIANGLE_MESH || tB == ColliderType::TRIANGLE_MESH)
            {
                // TRIANGLE_MESH vs TRIANGLE_MESH は両方 Static なのでスキップ
                if (tA == ColliderType::TRIANGLE_MESH && tB == ColliderType::TRIANGLE_MESH)
                {
                    // skip
                }
                else
                {
                    // TRIANGLE_MESH を常に B 側に正規化する
                    const bool swapped = (tA == ColliderType::TRIANGLE_MESH);
                    const ColliderInstance& dynInst  = *(swapped ? pair.colliderB : pair.colliderA);
                    const ColliderInstance& meshInst = *(swapped ? pair.colliderA : pair.colliderB);
                    const ColliderType dynType = dynInst.collider->GetType();
                    const auto& mesh = *static_cast<TriangleMeshCollider*>(meshInst.collider);

                    if (dynType == ColliderType::SPHERE)
                        hit = TestSphereTriangleMesh(
                            *static_cast<SphereCollider*>(dynInst.collider), mesh, cp);
                    else if (dynType == ColliderType::AABB)
                        hit = TestAABBTriangleMesh(
                            *static_cast<AABBCollider*>(dynInst.collider), mesh, cp);
                    else if (dynType == ColliderType::CAPSULE)
                        hit = TestCapsuleTriangleMesh(
                            *static_cast<CapsuleCollider*>(dynInst.collider), mesh, cp);
                    else if (dynType == ColliderType::OBB)
                        hit = TestOBBTriangleMesh(
                            *static_cast<OBBCollider*>(dynInst.collider), mesh, cp);
                    else if (dynType == ColliderType::CYLINDER)
                        hit = TestCylinderTriangleMesh(
                            *static_cast<CylinderCollider*>(dynInst.collider), mesh, cp);

                    // スワップした場合は法線を反転 (normal は dyn → mesh 方向)
                    if (hit && swapped)
                        cp.normal = -cp.normal;
                }
            }
            else if (tA == ColliderType::HEIGHT_FIELD || tB == ColliderType::HEIGHT_FIELD)
            {
                // HEIGHT_FIELD vs HEIGHT_FIELD は両方 Static なのでスキップ
                const bool isStaticA = (tA == ColliderType::HEIGHT_FIELD || tA == ColliderType::TRIANGLE_MESH);
                const bool isStaticB = (tB == ColliderType::HEIGHT_FIELD || tB == ColliderType::TRIANGLE_MESH);
                if (isStaticA && isStaticB)
                {
                    // skip
                }
                else
                {
                    // HEIGHT_FIELD を常に B 側に正規化する
                    const bool swapped = (tA == ColliderType::HEIGHT_FIELD);
                    const ColliderInstance& dynInst   = *(swapped ? pair.colliderB : pair.colliderA);
                    const ColliderInstance& fieldInst = *(swapped ? pair.colliderA : pair.colliderB);
                    const ColliderType dynType = dynInst.collider->GetType();
                    const auto& hf = *static_cast<HeightFieldCollider*>(fieldInst.collider);

                    if (dynType == ColliderType::SPHERE)
                        hit = TestSphereHeightField(
                            *static_cast<SphereCollider*>(dynInst.collider), hf, cp);
                    else if (dynType == ColliderType::AABB)
                        hit = TestAABBHeightField(
                            *static_cast<AABBCollider*>(dynInst.collider), hf, cp);
                    else if (dynType == ColliderType::CAPSULE)
                        hit = TestCapsuleHeightField(
                            *static_cast<CapsuleCollider*>(dynInst.collider), hf, cp);
                    else if (dynType == ColliderType::OBB)
                        hit = TestOBBHeightField(
                            *static_cast<OBBCollider*>(dynInst.collider), hf, cp);
                    else if (dynType == ColliderType::CONVEX_HULL)
                        hit = TestConvexHullHeightField(
                            *static_cast<ConvexHullCollider*>(dynInst.collider), hf, cp);
                    else if (dynType == ColliderType::CYLINDER)
                        hit = TestCylinderHeightField(
                            *static_cast<CylinderCollider*>(dynInst.collider), hf, cp);

                    if (hit && swapped)
                        cp.normal = -cp.normal;
                }
            }
            else if (tA == ColliderType::CYLINDER || tB == ColliderType::CYLINDER)
            {
                // ConvexHull / TriangleMesh / HeightField との組は上の分岐が先に拾う。
                // ここへ来るのは基本形状同士の組だけ。
                if (tA == ColliderType::CYLINDER && tB == ColliderType::CYLINDER)
                {
                    hit = TestCylinderCylinder(
                        *static_cast<CylinderCollider*>(pair.colliderA->collider),
                        *static_cast<CylinderCollider*>(pair.colliderB->collider), cp);
                }
                else
                {
                    // CYLINDER を常に B 側に正規化する
                    const bool swapped = (tA == ColliderType::CYLINDER);
                    const ColliderInstance& dynInst = *(swapped ? pair.colliderB : pair.colliderA);
                    const ColliderInstance& cylInst = *(swapped ? pair.colliderA : pair.colliderB);
                    const ColliderType dynType = dynInst.collider->GetType();
                    const auto& cylinder = *static_cast<CylinderCollider*>(cylInst.collider);

                    if (dynType == ColliderType::SPHERE)
                        hit = TestSphereCylinder(
                            *static_cast<SphereCollider*>(dynInst.collider), cylinder, cp);
                    else if (dynType == ColliderType::AABB)
                        hit = TestAABBCylinder(
                            *static_cast<AABBCollider*>(dynInst.collider), cylinder, cp);
                    else if (dynType == ColliderType::OBB)
                        hit = TestOBBCylinder(
                            *static_cast<OBBCollider*>(dynInst.collider), cylinder, cp);
                    else if (dynType == ColliderType::CAPSULE)
                        hit = TestCapsuleCylinder(
                            *static_cast<CapsuleCollider*>(dynInst.collider), cylinder, cp);

                    if (hit && swapped)
                        cp.normal = -cp.normal;
                }
            }

            if (hit && !pushedContacts) {
                cp.bodyA = pair.colliderA->body;
                cp.bodyB = pair.colliderB->body;
                cp.colliderA = pair.colliderA->collider;
                cp.colliderB = pair.colliderB->collider;
                cp.materialA = pair.colliderA->material;
                cp.materialB = pair.colliderB->material;
                cp.isTrigger = pair.colliderA->isTrigger || pair.colliderB->isTrigger;
                if (cp.bodyA && cp.bodyB)
                {
                    // 各テスト関数の戻り方向を最終的に bodyB → bodyA へ揃える。
                    const math::Vector3 bodyDelta = cp.bodyA->GetPosition() - cp.bodyB->GetPosition();
                    if (bodyDelta.LengthSq() > 1e-8f &&
                        math::Vector3::Dot(cp.normal, bodyDelta) < 0.0f)
                    {
                        cp.normal = -cp.normal;
                    }
                }
                outContacts.push_back(cp);
            }
        }
    }

    // --------------------------------------------------------------------- Resolve
    void PhysicsSolver::Resolve(std::vector<ContactPoint>& contacts)
    {
        if (contacts.empty()) return;

        // 事前計算: 全接触点の摩擦タンジェント軸を確定する
        for (auto& cp : contacts)
        {
            if (cp.isTrigger) continue;

            // normal に対して垂直な 2 軸を Gram-Schmidt で構築
            // 法線が潰れた接触では直交基底そのものが作れない。摩擦だけ切って
            // 法線インパルス側の処理は続けられるよう、既定軸を入れておく。
            math::Vector3 t0 = math::Vector3::Cross(cp.normal, math::Vector3::RIGHT);
            if (t0.LengthSq() < 1e-6f)
                t0 = math::Vector3::Cross(cp.normal, math::Vector3::UP);
            t0           = t0.NormalizedOr(math::Vector3::RIGHT);
            cp.tangent[0] = t0;
            cp.tangent[1] = math::Vector3::Cross(cp.normal, t0).NormalizedOr(math::Vector3::FORWARD);
        }

        if (contacts.size() == 1)
        {
            ContactPoint& cp = contacts[0];
            if (cp.isTrigger) return;

            const bool activeA = cp.bodyA && !cp.bodyA->IsStatic() && !cp.bodyA->IsSleeping();
            const bool activeB = cp.bodyB && !cp.bodyB->IsStatic() && !cp.bodyB->IsSleeping();
            if (!activeA && !activeB) return;

            // WHY: Player Capsule と Terrain Mesh のような単一接触では island graph を作る意味がない。
            //      unordered_map / vector island 構築を避け、Solver 本体だけを実行する。
            for (int i = 0; i < VELOCITY_ITER; ++i)
            {
                ResolveVelocity(cp);
                ResolveFriction(cp);
            }
            ResolvePosition(cp);
            return;
        }

        // WHY: Resolve は World::Step の substep ごとに呼ばれる。
        //      contacts が多いフレームで毎回 unordered_map のバケット確保を行うと、
        //      solver 本体以外の CPU 時間が増えるため容量を再利用する。
        static thread_local std::vector<std::vector<size_t>> islands;
        static thread_local std::unordered_map<RigidBody*, size_t> bodyToIsland;
        islands.clear();
        islands.reserve(contacts.size());
        bodyToIsland.clear();
        bodyToIsland.reserve(contacts.size() * 2);

        for (size_t i = 0; i < contacts.size(); ++i)
        {
            const ContactPoint& cp = contacts[i];
            if (cp.isTrigger) continue;
            RigidBody* a = cp.bodyA && !cp.bodyA->IsStatic() && !cp.bodyA->IsSleeping() ? cp.bodyA : nullptr;
            RigidBody* b = cp.bodyB && !cp.bodyB->IsStatic() && !cp.bodyB->IsSleeping() ? cp.bodyB : nullptr;
            if (!a && !b) continue;

            const auto itA = a ? bodyToIsland.find(a) : bodyToIsland.end();
            const auto itB = b ? bodyToIsland.find(b) : bodyToIsland.end();
            if (itA == bodyToIsland.end() && itB == bodyToIsland.end())
            {
                const size_t island = islands.size();
                islands.push_back({});
                if (a) bodyToIsland[a] = island;
                if (b) bodyToIsland[b] = island;
                islands[island].push_back(i);
            }
            else
            {
                size_t island = itA != bodyToIsland.end() ? itA->second : itB->second;
                if (itA != bodyToIsland.end() && itB != bodyToIsland.end() && itA->second != itB->second)
                {
                    const size_t other = itB->second;
                    islands[island].insert(islands[island].end(), islands[other].begin(), islands[other].end());
                    for (auto& entry : bodyToIsland)
                        if (entry.second == other) entry.second = island;
                    islands[other].clear();
                }
                if (a) bodyToIsland[a] = island;
                if (b) bodyToIsland[b] = island;
                islands[island].push_back(i);
            }
        }

        for (auto& island : islands)
        {
            if (island.empty()) continue;
            for (int i = 0; i < VELOCITY_ITER; ++i)
            {
                // PGS は接触を順に解くため、少ない反復でも前回フレームの Warm Start が効く。
                for (size_t contactIndex : island)
                {
                    ResolveVelocity(contacts[contactIndex]);
                    ResolveFriction(contacts[contactIndex]);
                }
            }

            for (size_t contactIndex : island)
                ResolvePosition(contacts[contactIndex]);
        }
    }

    // ---------------------------------------------------------- ResolveVelocity (PGS)
    math::Vector3 PhysicsSolver::RelativeVelocityAt(const ContactPoint& cp)
    {
        const RigidBody* bodyA = cp.bodyA;
        const RigidBody* bodyB = cp.bodyB;

        const math::Vector3 vA = bodyA ? bodyA->GetVelocity()        : math::Vector3::ZERO;
        const math::Vector3 vB = bodyB ? bodyB->GetVelocity()        : math::Vector3::ZERO;
        const math::Vector3 wA = bodyA ? bodyA->GetAngularVelocity() : math::Vector3::ZERO;
        const math::Vector3 wB = bodyB ? bodyB->GetAngularVelocity() : math::Vector3::ZERO;

        const math::Vector3 rA = bodyA ? cp.point - bodyA->GetPosition() : math::Vector3::ZERO;
        const math::Vector3 rB = bodyB ? cp.point - bodyB->GetPosition() : math::Vector3::ZERO;

        // 角速度による接触点の速度も含める。回転しながらぶつかる物体では
        // 重心速度だけを見ると実際の当たりの強さと合わない。
        const math::Vector3 vAContact = vA + math::Vector3::Cross(wA, rA);
        const math::Vector3 vBContact = vB + math::Vector3::Cross(wB, rB);
        return vAContact - vBContact;
    }

    void PhysicsSolver::ResolveVelocity(ContactPoint& cp)
    {
        if (cp.isTrigger) return;

        RigidBody* bodyA = cp.bodyA;
        RigidBody* bodyB = cp.bodyB;

        const float invMassA = bodyA ? bodyA->GetInvMass() : 0.0f;
        const float invMassB = bodyB ? bodyB->GetInvMass() : 0.0f;

        const math::Vector3 rA = bodyA ? cp.point - bodyA->GetPosition() : math::Vector3::ZERO;
        const math::Vector3 rB = bodyB ? cp.point - bodyB->GetPosition() : math::Vector3::ZERO;

        const math::Vector3 vRel  = RelativeVelocityAt(cp);
        const float         vRelN = math::Vector3::Dot(vRel, cp.normal);

        // WHY: Warm Start の過去インパルスが強すぎると、小さい Collider が大きい床上で微小な上向き速度を持つ。
        //      cachedNormalImpulse が残っている接触では即 return せず、下の PGS 累積クランプで過剰分を戻す。
        if (vRelN > 0.0f && (!cp.cacheImpulse || cp.cachedNormalImpulse <= 0.0f)) return;

        float e = 0.3f;
        if (cp.materialA && cp.materialB)
            e = PhysicsMaterial::CombineRestitution(*cp.materialA, *cp.materialB);

        // 静止接触の微小反発を消し、床上の物体が跳ね続けるのを防ぐ。
        constexpr float REST_THRESHOLD = 0.5f;
        if (vRelN >= 0.0f || std::abs(vRelN) < REST_THRESHOLD) e = 0.0f;
        const bool usesRestitution = e > 0.0f;

        float angTermA = 0.0f;
        float angTermB = 0.0f;
        if (bodyA)
        {
            const math::Vector3 rAxN = math::Vector3::Cross(rA, cp.normal);
            angTermA = math::Vector3::Dot(
                math::Vector3::Cross(bodyA->ApplyInvInertia(rAxN), rA), cp.normal);
        }
        if (bodyB)
        {
            const math::Vector3 rBxN = math::Vector3::Cross(rB, cp.normal);
            angTermB = math::Vector3::Dot(
                math::Vector3::Cross(bodyB->ApplyInvInertia(rBxN), rB), cp.normal);
        }

        const float denom = invMassA + invMassB + angTermA + angTermB;
        if (denom == 0.0f) return;

        // deltaJ は今回追加すべき法線インパルス。蓄積値は 0 未満にしない。
        const float deltaJ = -(1.0f + e) * vRelN / denom;
        float applyJ = 0.0f;
        if (usesRestitution)
        {
            // 反発インパルスは瞬間的な効果なので Warm Start へ持ち越さない。
            applyJ = std::max(0.0f, deltaJ);
            cp.cachedNormalImpulse = applyJ;
            cp.cacheImpulse = false;
        }
        else
        {
            const float oldAccum = cp.cachedNormalImpulse;
            const float newAccum = std::max(0.0f, oldAccum + deltaJ);
            applyJ = newAccum - oldAccum;
            cp.cachedNormalImpulse = newAccum;
        }

        const math::Vector3 impulse = cp.normal * applyJ;
        if (bodyA)
        {
            bodyA->SetVelocity(bodyA->GetVelocity() + impulse * invMassA);
            bodyA->ApplyAngularImpulse(math::Vector3::Cross(rA, impulse));
        }
        if (bodyB)
        {
            bodyB->SetVelocity(bodyB->GetVelocity() - impulse * invMassB);
            bodyB->ApplyAngularImpulse(-math::Vector3::Cross(rB, impulse));
        }
    }

    // ---------------------------------------------------------- ResolveFriction (PGS)
    void PhysicsSolver::ResolveFriction(ContactPoint& cp)
    {
        if (cp.isTrigger) return;

        RigidBody* bodyA = cp.bodyA;
        RigidBody* bodyB = cp.bodyB;

        const float invMassA = bodyA ? bodyA->GetInvMass() : 0.0f;
        const float invMassB = bodyB ? bodyB->GetInvMass() : 0.0f;
        if (invMassA + invMassB == 0.0f) return;

        float dynamicMu = 0.5f;
        float staticMu = 0.7f;
        if (cp.materialA && cp.materialB)
        {
            dynamicMu = PhysicsMaterial::CombineFriction(*cp.materialA, *cp.materialB);
            staticMu = PhysicsMaterial::CombineStaticFriction(*cp.materialA, *cp.materialB);
        }

        // 摩擦コーン制約: |Λt| ≤ μ * Λn
        const float maxDynamicFriction = dynamicMu * cp.cachedNormalImpulse;
        const float maxStaticFriction = staticMu * cp.cachedNormalImpulse;

        const math::Vector3 rA = bodyA ? cp.point - bodyA->GetPosition() : math::Vector3::ZERO;
        const math::Vector3 rB = bodyB ? cp.point - bodyB->GetPosition() : math::Vector3::ZERO;

        for (int k = 0; k < 2; ++k)
        {
            const math::Vector3 vA = bodyA ? bodyA->GetVelocity()        : math::Vector3::ZERO;
            const math::Vector3 vB = bodyB ? bodyB->GetVelocity()        : math::Vector3::ZERO;
            const math::Vector3 wA = bodyA ? bodyA->GetAngularVelocity() : math::Vector3::ZERO;
            const math::Vector3 wB = bodyB ? bodyB->GetAngularVelocity() : math::Vector3::ZERO;
            const math::Vector3 vRel = (vA + math::Vector3::Cross(wA, rA))
                                     - (vB + math::Vector3::Cross(wB, rB));
            const math::Vector3& t     = cp.tangent[k];
            const float          vRelT = math::Vector3::Dot(vRel, t);

            float angA = 0.0f;
            float angB = 0.0f;
            if (bodyA)
            {
                const math::Vector3 rAxT = math::Vector3::Cross(rA, t);
                angA = math::Vector3::Dot(math::Vector3::Cross(bodyA->ApplyInvInertia(rAxT), rA), t);
            }
            if (bodyB)
            {
                const math::Vector3 rBxT = math::Vector3::Cross(rB, t);
                angB = math::Vector3::Dot(math::Vector3::Cross(bodyB->ApplyInvInertia(rBxT), rB), t);
            }

            const float denom = invMassA + invMassB + angA + angB;
            if (denom == 0.0f) continue;

            const float deltaJt  = -vRelT / denom;
            const float oldAccum = cp.cachedTangentImpulse[k];
            // 蓄積摩擦インパルスを摩擦コーン内に投影する。
            const float targetAccum = oldAccum + deltaJt;
            const float newAccum = std::abs(targetAccum) <= maxStaticFriction
                ? targetAccum
                : std::clamp(targetAccum, -maxDynamicFriction, maxDynamicFriction);
            const float applyJt  = newAccum - oldAccum;
            cp.cachedTangentImpulse[k] = newAccum;

            const math::Vector3 impulse = t * applyJt;
            if (bodyA)
            {
                bodyA->SetVelocity(bodyA->GetVelocity() + impulse * invMassA);
                bodyA->ApplyAngularImpulse(math::Vector3::Cross(rA, impulse));
            }
            if (bodyB)
            {
                bodyB->SetVelocity(bodyB->GetVelocity() - impulse * invMassB);
                bodyB->ApplyAngularImpulse(-math::Vector3::Cross(rB, impulse));
            }
        }
    }

    // ---------------------------------------------------------- ResolvePosition
    void PhysicsSolver::ResolvePosition(ContactPoint& cp)
    {
        if (cp.isTrigger) return;

        const float invMassA = cp.bodyA ? cp.bodyA->GetInvMass() : 0.0f;
        const float invMassB = cp.bodyB ? cp.bodyB->GetInvMass() : 0.0f;
        const float invMassSum = invMassA + invMassB;
        if (invMassSum == 0.0f) return;

        // SLOP 分の浅い貫通は許容し、接触面の小さな振動を抑える。
        const float penetration = std::max(cp.depth - SLOP, 0.0f);
        const float scalar      = penetration / invMassSum * BAUMGARTE * cp.positionCorrectionWeight;
        math::Vector3 correction = cp.normal * scalar;

        if (cp.bodyA)
            cp.bodyA->SetPosition(cp.bodyA->GetPosition() + correction * invMassA);
        if (cp.bodyB)
            cp.bodyB->SetPosition(cp.bodyB->GetPosition() - correction * invMassB);
    }

    // ------------------------------------------------------- Narrow phase テスト関数

    bool PhysicsSolver::TestSphereSphere(const SphereCollider& a, const SphereCollider& b,
                                        ContactPoint& out)
    {
        math::Vector3 posA = a.GetAABB().Center();
        math::Vector3 posB = b.GetAABB().Center();
        math::Vector3 diff = posA - posB;
        float         dist = diff.Length();
        float         sumR = a.m_radius + b.m_radius;

        if (dist >= sumR || dist < 1e-6f) return false;

        out.normal = diff * (1.0f / dist);
        out.depth  = sumR - dist;
        out.point  = posB + out.normal * b.m_radius;
        return true;
    }

    bool PhysicsSolver::TestAABBAABB(const AABBCollider& a, const AABBCollider& b,
                                    ContactPoint& out)
    {
        AABB aabbA = a.GetAABB();
        AABB aabbB = b.GetAABB();

        float ox = std::min(aabbA.max.x, aabbB.max.x) - std::max(aabbA.min.x, aabbB.min.x);
        float oy = std::min(aabbA.max.y, aabbB.max.y) - std::max(aabbA.min.y, aabbB.min.y);
        float oz = std::min(aabbA.max.z, aabbB.max.z) - std::max(aabbA.min.z, aabbB.min.z);

        if (ox <= 0.0f || oy <= 0.0f || oz <= 0.0f) return false;

        math::Vector3 dir = aabbA.Center() - aabbB.Center();

        if (ox <= oy && ox <= oz)
        {
            out.depth  = ox;
            out.normal = { dir.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f };
            out.point = {
                out.normal.x > 0.0f ? (aabbA.min.x + aabbB.max.x) * 0.5f
                                    : (aabbA.max.x + aabbB.min.x) * 0.5f,
                (std::max(aabbA.min.y, aabbB.min.y) + std::min(aabbA.max.y, aabbB.max.y)) * 0.5f,
                (std::max(aabbA.min.z, aabbB.min.z) + std::min(aabbA.max.z, aabbB.max.z)) * 0.5f
            };
        }
        else if (oy <= ox && oy <= oz)
        {
            out.depth  = oy;
            out.normal = { 0.0f, dir.y >= 0.0f ? 1.0f : -1.0f, 0.0f };
            out.point = {
                (std::max(aabbA.min.x, aabbB.min.x) + std::min(aabbA.max.x, aabbB.max.x)) * 0.5f,
                out.normal.y > 0.0f ? (aabbA.min.y + aabbB.max.y) * 0.5f
                                    : (aabbA.max.y + aabbB.min.y) * 0.5f,
                (std::max(aabbA.min.z, aabbB.min.z) + std::min(aabbA.max.z, aabbB.max.z)) * 0.5f
            };
        }
        else
        {
            out.depth  = oz;
            out.normal = { 0.0f, 0.0f, dir.z >= 0.0f ? 1.0f : -1.0f };
            out.point = {
                (std::max(aabbA.min.x, aabbB.min.x) + std::min(aabbA.max.x, aabbB.max.x)) * 0.5f,
                (std::max(aabbA.min.y, aabbB.min.y) + std::min(aabbA.max.y, aabbB.max.y)) * 0.5f,
                out.normal.z > 0.0f ? (aabbA.min.z + aabbB.max.z) * 0.5f
                                    : (aabbA.max.z + aabbB.min.z) * 0.5f
            };
        }

        return true;
    }

    bool PhysicsSolver::TestOBBOBB(const OBBCollider& a, const OBBCollider& b,
                                   ContactPoint& out)
    {
        constexpr float EPS = 1e-6f;

        const math::Vector3 axesA[3] = { a.GetAxis(0), a.GetAxis(1), a.GetAxis(2) };
        const math::Vector3 axesB[3] = { b.GetAxis(0), b.GetAxis(1), b.GetAxis(2) };
        const float extA[3] = { a.m_halfExtents.x, a.m_halfExtents.y, a.m_halfExtents.z };
        const float extB[3] = { b.m_halfExtents.x, b.m_halfExtents.y, b.m_halfExtents.z };

        float minOverlap = std::numeric_limits<float>::max();
        math::Vector3 bestAxis = math::Vector3::UP;

        auto ProjectRadius = [](const math::Vector3 boxAxes[3],
                                const float extents[3],
                                const math::Vector3& axis) -> float
        {
            return extents[0] * std::abs(math::Vector3::Dot(boxAxes[0], axis))
                 + extents[1] * std::abs(math::Vector3::Dot(boxAxes[1], axis))
                 + extents[2] * std::abs(math::Vector3::Dot(boxAxes[2], axis));
        };

        auto TestAxis = [&](math::Vector3 axis) -> bool
        {
            const float lenSq = axis.LengthSq();
            if (lenSq < EPS) return true;

            axis = axis * (1.0f / std::sqrt(lenSq));
            const float centerA = math::Vector3::Dot(a.GetCenter(), axis);
            const float centerB = math::Vector3::Dot(b.GetCenter(), axis);
            const float radiusA = ProjectRadius(axesA, extA, axis);
            const float radiusB = ProjectRadius(axesB, extB, axis);
            const float overlap = radiusA + radiusB - std::abs(centerA - centerB);

            if (overlap <= 0.0f) return false;
            if (overlap < minOverlap)
            {
                minOverlap = overlap;
                bestAxis = axis;
            }
            return true;
        };

        for (int i = 0; i < 3; ++i)
        {
            if (!TestAxis(axesA[i])) return false;
            if (!TestAxis(axesB[i])) return false;
        }

        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
            {
                if (!TestAxis(math::Vector3::Cross(axesA[i], axesB[j]))) return false;
            }
        }

        const math::Vector3 delta = a.GetCenter() - b.GetCenter();
        if (math::Vector3::Dot(bestAxis, delta) < 0.0f)
            bestAxis = -bestAxis;

        out.normal = bestAxis;
        out.depth = minOverlap;
        out.point = (a.SupportPoint(-bestAxis) + b.SupportPoint(bestAxis)) * 0.5f;
        return true;
    }

    bool PhysicsSolver::TestSphereAABB(const SphereCollider& s, const AABBCollider& b,
                                        ContactPoint& out)
    {
        math::Vector3 center = s.GetAABB().Center();
        AABB          aabb   = b.GetAABB();

        math::Vector3 closest = {
            std::max(aabb.min.x, std::min(center.x, aabb.max.x)),
            std::max(aabb.min.y, std::min(center.y, aabb.max.y)),
            std::max(aabb.min.z, std::min(center.z, aabb.max.z))
        };

        math::Vector3 diff = center - closest;
        float         dist = diff.Length();

        if (dist >= s.m_radius) return false;

        if (dist < 1e-6f)
        {
            // 球中心がAABB内部: 最も浅い面に押し出す
            out.normal = math::Vector3::UP;
            out.depth  = s.m_radius;
        }
        else
        {
            out.normal = diff * (1.0f / dist);
            out.depth  = s.m_radius - dist;
        }

        out.point = closest;
        return true;
    }

    bool PhysicsSolver::TestSphereOBB(const SphereCollider& s, const OBBCollider& b,
                                      ContactPoint& out)
    {
        const math::Vector3 center = s.GetAABB().Center();
        const math::Vector3 delta = center - b.GetCenter();
        math::Vector3 closest = b.GetCenter();

        for (int i = 0; i < 3; ++i)
        {
            const math::Vector3 axis = b.GetAxis(i);
            const float extent = i == 0 ? b.m_halfExtents.x : (i == 1 ? b.m_halfExtents.y : b.m_halfExtents.z);
            const float distance = std::clamp(math::Vector3::Dot(delta, axis), -extent, extent);
            closest += axis * distance;
        }

        const math::Vector3 diff = center - closest;
        const float distSq = diff.LengthSq();
        if (distSq >= s.m_radius * s.m_radius) return false;

        const float dist = std::sqrt(distSq);
        if (dist < 1e-6f)
        {
            math::Vector3 bestAxis = b.GetAxis(0);
            float minFaceDistance = b.m_halfExtents.x - std::abs(math::Vector3::Dot(delta, bestAxis));
            for (int i = 1; i < 3; ++i)
            {
                const math::Vector3 axis = b.GetAxis(i);
                const float extent = i == 1 ? b.m_halfExtents.y : b.m_halfExtents.z;
                const float faceDistance = extent - std::abs(math::Vector3::Dot(delta, axis));
                if (faceDistance < minFaceDistance)
                {
                    minFaceDistance = faceDistance;
                    bestAxis = axis * (math::Vector3::Dot(delta, axis) >= 0.0f ? 1.0f : -1.0f);
                }
            }

            out.normal = bestAxis;
            out.depth = s.m_radius + minFaceDistance;
        }
        else
        {
            out.normal = diff * (1.0f / dist);
            out.depth = s.m_radius - dist;
        }

        out.point = closest;
        return true;
    }

    bool PhysicsSolver::TestSphereCapsule(const SphereCollider& s,
                                        const CapsuleCollider& c,
                                        ContactPoint& out)
    {
        const math::Vector3 center = s.GetAABB().Center();
        const math::Vector3 segStart = c.GetSegmentStart();
        const math::Vector3 segEnd = c.GetSegmentEnd();
        const math::Vector3 seg = segEnd - segStart;
        const float segLenSq = seg.LengthSq();
        float t = 0.0f;
        if (segLenSq > 1e-6f)
            t = std::clamp(math::Vector3::Dot(center - segStart, seg) / segLenSq, 0.0f, 1.0f);

        const math::Vector3 closest = segStart + seg * t;
        const math::Vector3 diff = center - closest;
        const float dist = diff.Length();
        const float sumR = s.m_radius + c.m_radius;
        if (dist >= sumR) return false;

        out.normal = dist < 1e-6f ? math::Vector3::UP : diff * (1.0f / dist);
        out.depth = sumR - dist;
        out.point = closest + out.normal * c.m_radius;
        return true;
    }

    bool PhysicsSolver::TestAABBCapsule(const AABBCollider& b,
                                        const CapsuleCollider& c,
                                        ContactPoint& out)
    {
        AABB aabb = b.GetAABB();
        math::Vector3 bestCapsulePoint = c.GetSegmentStart();
        math::Vector3 bestBoxPoint = aabb.Center();
        float bestDistSq = 3.402823466e+38f;

        for (int i = 0; i <= 6; ++i)
        {
            const float t = static_cast<float>(i) / 6.0f;
            const math::Vector3 p = c.GetSegmentStart() + (c.GetSegmentEnd() - c.GetSegmentStart()) * t;
            const math::Vector3 q = {
                std::max(aabb.min.x, std::min(p.x, aabb.max.x)),
                std::max(aabb.min.y, std::min(p.y, aabb.max.y)),
                std::max(aabb.min.z, std::min(p.z, aabb.max.z))
            };
            const float distSq = (p - q).LengthSq();
            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                bestCapsulePoint = p;
                bestBoxPoint = q;
            }
        }

        if (bestDistSq >= c.m_radius * c.m_radius) return false;

        const float dist = std::sqrt(bestDistSq);
        if (dist < 1e-6f)
        {
            const float toMinX = bestCapsulePoint.x - aabb.min.x;
            const float toMaxX = aabb.max.x - bestCapsulePoint.x;
            const float toMinY = bestCapsulePoint.y - aabb.min.y;
            const float toMaxY = aabb.max.y - bestCapsulePoint.y;
            const float toMinZ = bestCapsulePoint.z - aabb.min.z;
            const float toMaxZ = aabb.max.z - bestCapsulePoint.z;

            float faceDistance = toMinX;
            math::Vector3 faceOut = -math::Vector3::RIGHT;
            auto SelectFace = [&](float distance, const math::Vector3& outward)
            {
                if (distance < faceDistance)
                {
                    faceDistance = distance;
                    faceOut = outward;
                }
            };
            SelectFace(toMaxX, math::Vector3::RIGHT);
            SelectFace(toMinY, -math::Vector3::UP);
            SelectFace(toMaxY, math::Vector3::UP);
            SelectFace(toMinZ, -math::Vector3::FORWARD);
            SelectFace(toMaxZ, math::Vector3::FORWARD);

            // 接触法線は「B(カプセル)→A(ボックス)」方向に統一。
            // カプセル軸がボックス内部に埋まっている場合、最近接面の外向きノーマルの反対方向へ押し出す。
            out.normal = -faceOut;
            out.depth = c.m_radius + faceDistance;
            bestBoxPoint = bestCapsulePoint + faceOut * faceDistance;
        }
        else
        {
            out.normal = (bestBoxPoint - bestCapsulePoint) * (1.0f / dist);
            out.depth = c.m_radius - dist;
        }

        out.point = bestBoxPoint;
        return true;
    }

    bool PhysicsSolver::TestCapsuleCapsule(const CapsuleCollider& a,
                                           const CapsuleCollider& b,
                                           ContactPoint& out)
    {
        math::Vector3 bestA;
        math::Vector3 bestB;
        ClosestPointsOnSegments(a.GetSegmentStart(), a.GetSegmentEnd(),
                                b.GetSegmentStart(), b.GetSegmentEnd(),
                                bestA, bestB);

        const float sumR = a.m_radius + b.m_radius;
        const float bestDistSq = (bestA - bestB).LengthSq();
        if (bestDistSq >= sumR * sumR) return false;

        const float dist = std::sqrt(bestDistSq);
        out.normal = dist < 1e-6f ? math::Vector3::UP : (bestA - bestB) * (1.0f / dist);
        out.depth = sumR - dist;
        out.point = (bestA + bestB) * 0.5f;
        return true;
    }

    // ------------------------------------------------------- Triangle テスト関数

    namespace
    {
        // 三角形上の最近傍点を Voronoi 領域分類で求める
        math::Vector3 ClosestPointOnTriangle(const math::Vector3& p, const Triangle& tri)
        {
            const math::Vector3& a = tri.v[0];
            const math::Vector3& b = tri.v[1];
            const math::Vector3& c = tri.v[2];

            const math::Vector3 ab = b - a;
            const math::Vector3 ac = c - a;
            const math::Vector3 ap = p - a;

            const float d1 = math::Vector3::Dot(ab, ap);
            const float d2 = math::Vector3::Dot(ac, ap);
            if (d1 <= 0.0f && d2 <= 0.0f) return a;

            const math::Vector3 bp = p - b;
            const float d3 = math::Vector3::Dot(ab, bp);
            const float d4 = math::Vector3::Dot(ac, bp);
            if (d3 >= 0.0f && d4 <= d3) return b;

            const float vc = d1 * d4 - d3 * d2;
            if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
            {
                const float v = d1 / (d1 - d3);
                return a + ab * v;
            }

            const math::Vector3 cp2 = p - c;
            const float d5 = math::Vector3::Dot(ab, cp2);
            const float d6 = math::Vector3::Dot(ac, cp2);
            if (d6 >= 0.0f && d5 <= d6) return c;

            const float vb = d5 * d2 - d1 * d6;
            if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
            {
                const float w = d2 / (d2 - d6);
                return a + ac * w;
            }

            const float va = d3 * d6 - d5 * d4;
            if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
            {
                const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
                return b + (c - b) * w;
            }

            const float denom = 1.0f / (va + vb + vc);
            const float v = vb * denom;
            const float w = vc * denom;
            return a + ab * v + ac * w;
        }

        // 点が三角形の内側または辺上にあるかを符号付き面積で判定する。
        // WHY: 線分と三角形面の交差候補を、三角形の外側へ誤って採用しないため。
        bool IsPointOnTriangle(const math::Vector3& p, const Triangle& tri)
        {
            constexpr float EPS = 1e-5f;
            const math::Vector3& a = tri.v[0];
            const math::Vector3& b = tri.v[1];
            const math::Vector3& c = tri.v[2];
            const math::Vector3& n = tri.normal;

            const float edge0 = math::Vector3::Dot(math::Vector3::Cross(b - a, p - a), n);
            const float edge1 = math::Vector3::Dot(math::Vector3::Cross(c - b, p - b), n);
            const float edge2 = math::Vector3::Dot(math::Vector3::Cross(a - c, p - c), n);
            return edge0 >= -EPS && edge1 >= -EPS && edge2 >= -EPS;
        }

        // 線分と三角形の最近接点を、端点サンプリングなしで求める。
        // WHAT: 線分の端点と三角形、線分と三角形の各辺、線分と三角形面の
        //       内部交差を全候補として比較する。
        // WHY: 端点・中点だけの近似では、傾斜面や三角形境界で最近接点と法線が
        //      移動方向に応じて跳ぶため、カプセルの前後振動を誘発する。
        float ClosestPointsOnSegmentTriangle(const math::Vector3& segS,
                                              const math::Vector3& segE,
                                              const Triangle&       tri,
                                              math::Vector3&        outSegment,
                                              math::Vector3&        outTriangle)
        {
            float bestDistSq = std::numeric_limits<float>::max();
            outSegment = segS;
            outTriangle = tri.v[0];

            auto Consider = [&](const math::Vector3& segmentPoint,
                                const math::Vector3& trianglePoint)
            {
                const float distanceSq = (segmentPoint - trianglePoint).LengthSq();
                if (distanceSq < bestDistSq)
                {
                    bestDistSq = distanceSq;
                    outSegment = segmentPoint;
                    outTriangle = trianglePoint;
                }
            };

            // 線分の端点と三角形の最近接点。
            Consider(segS, ClosestPointOnTriangle(segS, tri));
            Consider(segE, ClosestPointOnTriangle(segE, tri));

            // 線分と三角形の各辺の最近接点。
            for (int edge = 0; edge < 3; ++edge)
            {
                math::Vector3 segmentPoint;
                math::Vector3 edgePoint;
                ClosestPointsOnSegments(segS, segE,
                                         tri.v[edge], tri.v[(edge + 1) % 3],
                                         segmentPoint, edgePoint);
                Consider(segmentPoint, edgePoint);
            }

            // 線分が三角形面を横切り、交点が三角形内にある場合は距離 0。
            const float signedStart = math::Vector3::Dot(segS - tri.v[0], tri.normal);
            const float signedEnd   = math::Vector3::Dot(segE - tri.v[0], tri.normal);
            const float planeDelta  = signedStart - signedEnd;
            if (std::abs(planeDelta) > 1e-6f)
            {
                const float planeT = signedStart / planeDelta;
                if (planeT >= 0.0f && planeT <= 1.0f)
                {
                    const math::Vector3 intersection = segS + (segE - segS) * planeT;
                    if (IsPointOnTriangle(intersection, tri))
                        Consider(intersection, intersection);
                }
            }
            else if (std::abs(signedStart) <= 1e-5f &&
                     (IsPointOnTriangle(segS, tri) || IsPointOnTriangle(segE, tri)))
            {
                // 線分全体が面とほぼ平行かつ同一平面にある退化ケース。
                Consider(IsPointOnTriangle(segS, tri) ? segS : segE,
                         IsPointOnTriangle(segS, tri) ? segS : segE);
            }

            return bestDistSq;
        }
    } // anonymous namespace

    bool PhysicsSolver::TestSphereTriangle(const SphereCollider& s,
                                            const Triangle& tri,
                                            ContactPoint& out)
    {
        const math::Vector3 center  = s.GetAABB().Center();
        const math::Vector3 closest = ClosestPointOnTriangle(center, tri);
        const math::Vector3 diff    = center - closest;
        const float         distSq  = diff.LengthSq();

        if (distSq >= s.m_radius * s.m_radius) return false;

        const float dist = std::sqrt(distSq);

        if (dist < 1e-6f)
        {
            // 球中心が三角形面上またはほぼ一致: 面法線を使用
            // 双面: 球中心が裏側なら法線を反転
            math::Vector3 n = tri.normal;
            if (math::Vector3::Dot(n, center - tri.v[0]) < 0.0f) n = -n;
            out.normal = n;
            out.depth  = s.m_radius;
        }
        else
        {
            out.normal = diff * (1.0f / dist);
            out.depth  = s.m_radius - dist;
        }

        out.point = closest;
        return true;
    }

    bool PhysicsSolver::TestAABBTriangle(const AABBCollider& b,
                                          const Triangle& tri,
                                          ContactPoint& out)
    {
        // SAT (Separating Axis Theorem): 13 軸をテストする
        // 軸: 3 面法線 (AABB 軸) + 3 辺 × 3 AABB 軸 = 9 + 1 三角形法線 = 13
        const AABB&         aabb = b.GetAABB();
        const math::Vector3 center = aabb.Center();
        const math::Vector3 half   = aabb.Extents();

        // 三角形頂点を AABB 中心相対座標に変換
        math::Vector3 v[3];
        for (int i = 0; i < 3; ++i) v[i] = tri.v[i] - center;

        math::Vector3 e[3];
        e[0] = v[1] - v[0];
        e[1] = v[2] - v[1];
        e[2] = v[0] - v[2];

        // AABB の 3 軸 (X, Y, Z)
        const math::Vector3 aabbAxes[3] = {
            {1.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, 1.0f}
        };

        float minOverlap = std::numeric_limits<float>::max();
        math::Vector3 bestAxis;

        constexpr float EPS = 1e-6f;

        auto TestAxis = [&](math::Vector3 axis) -> bool
        {
            const float lenSq = axis.LengthSq();
            if (lenSq < EPS) return true; // 縮退軸はスキップ (分離なし扱い)
            axis = axis * (1.0f / std::sqrt(lenSq));

            // AABB の投影半幅
            const float r = half.x * std::abs(axis.x)
                          + half.y * std::abs(axis.y)
                          + half.z * std::abs(axis.z);

            // 三角形の投影範囲
            const float p0 = math::Vector3::Dot(v[0], axis);
            const float p1 = math::Vector3::Dot(v[1], axis);
            const float p2 = math::Vector3::Dot(v[2], axis);
            const float triMin = std::min({p0, p1, p2});
            const float triMax = std::max({p0, p1, p2});

            const float overlap = std::min(r - triMin, triMax + r);
            if (overlap <= 0.0f) return false; // 分離軸発見

            if (overlap < minOverlap)
            {
                minOverlap = overlap;
                bestAxis   = axis;
            }
            return true;
        };

        // 3 AABB 軸
        for (int i = 0; i < 3; ++i)
            if (!TestAxis(aabbAxes[i])) return false;

        // 三角形法線
        if (!TestAxis(tri.normal)) return false;

        // 9 クロス積軸 (edgeI × aabbAxisJ)
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                if (!TestAxis(math::Vector3::Cross(e[i], aabbAxes[j]))) return false;

        // 全軸で重なり → 衝突
        // 法線方向: AABB 中心 → 三角形 重心 に対して bestAxis を合わせる
        const math::Vector3 triCentroid = (v[0] + v[1] + v[2]) / 3.0f;
        if (math::Vector3::Dot(bestAxis, triCentroid) < 0.0f)
            bestAxis = -bestAxis;

        out.normal = -bestAxis; // AABB → 三角形 方向
        out.depth  = minOverlap;
        out.point  = center + bestAxis * (half.x + half.y + half.z) / 3.0f; // 近似接触点
        return true;
    }

    bool PhysicsSolver::TestCapsuleTriangle(const CapsuleCollider& c,
                                              const Triangle& tri,
                                              ContactPoint& out)
    {
        const math::Vector3 segS = c.GetSegmentStart();
        const math::Vector3 segE = c.GetSegmentEnd();

        // WHY: Terrain MeshCollider では BroadPhase/BVH の AABB が重なっても、
        //      実際にはカプセルが三角形面から半径以上離れているケースが多い。
        //      三角形平面からの符号付き距離だけで届かないと分かる場合は、
        //      線分-線分最近傍や ClosestPointOnTriangle の重い計算に進まない。
        const float signedDistS = math::Vector3::Dot(segS - tri.v[0], tri.normal);
        const float signedDistE = math::Vector3::Dot(segE - tri.v[0], tri.normal);
        if ((signedDistS > c.m_radius && signedDistE > c.m_radius) ||
            (signedDistS < -c.m_radius && signedDistE < -c.m_radius))
        {
            return false;
        }

        math::Vector3 bestCapsule;
        math::Vector3 bestTriPt;
        const float bestDistSq = ClosestPointsOnSegmentTriangle(
            segS, segE, tri, bestCapsule, bestTriPt);

        if (bestDistSq >= c.m_radius * c.m_radius) return false;

        const float dist = std::sqrt(bestDistSq);
        if (dist < 1e-6f)
        {
            math::Vector3 n = tri.normal;
            if (math::Vector3::Dot(n, bestCapsule - tri.v[0]) < 0.0f) n = -n;
            out.normal = n;
            out.depth  = c.m_radius;
        }
        else
        {
            out.normal = (bestCapsule - bestTriPt) * (1.0f / dist);
            out.depth  = c.m_radius - dist;
        }

        out.point = bestTriPt;
        return true;
    }

    // ---------------------------------------------------- ConvexHull テスト関数

    namespace
    {
        // 各形状の GJK サポート関数
        math::Vector3 SupportSphere(const void* shape, const math::Vector3& dir)
        {
            const auto* s = static_cast<const SphereCollider*>(shape);
            const math::Vector3 center = s->GetAABB().Center();
            return center + dir.Normalized() * s->m_radius;
        }

        math::Vector3 SupportAABB(const void* shape, const math::Vector3& dir)
        {
            const AABB aabb = static_cast<const AABBCollider*>(shape)->GetAABB();
            return {
                dir.x >= 0.0f ? aabb.max.x : aabb.min.x,
                dir.y >= 0.0f ? aabb.max.y : aabb.min.y,
                dir.z >= 0.0f ? aabb.max.z : aabb.min.z
            };
        }

        math::Vector3 SupportOBB(const void* shape, const math::Vector3& dir)
        {
            return static_cast<const OBBCollider*>(shape)->SupportPoint(dir);
        }

        math::Vector3 SupportCapsule(const void* shape, const math::Vector3& dir)
        {
            const auto* c = static_cast<const CapsuleCollider*>(shape);
            const math::Vector3 s = c->GetSegmentStart();
            const math::Vector3 e = c->GetSegmentEnd();
            // セグメント上で dir と最も内積が大きい点 + radius
            const math::Vector3 best = (math::Vector3::Dot(s, dir) >= math::Vector3::Dot(e, dir))
                                        ? s : e;
            return best + dir.Normalized() * c->m_radius;
        }

        math::Vector3 SupportTriangle(const void* shape, const math::Vector3& dir)
        {
            const auto* tri = static_cast<const Triangle*>(shape);
            const float d0 = math::Vector3::Dot(tri->v[0], dir);
            const float d1 = math::Vector3::Dot(tri->v[1], dir);
            const float d2 = math::Vector3::Dot(tri->v[2], dir);
            if (d0 >= d1 && d0 >= d2) return tri->v[0];
            return d1 >= d2 ? tri->v[1] : tri->v[2];
        }

        // GJK (Simplex 付き) + EPA から ContactPoint を構築するヘルパー
        bool GJKEPAToContact(const void* shapeA, SupportFn fnA,
                             const void* shapeB, SupportFn fnB,
                             ContactPoint& out)
        {
            const GJKResult gjk = GJK_Intersect(shapeA, fnA, shapeB, fnB);
            if (!gjk.intersects) return false;

            const EPAResult epa = EPA_GetContactInfo(shapeA, fnA, shapeB, fnB, gjk.simplex);
            if (!epa.valid) return false;

            out.normal = epa.normal;
            out.depth  = epa.depth;
            out.point  = (epa.contactA + epa.contactB) * 0.5f;
            return true;
        }
    } // anonymous namespace

    bool PhysicsSolver::TestAABBOBB(const AABBCollider& a,
                                    const OBBCollider& b,
                                    ContactPoint& out)
    {
        return GJKEPAToContact(&a, SupportAABB, &b, SupportOBB, out);
    }

    bool PhysicsSolver::TestOBBCapsule(const OBBCollider& b,
                                       const CapsuleCollider& c,
                                       ContactPoint& out)
    {
        const math::Vector3 axes[3] = { b.GetAxis(0), b.GetAxis(1), b.GetAxis(2) };
        const float extents[3] = { b.m_halfExtents.x, b.m_halfExtents.y, b.m_halfExtents.z };
        const math::Vector3 segment = c.GetSegmentEnd() - c.GetSegmentStart();

        math::Vector3 bestCapsulePoint = c.GetSegmentStart();
        math::Vector3 bestBoxPoint = b.GetCenter();
        math::Vector3 bestLocalPoint = math::Vector3::ZERO;
        float bestDistSq = std::numeric_limits<float>::max();

        for (int i = 0; i <= 8; ++i)
        {
            const float t = static_cast<float>(i) / 8.0f;
            const math::Vector3 capsulePoint = c.GetSegmentStart() + segment * t;
            const math::Vector3 delta = capsulePoint - b.GetCenter();
            const math::Vector3 localPoint = {
                math::Vector3::Dot(delta, axes[0]),
                math::Vector3::Dot(delta, axes[1]),
                math::Vector3::Dot(delta, axes[2])
            };
            const math::Vector3 clampedLocal = {
                std::clamp(localPoint.x, -b.m_halfExtents.x, b.m_halfExtents.x),
                std::clamp(localPoint.y, -b.m_halfExtents.y, b.m_halfExtents.y),
                std::clamp(localPoint.z, -b.m_halfExtents.z, b.m_halfExtents.z)
            };
            const math::Vector3 boxPoint =
                b.GetCenter() +
                axes[0] * clampedLocal.x +
                axes[1] * clampedLocal.y +
                axes[2] * clampedLocal.z;
            const float distSq = (capsulePoint - boxPoint).LengthSq();
            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                bestCapsulePoint = capsulePoint;
                bestBoxPoint = boxPoint;
                bestLocalPoint = localPoint;
            }
        }

        if (bestDistSq >= c.m_radius * c.m_radius) return false;

        const float dist = std::sqrt(bestDistSq);
        if (dist < 1e-6f)
        {
            float faceDistance = extents[0] - std::abs(bestLocalPoint.x);
            int faceAxis = 0;
            float faceSign = bestLocalPoint.x >= 0.0f ? 1.0f : -1.0f;

            for (int axis = 1; axis < 3; ++axis)
            {
                const float localValue =
                    axis == 1 ? bestLocalPoint.y : bestLocalPoint.z;
                const float distance = extents[axis] - std::abs(localValue);
                if (distance < faceDistance)
                {
                    faceDistance = distance;
                    faceAxis = axis;
                    faceSign = localValue >= 0.0f ? 1.0f : -1.0f;
                }
            }

            const math::Vector3 faceOut = axes[faceAxis] * faceSign;
            // 接触法線は「B(カプセル)→A(OBB)」方向に統一。
            // OBB 内部では最近接面の外向きノーマルの反対方向へカプセルを押し出す。
            out.normal = -faceOut;
            out.depth = c.m_radius + faceDistance;
            bestBoxPoint = bestCapsulePoint + faceOut * faceDistance;
        }
        else
        {
            out.normal = (bestBoxPoint - bestCapsulePoint) * (1.0f / dist);
            out.depth = c.m_radius - dist;
        }

        out.point = bestBoxPoint;
        return true;
    }

    bool PhysicsSolver::TestConvexConvex(const ConvexHullCollider& a,
                                          const ConvexHullCollider& b,
                                          ContactPoint& out)
    {
        return GJKEPAToContact(&a, ConvexHullCollider::SupportFnImpl,
                               &b, ConvexHullCollider::SupportFnImpl, out);
    }

    bool PhysicsSolver::TestSphereConvex(const SphereCollider& s,
                                          const ConvexHullCollider& hull,
                                          ContactPoint& out)
    {
        return GJKEPAToContact(&s,    SupportSphere,
                               &hull, ConvexHullCollider::SupportFnImpl, out);
    }

    bool PhysicsSolver::TestAABBConvex(const AABBCollider& b,
                                        const ConvexHullCollider& hull,
                                        ContactPoint& out)
    {
        return GJKEPAToContact(&b,    SupportAABB,
                               &hull, ConvexHullCollider::SupportFnImpl, out);
    }

    bool PhysicsSolver::TestOBBConvex(const OBBCollider& b,
                                      const ConvexHullCollider& hull,
                                      ContactPoint& out)
    {
        return GJKEPAToContact(&b,    SupportOBB,
                               &hull, ConvexHullCollider::SupportFnImpl, out);
    }

    bool PhysicsSolver::TestCapsuleConvex(const CapsuleCollider& c,
                                           const ConvexHullCollider& hull,
                                           ContactPoint& out)
    {
        return GJKEPAToContact(&c,    SupportCapsule,
                               &hull, ConvexHullCollider::SupportFnImpl, out);
    }

    // ------------------------------------------------------ Cylinder テスト関数

    bool PhysicsSolver::TestSphereCylinder(const SphereCollider& s,
                                            const CylinderCollider& c,
                                            ContactPoint& out)
    {
        const math::Vector3 center  = s.GetAABB().Center();
        const math::Vector3 closest = c.ClosestPoint(center);
        const math::Vector3 diff    = center - closest;
        const float         distSq  = diff.LengthSq();
        if (distSq >= s.m_radius * s.m_radius) return false;

        const float dist = std::sqrt(distSq);
        if (dist < 1e-6f)
        {
            // 球中心が円柱の内部にあり方向が決まらない。側面と円板のうち脱出が浅い方へ押し出す。
            const math::Vector3 delta     = center - c.GetCenter();
            const float         axial     = math::Vector3::Dot(delta, c.GetAxis());
            const math::Vector3 radial    = delta - c.GetAxis() * axial;
            const float         radialLen = radial.Length();

            const float sideDistance = c.m_radius - radialLen;
            const float capDistance  = c.m_halfHeight - std::abs(axial);

            // 中心軸上に完全に乗ると側面方向が定まらないため、その場合は必ず円板側へ逃がす。
            if (radialLen > 1e-6f && sideDistance <= capDistance)
            {
                out.normal = radial * (1.0f / radialLen);
                out.depth  = s.m_radius + sideDistance;
            }
            else
            {
                out.normal = c.GetAxis() * (axial >= 0.0f ? 1.0f : -1.0f);
                out.depth  = s.m_radius + capDistance;
            }
        }
        else
        {
            out.normal = diff * (1.0f / dist);
            out.depth  = s.m_radius - dist;
        }

        out.point = closest;
        return true;
    }

    bool PhysicsSolver::TestAABBCylinder(const AABBCollider& b,
                                          const CylinderCollider& c,
                                          ContactPoint& out)
    {
        return GJKEPAToContact(&b, SupportAABB,
                               &c, CylinderCollider::SupportFnImpl, out);
    }

    bool PhysicsSolver::TestOBBCylinder(const OBBCollider& b,
                                         const CylinderCollider& c,
                                         ContactPoint& out)
    {
        return GJKEPAToContact(&b, SupportOBB,
                               &c, CylinderCollider::SupportFnImpl, out);
    }

    bool PhysicsSolver::TestCapsuleCylinder(const CapsuleCollider& a,
                                             const CylinderCollider& c,
                                             ContactPoint& out)
    {
        return GJKEPAToContact(&a, SupportCapsule,
                               &c, CylinderCollider::SupportFnImpl, out);
    }

    bool PhysicsSolver::TestCylinderCylinder(const CylinderCollider& a,
                                              const CylinderCollider& b,
                                              ContactPoint& out)
    {
        return GJKEPAToContact(&a, CylinderCollider::SupportFnImpl,
                               &b, CylinderCollider::SupportFnImpl, out);
    }

    bool PhysicsSolver::TestCylinderConvex(const CylinderCollider& c,
                                            const ConvexHullCollider& hull,
                                            ContactPoint& out)
    {
        return GJKEPAToContact(&c,    CylinderCollider::SupportFnImpl,
                               &hull, ConvexHullCollider::SupportFnImpl, out);
    }

    bool PhysicsSolver::TestSphereTriangleMesh(const SphereCollider& s,
                                                const TriangleMeshCollider& mesh,
                                                ContactPoint& out)
    {
        // BVH を使って候補三角形を絞り込み、最も深い接触点を採用する
        AABB queryAABB;
        const math::Vector3 center = s.GetAABB().Center();
        queryAABB.min = center - math::Vector3{s.m_radius, s.m_radius, s.m_radius};
        queryAABB.max = center + math::Vector3{s.m_radius, s.m_radius, s.m_radius};

        bool    found   = false;
        float   maxDepth = -1.0f;
        ContactPoint best;

        mesh.GetBVH().Query(queryAABB, [&](const Triangle& tri)
        {
            ContactPoint cp;
            if (TestSphereTriangle(s, tri, cp) && cp.depth > maxDepth)
            {
                maxDepth = cp.depth;
                best     = cp;
                found    = true;
            }
        });

        if (found) out = best;
        return found;
    }

    bool PhysicsSolver::TestAABBTriangleMesh(const AABBCollider& b,
                                              const TriangleMeshCollider& mesh,
                                              ContactPoint& out)
    {
        bool    found    = false;
        float   maxDepth = -1.0f;
        ContactPoint best;

        mesh.GetBVH().Query(b.GetAABB(), [&](const Triangle& tri)
        {
            ContactPoint cp;
            if (TestAABBTriangle(b, tri, cp) && cp.depth > maxDepth)
            {
                maxDepth = cp.depth;
                best     = cp;
                found    = true;
            }
        });

        if (found) out = best;
        return found;
    }

    bool PhysicsSolver::TestCapsuleTriangleMesh(const CapsuleCollider& c,
                                                  const TriangleMeshCollider& mesh,
                                                  ContactPoint& out)
    {
        // カプセルの AABB で BVH をクエリ
        const AABB queryAABB = c.GetAABB();
        std::vector<ContactPoint> candidates;
        candidates.reserve(8);

        mesh.GetBVH().Query(queryAABB, [&](const Triangle& tri)
        {
            ContactPoint cp;
            if (TestCapsuleTriangle(c, tri, cp))
                candidates.push_back(cp);
        });

        if (candidates.empty()) return false;

        // 最深接触を基準に、同じ接触パッチに属する三角形だけをマージする。
        // WHY: MeshCollider は三角形ごとに法線を持つため、境界を跨ぐたびに
        //      最深の1枚を選ぶと接触法線・摩擦方向がフレーム単位で切り替わる。
        //      法線差が大きい面まで平均すると鋭い角を丸めてしまうため、
        //      近い接触点かつ近い法線の候補に限定する。
        auto deepest = std::max_element(candidates.begin(), candidates.end(),
            [](const ContactPoint& lhs, const ContactPoint& rhs) {
                return lhs.depth < rhs.depth;
            });
        const float maxDepth = deepest->depth;
        const math::Vector3 referencePoint = deepest->point;
        const math::Vector3 referenceNormal = deepest->normal;
        const float mergeRadius = std::max(0.05f, c.m_radius * 0.5f);
        const float mergeRadiusSq = mergeRadius * mergeRadius;
        constexpr float DEPTH_TOLERANCE = 0.02f;
        constexpr float NORMAL_MERGE_DOT = 0.96f;

        math::Vector3 normalSum = math::Vector3::ZERO;
        math::Vector3 pointSum = math::Vector3::ZERO;
        float weightSum = 0.0f;
        int mergedCount = 0;
        for (const ContactPoint& candidate : candidates)
        {
            if (maxDepth - candidate.depth > DEPTH_TOLERANCE) continue;
            if ((candidate.point - referencePoint).LengthSq() > mergeRadiusSq) continue;
            if (math::Vector3::Dot(candidate.normal, referenceNormal) < NORMAL_MERGE_DOT) continue;

            // 深い接触ほど大きく反映し、接触点・法線の急な切り替わりを抑える。
            const float weight = std::max(candidate.depth, 1e-4f);
            normalSum += candidate.normal * weight;
            pointSum += candidate.point * weight;
            weightSum += weight;
            ++mergedCount;
        }

        out = *deepest;
        if (mergedCount > 1 && weightSum > 0.0f && normalSum.LengthSq() > 1e-8f)
        {
            out.normal = normalSum.Normalized();
            out.point = pointSum * (1.0f / weightSum);
            // 深度は最大値を残す。平均すると位置補正が不足して再び貫通するため。
            out.depth = maxDepth;
        }
        return true;
    }

    bool PhysicsSolver::TestOBBTriangleMesh(const OBBCollider& b,
                                             const TriangleMeshCollider& mesh,
                                             ContactPoint& out)
    {
        const math::Quaternion invRot = b.GetRotation().Inverse();
        AABBCollider localBox(b.m_halfExtents);
        localBox.Update(math::Vector3::ZERO, math::Quaternion::Identity());

        bool found = false;
        float maxDepth = -1.0f;
        ContactPoint best;

        mesh.GetBVH().Query(b.GetAABB(), [&](const Triangle& tri)
        {
            Triangle localTri;
            for (int i = 0; i < 3; ++i)
                localTri.v[i] = invRot * (tri.v[i] - b.GetCenter());
            localTri.normal = (invRot * tri.normal).Normalized();
            localTri.index = tri.index;

            ContactPoint cp;
            if (TestAABBTriangle(localBox, localTri, cp) && cp.depth > maxDepth)
            {
                cp.normal = (b.GetRotation() * cp.normal).Normalized();
                cp.point = b.GetCenter() + b.GetRotation() * cp.point;
                maxDepth = cp.depth;
                best = cp;
                found = true;
            }
        });

        if (found) out = best;
        return found;
    }

    bool PhysicsSolver::TestConvexHullTriangleMesh(const ConvexHullCollider& hull,
                                                    const TriangleMeshCollider& mesh,
                                                    ContactPoint& out)
    {
        bool found = false;
        float maxDepth = -1.0f;
        ContactPoint best;

        mesh.GetBVH().Query(hull.GetAABB(), [&](const Triangle& tri)
        {
            ContactPoint cp;
            if (GJKEPAToContact(&hull, ConvexHullCollider::SupportFnImpl,
                                &tri, SupportTriangle, cp) && cp.depth > maxDepth)
            {
                maxDepth = cp.depth;
                best = cp;
                found = true;
            }
        });

        if (found) out = best;
        return found;
    }

    bool PhysicsSolver::TestCylinderTriangleMesh(const CylinderCollider& c,
                                                  const TriangleMeshCollider& mesh,
                                                  ContactPoint& out)
    {
        bool found = false;
        float maxDepth = -1.0f;
        ContactPoint best;

        mesh.GetBVH().Query(c.GetAABB(), [&](const Triangle& tri)
        {
            ContactPoint cp;
            if (GJKEPAToContact(&c, CylinderCollider::SupportFnImpl,
                                &tri, SupportTriangle, cp) && cp.depth > maxDepth)
            {
                maxDepth = cp.depth;
                best = cp;
                found = true;
            }
        });

        if (found) out = best;
        return found;
    }

    // ─── HeightFieldCollider 用テスト関数 ───────────────────────────────────────
    // WHY: HeightFieldCollider は内部 BVH を持ち TriangleMeshCollider と同一アルゴリズムで
    //      衝突判定できる。型が異なるだけで実装は BVH Query に委譲する点で同一。

    bool PhysicsSolver::TestSphereHeightField(const SphereCollider& s,
                                               const HeightFieldCollider& hf,
                                               ContactPoint& out)
    {
        AABB queryAABB;
        const math::Vector3 center = s.GetAABB().Center();
        queryAABB.min = center - math::Vector3{ s.m_radius, s.m_radius, s.m_radius };
        queryAABB.max = center + math::Vector3{ s.m_radius, s.m_radius, s.m_radius };

        bool found = false;
        float maxDepth = -1.0f;
        ContactPoint best;
        hf.GetBVH().Query(queryAABB, [&](const Triangle& tri) {
            ContactPoint cp;
            if (TestSphereTriangle(s, tri, cp) && cp.depth > maxDepth) {
                maxDepth = cp.depth; best = cp; found = true;
            }
        });
        if (found) out = best;
        return found;
    }

    bool PhysicsSolver::TestAABBHeightField(const AABBCollider& b,
                                             const HeightFieldCollider& hf,
                                             ContactPoint& out)
    {
        bool found = false;
        float maxDepth = -1.0f;
        ContactPoint best;
        hf.GetBVH().Query(b.GetAABB(), [&](const Triangle& tri) {
            ContactPoint cp;
            if (TestAABBTriangle(b, tri, cp) && cp.depth > maxDepth) {
                maxDepth = cp.depth; best = cp; found = true;
            }
        });
        if (found) out = best;
        return found;
    }

    bool PhysicsSolver::TestCapsuleHeightField(const CapsuleCollider& c,
                                                const HeightFieldCollider& hf,
                                                ContactPoint& out)
    {
        bool found = false;
        float maxDepth = -1.0f;
        ContactPoint best;
        hf.GetBVH().Query(c.GetAABB(), [&](const Triangle& tri) {
            ContactPoint cp;
            if (TestCapsuleTriangle(c, tri, cp) && cp.depth > maxDepth) {
                maxDepth = cp.depth; best = cp; found = true;
            }
        });
        if (found) out = best;
        return found;
    }

    bool PhysicsSolver::TestOBBHeightField(const OBBCollider& b,
                                            const HeightFieldCollider& hf,
                                            ContactPoint& out)
    {
        const math::Quaternion invRot = b.GetRotation().Inverse();
        AABBCollider localBox(b.m_halfExtents);
        localBox.Update(math::Vector3::ZERO, math::Quaternion::Identity());

        bool found = false;
        float maxDepth = -1.0f;
        ContactPoint best;
        hf.GetBVH().Query(b.GetAABB(), [&](const Triangle& tri) {
            Triangle localTri;
            for (int i = 0; i < 3; ++i)
                localTri.v[i] = invRot * (tri.v[i] - b.GetCenter());
            localTri.normal = (invRot * tri.normal).Normalized();
            localTri.index = tri.index;

            ContactPoint cp;
            if (TestAABBTriangle(localBox, localTri, cp) && cp.depth > maxDepth) {
                cp.normal = (b.GetRotation() * cp.normal).Normalized();
                cp.point  = b.GetCenter() + b.GetRotation() * cp.point;
                maxDepth = cp.depth; best = cp; found = true;
            }
        });
        if (found) out = best;
        return found;
    }

    bool PhysicsSolver::TestConvexHullHeightField(const ConvexHullCollider& hull,
                                                   const HeightFieldCollider& hf,
                                                   ContactPoint& out)
    {
        bool found = false;
        float maxDepth = -1.0f;
        ContactPoint best;
        hf.GetBVH().Query(hull.GetAABB(), [&](const Triangle& tri) {
            ContactPoint cp;
            if (GJKEPAToContact(&hull, ConvexHullCollider::SupportFnImpl,
                                &tri, SupportTriangle, cp) && cp.depth > maxDepth) {
                maxDepth = cp.depth; best = cp; found = true;
            }
        });
        if (found) out = best;
        return found;
    }

    bool PhysicsSolver::TestCylinderHeightField(const CylinderCollider& c,
                                                 const HeightFieldCollider& hf,
                                                 ContactPoint& out)
    {
        bool found = false;
        float maxDepth = -1.0f;
        ContactPoint best;
        hf.GetBVH().Query(c.GetAABB(), [&](const Triangle& tri) {
            ContactPoint cp;
            if (GJKEPAToContact(&c, CylinderCollider::SupportFnImpl,
                                &tri, SupportTriangle, cp) && cp.depth > maxDepth) {
                maxDepth = cp.depth; best = cp; found = true;
            }
        });
        if (found) out = best;
        return found;
    }

} // namespace fbzz::physics
