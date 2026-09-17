/// @file    ScriptDebugProxy.hpp
/// @brief   Script からログとデバッグ描画を扱うショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#pragma once

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <span>
#include <string_view>

namespace fbzz::scene {

class Script;

/// @brief ログと、任意のタイミングから積めるデバッグ描画。
/// @note 描画は Scene View の «Script Gizmos» が点いているときだけ出る。配布ビルドでは出ない。
/// @note duration は秒。0 はこのフレームだけ。座標はすべてワールド空間。
struct ScriptDebugProxy {
    Script* script = nullptr;
    /// 以降の Draw* をシーン深度で遮蔽させるか。既定 false (物越しに見える)。
    bool depthTest = false;

    void Log(std::string_view msg) const;
    void LogWarning(std::string_view msg) const;
    void LogError(std::string_view msg) const;

    void SetDepthTest(bool enabled) { depthTest = enabled; }

    void DrawLine(const math::Vector3& a, const math::Vector3& b, const math::Vector4& color, float duration = 0.0f) const;
    void DrawSphere(const math::Vector3& center, float radius, const math::Vector4& color, float duration = 0.0f) const;
    void DrawBox(const math::Vector3& center, const math::Vector3& halfExtents, const math::Vector4& color, float duration = 0.0f) const;
    void DrawBox(const math::Vector3& center, const math::Vector3& halfExtents, const math::Quaternion& rotation,
                 const math::Vector4& color, float duration = 0.0f) const;
    void DrawRay(const math::Vector3& origin, const math::Vector3& dir, const math::Vector4& color, float duration = 0.0f) const;
    void DrawArrow(const math::Vector3& from, const math::Vector3& to,
                   float headLength = 0.2f, float headRadius = 0.05f,
                   const math::Vector4& color = { 1,1,0,1 }, float duration = 0.0f) const;
    void DrawCone(const math::Vector3& apex, const math::Vector3& direction,
                  float height = 1.0f, float baseRadius = 0.3f,
                  const math::Vector4& color = { 1,1,0,1 }, float duration = 0.0f) const;
    /// @param halfHeight 半球中心までの距離 (半球を含まない)。軸は rotation のローカル Y。
    void DrawCapsule(const math::Vector3& center, float radius, float halfHeight,
                     const math::Quaternion& rotation, const math::Vector4& color, float duration = 0.0f) const;
    void DrawCircle(const math::Vector3& center, const math::Vector3& normal, float radius,
                    const math::Vector4& color, float duration = 0.0f) const;
    /// @param angleDegrees fromDirection から normal まわりに回る角度 (右手系)。
    void DrawArc(const math::Vector3& center, const math::Vector3& normal, const math::Vector3& fromDirection,
                 float radius, float angleDegrees, const math::Vector4& color, float duration = 0.0f) const;
    void DrawPolyline(std::span<const math::Vector3> points, bool closed, const math::Vector4& color,
                      float duration = 0.0f) const;
};

} // namespace fbzz::scene
