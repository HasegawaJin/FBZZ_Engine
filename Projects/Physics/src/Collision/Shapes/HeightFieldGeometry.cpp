/// @file    HeightFieldGeometry.cpp
/// @brief   格子の高さからワールド三角形と面法線を生成する。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/Collision/Shapes/HeightFieldCollider.hpp>
#include <cstddef>
#include <iterator>

namespace fbzz::physics
{

namespace
{
    struct CornerOffset
    {
        int column;
        int row;
    };

    constexpr uint32_t TRIANGLES_PER_CELL = 2u;
    constexpr CornerOffset CELL_CORNERS[] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    /// @note 生成と refit は同じ巻き順を共有する。セル内番号 0 は下三角、1 は上三角。
    constexpr std::size_t TRIANGLE_CORNERS[TRIANGLES_PER_CELL][3] = {{0, 2, 1}, {1, 2, 3}};
    constexpr float MIN_FACE_NORMAL_LENGTH_SQUARED = 1e-10f;

    bool TryUpdateNormal(Triangle& triangle)
    {
        const math::Vector3 normal = math::Vector3::Cross(triangle.v[1] - triangle.v[0], triangle.v[2] - triangle.v[0]);
        if (!(normal.LengthSq() >= MIN_FACE_NORMAL_LENGTH_SQUARED)) return false;
        triangle.normal = normal.Normalized();
        return true;
    }
}

math::Vector3 HeightFieldCollider::LocalVertex(int vertexColumn, int vertexRow) const
{
    const std::size_t sampleIndex = static_cast<std::size_t>(vertexRow) * static_cast<std::size_t>(m_columnCount)
                                  + static_cast<std::size_t>(vertexColumn);
    const float localHeight = m_heightSamples[sampleIndex] * m_maxHeight;
    return {static_cast<float>(vertexColumn) * m_cellSize, localHeight, static_cast<float>(vertexRow) * m_cellSize};
}

math::Vector3 HeightFieldCollider::ToWorld(const math::Vector3& localPosition) const
{
    const math::Vector3 scaledPosition = {localPosition.x * m_worldScale.x, localPosition.y * m_worldScale.y,
                                          localPosition.z * m_worldScale.z};
    return m_worldPosition + m_worldRotation * scaledPosition;
}

std::vector<Triangle> HeightFieldCollider::BuildWorldTriangles() const
{
    const int cellColumnCount = m_columnCount - 1;
    const int cellRowCount = m_rowCount - 1;
    std::vector<Triangle> worldTriangles;
    worldTriangles.reserve(static_cast<std::size_t>(cellColumnCount) * static_cast<std::size_t>(cellRowCount)
                           * TRIANGLES_PER_CELL);

    for (int cellRow = 0; cellRow < cellRowCount; ++cellRow)
    {
        for (int cellColumn = 0; cellColumn < cellColumnCount; ++cellColumn)
        {
            if (IsHoleCell(cellColumn, cellRow)) continue;

            math::Vector3 worldCorners[std::size(CELL_CORNERS)];
            for (std::size_t cornerIndex = 0; cornerIndex < std::size(CELL_CORNERS); ++cornerIndex)
            {
                const CornerOffset& corner = CELL_CORNERS[cornerIndex];
                worldCorners[cornerIndex] = ToWorld(LocalVertex(cellColumn + corner.column, cellRow + corner.row));
            }

            /// @note 穴や縮退面を飛ばしても、入力格子から決まる三角形番号は詰めない。
            const uint32_t cellIndex = static_cast<uint32_t>(cellRow) * static_cast<uint32_t>(cellColumnCount)
                                     + static_cast<uint32_t>(cellColumn);
            const uint32_t firstTriangleIndex = cellIndex * TRIANGLES_PER_CELL;
            for (uint32_t triangleInCell = 0; triangleInCell < TRIANGLES_PER_CELL; ++triangleInCell)
            {
                const auto& corners = TRIANGLE_CORNERS[triangleInCell];
                Triangle triangle{{worldCorners[corners[0]], worldCorners[corners[1]], worldCorners[corners[2]]},
                                  {}, firstTriangleIndex + triangleInCell};
                if (TryUpdateNormal(triangle))
                    worldTriangles.push_back(triangle);
            }
        }
    }
    return worldTriangles;
}

void HeightFieldCollider::UpdateWorldTriangles(std::vector<Triangle>& worldTriangles) const
{
    const uint32_t cellColumnCount = static_cast<uint32_t>(m_columnCount - 1);
    const uint32_t cellCount = cellColumnCount * static_cast<uint32_t>(m_rowCount - 1);
    for (Triangle& triangle : worldTriangles)
    {
        const uint32_t cellIndex = triangle.index / TRIANGLES_PER_CELL;
        if (cellIndex >= cellCount) continue;

        const int cellColumn = static_cast<int>(cellIndex % cellColumnCount);
        const int cellRow = static_cast<int>(cellIndex / cellColumnCount);
        const auto& corners = TRIANGLE_CORNERS[triangle.index % TRIANGLES_PER_CELL];
        for (std::size_t vertexIndex = 0; vertexIndex < std::size(corners); ++vertexIndex)
        {
            const CornerOffset& corner = CELL_CORNERS[corners[vertexIndex]];
            triangle.v[vertexIndex] = ToWorld(LocalVertex(cellColumn + corner.column, cellRow + corner.row));
        }

        /// @note スケール 0 などで一時的に縮退した場合は前の法線を保持する。
        TryUpdateNormal(triangle);
    }
}

} /// @note namespace fbzz::physics
