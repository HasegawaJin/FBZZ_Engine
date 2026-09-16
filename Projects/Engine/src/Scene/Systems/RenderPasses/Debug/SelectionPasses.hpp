/// @file    SelectionPasses.hpp
/// @brief   Selection mask render pass declarations.
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Engine/Scene/Systems/RenderPasses/PassResources.hpp>

namespace fbzz::renderer { struct RenderSettings; }

namespace fbzz::scene {

struct RenderPassContext;
class GameObject;

// WHY PassResources を先頭で受けるか: RT は «申告した名前» からしか引かない。
//     ctx.handles を直接触れる限り、申告と束縛は別々に書き換えられてしまう。
void ExecuteSelectionMaskPass(PassResources& res, RenderPassContext& ctx);
void ExecuteSelectionOutlinePass(PassResources& res, RenderPassContext& ctx);

// 選択輪郭を出す対象か。UI 要素の選択マスクも同じ判定で拾う。
[[nodiscard]] bool IsSelectedForOutline(const GameObject& go,
                                        const renderer::RenderSettings& settings);

} // namespace fbzz::scene
