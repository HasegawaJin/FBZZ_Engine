/// @file    RenderSceneDraw.hpp
/// @brief   抽出済みメッシュから描画定数と前方描画コマンドを構築する共通入口。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Pipeline/RenderConstants.hpp>
#include <Graphics/Renderer/RenderGeometryInput.hpp>

namespace fbzz::renderer {
struct DrawCall;
struct RenderPassContext;

/// @note objectParams.z/w は頂点シェーダーが参照する GPU ボーンパレットの有無と行オフセット。
[[nodiscard]] inline PerObjectCB ObjectConstants(const RenderObject& object)
{
    PerObjectCB data{};
    data.world = object.world;
    data.worldInvTranspose = object.worldInvTranspose;
    data.objectParams.x = object.lodDither;
    data.objectParams.z = object.gpuBones.current.IsValid() ? 1.0f : 0.0f;
    data.objectParams.w = static_cast<float>(object.gpuBones.currentOffset);
    return data;
}

/// @note skinning・material・shadow の確定済みハンドルを借用し、資源の所有権は移さない。
/// @param bindIbl false なら IBL の t16..t18 を割り当てない。
FBZZ_GRAPHICS_API DrawCall ForwardDraw(RenderPassContext& ctx, const RenderObject& object,
                     const RenderMeshItem& item, bool bindIbl = true);

}
