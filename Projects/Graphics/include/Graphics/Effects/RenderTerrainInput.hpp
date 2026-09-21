/// @file    RenderTerrainInput.hpp
/// @brief   地形パッチと解決済み層資源の入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/RenderConstants.hpp>
namespace fbzz::renderer {
struct RenderTerrainPatch {
    renderer::ResourceHandle<renderer::BufferTag> vertexBuffer;
    std::array<renderer::ResourceHandle<renderer::BufferTag>, 3> indexBufferLOD;
    std::array<uint32_t, 3> indexCountLOD = {};
    math::Vector3 aabbMin;
    math::Vector3 aabbMax;
};

struct RenderTerrainInput {
    uint32_t layer = 0;
    bool selected = false;
    bool fiberSurface = false;
    float chunkWorldSize = 1;
    TerrainObjectCB constants{};
    ResourceHandle<TextureTag> splatIndices, splatWeights;
    std::vector<RenderTerrainPatch> patches;
};
}
