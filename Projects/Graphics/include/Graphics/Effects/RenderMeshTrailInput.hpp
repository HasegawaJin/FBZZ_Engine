/// @file    RenderMeshTrailInput.hpp
/// @brief   メッシュ残像の解決済み姿勢・骨・材質入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/RenderConstants.hpp>
namespace fbzz::renderer {
struct RenderMeshTrailInput {
    uint32_t layer = 0;
    PerObjectCB object{};
    math::Vector4 color;
    ResourceHandle<BufferTag> vertices, indices;
    uint32_t indexCount = 0, vertexCount = 0;
    ResourceHandle<ShaderTag> shader;
    ResourceHandle<ConstantBufferTag> material, colorBuffer, skin;
    ResourceHandle<TextureTag> texture;
    bool doubleSided = false, skinned = false;
};
}
