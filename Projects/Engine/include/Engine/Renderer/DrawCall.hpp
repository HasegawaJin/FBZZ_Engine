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
    // 両バックエンドで b0〜b13 を共通契約とし、DX12 は root CBV へ Submit 時の GPU VA を記録する。
    std::array<ResourceHandle<ConstantBufferTag>, 14> constantBuffers = {};

    // スロット割り当て (Constants.hlsli と同期すること):
    //   [0]=Albedo [1]=Normal [5]=HDR [7]=Depth [8]=ShadowMap [10]=Bloom
    // Advanced Graphicsがt16〜t24を使うため、使用範囲を包含する32スロットを保持する。
    std::array<ResourceHandle<TextureTag>, 32> textures = {};

    uint32_t indexCount  = 0;
    uint32_t vertexCount = 0; // indexCount=0 かつ vertexCount>0 でインデックスなし描画 (フルスクリーントライアングル等)
    uint32_t startIndex  = 0;
    uint32_t baseVertex  = 0;

    // GPU Instancing: instanceBuffer が有効なら instanceCount 1 以上で Instanced Draw を使用する。
    // instanceBuffer は VS の t0 に StructuredBuffer<T> としてバインドされ、
    // HLSL 側で SV_InstanceID でインデックスしてインスタンスデータを取得する。
    uint32_t instanceCount = 1;
    ResourceHandle<StructuredBufferTag> instanceBuffer;

    // VS-readable StructuredBuffer (t14〜t15): GPU パーティクル等の頂点データをバッファで渡す
    // WHY: SV_VertexID ベースの描画は頂点バッファを持たず、StructuredBuffer からデータを取り出す。
    //      t14 を使うのはテクスチャ SRV (t0〜t13) と重複しないため。
    std::array<ResourceHandle<StructuredBufferTag>, 2> vsBuffers = {}; // t14〜t15

    RenderLayer layer = RenderLayer::OPAQUE_LAYER;
    PrimitiveTopology topology = PrimitiveTopology::TRIANGLE_LIST;
};

} // namespace fbzz::renderer
