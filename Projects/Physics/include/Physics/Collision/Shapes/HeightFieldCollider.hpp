/// @file    HeightFieldCollider.hpp
/// @brief   ハイトマップ地形向け最適化コライダー。
/// @author  Hasegawa Jin
/// @date    2026-06-16

/// @note 均一な格子を使い、ローカルの頂点・添字の配列を持たずにワールド三角形を生成する。
#pragma once
#include <Physics/Api.hpp>
#include <Physics/Collision/Shapes/Collider.hpp>
#include <Physics/Collision/BroadPhase/BVHNode.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <cstdint>
#include <vector>

namespace fbzz::physics
{

class HeightFieldCollider : public Collider
{
public:
    /// @param heightSamples 行優先、値域 [-1, 1]。maxHeight を掛けた値がローカル高さ。
    /// @param holeMask セルの行優先配列。非 0 が穴で、空またはセル数と違う配列は穴なしとして扱う。
    /// @see Docs/design/terrain-layers.md §4 穴
    FBZZ_PHYSICS_API HeightFieldCollider(const std::vector<float>& heightSamples,
                                        int rowCount, int columnCount,
                                        float cellSize, float maxHeight,
                                        std::vector<std::uint8_t> holeMask = {});

    AABB         GetAABB() const override { return m_worldAABB; }
    ColliderType GetType() const override { return ColliderType::HEIGHT_FIELD; }

    /// @note 姿勢だけの変更は、格子と三角形の対応を保ったまま BVH の境界を更新する。
    FBZZ_PHYSICS_API void Update(const math::Vector3& worldPosition,
                                const math::Quaternion& worldRotation) override;
    FBZZ_PHYSICS_API void UpdateWithScale(const math::Vector3& worldPosition,
                                         const math::Quaternion& worldRotation,
                                         const math::Vector3& worldScale);

    /// @note 高さや穴が変わったときは格子を置き換えて BVH を再構築する。
    /// @param holeMask コンストラクタと同じ規約。空なら穴なし。
    FBZZ_PHYSICS_API void Rebuild(const std::vector<float>& heightSamples,
                                 int rowCount, int columnCount,
                                 float cellSize, float maxHeight,
                                 std::vector<std::uint8_t> holeMask = {});

    const BVHTree& GetBVH() const { return m_bvh; }

    /// @return 指定したセルが穴なら true。範囲外は false。
    [[nodiscard]] FBZZ_PHYSICS_API bool IsHoleCell(int cellColumn, int cellRow) const;
    /// @return 穴マスク。穴なしなら空。
    const std::vector<std::uint8_t>& GetHoles() const { return m_holeMask; }

    int   GetRows()      const { return m_rowCount; }
    int   GetCols()      const { return m_columnCount; }
    float GetCellSize()  const { return m_cellSize; }
    float GetMaxHeight() const { return m_maxHeight; }

private:
    int   m_rowCount = 0;
    int   m_columnCount = 0;
    float m_cellSize  = 1.0f;
    float m_maxHeight = 1.0f;

    std::vector<float> m_heightSamples;
    std::vector<std::uint8_t> m_holeMask; ///< @note 空、またはセル数と同じ長さ。

    BVHTree m_bvh;
    AABB    m_worldAABB;

    math::Vector3    m_worldPosition;
    math::Quaternion m_worldRotation;
    math::Vector3    m_worldScale = { 1.f, 1.f, 1.f };

    FBZZ_PHYSICS_API math::Vector3 LocalVertex(int vertexColumn, int vertexRow) const;
    /// @brief ローカル座標へ scale → rotation → translation を掛ける。
    FBZZ_PHYSICS_API math::Vector3 ToWorld(const math::Vector3& localPosition) const;

    [[nodiscard]] std::vector<Triangle> BuildWorldTriangles() const;
    /// @note 三角形の入力上の番号から格子を逆算し、BVH の葉への所属を維持する。
    void UpdateWorldTriangles(std::vector<Triangle>& worldTriangles) const;

    /// @brief 穴マスクの大きさがセル数と違う、または穴が 1 つも無いなら空にする。
    FBZZ_PHYSICS_API void SanitizeHoles();
    /// @brief 現在の transform を使って BVH をフル再構築する。
    FBZZ_PHYSICS_API void RebuildBVH();
    /// @brief 木の構造はそのままに、現在の transform で三角形を置き直して AABB を refit する。
    FBZZ_PHYSICS_API void RefitTransform();
};

} /// @note namespace fbzz::physics
