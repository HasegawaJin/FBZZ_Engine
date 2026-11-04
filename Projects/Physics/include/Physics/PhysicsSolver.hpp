/// @file    PhysicsSolver.hpp
/// @brief   衝突検出 (Broad/Narrow フェーズ) と衝突解決 (PGS インパルスベース)。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Physics/Api.hpp>
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
    struct BroadPhaseDebugRequest;
    struct BroadPhaseDebugSnapshot;

    class PhysicsSolver
    {
    public:
        FBZZ_PHYSICS_API PhysicsSolver();
        FBZZ_PHYSICS_API ~PhysicsSolver();
        FBZZ_PHYSICS_API PhysicsSolver(const PhysicsSolver& other);
        FBZZ_PHYSICS_API PhysicsSolver& operator=(const PhysicsSolver& other);
        FBZZ_PHYSICS_API PhysicsSolver(PhysicsSolver&& other) noexcept;
        FBZZ_PHYSICS_API PhysicsSolver& operator=(PhysicsSolver&& other) noexcept;

        /// @brief AABB の重なりだけを見て NarrowPhase 対象を絞る。レイヤー行列はここで適用する。
        /// @note 境界と索引だけを保持し、ColliderInstanceとbody状態は呼出しごとに読む。結果はoutPairsへ追記する。
        /// @pre 同じsolverの並行実行、およびfilterからの同じsolverへの再入・入力変更は禁止。
        /// @see Docs/design/broadphase-incremental.md
        FBZZ_PHYSICS_API void BroadPhase(const std::vector<ColliderInstance>& colliders,
                        std::vector<CollisionPair>& outPairs,
                        const std::function<bool(int, int)>& layerFilter);

        /// @brief 形状の組み合わせごとに詳細判定し、ContactPoint を生成する。
        FBZZ_PHYSICS_API void NarrowPhase(const std::vector<CollisionPair>& pairs,
                        std::vector<ContactPoint>& outContacts);

        /// @brief PGS ベースの衝突解決 (Warm Starting 済みの蓄積インパルスを引き継ぐ)。
        FBZZ_PHYSICS_API void Resolve(std::vector<ContactPoint>& contacts);

        /// @brief 接触点における 2 剛体の相対速度 (A から見た B との差、角速度の寄与を含む)。
        /// @note Resolve は速度を書き換えるため、呼んだ後では「ぶつかった勢い」が残らない。
        /// @note ゲーム側が衝突の強さを知るには Resolve の前に同じ値を取る必要があり、
        /// @note ResolveVelocity 内の計算と食い違わないよう両者で同じ関数を使う。
        [[nodiscard]] FBZZ_PHYSICS_API static math::Vector3 RelativeVelocityAt(const ContactPoint& cp);

    private:
        friend class World;
        /// @pre Called on the owner thread immediately after BroadPhase with its current inputs/output.
        FBZZ_PHYSICS_API void CaptureBroadPhaseDebugSnapshot(const BroadPhaseDebugRequest& request,
            const std::vector<ColliderInstance>& colliders, const std::vector<CollisionPair>& pairs,
            std::size_t selectedIndex, BroadPhaseDebugSnapshot& snapshot) const;
        struct BroadPhaseState;
        std::unique_ptr<BroadPhaseState> m_broadPhase;

        FBZZ_PHYSICS_API bool TestSphereSphere(const SphereCollider& a, const SphereCollider& b,
                            ContactPoint& out);
        FBZZ_PHYSICS_API bool TestAABBAABB    (const AABBCollider&   a, const AABBCollider&   b,
                            ContactPoint& out);
        FBZZ_PHYSICS_API bool TestOBBOBB      (const OBBCollider&    a, const OBBCollider&    b,
                            ContactPoint& out);
        FBZZ_PHYSICS_API bool TestSphereOBB   (const SphereCollider& s, const OBBCollider&    b,
                            ContactPoint& out);
        FBZZ_PHYSICS_API bool TestAABBOBB     (const AABBCollider&   a, const OBBCollider&    b,
                            ContactPoint& out);
        FBZZ_PHYSICS_API bool TestSphereAABB  (const SphereCollider& s, const AABBCollider&   b,
                            ContactPoint& out);
        FBZZ_PHYSICS_API bool TestSphereCapsule(const SphereCollider& s, const CapsuleCollider& c,
                            ContactPoint& out);
        FBZZ_PHYSICS_API bool TestAABBCapsule(const AABBCollider& b, const CapsuleCollider& c,
                            ContactPoint& out);
        FBZZ_PHYSICS_API bool TestOBBCapsule (const OBBCollider& b, const CapsuleCollider& c,
                            ContactPoint& out);
        FBZZ_PHYSICS_API bool TestCapsuleCapsule(const CapsuleCollider& a, const CapsuleCollider& b,
                            ContactPoint& out);

        /// @name Cylinder 用テスト関数。球以外は解析解の場合分けが増えすぎるため GJK + EPA に委譲する。
        /// @{
        FBZZ_PHYSICS_API bool TestSphereCylinder (const SphereCollider&  s, const CylinderCollider& c,
                            ContactPoint& out);
        FBZZ_PHYSICS_API bool TestAABBCylinder   (const AABBCollider&    b, const CylinderCollider& c,
                            ContactPoint& out);
        FBZZ_PHYSICS_API bool TestOBBCylinder    (const OBBCollider&     b, const CylinderCollider& c,
                            ContactPoint& out);
        FBZZ_PHYSICS_API bool TestCapsuleCylinder(const CapsuleCollider& a, const CylinderCollider& c,
                            ContactPoint& out);
        FBZZ_PHYSICS_API bool TestCylinderCylinder(const CylinderCollider& a, const CylinderCollider& b,
                            ContactPoint& out);
        /// @}

        /// @name TriangleMesh 用テスト関数。メッシュ全体ではなく BVH で候補三角形を絞ってから呼ぶ。
        /// @{
        FBZZ_PHYSICS_API bool TestSphereTriangle  (const SphereCollider&  s, const Triangle& tri, ContactPoint& out);
        FBZZ_PHYSICS_API bool TestAABBTriangle    (const AABBCollider&    b, const Triangle& tri, ContactPoint& out);
        FBZZ_PHYSICS_API bool TestCapsuleTriangle (const CapsuleCollider& c, const Triangle& tri, ContactPoint& out);

        FBZZ_PHYSICS_API bool TestSphereTriangleMesh  (const SphereCollider&,  const TriangleMeshCollider&, ContactPoint& out);
        FBZZ_PHYSICS_API bool TestAABBTriangleMesh    (const AABBCollider&,    const TriangleMeshCollider&, ContactPoint& out);
        FBZZ_PHYSICS_API bool TestCapsuleTriangleMesh (const CapsuleCollider&, const TriangleMeshCollider&, ContactPoint& out);
        FBZZ_PHYSICS_API bool TestOBBTriangleMesh     (const OBBCollider&,      const TriangleMeshCollider&, ContactPoint& out);
        FBZZ_PHYSICS_API bool TestConvexHullTriangleMesh(const ConvexHullCollider&, const TriangleMeshCollider&, ContactPoint& out);
        FBZZ_PHYSICS_API bool TestCylinderTriangleMesh  (const CylinderCollider&,   const TriangleMeshCollider&, ContactPoint& out);
        /// @}

        /// @name HeightField 用テスト関数。内部 BVH に対して TriangleMesh と同一アルゴリズムを適用する。
        /// @{
        FBZZ_PHYSICS_API bool TestSphereHeightField    (const SphereCollider&,  const HeightFieldCollider&, ContactPoint& out);
        FBZZ_PHYSICS_API bool TestAABBHeightField      (const AABBCollider&,    const HeightFieldCollider&, ContactPoint& out);
        FBZZ_PHYSICS_API bool TestCapsuleHeightField   (const CapsuleCollider&, const HeightFieldCollider&, ContactPoint& out);
        FBZZ_PHYSICS_API bool TestOBBHeightField       (const OBBCollider&,     const HeightFieldCollider&, ContactPoint& out);
        FBZZ_PHYSICS_API bool TestConvexHullHeightField(const ConvexHullCollider&, const HeightFieldCollider&, ContactPoint& out);
        FBZZ_PHYSICS_API bool TestCylinderHeightField  (const CylinderCollider&,   const HeightFieldCollider&, ContactPoint& out);
        /// @}

        /// @name ConvexHull 用テスト関数 (GJK + EPA)。形状差分をサポート関数で抽象化する。
        /// @{
        FBZZ_PHYSICS_API bool TestConvexConvex  (const ConvexHullCollider&, const ConvexHullCollider&, ContactPoint& out);
        FBZZ_PHYSICS_API bool TestSphereConvex  (const SphereCollider&,     const ConvexHullCollider&, ContactPoint& out);
        FBZZ_PHYSICS_API bool TestAABBConvex    (const AABBCollider&,        const ConvexHullCollider&, ContactPoint& out);
        FBZZ_PHYSICS_API bool TestOBBConvex     (const OBBCollider&,         const ConvexHullCollider&, ContactPoint& out);
        FBZZ_PHYSICS_API bool TestCapsuleConvex (const CapsuleCollider&,     const ConvexHullCollider&, ContactPoint& out);
        FBZZ_PHYSICS_API bool TestCylinderConvex(const CylinderCollider&,    const ConvexHullCollider&, ContactPoint& out);
        /// @}

        /// @brief 法線方向の PGS インパルス解決 (クランプ付き蓄積)。
        FBZZ_PHYSICS_API void ResolveVelocity(ContactPoint& cp);
        /// @brief 摩擦インパルス解決 (コーン制約)。
        FBZZ_PHYSICS_API void ResolveFriction(ContactPoint& cp);
        /// @brief 速度解決だけでは残る貫通を Baumgarte で補正する。
        FBZZ_PHYSICS_API void ResolvePosition(ContactPoint& cp);

        static constexpr int   VELOCITY_ITER = 6;   ///< @note Warm Starting で削減
        static constexpr float SLOP          = 0.01f;
        static constexpr float BAUMGARTE     = 0.8f;
    };

} /// @note namespace fbzz::physics
