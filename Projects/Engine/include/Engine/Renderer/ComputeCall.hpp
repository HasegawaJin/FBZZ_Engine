// FBZZ Engine
// ComputeCall.hpp | fbzz::renderer
// Compute Shader ディスパッチ記述子。DrawCall の CS 版
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include "IShader.hpp"
#include "IConstantBuffer.hpp"
#include "ITexture.hpp"

namespace fbzz::renderer {

struct ComputeCall {
    std::shared_ptr<IShader> shader;

    // b0〜b7 の定数バッファ (CS ステージへバインド)
    std::array<std::shared_ptr<IConstantBuffer>, 8> constantBuffers = {};

    // t0〜t15 の SRV 入力 (CSSetShaderResources)
    std::array<std::shared_ptr<ITexture>, 16> srvInputs = {};

    // u0〜u1 の UAV 出力 (CSSetUnorderedAccessViews)
    // CreateComputeTexture() で生成した ITexture のみ渡せる (内部に UAV を持つ)
    std::array<std::shared_ptr<ITexture>, 2> uavOutputs = {};

    uint32_t dispatchX = 1;
    uint32_t dispatchY = 1;
    uint32_t dispatchZ = 1;
};

} // namespace fbzz::renderer
