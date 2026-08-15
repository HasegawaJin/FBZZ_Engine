// FBZZ Engine
// Collider.hpp | fbzz::physics
// コライダー形状の定義 (Sphere / AABB / Capsule / Mesh / ConvexHull)
#pragma once
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>

namespace fbzz::physics 
{
    // BroadPhase 用の軸整合境界。形状ごとの詳細判定より先に粗い重なりを調べる。
    struct AABB {
        math::Vector3 min;
        math::Vector3 max;

        bool          Overlaps(const AABB& other) const;
        AABB          Merge(const AABB& other)    const;
        math::Vector3 Center()  const { return (min + max) * 0.5f; }
        math::Vector3 Extents() const { return (max - min) * 0.5f; }
    };

    enum class ColliderType { SPHERE, AABB, OBB, CAPSULE, TRIANGLE_MESH, CONVEX_HULL, HEIGHT_FIELD };

    // World は Collider を所有しない。Scene 側の ColliderComponent が共有所有し、World は参照して使う。
    class Collider {
    public:
        virtual ~Collider() = default;

        virtual AABB         GetAABB() const = 0;
        virtual ColliderType GetType() const = 0;

        // 形状の体積。PhysicsMaterial::density から質量を求めるのに使う。
        //
        // WHY 既定を AABB 体積にするか: 三角メッシュや凸包の厳密な体積を出すには
        //     多面体の符号付き四面体積分が要り、開いたメッシュでは破綻する。
        //     質量は「妥当な桁に収まっていれば良い」値なので、外接箱の体積で近似し、
        //     厳密さが要る基本形状 (球・箱・カプセル) だけが override する。
        //     純粋仮想にしないのは、形状を追加するたびに体積計算を強制しないため。
        [[nodiscard]] virtual float ComputeVolume() const;

        // World::UpdateColliders() から毎フレーム呼ばれ、剛体の Transform を形状へ同期する。
        virtual void Update(const math::Vector3& worldPos,
                            const math::Quaternion& worldRot) = 0;
    };

} // namespace fbzz::physics
