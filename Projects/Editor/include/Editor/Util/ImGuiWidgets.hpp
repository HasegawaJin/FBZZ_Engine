// FBZZ Engine
// ImGuiWidgets.hpp | fbzz::editor
// プロジェクト固有の ImGui カスタムウィジェット集
#pragma once
#include <imgui.h>
#include <algorithm>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Math/Quaternion.hpp>
#include <cmath>
#include <string>

namespace fbzz::renderer { class ResourceManager; class IImGuiRenderer; }

namespace fbzz::editor::widgets {

// Vector3 の DragFloat3 (ラベル幅を統一)
bool DragVec3(const char* label, math::Vector3& v, float speed = 0.1f,
              float min = 0.0f, float max = 0.0f);

// RGB カラーピッカー (Vector3 を [0,1] で扱う)
bool ColorEdit3(const char* label, math::Vector3& color);

// RGBA カラーピッカー (Vector4 を [0,1] で扱う)
inline bool ColorEdit4(const char* label, math::Vector4& color) {
    float v[4] = { color.x, color.y, color.z, color.w };
    if (ImGui::ColorEdit4(label, v)) {
        color = { v[0], v[1], v[2], v[3] };
        return true;
    }
    return false;
}

// セクションヘッダー (太字テキスト + 区切り線)
void SectionHeader(const char* label);

// 色付きテキスト
void ColoredText(const char* text, ImVec4 color);

// 読み取り専用テキストフィールド
void ReadOnlyText(const char* label, const char* text);

// レンジ付き数値フィールド: スライダー (ゲージ) + 編集可能な数値入力ボックスを 1 行に並べる。
// WHY: SliderFloat 単体は正確な値入力がしづらく、DragFloat 単体は範囲内の量感が掴めない。
//      ゲージで量感とドラッグ操作を、右の入力ボックスで正確なタイプ入力を同時に満たす。
//      スクリプトの FBZZ_FIELD_RANGE (ImGuiReflector::FloatRange) から共通で使う。
// @return true if value changed
bool RangeField(const char* label, float& value, float min, float max,
                const char* fmt = "%.3f");

// アセットパス入力フィールド。"..." ボタンで projectRoot/ 以下を検索できるモーダルを開く。
// filterExts: カンマ区切り拡張子 ".mat,.hlsl" (空 = すべてのファイル)
// @return true if path was changed (InputText 編集 / drag-drop / picker 選択のいずれか)
bool AssetPathField(const char* label, std::string& path,
                    const char* filterExts,
                    const std::string& projectRoot);

// AssetPathField + ロードコールバック付き版。
// WHY: パス変更時に必ずアセット再ロードが必要なパターン (MeshRenderer/SkinnedMesh 等) の
//      if (AssetPathField(...)) { reload(); } ボイラープレートを排除する。
// @return true if path was changed (AssetPathField と同じ)
template<typename Fn>
inline bool AssetPathFieldWithLoad(const char* label, std::string& path,
                                   const char* filterExts,
                                   const std::string& projectRoot,
                                   Fn&& onLoad)
{
    if (AssetPathField(label, path, filterExts, projectRoot)) {
        std::forward<Fn>(onLoad)();
        return true;
    }
    return false;
}

// InspectorPanel の OnRenderContent 先頭で毎フレーム 1 回だけ呼ぶ。
// resources / imgui を渡すとピッカーに Unity 風のサムネイル可視化が有効になる
// (画像はテクスチャプレビュー、.mat はアルベドのテクスチャ/色スウォッチ)。null でもリスト表示は動く。
void DrawAssetPickerModal(renderer::ResourceManager* resources = nullptr,
                          renderer::IImGuiRenderer* imgui = nullptr);

// アセット検索ピッカーを任意の文字列ターゲットに対して開く (AssetPathField の "..." と同じ実体)。
// WHY: 独自描画のアセットスロット (参照ボタン形式) からも「パス検索」を使えるようにするための公開口。
//      選択時に target へ正規化済み相対パスが書き込まれる。描画は DrawAssetPickerModal() が担う。
//   filterExts: カンマ区切り拡張子 (".prefab" / ".fzdata" 等、空ですべて)
//   anchorPos : ポップアップを出す画面座標 (通常は呼び出し元ボタンの直下)
void OpenAssetPicker(std::string& target, const char* filterExts,
                     const std::string& projectRoot, ImVec2 anchorPos);

// 直前のアイテムを ASSET_PATH ドラッグ＆ドロップの受け皿にする共通ヘルパー。
// WHY: BeginDragDropTarget / AcceptDragDropPayload("ASSET_PATH") / Normalize / End の
//      定型がパス欄やリスト行に散在していたため集約する。ドロップ後の処理 (拡張子除去・
//      リロード等の特殊挙動) は呼び出し側に委ねるので、既存挙動を保ったまま重複だけ消せる。
// @return true if an asset path was dropped (outPath に正規化済みパスを格納)
bool AcceptAssetPathDrop(std::string& outPath);

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
