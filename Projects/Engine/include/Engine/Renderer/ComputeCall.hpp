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
    std::array<ResourceHandle<TextureTag>, 16> srvInputs = {};
    std::array<ResourceHandle<TextureTag>, 2> uavOutputs = {};

    uint32_t dispatchX = 1;
    uint32_t dispatchY = 1;
    uint32_t dispatchZ = 1;
};

} // namespace fbzz::renderer
