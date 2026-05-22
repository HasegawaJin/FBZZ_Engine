// FBZZ Engine
// ImGuiWidgets.hpp | fbzz::editor
// プロジェクト固有の ImGui カスタムウィジェット集
#pragma once
#include <imgui.h>
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>
#include <cmath>

namespace fbzz::editor::widgets {

// Vector3 の DragFloat3 (ラベル幅を統一)
bool DragVec3(const char* label, math::Vector3& v, float speed = 0.1f,
              float min = 0.0f, float max = 0.0f);

// RGB カラーピッカー (Vector3 を [0,1] で扱う)
bool ColorEdit3(const char* label, math::Vector3& color);

// セクションヘッダー (太字テキスト + 区切り線)
void SectionHeader(const char* label);

// 色付きテキスト
void ColoredText(const char* text, ImVec4 color);

// 読み取り専用テキストフィールド
void ReadOnlyText(const char* label, const char* text);

// Quaternion → オイラー角 (度, XYZ 順)。InspectorPanel と ImGuiReflector で共用
inline math::Vector3 QuatToEulerDeg(const math::Quaternion& q)
{
    constexpr float DEG = 180.0f / 3.14159265f;

    float sinr = 2.0f * (q.w * q.x - q.y * q.z);
    float cosr = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    float roll  = std::atan2(sinr, cosr) * DEG;

    float sinp  = 2.0f * (q.w * q.y + q.z * q.x);
    float pitch = (std::abs(sinp) >= 1.0f)
                ? std::copysign(90.0f, sinp)
                : std::asin(sinp) * DEG;

    float siny = 2.0f * (q.w * q.z - q.x * q.y);
    float cosy = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    float yaw   = std::atan2(siny, cosy) * DEG;

    return { roll, pitch, yaw };
}

// オイラー角 (度, XYZ 順) → Quaternion
inline math::Quaternion EulerDegToQuat(const math::Vector3& deg)
{
    constexpr float RAD = 3.14159265f / 180.0f;
    return math::Quaternion::FromEuler({ deg.x * RAD, deg.y * RAD, deg.z * RAD });
}

} // namespace fbzz::editor::widgets
