/// @file    RenderSceneDraw.cpp
/// @brief   Scene に依存しない前方描画コマンドの構築。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Graphics/Passes/Geometry/RenderSceneDraw.hpp>
#include "RenderScenePassHelpers.hpp"

namespace fbzz::renderer {

DrawCall ForwardDraw(RenderPassContext& ctx, const RenderObject& object,
                     const RenderMeshItem& item, bool bindIbl)
{
    return ForwardDrawWithBindings(ctx, object, item, ForwardPassBindings(ctx, bindIbl));
}

}
