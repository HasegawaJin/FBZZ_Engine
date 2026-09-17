/// @file    PhysicsSolver.hpp
/// @brief   衝突検出 (Broad/Narrow フェーズ) と衝突解決 (PGS インパルスベース)。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <functional>
#include <Physics/ContactPoint.hpp>
#include <Physics/CollisionPair.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/CylinderCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>
#include <Physics/HeightFieldCollider.hpp>
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
        /// @brief AABB の重なりだけを見て NarrowPhase 対象を絞る。レイヤー行列はここで適用する。
        void BroadPhase(const std::vector<ColliderInstance>& colliders,
                        std::vector<CollisionPair>& outPairs,
                        const std::function<bool(int, int)>& layerFilter);

        /// @brief 形状の組み合わせごとに詳細判定し、ContactPoint を生成する。
        void NarrowPhase(const std::vector<CollisionPair>& pairs,
                        std::vector<ContactPoint>& outContacts);

        /// @brief PGS ベースの衝突解決 (Warm Starting 済みの蓄積インパルスを引き継ぐ)。
        void Resolve(std::vector<ContactPoint>& contacts);

        /// @brief 接触点における 2 剛体の相対速度 (A から見た B との差、角速度の寄与を含む)。
        /// @note Resolve は速度を書き換えるため、呼んだ後では「ぶつかった勢い」が残らない。
        ///       ゲーム側が衝突の強さを知るには Resolve の前に同じ値を取る必要があり、
        ///       ResolveVelocity 内の計算と食い違わないよう両者で同じ関数を使う。
        [[nodiscard]] static math::Vector3 RelativeVelocityAt(const ContactPoint& cp);

    private:
        bool TestSphereSphere(const SphereCollider& a, const SphereCollider& b,
                            ContactPoint& out);
        bool TestAABBAABB    (const AABBCollider&   a, const AABBCollider&   b,
                            ContactPoint& out);
        bool TestOBBOBB      (const OBBCollider&    a, const OBBCollider&    b,
                            ContactPoint& out);
        bool TestSphereOBB   (const SphereCollider& s, const OBBCollider&    b,
                            ContactPoint& out);
        bool TestAABBOBB     (const AABBCollider&   a, const OBBCollider&    b,
                            ContactPoint& out);
        bool TestSphereAABB  (const SphereCollider& s, const AABBCollider&   b,
                            ContactPoint& out);
        bool TestSphereCapsule(const SphereCollider& s, const CapsuleCollider& c,
                            ContactPoint& out);
        bool TestAABBCapsule(const AABBCollider& b, const CapsuleCollider& c,
                            ContactPoint& out);
        bool TestOBBCapsule (const OBBCollider& b, const CapsuleCollider& c,
                            ContactPoint& out);
        bool TestCapsuleCapsule(const CapsuleCollider& a, const CapsuleCollider& b,
                            ContactPoint& out);

        /// @name Cylinder 用テスト関数。球以外は解析解の場合分けが増えすぎるため GJK + EPA に委譲する。
        /// @{
        bool TestSphereCylinder (const SphereCollider&  s, const CylinderCollider& c,
                            ContactPoint& out);
        bool TestAABBCylinder   (const AABBCollider&    b, const CylinderCollider& c,
                            ContactPoint& out);
        bool TestOBBCylinder    (const OBBCollider&     b, const CylinderCollider& c,
                            ContactPoint& out);
        bool TestCapsuleCylinder(const CapsuleCollider& a, const CylinderCollider& c,
                            ContactPoint& out);
        bool TestCylinderCylinder(const CylinderCollider& a, const CylinderCollider& b,
                            ContactPoint& out);
        /// @}

        /// @name TriangleMesh 用テスト関数。メッシュ全体ではなく BVH で候補三角形を絞ってから呼ぶ。
        /// @{
        bool TestSphereTriangle  (const SphereCollider&  s, const Triangle& tri, ContactPoint& out);
        bool TestAABBTriangle    (const AABBCollider&    b, const Triangle& tri, ContactPoint& out);
        bool TestCapsuleTriangle (const CapsuleCollider& c, const Triangle& tri, ContactPoint& out);

        bool TestSphereTriangleMesh  (const SphereCollider&,  const TriangleMeshCollider&, ContactPoint& out);
        bool TestAABBTriangleMesh    (const AABBCollider&,    const TriangleMeshCollider&, ContactPoint& out);
        bool TestCapsuleTriangleMesh (const CapsuleCollider&, const TriangleMeshCollider&, ContactPoint& out);
        bool TestOBBTriangleMesh     (const OBBCollider&,      const TriangleMeshCollider&, ContactPoint& out);
        bool TestConvexHullTriangleMesh(const ConvexHullCollider&, const TriangleMeshCollider&, ContactPoint& out);
        bool TestCylinderTriangleMesh  (const CylinderCollider&,   const TriangleMeshCollider&, ContactPoint& out);
        /// @}

        /// @name HeightField 用テスト関数。内部 BVH に対して TriangleMesh と同一アルゴリズムを適用する。
        /// @{
        bool TestSphereHeightField    (const SphereCollider&,  const HeightFieldCollider&, ContactPoint& out);
        bool TestAABBHeightField      (const AABBCollider&,    const HeightFieldCollider&, ContactPoint& out);
        bool TestCapsuleHeightField   (const CapsuleCollider&, const HeightFieldCollider&, ContactPoint& out);
        bool TestOBBHeightField       (const OBBCollider&,     const HeightFieldCollider&, ContactPoint& out);
        bool TestConvexHullHeightField(const ConvexHullCollider&, const HeightFieldCollider&, ContactPoint& out);
        bool TestCylinderHeightField  (const CylinderCollider&,   const HeightFieldCollider&, ContactPoint& out);
        /// @}

        /// @name ConvexHull 用テスト関数 (GJK + EPA)。形状差分をサポート関数で抽象化する。
        /// @{
        bool TestConvexConvex  (const ConvexHullCollider&, const ConvexHullCollider&, ContactPoint& out);
        bool TestSphereConvex  (const SphereCollider&,     const ConvexHullCollider&, ContactPoint& out);
        bool TestAABBConvex    (const AABBCollider&,        const ConvexHullCollider&, ContactPoint& out);
        bool TestOBBConvex     (const OBBCollider&,         const ConvexHullCollider&, ContactPoint& out);
        bool TestCapsuleConvex (const CapsuleCollider&,     const ConvexHullCollider&, ContactPoint& out);
        bool TestCylinderConvex(const CylinderCollider&,    const ConvexHullCollider&, ContactPoint& out);
        /// @}

        /// @brief 法線方向の PGS インパルス解決 (クランプ付き蓄積)。
        void ResolveVelocity(ContactPoint& cp);
        /// @brief 摩擦インパルス解決 (コーン制約)。
        void ResolveFriction(ContactPoint& cp);
        /// @brief 速度解決だけでは残る貫通を Baumgarte で補正する。
        void ResolvePosition(ContactPoint& cp);

        static constexpr int   VELOCITY_ITER = 6;   ///< Warm Starting で削減
        static constexpr float SLOP          = 0.01f;
        static constexpr float BAUMGARTE     = 0.8f;
    };

} // namespace fbzz::physics
