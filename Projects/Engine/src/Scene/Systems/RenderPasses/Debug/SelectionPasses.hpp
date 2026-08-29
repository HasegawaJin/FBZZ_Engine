/// @file    SelectionPasses.hpp
/// @brief   Selection mask render pass declarations.
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once

namespace fbzz::renderer { struct RenderSettings; }

namespace fbzz::scene {

struct RenderPassContext;
class GameObject;

void ExecuteSelectionMaskPass(RenderPassContext& ctx);
void ExecuteSelectionOutlinePass(RenderPassContext& ctx);

// 選択輪郭を出す対象か。UI 要素の選択マスクも同じ判定で拾う。
[[nodiscard]] bool IsSelectedForOutline(const GameObject& go,
                                        const renderer::RenderSettings& settings);

} // namespace fbzz::scene
