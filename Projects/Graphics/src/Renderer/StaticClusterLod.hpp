/// @file    StaticClusterLod.hpp
/// @brief   Import-time hierarchy for opaque static triangle meshes.
/// @author  Hasegawa Jin
/// @date    2026-11-11
#pragma once
#include <Graphics/Renderer/Mesh.hpp>
#include <span>

namespace fbzz::renderer {

/// @note The 32-byte GPU node bounds enclose the source geometry, including all descendants.
struct StaticClusterNode {
    math::Vector3 boundsMin;
    float error = 0.0f;
    math::Vector3 boundsMax;
    uint32_t parent = UINT32_MAX;
};
static_assert(sizeof(StaticClusterNode) == 32);

struct StaticClusterRange {
    size_t endIndex;
    uint32_t node;
};

struct StaticClusterBuild {
    std::vector<StaticClusterNode> nodes;
    std::vector<StaticClusterRange> ranges;
    std::vector<uint32_t> indices;
};

/// @see https://dev.epicgames.com/documentation/unreal-engine/nanite-in-unreal-engine Nanite Virtualized Geometry Overview, hierarchical clusters.
[[nodiscard]] bool BuildStaticClusterLod(std::span<const Vertex> vertices,
    std::span<const uint32_t> indices, StaticClusterBuild& output);

} /// @note namespace fbzz::renderer
