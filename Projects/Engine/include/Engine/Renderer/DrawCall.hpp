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

    // スロット割り当て (Constants.hlsli と同期すること):
    //   [0] = CameraConstants (b0)   [1] = ObjectConstants (b1)
    //   [2] = MaterialConstants (b2) [3] = LightConstants (b3)
    //   [4] = ShadowConstants (b4)   [5] = PostProcConstants (b5)
    //   [6] = AtmosphereConstants (b6) [7] = SkinningConstants (b7)
    std::array<ResourceHandle<ConstantBufferTag>, 8> constantBuffers = {};

    // スロット割り当て (Constants.hlsli と同期すること):
    //   [0]=Albedo [1]=Normal [5]=HDR [7]=Depth [8]=ShadowMap [10]=Bloom
    std::array<ResourceHandle<TextureTag>, 16> textures = {};

    uint32_t indexCount  = 0;
    uint32_t vertexCount = 0; // indexCount=0 かつ vertexCount>0 でインデックスなし描画 (フルスクリーントライアングル等)
    uint32_t startIndex  = 0;
    uint32_t baseVertex  = 0;

    RenderLayer layer = RenderLayer::OPAQUE_LAYER;
    PrimitiveTopology topology = PrimitiveTopology::TRIANGLE_LIST;
};

} // namespace fbzz::renderer
