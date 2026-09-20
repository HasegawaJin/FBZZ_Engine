/// @file    RenderWaterInput.hpp
/// @brief   水面の形状・波・材質の解決済み入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/RenderConstants.hpp>
namespace fbzz::renderer {
struct RenderWaterPatch {
    renderer::ResourceHandle<renderer::BufferTag> vertexBuffer;
    renderer::ResourceHandle<renderer::BufferTag> indexBuffer;
    uint32_t indexCount = 0;
    math::Vector3 aabbMin;
    math::Vector3 aabbMax;
};
struct WaterWaveMargin {
    float horizontal = 0.0f;
    float vertical   = 0.0f;
};

struct RenderWaterInput {
    uint32_t layer = 0;
    bool selected = false;
    WaterCB constants{};
    WaterEffectParams effects{};
    WaterWaveMargin margin;
    math::Vector3 aabbMin, aabbMax;
    ResourceHandle<TextureTag> foam, ripple, detailNoise, velocityField;
    ResourceHandle<ShaderTag> shader;
    std::vector<RenderWaterPatch> patches;
};
}
