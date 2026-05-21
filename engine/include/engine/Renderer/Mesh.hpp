// FBZZ Engine
// Mesh.hpp | fbzz::renderer
// GPU メッシュ。頂点・インデックスバッファのラッパー
#pragma once
#include <memory>
#include <cstdint>
#include <vector>
#include "IBuffer.hpp"
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>

namespace fbzz::renderer {

struct Vertex {
    math::Vector3 position;
    math::Vector3 normal;
    math::Vector3 tangent;
    math::Vector2 uv;
};

struct Mesh {
    std::shared_ptr<IBuffer> vertexBuffer;
    std::shared_ptr<IBuffer> indexBuffer;
    uint32_t vertexCount = 0;
    uint32_t indexCount  = 0;
    std::vector<Vertex>   cpuVertices;
    std::vector<uint32_t> cpuIndices;
};

} // namespace fbzz::renderer
