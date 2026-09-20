/// @file    RenderSkinningInput.hpp
/// @brief   解決済みバッファへの GPU スキニング要求。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <cstdint>
namespace fbzz::renderer {
struct RenderSkinningInput {
    ResourceHandle<StructuredBufferTag> sourceVertices;
    ResourceHandle<StructuredBufferTag> bonePalette;
    ResourceHandle<BufferTag> outputVertices;
    uint32_t vertexCount = 0;
};
}
