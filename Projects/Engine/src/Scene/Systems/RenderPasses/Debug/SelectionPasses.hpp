/// @file    SelectionPasses.hpp
/// @brief   Selection mask render pass declarations.
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/Systems/RenderPasses/PassResources.hpp>

namespace fbzz::renderer { struct RenderSettings; }

namespace fbzz::scene {

struct RenderPassContext;
class GameObject;

/// @note PassResources を先頭で受ける理由: RT は «申告した名前» からしか引けない。ctx.handles を直接触れる限り、申告と束縛は別々に書き換えられてしまう。
FBZZ_ENGINE_API void ExecuteSelectionMaskPass(PassResources& res, RenderPassContext& ctx);
FBZZ_ENGINE_API void ExecuteSelectionOutlinePass(PassResources& res, RenderPassContext& ctx);

/// @note 選択輪郭を出す対象か。UI 要素の選択マスクも同じ判定で拾う。
[[nodiscard]] FBZZ_ENGINE_API bool IsSelectedForOutline(const GameObject& go,
                                        const renderer::RenderSettings& settings);

} /// @note namespace fbzz::scene
