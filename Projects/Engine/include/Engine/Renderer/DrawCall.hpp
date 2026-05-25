// FBZZ Engine
// DrawCall.hpp | fbzz::renderer
// レンダラーへ渡す描画要求データ
// 頂点・インデックス・シェーダー・定数バッファを ResourceHandle で参照する。
// RenderSystem が生成し、IRenderer が ResourceManager で実体へ解決する。
#pragma once
#include <array>
#include <cstdint>
#include "RenderLayer.hpp"
#include "RenderState.hpp"
#include "ResourceHandle.hpp"

namespace fbzz::renderer {

struct DrawCall {
    ResourceHandle<BufferTag> vertexBuffer;
    ResourceHandle<BufferTag> indexBuffer;
    ResourceHandle<ShaderTag> shader;
    ResourceHandle<PipelineStateTag> pipelineState;

    std::array<ResourceHandle<ConstantBufferTag>, 8> constantBuffers = {};
    std::array<ResourceHandle<TextureTag>, 16> textures = {};

    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    uint32_t startIndex = 0;
    uint32_t baseVertex = 0;

    RenderLayer layer = RenderLayer::OPAQUE;
    PrimitiveTopology topology = PrimitiveTopology::TRIANGLE_LIST;
};

} // namespace fbzz::renderer
