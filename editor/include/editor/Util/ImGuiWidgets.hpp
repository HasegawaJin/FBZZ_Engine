// FBZZ Engine
// ImGuiWidgets.hpp | fbzz::editor
// プロジェクト固有の ImGui カスタムウィジェット集
#pragma once
#include <imgui.h>

namespace fbzz::math { struct Vector3; }

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

} // namespace fbzz::editor::widgets
