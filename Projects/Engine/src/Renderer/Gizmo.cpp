// FBZZ Engine
// Gizmo.cpp | fbzz::renderer
// Gizmo クラスの実装。DebugDraw の低レベル API を組み合わせて AI デバッグ向け複合プリミティブを描く。
// BeginFrame/Flush の外から呼ばれた場合は DebugDraw の assert が検知する。
#include <Engine/Renderer/Gizmo.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Math/MathUtils.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>

namespace fbzz::renderer {

namespace {

constexpr float PI           = 3.14159265358979f;
constexpr float DEG_TO_RAD   = PI / 180.0f;
// 視野錐弧の分割数。細かすぎると頂点が増えるので 20 で十分な滑らかさ。
constexpr int   CONE_SEGS    = 20;
// 矢印ヘッドの比率 (シャフト長に対する割合の上限)
constexpr float ARROW_HEAD_RATIO    = 0.2f;
constexpr float ARROW_RADIUS_RATIO  = 0.05f;

// 方向ベクトル dir に直交する right / up 軸ペアを返す。
// WHY: 視野錐や円弧の計算で dir に直交する 2 軸が必要になるため共通化する。
void BuildOrthoBasis(const math::Vector3& dir,
                     math::Vector3& outRight,
                     math::Vector3& outUp)
{
    outRight = (std::abs(dir.y) < 0.99f)
        ? math::Vector3::Cross(dir, { 0.0f, 1.0f, 0.0f }).Normalized()
        : math::Vector3::Cross(dir, { 1.0f, 0.0f, 0.0f }).Normalized();
    outUp = math::Vector3::Cross(outRight, dir).Normalized();
}

} // namespace

// =============================================================================
// 汎用
// =============================================================================

void Gizmo::TransformAxes(IRenderer& r,
                            const math::Vector3& pos,
                            const math::Quaternion& rot,
                            float size)
{
    const math::Vector3 right = rot * math::Vector3{ 1.0f, 0.0f, 0.0f };
    const math::Vector3 up    = rot * math::Vector3{ 0.0f, 1.0f, 0.0f };
    const math::Vector3 fwd   = rot * math::Vector3{ 0.0f, 0.0f, 1.0f };

    const float headLen = size * ARROW_HEAD_RATIO;
    const float headRad = size * ARROW_RADIUS_RATIO;

    DebugDraw::Arrow(r, pos, pos + right * size, headLen, headRad, { 1.0f, 0.0f, 0.0f, 1.0f });
    DebugDraw::Arrow(r, pos, pos + up    * size, headLen, headRad, { 0.0f, 1.0f, 0.0f, 1.0f });
    DebugDraw::Arrow(r, pos, pos + fwd   * size, headLen, headRad, { 0.0f, 0.0f, 1.0f, 1.0f });
}

// =============================================================================
// AI デバッグ
// =============================================================================

void Gizmo::SightCone(IRenderer& r,
                       const math::Vector3& position,
                       const math::Vector3& forward,
                       float fovDegrees,
                       float range,
                       const math::Vector4& color)
{
    if (range < 1e-6f || fovDegrees < 1e-3f) return;

    // forward に直交する right を求める。水平面上の扇形なので world-up を優先する。
    // WHY: 敵 AI の視野は通常水平方向の扇形であり、world-up 基準の right が直感的。
    math::Vector3 right, up;
    BuildOrthoBasis(forward, right, up);

    const float halfRad = fovDegrees * 0.5f * DEG_TO_RAD;

    // ── 弧 (円弧端点を繋ぐ線分列) ─────────────────────────────────────────
    // WHAT: forward を 0 度として -halfRad 〜 +halfRad の範囲で CONE_SEGS 分割した弧を描く。
    const float step = (2.0f * halfRad) / static_cast<float>(CONE_SEGS);
    math::Vector3 prev = position
        + (forward * std::cos(-halfRad) + right * std::sin(-halfRad)) * range;

    for (int i = 1; i <= CONE_SEGS; ++i) {
        const float angle = -halfRad + step * static_cast<float>(i);
        const math::Vector3 cur = position
            + (forward * std::cos(angle) + right * std::sin(angle)) * range;
        DebugDraw::Line(r, prev, cur, color);
        prev = cur;
    }

    // ── 両端の辺 (原点 → 弧端点) ─────────────────────────────────────────
    const math::Vector3 leftEdge  = position
        + (forward * std::cos(-halfRad) + right * std::sin(-halfRad)) * range;
    const math::Vector3 rightEdge = position
        + (forward * std::cos( halfRad) + right * std::sin( halfRad)) * range;

    DebugDraw::Line(r, position, leftEdge,  color);
    DebugDraw::Line(r, position, rightEdge, color);

    // ── 注視方向の中心線 (短め) ────────────────────────────────────────────
    // WHY: 正面方向を一本引くことで、視野の中心がどこを向いているか一目で分かる。
    DebugDraw::Line(r, position, position + forward * range, color);
}

void Gizmo::WaypointPath(IRenderer& r,
                           std::span<const math::Vector3> waypoints,
                           bool loop,
                           const math::Vector4& color)
{
    const int count = static_cast<int>(waypoints.size());
    if (count < 2) {
        // ウェイポイントが 1 つの場合は小球だけ描いて終了
        if (count == 1)
            DebugDraw::Sphere(r, waypoints[0], 0.15f, color);
        return;
    }

    // 各ウェイポイントを小球でマーク
    for (const auto& wp : waypoints)
        DebugDraw::Sphere(r, wp, 0.12f, color);

    // 連続する 2 点を矢印で繋ぐ
    for (int i = 0; i < count - 1; ++i) {
        const math::Vector3& from = waypoints[i];
        const math::Vector3& to   = waypoints[i + 1];
        const float dist = (to - from).Length();
        if (dist < 1e-6f) continue;

        const float headLen = std::min(0.3f, dist * ARROW_HEAD_RATIO);
        const float headRad = headLen * 0.25f;
        DebugDraw::Arrow(r, from, to, headLen, headRad, color);
    }

    // loop=true なら末尾→先頭も結ぶ
    if (loop) {
        const math::Vector3& from = waypoints[count - 1];
        const math::Vector3& to   = waypoints[0];
        const float dist = (to - from).Length();
        if (dist >= 1e-6f) {
            const float headLen = std::min(0.3f, dist * ARROW_HEAD_RATIO);
            DebugDraw::Arrow(r, from, to, headLen, headLen * 0.25f, color);
        }
    }
}

void Gizmo::DetectionRange(IRenderer& r,
                             const math::Vector3& center,
                             float innerRadius,
                             float outerRadius,
                             const math::Vector4& innerColor,
                             const math::Vector4& outerColor)
{
    // WHY: 2 種の球を別色で描くことで「攻撃範囲」と「索敵範囲」が色で区別できる。
    if (innerRadius > 1e-6f)
        DebugDraw::Sphere(r, center, innerRadius, innerColor);
    if (outerRadius > innerRadius)
        DebugDraw::Sphere(r, center, outerRadius, outerColor);
}

void Gizmo::TargetLine(IRenderer& r,
                        const math::Vector3& from,
                        const math::Vector3& to,
                        const math::Vector4& color)
{
    const float dist = (to - from).Length();
    if (dist < 1e-6f) return;

    const float headLen = std::min(0.35f, dist * ARROW_HEAD_RATIO);
    const float headRad = headLen * 0.3f;

    // 矢印本体
    DebugDraw::Arrow(r, from, to, headLen, headRad, color);

    // ターゲット位置に小球を描いて「どこを狙っているか」を強調する
    DebugDraw::Sphere(r, to, headRad * 1.5f, color);
}

} // namespace fbzz::renderer
