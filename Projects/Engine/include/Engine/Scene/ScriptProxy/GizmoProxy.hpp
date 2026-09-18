/// @file    GizmoProxy.hpp
/// @brief   OnDrawGizmos / OnDrawGizmosSelected の中から Gizmo / DebugDraw を呼ぶプロキシ。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#pragma once

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <span>

namespace fbzz::renderer {
class IRenderer;
}

namespace fbzz::scene {

/// @brief ギズモ描画の即時 API。座標はすべてワールド空間。
/// @pre OnDrawGizmos / OnDrawGizmosSelected の中でだけ呼ぶ (それ以外は assert)。
/// @note 深度テストの指定はコールバック 1 回ごとに既定 (off) へ戻る。
struct GizmoProxy {
    /// エンジンがコールバックの前に注入し、後で nullptr へ戻す。スクリプトは触らない。
    renderer::IRenderer* renderer = nullptr;

    /// 以降の線をシーン深度で遮蔽させるか。
    void SetDepthTest(bool enabled) const;

    /// @brief XYZ 軸を 3 色の矢印で描く (X=赤, Y=緑, Z=青)。
    void DrawTransformAxes(const math::Vector3& position,
                            const math::Quaternion& rotation,
                            float size = 1.0f) const;

    void DrawLine(const math::Vector3& from, const math::Vector3& to,
                  const math::Vector4& color = { 1.0f, 1.0f, 1.0f, 1.0f }) const;

    void DrawArrow(const math::Vector3& from, const math::Vector3& to,
                   float headLength = 0.2f, float headRadius = 0.05f,
                   const math::Vector4& color = { 1.0f, 1.0f, 0.0f, 1.0f }) const;

    void DrawSphere(const math::Vector3& center, float radius,
                    const math::Vector4& color = { 0.0f, 1.0f, 0.0f, 1.0f }) const;

    void DrawBox(const math::Vector3& center, const math::Vector3& halfExtents,
                 const math::Vector4& color = { 0.0f, 1.0f, 0.0f, 1.0f }) const;

    void DrawBox(const math::Vector3& center, const math::Vector3& halfExtents,
                 const math::Quaternion& rotation,
                 const math::Vector4& color = { 0.0f, 1.0f, 0.0f, 1.0f }) const;

    /// @param halfHeight 半球中心までの距離 (半球を含まない)。軸は rotation のローカル Y。
    void DrawCapsule(const math::Vector3& center, float radius, float halfHeight,
                     const math::Quaternion& rotation,
                     const math::Vector4& color = { 0.0f, 1.0f, 0.0f, 1.0f }) const;

    void DrawCone(const math::Vector3& apex, const math::Vector3& direction,
                  float height = 1.0f, float baseRadius = 0.3f,
                  const math::Vector4& color = { 1.0f, 1.0f, 0.0f, 1.0f }) const;

    void DrawCircle(const math::Vector3& center, const math::Vector3& normal, float radius,
                    const math::Vector4& color = { 1.0f, 1.0f, 1.0f, 1.0f }) const;

    /// @param angleDegrees fromDirection から normal まわりに回る角度 (右手系)。
    void DrawArc(const math::Vector3& center, const math::Vector3& normal,
                 const math::Vector3& fromDirection, float radius, float angleDegrees,
                 const math::Vector4& color = { 1.0f, 1.0f, 1.0f, 1.0f }) const;

    void DrawPolyline(std::span<const math::Vector3> points, bool closed = false,
                      const math::Vector4& color = { 1.0f, 1.0f, 1.0f, 1.0f }) const;

    /// @brief 水平面上の視野の扇形。fovDegrees は左右合計。
    void DrawSightCone(const math::Vector3& position,
                       const math::Vector3& forward,
                       float fovDegrees,
                       float range,
                       const math::Vector4& color = { 1.0f, 1.0f, 0.0f, 1.0f }) const;

    /// @brief waypoints を矢印の連鎖で結ぶ。loop で末尾→先頭も結ぶ。
    void DrawWaypointPath(std::span<const math::Vector3> waypoints,
                          bool loop = false,
                          const math::Vector4& color = { 0.0f, 1.0f, 1.0f, 1.0f }) const;

    /// @brief 内側 (攻撃) と外側 (索敵) の同心球。
    void DrawDetectionRange(const math::Vector3& center,
                             float innerRadius,
                             float outerRadius,
                             const math::Vector4& innerColor = { 1.0f, 0.3f, 0.3f, 1.0f },
                             const math::Vector4& outerColor = { 1.0f, 1.0f, 0.0f, 1.0f }) const;

    /// @brief 標的への矢印と標的位置の小球。
    void DrawTargetLine(const math::Vector3& from, const math::Vector3& to,
                        const math::Vector4& color = { 1.0f, 0.2f, 0.2f, 1.0f }) const;
};

} // namespace fbzz::scene
