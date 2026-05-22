// FBZZ Engine
// ComputeCall.hpp | fbzz::renderer
// Compute shader dispatch submission data
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
