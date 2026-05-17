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

namespace fbzz::renderer {

struct DrawCall {
    std::shared_ptr<IBuffer>        vertexBuffer;
    std::shared_ptr<IBuffer>        indexBuffer;      // nullptr = 非インデックス描画
    std::shared_ptr<IShader>        shader;
    std::shared_ptr<IPipelineState> pipelineState;

    // スロット 0〜3 の定数バッファ
    std::array<std::shared_ptr<IConstantBuffer>, 4> constantBuffers = {};
    // スロット 0〜7 のテクスチャ
    std::array<std::shared_ptr<ITexture>, 8>        textures        = {};

    uint32_t indexCount  = 0;
    uint32_t vertexCount = 0;
    uint32_t startIndex  = 0;
    uint32_t baseVertex  = 0;

    RenderLayer layer = RenderLayer::OPAQUE;  // 描画順制御 (render_queue.md 参照)
};

} // namespace fbzz::renderer