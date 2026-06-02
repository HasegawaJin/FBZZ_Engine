// FBZZ Engine
// GizmoProxy.hpp | fbzz::scene
// OnDrawGizmos() 内から Gizmo:: / DebugDraw:: を呼ぶための Script 向けプロキシ。
// WHY: Script が IRenderer や ResourceManager を直接参照しないよう、
//      RenderSystem が renderer ポインタを注入してから OnDrawGizmos() を呼ぶ設計にする。
//      ScriptDebugProxy (コマンドキュー経由) とは異なり、GizmoProxy は BeginFrame/Flush の
//      区間内で直接 DebugDraw / Gizmo を呼ぶため遅延なしで描画できる。
#pragma once

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <span>

namespace fbzz::renderer {
class IRenderer;
}

namespace fbzz::scene {

struct GizmoProxy {
    // RenderSystem が OnDrawGizmos() 呼び出し前に注入し、呼び出し後に nullptr に戻す。
    renderer::IRenderer* renderer = nullptr;

    // ── 汎用 ──────────────────────────────────────────────────────────────

    // XYZ 軸を 3 色矢印で描く (X=赤, Y=緑, Z=青)
    void DrawTransformAxes(const math::Vector3& position,
                            const math::Quaternion& rotation,
                            float size = 1.0f) const;

    // 任意の線分を描く
    void DrawLine(const math::Vector3& from, const math::Vector3& to,
                  const math::Vector4& color = { 1.0f, 1.0f, 1.0f, 1.0f }) const;

    // 矢印 (シャフト + コーン型ヘッド)
    void DrawArrow(const math::Vector3& from, const math::Vector3& to,
                   float headLength = 0.2f, float headRadius = 0.05f,
                   const math::Vector4& color = { 1.0f, 1.0f, 0.0f, 1.0f }) const;

    // ワイヤーフレーム球
    void DrawSphere(const math::Vector3& center, float radius,
                    const math::Vector4& color = { 0.0f, 1.0f, 0.0f, 1.0f }) const;

    // ワイヤーフレームボックス
    void DrawBox(const math::Vector3& center, const math::Vector3& halfExtents,
                 const math::Vector4& color = { 0.0f, 1.0f, 0.0f, 1.0f }) const;

    void DrawBox(const math::Vector3& center, const math::Vector3& halfExtents,
                 const math::Quaternion& rotation,
                 const math::Vector4& color = { 0.0f, 1.0f, 0.0f, 1.0f }) const;

    // ── AI デバッグ ─────────────────────────────────────────────────────

    // 視野錐 — forward 方向を中心に fovDegrees の水平扇形をワイヤーで描く
    void DrawSightCone(const math::Vector3& position,
                       const math::Vector3& forward,
                       float fovDegrees,
                       float range,
                       const math::Vector4& color = { 1.0f, 1.0f, 0.0f, 1.0f }) const;

    // ウェイポイント経路 — waypoints を矢印の連鎖で繋ぐ (loop=true で末尾→先頭も繋ぐ)
    void DrawWaypointPath(std::span<const math::Vector3> waypoints,
                          bool loop = false,
                          const math::Vector4& color = { 0.0f, 1.0f, 1.0f, 1.0f }) const;

    // 検知範囲 — 内側球 (攻撃範囲) と外側球 (索敵範囲) を別色で描く
    void DrawDetectionRange(const math::Vector3& center,
                             float innerRadius,
                             float outerRadius,
                             const math::Vector4& innerColor = { 1.0f, 0.3f, 0.3f, 1.0f },
                             const math::Vector4& outerColor = { 1.0f, 1.0f, 0.0f, 1.0f }) const;

    // ターゲットライン — AI から標的へ矢印 + 標的位置に小球
    void DrawTargetLine(const math::Vector3& from, const math::Vector3& to,
                        const math::Vector4& color = { 1.0f, 0.2f, 0.2f, 1.0f }) const;
};

} // namespace fbzz::scene
