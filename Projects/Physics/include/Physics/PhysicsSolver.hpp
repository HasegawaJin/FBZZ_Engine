// FBZZ Engine
// PhysicsSolver.hpp | fbzz::physics
// 衝突検出 (Broad/Narrow フェーズ) と衝突解決 (PGS インパルスベース)
#pragma once
#include <functional>
#include <Physics/ContactPoint.hpp>
#include <Physics/CollisionPair.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/EPA.hpp>
#include <Physics/RigidBody.hpp>
#include <vector>
#include <memory>

namespace fbzz::physics
{

    class PhysicsSolver
    {
    public:
        // AABB の重なりだけを見て NarrowPhase 対象を絞る。レイヤー行列はここで適用する。
        void BroadPhase(const std::vector<ColliderInstance>& colliders,
                        std::vector<CollisionPair>& outPairs,
                        const std::function<bool(int, int)>& layerFilter);

        // 形状の組み合わせごとに詳細判定し、ContactPoint を生成する。
        void NarrowPhase(const std::vector<CollisionPair>& pairs,
                        std::vector<ContactPoint>& outContacts);

        // PGS ベースの衝突解決 (Warm Starting 済みの蓄積インパルスを引き継ぐ)
        void Resolve(std::vector<ContactPoint>& contacts);

    private:
        bool TestSphereSphere(const SphereCollider& a, const SphereCollider& b,
                            ContactPoint& out);
        bool TestAABBAABB    (const AABBCollider&   a, const AABBCollider&   b,
                            ContactPoint& out);
        bool TestSphereAABB  (const SphereCollider& s, const AABBCollider&   b,
                            ContactPoint& out);
        bool TestSphereCapsule(const SphereCollider& s, const CapsuleCollider& c,
                            ContactPoint& out);
        bool TestAABBCapsule(const AABBCollider& b, const CapsuleCollider& c,
                            ContactPoint& out);
        bool TestCapsuleCapsule(const CapsuleCollider& a, const CapsuleCollider& b,
                            ContactPoint& out);

        // TriangleMesh 用テスト関数。メッシュ全体ではなく BVH で候補三角形を絞ってから呼ぶ。
        bool TestSphereTriangle  (const SphereCollider&  s, const Triangle& tri, ContactPoint& out);
        bool TestAABBTriangle    (const AABBCollider&    b, const Triangle& tri, ContactPoint& out);
        bool TestCapsuleTriangle (const CapsuleCollider& c, const Triangle& tri, ContactPoint& out);

        bool TestSphereTriangleMesh  (const SphereCollider&,  const TriangleMeshCollider&, ContactPoint& out);
        bool TestAABBTriangleMesh    (const AABBCollider&,    const TriangleMeshCollider&, ContactPoint& out);
        bool TestCapsuleTriangleMesh (const CapsuleCollider&, const TriangleMeshCollider&, ContactPoint& out);

        // ConvexHull 用テスト関数 (GJK + EPA)。形状差分をサポート関数で抽象化する。
        bool TestConvexConvex  (const ConvexHullCollider&, const ConvexHullCollider&, ContactPoint& out);
        bool TestSphereConvex  (const SphereCollider&,     const ConvexHullCollider&, ContactPoint& out);
        bool TestAABBConvex    (const AABBCollider&,        const ConvexHullCollider&, ContactPoint& out);
        bool TestCapsuleConvex (const CapsuleCollider&,     const ConvexHullCollider&, ContactPoint& out);

        // 法線方向の PGS インパルス解決 (クランプ付き蓄積)
        void ResolveVelocity(ContactPoint& cp);
        // 摩擦インパルス解決 (コーン制約)
        void ResolveFriction(ContactPoint& cp);
        // 速度解決だけでは残る貫通を Baumgarte で補正する。
        void ResolvePosition(ContactPoint& cp);

        static constexpr int   VELOCITY_ITER = 6;   // Warm Starting で削減
        static constexpr float SLOP          = 0.01f;
        static constexpr float BAUMGARTE     = 0.8f;
    };

} // namespace fbzz::physics
