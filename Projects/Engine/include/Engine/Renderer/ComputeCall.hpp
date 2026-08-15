// FBZZ Engine
// ComputeCall.hpp | fbzz::renderer
// コンピュートシェーダーの実行要求データ
// IRenderer::Dispatch に渡す軽量な値型。
// GPU リソースは ResourceHandle で参照し、共有ポインタは持たない。
#pragma once
#include <array>
#include <cstdint>
#include "ResourceHandle.hpp"

namespace fbzz::renderer {

struct ComputeCall {
    ResourceHandle<ShaderTag> shader;
    std::array<ResourceHandle<ConstantBufferTag>, 14> constantBuffers = {}; // b0〜b13
    std::array<ResourceHandle<TextureTag>, 32>        srvInputs       = {}; // t0〜t31
    std::array<ResourceHandle<TextureTag>, 8>         uavOutputs      = {}; // u0〜u7 (DX11 CS上限)
    // StructuredBuffer SRV (t14〜t15): CS 入力バッファ。Texture SRV が t0〜t13 を占めるため t14 以降に配置。
    // WHY: GPU パーティクルのスポーンバッファ等、テクスチャとは独立した型付きバッファを CS に渡すために追加。
    std::array<ResourceHandle<StructuredBufferTag>, 2> srvBuffers       = {}; // StructuredBuffer SRV: t14〜t15
    std::array<ResourceHandle<StructuredBufferTag>, 2> uavBuffers       = {}; // RWStructuredBuffer UAV: u2〜u3
    // GPU 書き込み可能な頂点バッファ UAV: u4。コンピュートスキニングの出力先。
    // WHY: 出力は「CS が書いて IA が読む」二役なので StructuredBufferTag ではなく
    //      BufferTag (頂点バッファ) 側で確保する。バインド口だけここに分けて持つ。
    ResourceHandle<BufferTag>                         uavVertexBuffer  = {}; // u4

    uint32_t dispatchX = 1;
    uint32_t dispatchY = 1;
    uint32_t dispatchZ = 1;
};

} // namespace fbzz::renderer
