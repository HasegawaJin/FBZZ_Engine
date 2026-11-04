/// @file    RenderSceneDraw.hpp
/// @brief   抽出済みメッシュから前方描画コマンドを構築する共通入口。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Graphics/Api.hpp>

namespace fbzz::renderer {
struct DrawCall;
struct RenderPassContext;
struct RenderObject;
struct RenderMeshItem;

/// @note skinning・material・shadow の確定済みハンドルを借用し、資源の所有権は移さない。
/// @param bindIbl false なら IBL の t16..t18 を割り当てない。
FBZZ_GRAPHICS_API DrawCall ForwardDraw(RenderPassContext& ctx, const RenderObject& object,
                     const RenderMeshItem& item, bool bindIbl = true);

}
