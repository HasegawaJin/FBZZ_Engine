#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include "IBuffer.hpp"
#include "IShader.hpp"
#include "IPipelineState.hpp"
#include "IConstantBuffer.hpp"
#include "ITexture.hpp"
#include "RenderLayer.hpp"
#include "RenderState.hpp"

namespace fbzz::renderer {

struct DrawCall {
    std::shared_ptr<IBuffer>        vertexBuffer;
    std::shared_ptr<IBuffer>        indexBuffer;      // nullptr = 非インデックス描画
    std::shared_ptr<IShader>        shader;
    std::shared_ptr<IPipelineState> pipelineState;

    // スロット 0〜7 の定数バッファ (b0〜b6 を収容)
    std::array<std::shared_ptr<IConstantBuffer>, 8> constantBuffers = {};
    // スロット 0〜15 のテクスチャ (t0〜t12 を収容)
    std::array<std::shared_ptr<ITexture>, 16>       textures        = {};

    uint32_t indexCount  = 0;
    uint32_t vertexCount = 0;
    uint32_t startIndex  = 0;
    uint32_t baseVertex  = 0;

    RenderLayer      layer    = RenderLayer::OPAQUE;
    PrimitiveTopology topology = PrimitiveTopology::TRIANGLE_LIST;
};

} // namespace fbzz::renderer