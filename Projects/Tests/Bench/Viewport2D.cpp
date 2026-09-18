/// @file    Viewport2D.cpp
/// @brief   2D 直交ビューポートの実装。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include "Viewport2D.hpp"

#include <algorithm>
#include <cmath>

namespace fbzz::bench {

void Viewport2D::Begin(const char* id, const ImVec2& size)
{
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    m_origin = ImGui::GetCursorScreenPos();

    /// @note ウィンドウを潰したときに 0 や負が来る。InvisibleButton はそこで assert するので、
    ///       最低 1px を保証してから使う。
    const ImVec2 available = ImGui::GetContentRegionAvail();
    m_size = {(std::max)(1.0f, available.x), (std::max)(1.0f, available.y)};

    m_drawList = ImGui::GetWindowDrawList();
    m_drawList->PushClipRect(m_origin, {m_origin.x + m_size.x, m_origin.y + m_size.y}, true);
}

void Viewport2D::End()
{
    if (m_drawList) m_drawList->PopClipRect();
    m_drawList = nullptr;
    ImGui::EndChild();
}

void Viewport2D::HandlePanZoom()
{
    ImGui::InvisibleButton("##viewport_input", m_size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();

    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        /// @note 画面を掴んで動かす感触にするため、世界は逆向きへずらす。
        const float dx = -delta.x / m_pixelsPerMeter;
        /// @note 画面 Y は下向き
        const float dy = delta.y / m_pixelsPerMeter;

        if (m_plane == ViewPlane::XY) {
            m_center.x += dx;
            m_center.y += dy;
        } else {
            m_center.x += dx;
            m_center.z += dy;
        }
    }

    const float wheel = ImGui::GetIO().MouseWheel;
    if (hovered && wheel != 0.0f) {
        m_pixelsPerMeter *= std::pow(1.15f, wheel);
        m_pixelsPerMeter = (std::max)(4.0f, (std::min)(m_pixelsPerMeter, 2000.0f));
    }
}

void Viewport2D::SetFocus(const math::Vector3& center, float pixelsPerMeter)
{
    m_center         = center;
    m_pixelsPerMeter = pixelsPerMeter;
}

ImVec2 Viewport2D::PlaneCoords(const math::Vector3& world) const
{
    if (m_plane == ViewPlane::XY) return {world.x, world.y};
    return {world.x, world.z};
}

ImVec2 Viewport2D::ToScreen(const math::Vector3& world) const
{
    const ImVec2 point  = PlaneCoords(world);
    const ImVec2 center = PlaneCoords(m_center);

    return {m_origin.x + m_size.x * 0.5f + (point.x - center.x) * m_pixelsPerMeter,
            m_origin.y + m_size.y * 0.5f - (point.y - center.y) * m_pixelsPerMeter};
}

void Viewport2D::DrawGrid(float stepMeters)
{
    if (!m_drawList || stepMeters <= 0.0f) return;

    const ImVec2 center      = PlaneCoords(m_center);
    const float  halfWidth   = m_size.x * 0.5f / m_pixelsPerMeter;
    const float  halfHeight  = m_size.y * 0.5f / m_pixelsPerMeter;
    const float  left        = std::floor((center.x - halfWidth) / stepMeters) * stepMeters;
    const float  bottom      = std::floor((center.y - halfHeight) / stepMeters) * stepMeters;

    for (float x = left; x <= center.x + halfWidth; x += stepMeters) {
        const bool  isAxis = std::fabs(x) < stepMeters * 0.01f;
        const float sx     = m_origin.x + m_size.x * 0.5f + (x - center.x) * m_pixelsPerMeter;
        m_drawList->AddLine({sx, m_origin.y}, {sx, m_origin.y + m_size.y},
                            isAxis ? colors::kAxis : colors::kGrid, isAxis ? 1.5f : 1.0f);
    }
    for (float y = bottom; y <= center.y + halfHeight; y += stepMeters) {
        const bool  isAxis = std::fabs(y) < stepMeters * 0.01f;
        const float sy     = m_origin.y + m_size.y * 0.5f - (y - center.y) * m_pixelsPerMeter;
        m_drawList->AddLine({m_origin.x, sy}, {m_origin.x + m_size.x, sy},
                            isAxis ? colors::kAxis : colors::kGrid, isAxis ? 1.5f : 1.0f);
    }
}

void Viewport2D::DrawLine(const math::Vector3& a, const math::Vector3& b, ImU32 color,
                          float thickness)
{
    if (!m_drawList) return;
    m_drawList->AddLine(ToScreen(a), ToScreen(b), color, thickness);
}

void Viewport2D::DrawArrow(const math::Vector3& from, const math::Vector3& to, ImU32 color,
                           float thickness)
{
    if (!m_drawList) return;

    const ImVec2 start = ToScreen(from);
    const ImVec2 end   = ToScreen(to);
    m_drawList->AddLine(start, end, color, thickness);

    const float dx     = end.x - start.x;
    const float dy     = end.y - start.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1.0f) return;

    /// @note 矢じりは画面上の固定サイズ。拡大率を変えても «向きが読める» 大きさを保つ。
    constexpr float kHead = 9.0f;
    const float     ux    = dx / length;
    const float     uy    = dy / length;
    m_drawList->AddTriangleFilled(
        end, {end.x - ux * kHead - uy * kHead * 0.5f, end.y - uy * kHead + ux * kHead * 0.5f},
        {end.x - ux * kHead + uy * kHead * 0.5f, end.y - uy * kHead - ux * kHead * 0.5f}, color);
}

void Viewport2D::DrawCircle(const math::Vector3& center, float radiusMeters, ImU32 color,
                            bool filled, float thickness)
{
    if (!m_drawList) return;

    const ImVec2 screen = ToScreen(center);
    const float  radius = ToPixels(radiusMeters);
    if (filled) m_drawList->AddCircleFilled(screen, radius, color, 32);
    else        m_drawList->AddCircle(screen, radius, color, 32, thickness);
}

void Viewport2D::DrawBox(const math::Vector3& center, const math::Vector3& halfExtents,
                         ImU32 color, bool filled, float thickness)
{
    if (!m_drawList) return;

    const ImVec2 minCorner = ToScreen(center - halfExtents);
    const ImVec2 maxCorner = ToScreen(center + halfExtents);
    const ImVec2 upperLeft{(std::min)(minCorner.x, maxCorner.x), (std::min)(minCorner.y, maxCorner.y)};
    const ImVec2 lowerRight{(std::max)(minCorner.x, maxCorner.x), (std::max)(minCorner.y, maxCorner.y)};

    if (filled) m_drawList->AddRectFilled(upperLeft, lowerRight, color);
    else        m_drawList->AddRect(upperLeft, lowerRight, color, 0.0f, 0, thickness);
}

void Viewport2D::DrawPoint(const math::Vector3& position, ImU32 color, float radiusPixels)
{
    if (!m_drawList) return;
    m_drawList->AddCircleFilled(ToScreen(position), radiusPixels, color, 16);
}

void Viewport2D::DrawText(const math::Vector3& position, ImU32 color, const char* text)
{
    if (!m_drawList) return;
    m_drawList->AddText(ToScreen(position), color, text);
}

void Viewport2D::DrawGroundLine(float height, ImU32 color)
{
    if (!m_drawList || m_plane != ViewPlane::XY) return;

    const float sy = ToScreen({0.0f, height, 0.0f}).y;
    m_drawList->AddLine({m_origin.x, sy}, {m_origin.x + m_size.x, sy}, color, 2.0f);
}

} // namespace fbzz::bench
