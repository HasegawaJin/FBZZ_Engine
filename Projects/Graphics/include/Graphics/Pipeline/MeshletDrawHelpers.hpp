/// @file    MeshletDrawHelpers.hpp
/// @brief   Eligibility and draw preparation for static meshlet geometry.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Graphics/Api.hpp>

#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Math/Matrix4.hpp>
#include <cstdint>

namespace fbzz::renderer {

/// @brief Pass-specific meshlet shader and eligibility rules.
enum class MeshletDrawPass : uint8_t {
    GBUFFER,
    SHADOW,
};

/// @note The unjittered color-view projection keeps the screen-error decision stable across TAA samples.
struct MeshletClusterView {
    math::Matrix4 viewProjection;
    uint32_t width;
    uint32_t height;
};

/// @brief Checks whether a static standard-surface item can use the meshlet path.
/// @param item Resolved mesh and material snapshot.
/// @param skinned True when the owning render object uses skinning.
/// @param pass GBuffer keeps the standard 60-byte vertex ABI; shadows read only positions.
/// @return False for unsupported, small, deformed, custom, or shadow alpha-cutout inputs.
[[nodiscard]] FBZZ_GRAPHICS_API bool IsMeshletDrawCandidate(const RenderMeshItem& item, bool skinned,
                                          MeshletDrawPass pass);

/// @brief Prepares a mesh-shader DrawCall while leaving the input unchanged on failure.
/// @param viewProjection Exact camera or shadow view-projection matrix for this draw.
/// @param world Local-to-world matrix for the object's aggregate meshlet bounds.
/// @param forceMesh Keeps the mesh-shader path enabled for explicit comparison even when the whole object is visible.
/// @return True when inOut is a fully configured direct mesh-shader draw.
/// @note Flat meshlets require at least 25 percent estimated clipping unless forced. Cluster LOD also handles fully visible geometry.
/// @note Callers update b1 and submit immediately after success so the shared b13 upload and draw stay ordered.
/// @see https://microsoft.github.io/DirectX-Specs/d3d/MeshShader.html DispatchMesh groups and mesh output limits.
[[nodiscard]] FBZZ_GRAPHICS_API bool TryPrepareMeshletDraw(ResourceManager& resources, IRenderer& renderer,
                                         const math::Matrix4& viewProjection,
                                         const math::Matrix4& world,
                                         MeshletDrawPass pass, const RenderMeshItem& item,
                                         bool skinned, DrawCall& inOut,
                                         bool frustumCullingEnabled = true,
                                         bool forceMesh = false,
                                         const MeshletClusterView* clusterView = nullptr);

} /// @note namespace fbzz::renderer
