/// @file    WorldHlod.hpp
/// @brief   Deterministic cell HLOD proxies aggregate the lowest mesh LOD across authored owners.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Scene/WorldCell.hpp>
#include <Math/Matrix4.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fbzz::scene {

/// @note Spans borrow immutable authoring data for BuildWorldHlod only; sourceKey identifies one authored owner.
struct WorldHlodSource {
    std::string sourceKey;
    math::Matrix4 worldTransform = math::Matrix4::Identity();
    std::span<const asset::InstanceMeshPart> parts;
    std::span<const float> lodHeights;
    std::span<const renderer::InstancePlacement> placements;
    uint32_t layer = 0;
    bool castShadows = true;
    bool includeHlod = true;
    bool hasLocalBounds = false;
    /// @note Mesh-local bounds must enclose every selected lowest-LOD part and any material vertex displacement.
    WorldCellBounds localBounds;
};

struct WorldHlodBuildSettings {
    WorldCellGrid grid;
    uint32_t maxGroups = 65536;
    uint32_t maxInstances = 1048576;
};

struct WorldHlodPlacementSource {
    std::string sourceKey;
    uint32_t sourceId = 0;
};

struct WorldHlodGroup {
    WorldCellId cell;
    math::Vector3 origin;
    uint32_t layer = 0;
    bool castShadows = true;
    /// @note Missing bounds in any contributing source set hasWorldBounds=false; consumers must not reject that group by this AABB.
    WorldCellBounds worldBounds;
    bool hasWorldBounds = false;
    asset::MeshInstancesAsset proxy;
    /// @note sources[i] identifies proxy placement i; generated IDs are positive, unique, and independent of source traversal order.
    std::vector<WorldHlodPlacementSource> sources;
};

/// @note Each cell aggregates compatible owners into independent lowest-LOD instance assets, preserving all parts of that LOD.
/// @note Placement scales must be positive; composed transforms must be finite, affine, orientation-preserving TRS without shear.
/// @return False leaves out unchanged and sets diagnostic. Inputs and referenced assets are never modified.
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition---hierarchical-level-of-detail-in-unreal-engine HLOD Layers, Instancing.
[[nodiscard]] FBZZ_ENGINE_API bool BuildWorldHlod(std::span<const WorldHlodSource> sources,
    const WorldHlodBuildSettings& settings, std::vector<WorldHlodGroup>& out, std::string& diagnostic);

} /// @note namespace fbzz::scene
