// FBZZ Engine
// TriangleMeshCollider.hpp | fbzz::physics
// 任意メッシュの静的コライダー (BVH 加速)
// Physics は Engine に依存しないため、Mesh の代わりに生データを受け取る
#pragma once
#include <vector>
#include <cstdint>
#include <Physics/Collider.hpp>
#include <Physics/BVHNode.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::physics
{

    // 静的な任意メッシュ用コライダー。動的メッシュは BVH 再構築コストが高いため想定外。
    class TriangleMeshCollider : public Collider
    {
    public:
        // 頂点位置リストとインデックスリスト (三角形ごとに 3 つ) から BVH を構築する
        // positions: ローカル空間の頂点位置
        // indices:   三角形インデックス (要素数は 3 の倍数)
        TriangleMeshCollider(const std::vector<math::Vector3>& positions,
                             const std::vector<uint32_t>&      indices);

        AABB         GetAABB() const override { return m_worldAABB; }
        ColliderType GetType() const override { return ColliderType::TRIANGLE_MESH; }

        // Static 前提: 初期化後は位置・回転が変わらないことを想定。
        // 動的オブジェクトへの適用は将来拡張 (UpdateWithScale 等)
        void Update(const math::Vector3& worldPos,
                    const math::Quaternion& worldRot) override;

        // スケールを含むワールド変換で再構築する (Static 初期化時に明示的に呼ぶ)。
        // Transform が変わっていなければ内部で再構築をスキップする。
        void UpdateWithScale(const math::Vector3&    worldPos,
                             const math::Quaternion& worldRot,
                             const math::Vector3&    worldScale);

        const BVHTree& GetBVH() const { return m_bvh; }

    private:
        std::vector<math::Vector3> m_localPositions; // ローカル空間頂点 (スケール前)
        std::vector<uint32_t>      m_indices;

        BVHTree       m_bvh;
        AABB          m_worldAABB;

        math::Vector3    m_lastPos;
        math::Quaternion m_lastRot;
        math::Vector3    m_lastScale = {1.0f, 1.0f, 1.0f};

        void RebuildBVH(const math::Vector3&    pos,
                        const math::Quaternion& rot,
                        const math::Vector3&    scale);
    };

} // namespace fbzz::physics
