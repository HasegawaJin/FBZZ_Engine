/// @file    FluidPartOverlay.hpp
/// @brief   Fluid Editor のビューポートに部品 (発生源・力・障害物) を重ねて描き、つかめる点 (ハンドル) を返す
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// 2D のビューポートは領域 [-1,1] (y 上向き) を正方形へ写す。3D の奥行きは見ない (z はそのまま残す)。
/// ドラッグで何を書き換えるかもここが決める (パネルは «どのハンドルを、どこへ» だけを渡す)。
#pragma once

#include <Editor/Util/FluidDocument.hpp>
#include <Engine/Asset/FluidRecipe.hpp>
#include <Math/Vector3.hpp>
#include <imgui.h>

#include <functional>
#include <vector>

namespace fbzz::editor {

/// 領域 [-1,1]² ↔ 画面の正方形 (origin = 左上、size = 1 辺の px)。
struct FluidViewMapping {
    ImVec2 origin{ 0.0f, 0.0f };
    float size = 1.0f;

    [[nodiscard]] ImVec2 ToScreen(const math::Vector3& domain) const
    {
        return { origin.x + (domain.x + 1.0f) * 0.5f * size, origin.y + (1.0f - domain.y) * 0.5f * size };
    }
    /// z は 0 (呼び手が元の z を残す)。
    [[nodiscard]] math::Vector3 ToDomain(const ImVec2& screen) const
    {
        return { (screen.x - origin.x) / size * 2.0f - 1.0f, 1.0f - (screen.y - origin.y) / size * 2.0f, 0.0f };
    }
    /// 領域の長さ → px。
    [[nodiscard]] float ToPixels(float domainLength) const { return domainLength * 0.5f * size; }
};

enum class FluidHandleKind : std::uint8_t {
    Center,     ///< 部品の中心 (動きがあれば time の位置)。ドラッグで基準の center を動かす
    Size,       ///< 大きさ (半径・半幅)。ドラッグで size を変える
    Direction,  ///< 向きの矢先 (円錐・輪・板・カプセル・円柱・風・渦・平面)。ドラッグで direction を変える
    MotionKey,  ///< 動きのキー。ドラッグでそのキーの offset を変える
};

struct FluidPartHandle {
    FluidSelectionKind list = FluidSelectionKind::None;  ///< Source / Force / Collider
    int index = -1;
    FluidHandleKind kind = FluidHandleKind::Center;
    int keyIndex = -1;       ///< MotionKey のときだけ
    ImVec2 screen{ 0.0f, 0.0f };
    float radius = 6.0f;     ///< つかめる半径 [px]
};

/// 部品を表示から外すか (hide/solo)。空なら全部描く。
using FluidPartVisibility = std::function<bool(FluidSelectionKind list, int index)>;

/// 部品の形・向き・動きの道筋・キーを描く。選択中の部品は強調し、ハンドルも描く。time は warmup の後を 0 とする秒。
/// warmup は recipe.output.warmup (動きの評価は warmup + time で行う — ソルバーの時計と同じ)。
void DrawFluidPartOverlays(ImDrawList* drawList, const FluidViewMapping& mapping, const asset::FluidRecipe& recipe,
                           float time, const FluidSelection& selection, const FluidPartVisibility& visible);

/// 今つかめる点の一覧。選択中の部品は全ハンドル、それ以外は Center だけ (つかむと選択が移る)。
[[nodiscard]] std::vector<FluidPartHandle> CollectFluidPartHandles(const FluidViewMapping& mapping,
                                                                   const asset::FluidRecipe& recipe, float time,
                                                                   const FluidSelection& selection,
                                                                   const FluidPartVisibility& visible);

/// mouse に一番近いハンドル (つかめる半径の中で)。無ければ nullptr。
[[nodiscard]] const FluidPartHandle* PickFluidPartHandle(const std::vector<FluidPartHandle>& handles, ImVec2 mouse);

/// ハンドルを domainPosition (領域座標。z は呼び手が元の値を残して渡す) まで動かしたときのレシピの書き換え。
/// Center は動きがあっても基準の center を «今の見かけの位置が domainPosition になる» ように動かす。
void ApplyFluidHandleDrag(asset::FluidRecipe& recipe, const FluidPartHandle& handle,
                          const math::Vector3& domainPosition, float time);

/// 画面上の点が部品の形の上か (Center のハンドルが隠れていても形をクリックして選べるように)。一番上の部品を返す。
[[nodiscard]] FluidSelection PickFluidPart(const FluidViewMapping& mapping, const asset::FluidRecipe& recipe,
                                           float time, ImVec2 mouse, const FluidPartVisibility& visible);

} // namespace fbzz::editor
