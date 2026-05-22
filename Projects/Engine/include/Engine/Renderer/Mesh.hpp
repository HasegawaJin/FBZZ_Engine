// FBZZ Engine
// Mesh.hpp | fbzz::renderer
// GPU メッシュ。頂点・インデックスバッファのラッパー
#pragma once
#include <cstdint>
#include <vector>
#include "ResourceHandle.hpp"
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::renderer {

struct Vertex {
    math::Vector3 position;
    math::Vector3 normal;
    math::Vector3 tangent;
    math::Vector2 uv;
};

struct Mesh {
    ResourceHandle<BufferTag> vertexBuffer;
    ResourceHandle<BufferTag> indexBuffer;
    uint32_t vertexCount = 0;
    uint32_t indexCount  = 0;
    std::vector<Vertex>   cpuVertices;
    std::vector<uint32_t> cpuIndices;
};

} // namespace fbzz::renderer
