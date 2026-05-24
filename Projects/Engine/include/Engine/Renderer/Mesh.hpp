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

struct SkinnedVertex {
    math::Vector3 position;
    math::Vector3 normal;
    math::Vector3 tangent;
    math::Vector2 uv;
    uint32_t boneIndices[4] = {};
    float boneWeights[4] = {};
};

struct Mesh {
    ResourceHandle<BufferTag> vertexBuffer;
    ResourceHandle<BufferTag> indexBuffer;
    uint32_t vertexCount = 0;
    uint32_t indexCount  = 0;
    bool isSkinned = false;
    std::vector<Vertex>   cpuVertices;
    std::vector<SkinnedVertex> cpuSkinnedVertices;
    std::vector<uint32_t> cpuIndices;
};

} // namespace fbzz::renderer
