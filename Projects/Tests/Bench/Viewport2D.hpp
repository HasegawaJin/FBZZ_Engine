/// @file    Viewport2D.hpp
/// @brief   ベンチ用の 2D 直交ビューポート。ImDrawList へ世界座標のまま描く。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// @note ここで見たいのは «収束したか» «向きが合っているか» であって見栄えではない。3D レンダラを噛ませるとシェーダー・カメラ・ライトの都合が混ざり «物理が変なのか描画が変なのか» が切り分けられなくなる。
#pragma once

#include <Math/Vector3.hpp>

#include <imgui.h>

namespace fbzz::bench {

/// 世界の 1 つの平面をスクリーンへ写す。
enum class ViewPlane {
    XY, ///< 側面図。重力方向 (Y) が画面の上下になる。
    XZ, ///< 平面図。地面を真上から見る。
};

class Viewport2D {
public:
    /// 子ウィンドウを開いて描画領域を確保する。End() と対にすること。
    void Begin(const char* id, const ImVec2& size);
    void End();

    /// マウスドラッグで平行移動、ホイールで拡大縮小。Begin と End の間で呼ぶ。
    void HandlePanZoom();

    void SetPlane(ViewPlane plane) { m_plane = plane; }
    [[nodiscard]] ViewPlane GetPlane() const { return m_plane; }

    /// 表示の中心 (世界座標) と、画面 1px あたりの世界の長さ。
    void  SetFocus(const math::Vector3& center, float pixelsPerMeter);
    float PixelsPerMeter() const { return m_pixelsPerMeter; }

    [[nodiscard]] ImVec2 ToScreen(const math::Vector3& world) const;
    [[nodiscard]] float  ToPixels(float meters) const { return meters * m_pixelsPerMeter; }

    void DrawGrid(float stepMeters);
    void DrawLine(const math::Vector3& a, const math::Vector3& b, ImU32 color,
                  float thickness = 1.5f);
    void DrawArrow(const math::Vector3& from, const math::Vector3& to, ImU32 color,
                   float thickness = 2.0f);
    void DrawCircle(const math::Vector3& center, float radiusMeters, ImU32 color,
                    bool filled = false, float thickness = 1.5f);
    /// 平面に平行な軸整合の箱。halfExtents は世界の長さ。
    void DrawBox(const math::Vector3& center, const math::Vector3& halfExtents, ImU32 color,
                 bool filled = false, float thickness = 1.5f);
    void DrawPoint(const math::Vector3& position, ImU32 color, float radiusPixels = 4.0f);
    void DrawText(const math::Vector3& position, ImU32 color, const char* text);
    /// 無限平面を表す水平線 (XY のとき y = height、XZ のときは描かない)。
    void DrawGroundLine(float height, ImU32 color);

private:
    [[nodiscard]] ImVec2 PlaneCoords(const math::Vector3& world) const;

    ImDrawList*   m_drawList       = nullptr;
    ImVec2        m_origin{};   ///< 子ウィンドウ左上のスクリーン座標
    ImVec2        m_size{};
    ViewPlane     m_plane          = ViewPlane::XY;
    math::Vector3 m_center{0.0f, 0.0f, 0.0f};
    float         m_pixelsPerMeter = 80.0f;
};

namespace colors {
inline constexpr ImU32 kGrid     = IM_COL32(70, 76, 88, 255);
inline constexpr ImU32 kAxis     = IM_COL32(110, 118, 134, 255);
inline constexpr ImU32 kGround   = IM_COL32(150, 158, 172, 255);
inline constexpr ImU32 kBody     = IM_COL32(96, 170, 255, 255);
inline constexpr ImU32 kBodyAlt  = IM_COL32(255, 176, 84, 255);
inline constexpr ImU32 kContact  = IM_COL32(255, 92, 106, 255);
inline constexpr ImU32 kNormal   = IM_COL32(122, 230, 140, 255);
inline constexpr ImU32 kHint     = IM_COL32(190, 196, 208, 255);
inline constexpr ImU32 kQuery    = IM_COL32(232, 214, 96, 255);
} // namespace colors

} // namespace fbzz::bench
