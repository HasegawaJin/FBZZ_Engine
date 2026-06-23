// FBZZ Engine
// ScriptDebugProxy.hpp | fbzz::scene
// Script からログとデバッグ描画を扱うショートハンド
#pragma once

#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptDebugProxy {
    Script* script = nullptr;

    void Log(std::string_view msg) const;
    void LogWarning(std::string_view msg) const;
    void LogError(std::string_view msg) const;
    void DrawLine(const math::Vector3& a, const math::Vector3& b, const math::Vector4& color, float duration = 0.0f) const;
    void DrawSphere(const math::Vector3& center, float radius, const math::Vector4& color, float duration = 0.0f) const;
    void DrawBox(const math::Vector3& center, const math::Vector3& halfExtents, const math::Vector4& color, float duration = 0.0f) const;
    void DrawRay(const math::Vector3& origin, const math::Vector3& dir, const math::Vector4& color, float duration = 0.0f) const;
    // from → to の矢印 (シャフト + コーン型ヘッド)
    void DrawArrow(const math::Vector3& from, const math::Vector3& to,
                   float headLength = 0.2f, float headRadius = 0.05f,
                   const math::Vector4& color = { 1,1,0,1 }, float duration = 0.0f) const;
    // ワイヤーフレームのコーン
    void DrawCone(const math::Vector3& apex, const math::Vector3& direction,
                  float height = 1.0f, float baseRadius = 0.3f,
                  const math::Vector4& color = { 1,1,0,1 }, float duration = 0.0f) const;
};

} // namespace fbzz::scene
