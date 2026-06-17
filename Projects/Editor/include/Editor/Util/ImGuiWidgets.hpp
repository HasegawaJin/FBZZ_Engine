// FBZZ Engine
// ImGuiWidgets.hpp | fbzz::editor
// プロジェクト固有の ImGui カスタムウィジェット集
#pragma once
#include <imgui.h>
#include <algorithm>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>
#include <cmath>
#include <string>

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

// アセットパス入力フィールド。"..." ボタンで projectRoot/ 以下を検索できるモーダルを開く。
// filterExts: カンマ区切り拡張子 ".mat,.hlsl" (空 = すべてのファイル)
// @return true if path was changed (InputText 編集 / drag-drop / picker 選択のいずれか)
bool AssetPathField(const char* label, std::string& path,
                    const char* filterExts,
                    const std::string& projectRoot);

// InspectorPanel の OnRenderContent 先頭で毎フレーム 1 回だけ呼ぶ
void DrawAssetPickerModal();

// Quaternion → オイラー角 (度, YXZ 順)。InspectorPanel と ImGuiReflector で共用
// YXZ 内因順: X(Pitch) が中間角で ±90° 制約、Y(Yaw) は ±180° 任意範囲。
inline math::Vector3 QuatToEulerDeg(const math::Quaternion& q)
{
    constexpr float DEG = 180.0f / 3.14159265f;
    const math::Matrix4 m = math::Matrix4::Rotate(q);

    // R = Ry * Rx * Rz: R[1][2] = -sin(X)
    float x = std::asin((std::max)(-1.0f, (std::min)(1.0f, -m.m[1][2])));
    float y = 0.0f;
    float z = 0.0f;

    if (std::abs(std::cos(x)) > 1e-6f) {
        // R[0][2] = sin(Y)*cos(X), R[2][2] = cos(Y)*cos(X)
        y = std::atan2(m.m[0][2], m.m[2][2]);
        // R[1][0] = cos(X)*sin(Z), R[1][1] = cos(X)*cos(Z)
        z = std::atan2(m.m[1][0], m.m[1][1]);
    } else {
        // ジンバルロック (X ≈ ±90°): Z=0 とし Y を復元
        y = std::atan2(-m.m[2][0], m.m[0][0]);
    }

    return { x * DEG, y * DEG, z * DEG };
}

// オイラー角 (度, YXZ 順) → Quaternion
inline math::Quaternion EulerDegToQuat(const math::Vector3& deg)
{
    constexpr float RAD = 3.14159265f / 180.0f;
    return math::Quaternion::FromEuler({ deg.x * RAD, deg.y * RAD, deg.z * RAD });
}

inline float SanitizeEulerDeg(float value)
{
    if (std::abs(value) < 0.0001f) return 0.0f;
    return value;
}

inline bool DragQuatEuler3(const char* label, math::Quaternion& q, float speed = 0.5f)
{
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID id = ImGui::GetID(label);
    const ImGuiID initKey = id + 1;
    const ImGuiID eulerXKey = id + 2;
    const ImGuiID eulerYKey = id + 3;
    const ImGuiID eulerZKey = id + 4;
    const ImGuiID quatXKey = id + 5;
    const ImGuiID quatYKey = id + 6;
    const ImGuiID quatZKey = id + 7;
    const ImGuiID quatWKey = id + 8;

    const bool initialized = storage->GetBool(initKey, false);
    const math::Quaternion normalized = q.Normalized();
    const float prevX = storage->GetFloat(quatXKey, normalized.x);
    const float prevY = storage->GetFloat(quatYKey, normalized.y);
    const float prevZ = storage->GetFloat(quatZKey, normalized.z);
    const float prevW = storage->GetFloat(quatWKey, normalized.w);
    const bool quaternionChanged = !initialized ||
        std::abs(prevX - normalized.x) > 0.0001f ||
        std::abs(prevY - normalized.y) > 0.0001f ||
        std::abs(prevZ - normalized.z) > 0.0001f ||
        std::abs(prevW - normalized.w) > 0.0001f;

    if (quaternionChanged) {
        const math::Vector3 euler = QuatToEulerDeg(normalized);
        storage->SetFloat(eulerXKey, SanitizeEulerDeg(euler.x));
        storage->SetFloat(eulerYKey, SanitizeEulerDeg(euler.y));
        storage->SetFloat(eulerZKey, SanitizeEulerDeg(euler.z));
        storage->SetFloat(quatXKey, normalized.x);
        storage->SetFloat(quatYKey, normalized.y);
        storage->SetFloat(quatZKey, normalized.z);
        storage->SetFloat(quatWKey, normalized.w);
        storage->SetBool(initKey, true);
    }

    float values[3] = {
        storage->GetFloat(eulerXKey, 0.0f),
        storage->GetFloat(eulerYKey, 0.0f),
        storage->GetFloat(eulerZKey, 0.0f)
    };

    if (!ImGui::DragFloat3(label, values, speed)) return false;

    values[0] = SanitizeEulerDeg(values[0]);
    values[1] = SanitizeEulerDeg(values[1]);
    values[2] = SanitizeEulerDeg(values[2]);
    q = EulerDegToQuat({ values[0], values[1], values[2] }).Normalized();

    storage->SetFloat(eulerXKey, values[0]);
    storage->SetFloat(eulerYKey, values[1]);
    storage->SetFloat(eulerZKey, values[2]);
    storage->SetFloat(quatXKey, q.x);
    storage->SetFloat(quatYKey, q.y);
    storage->SetFloat(quatZKey, q.z);
    storage->SetFloat(quatWKey, q.w);
    storage->SetBool(initKey, true);
    return true;
}

} // namespace fbzz::editor::widgets
