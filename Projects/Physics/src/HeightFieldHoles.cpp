/// @file    HeightFieldHoles.cpp
/// @brief   高さ場の穴マスクの正規化とセル単位の照会。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/HeightFieldCollider.hpp>
#include <algorithm>
#include <cstddef>

namespace fbzz::physics
{

bool HeightFieldCollider::IsHoleCell(int cellColumn, int cellRow) const
{
    const int cellColumnCount = m_columnCount - 1;
    const int cellRowCount = m_rowCount - 1;
    if (m_holeMask.empty() || cellColumn < 0 || cellRow < 0 ||
        cellColumn >= cellColumnCount || cellRow >= cellRowCount)
        return false;

    const std::size_t maskIndex = static_cast<std::size_t>(cellRow) * static_cast<std::size_t>(cellColumnCount)
                                + static_cast<std::size_t>(cellColumn);
    return m_holeMask[maskIndex] != 0;
}

void HeightFieldCollider::SanitizeHoles()
{
    const std::size_t cellCount = m_rowCount >= 2 && m_columnCount >= 2
        ? static_cast<std::size_t>(m_rowCount - 1) * static_cast<std::size_t>(m_columnCount - 1) : 0;
    const bool hasHoles = std::any_of(m_holeMask.begin(), m_holeMask.end(),
        [](std::uint8_t cellMaskValue) { return cellMaskValue != 0; });
    /// @note サイズが合わないマスクを部分的に読むと穴の位置がずれる。
    if (m_holeMask.size() != cellCount || !hasHoles)
        m_holeMask.clear();
}

} /// @note namespace fbzz::physics
