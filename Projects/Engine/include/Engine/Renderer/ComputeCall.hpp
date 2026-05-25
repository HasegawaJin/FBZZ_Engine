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
    std::array<ResourceHandle<ConstantBufferTag>, 8> constantBuffers = {};
    std::array<ResourceHandle<TextureTag>, 16> srvInputs  = {}; // SRV (読み取り専用): Texture2D t0〜t15
    std::array<ResourceHandle<TextureTag>, 2>  uavOutputs = {}; // UAV (書き込み可): RWTexture2D u0〜u1

    uint32_t dispatchX = 1;
    uint32_t dispatchY = 1;
    uint32_t dispatchZ = 1;
};

} // namespace fbzz::renderer
