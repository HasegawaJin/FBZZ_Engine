/// @file    ClothPinBrush.hpp
/// @brief   布の固定点ブラシのストローク状態とビューポート入口。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <array>
#include <string>
#include <vector>
#include <cstdint>
struct ImVec2;
namespace fbzz::scene { class Scene; }
namespace fbzz::editor {
struct EditorContext;
/// @note ドラッグ中は候補だけ保持し、離したとき Operator に一括適用する。中断時にシーンを戻す必要がない。
struct ClothPinBrush {
    float radius = 24.0f;
    float lastX = 0.0f;
    float lastY = 0.0f;
    bool dragging = false;
    bool remove = false;
    bool beforeOverride = false;
    int lastFrame = -1;
    scene::Scene* scene = nullptr;
    std::string target;
    std::string assetPath;
    uint64_t revision = 0;
    std::array<float, 9> shape{};
    std::vector<int> beforePins;
    std::vector<bool> touched;
};
/// @note 画面座標の円で奥の質点も選ぶ。候補は物理質点 ID なので UV seam も一度だけ編集する。
/// @see Docs/design/cloth.md 固定点ブラシ。
void DrawClothPinBrush(EditorContext& ctx, ClothPinBrush& brush, const ImVec2& min, const ImVec2& size, bool hovered, bool editable);
}
