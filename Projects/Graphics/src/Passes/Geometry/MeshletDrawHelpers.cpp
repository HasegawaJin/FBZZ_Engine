/// @file    MeshletDrawHelpers.cpp
/// @brief   Owner-scoped meshlet shader state and direct mesh draw preparation.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Graphics/Pipeline/MeshletDrawHelpers.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>

#include <Graphics/Renderer/IShader.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Math/Frustum.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace fbzz::renderer {
namespace {

constexpr uint32_t kMeshletMinimumTriangleCount = 8192u;
constexpr uint32_t kMeshletMaximumDispatchGroups = 65535u;
constexpr float kMinimumMeshletClippedFraction = 0.25f;

struct MeshletGeometryConstants {
    std::array<math::Vector4, math::Frustum::PLANE_COUNT> frustumPlanes{};
    uint32_t meshletCount = 0;
    uint32_t vertexStride = 0;
    uint32_t positionOffset = 0;
    uint32_t cullingEnabled = 0;
};
static_assert(sizeof(MeshletGeometryConstants) == 112);

MeshletDrawResources& StateFor(ResourceManager& resources)
{
    MeshletDrawResources& state = resources.Rendering().Shared().meshletDraw;

    const uint64_t resetVersion = resources.GetResetVersion();
    if (state.ownerId != resources.GetInstanceId() || state.resetVersion != resetVersion) {
        /// @note Reset invalidates every old handle; never release one through a different owner or device generation.
        state = {};
        state.ownerId = resources.GetInstanceId();
        state.resetVersion = resetVersion;
    }

    const uint64_t shaderVersion = resources.GetShaderVersion();
    if (state.shaderVersion != shaderVersion) {
        state.shaderVersion = shaderVersion;
        state.gbufferShader = {};
        state.shadowShader = {};
        state.gbufferShaderAttempted = false;
        state.shadowShaderAttempted = false;
    }
    return state;
}

ResourceHandle<ShaderTag> MeshletShader(ResourceManager& resources, MeshletDrawResources& state,
                                         MeshletDrawPass pass)
{
    bool& attempted = pass == MeshletDrawPass::GBUFFER
        ? state.gbufferShaderAttempted : state.shadowShaderAttempted;
    ResourceHandle<ShaderTag>& shader = pass == MeshletDrawPass::GBUFFER
        ? state.gbufferShader : state.shadowShader;
    if (!attempted) {
        attempted = true;
        shader = resources.LoadShader(pass == MeshletDrawPass::GBUFFER
            ? "Assets/Shaders/Pipeline/Deferred/GBufferMesh.ms.hlsl"
            : "Assets/Shaders/Pipeline/Shadow/ShadowMapMesh.ms.hlsl");
    }
    const IShader* shaderObject = resources.Get(shader);
    return shaderObject && shaderObject->IsMeshShader() ? shader : ResourceHandle<ShaderTag>{};
}

bool HasStandardPbrVariant(const RenderMeshItem& item)
{
    const ShaderCapabilities& shader = item.material.shaderCapabilities;
    return shader.vertex == ShaderVertexContract::STANDARD_SURFACE
        && shader.surface == ShaderSurfaceContract::STANDARD_PBR
        && shader.HasStandardVariants();
}

bool BuildConstants(const math::Frustum& frustum, uint32_t meshletCount,
                    uint32_t vertexStride, bool frustumCullingEnabled,
                    MeshletGeometryConstants& out)
{
    out = {};
    out.meshletCount = meshletCount;
    out.vertexStride = vertexStride;
    out.positionOffset = 0;
    bool validFrustum = true;
    for (int i = 0; i < math::Frustum::PLANE_COUNT; ++i) {
        const math::Plane plane = frustum.GetPlane(i);
        const float normalLengthSquared = plane.normal.LengthSq();
        if (!std::isfinite(plane.normal.x) || !std::isfinite(plane.normal.y)
            || !std::isfinite(plane.normal.z) || !std::isfinite(plane.distance)
            || !std::isfinite(normalLengthSquared) || normalLengthSquared <= 1.0e-20f) {
            validFrustum = false;
            continue;
        }
        out.frustumPlanes[static_cast<size_t>(i)] = {
            plane.normal.x, plane.normal.y, plane.normal.z, plane.distance
        };
    }
    out.cullingEnabled = validFrustum && frustumCullingEnabled ? 1u : 0u;
    return validFrustum;
}

/// @brief Estimates whether meshlet rejection can offset mesh-shader dispatch cost.
/// @note The normalized plane intrusion threshold is benchmark-guided; meshlet distribution still determines the actual benefit.
/// @see Docs/design/gpu-driven-geometry.md partial visibility crossover.
bool HasMeaningfulFrustumClipping(const MeshletGeometry& geometry,
    const math::Matrix4& world, const math::Frustum& frustum)
{
    const math::Vector3 localMin{geometry.boundsMin[0], geometry.boundsMin[1], geometry.boundsMin[2]};
    const math::Vector3 localMax{geometry.boundsMax[0], geometry.boundsMax[1], geometry.boundsMax[2]};
    if (!std::isfinite(localMin.x) || !std::isfinite(localMin.y) || !std::isfinite(localMin.z)
        || !std::isfinite(localMax.x) || !std::isfinite(localMax.y) || !std::isfinite(localMax.z)
        || localMin.x > localMax.x || localMin.y > localMax.y || localMin.z > localMax.z)
        return false;

    const math::Vector3 localCenter = (localMin + localMax) * 0.5f;
    const math::Vector3 localExtents = (localMax - localMin) * 0.5f;
    if (std::abs(world.m[3][0]) > 1.0e-6f || std::abs(world.m[3][1]) > 1.0e-6f
        || std::abs(world.m[3][2]) > 1.0e-6f || std::abs(world.m[3][3] - 1.0f) > 1.0e-6f)
        return false;
    const math::Vector4 worldCenter4 = world * math::Vector4{localCenter, 1.0f};
    const auto projectRow = [&](size_t row) {
        return std::abs(world.m[row][0]) * localExtents.x
            + std::abs(world.m[row][1]) * localExtents.y
            + std::abs(world.m[row][2]) * localExtents.z;
    };
    math::Vector3 worldExtents{projectRow(0), projectRow(1), projectRow(2)};
    math::Vector3 worldCenter{worldCenter4.x, worldCenter4.y, worldCenter4.z};
    if (!std::isfinite(worldCenter.x) || !std::isfinite(worldCenter.y) || !std::isfinite(worldCenter.z)
        || !std::isfinite(worldCenter4.w) || !std::isfinite(worldExtents.x)
        || !std::isfinite(worldExtents.y) || !std::isfinite(worldExtents.z))
        return false;

    const float boundsMagnitude = (std::max)({
        std::abs(worldCenter.x), std::abs(worldCenter.y), std::abs(worldCenter.z),
        worldExtents.x, worldExtents.y, worldExtents.z });
    const float roundingGuard = (std::max)(1.0e-5f, boundsMagnitude * 1.0e-6f);
    worldExtents.x += roundingGuard;
    worldExtents.y += roundingGuard;
    worldExtents.z += roundingGuard;
    for (int planeIndex = 0; planeIndex < math::Frustum::PLANE_COUNT; ++planeIndex) {
        const math::Plane plane = frustum.GetPlane(planeIndex);
        const float normalLengthSquared = plane.normal.LengthSq();
        if (!std::isfinite(plane.normal.x) || !std::isfinite(plane.normal.y)
            || !std::isfinite(plane.normal.z) || !std::isfinite(plane.distance)
            || !std::isfinite(normalLengthSquared) || normalLengthSquared <= 1.0e-20f)
            return false;
        const float support = std::abs(plane.normal.x) * worldExtents.x
            + std::abs(plane.normal.y) * worldExtents.y
            + std::abs(plane.normal.z) * worldExtents.z;
        const float centerDistance = math::Vector3::Dot(plane.normal, worldCenter) + plane.distance;
        if (!std::isfinite(support) || !std::isfinite(centerDistance)) return false;
        if (support <= 1.0e-20f) {
            if (centerDistance < 0.0f) return true;
            continue;
        }
        const float clippedFraction = (support - centerDistance) / (2.0f * support);
        if (std::isfinite(clippedFraction) && clippedFraction >= kMinimumMeshletClippedFraction) return true;
    }
    return false;
}

} /// @note namespace

bool IsMeshletDrawCandidate(const RenderMeshItem& item, bool skinned, MeshletDrawPass pass)
{
    if (skinned || item.deformedVertexBuffer.IsValid()
        || !item.vertexBuffer.IsValid() || !item.indexBuffer.IsValid()
        || !item.material.valid
        || item.vertexCount == 0 || item.indexCount < kMeshletMinimumTriangleCount * 3u
        || item.indexCount % 3u != 0 || item.vertexPositionOffset != 0
        || !HasStandardPbrVariant(item)) return false;

    if (pass == MeshletDrawPass::GBUFFER) {
        return item.vertexStride == sizeof(Vertex)
            && item.material.capabilities.gbufferEquivalentShader
            && !item.material.capabilities.advancedLobe
            && item.material.shaderCapabilities.SupportsGBuffer();
    }

    return item.vertexStride >= sizeof(math::Vector3) && item.vertexStride % sizeof(uint32_t) == 0
        && item.material.capabilities.blend == BlendMode::OPAQUE_BLEND
        && item.material.surface.alphaCutoff <= 0.0f;
}

bool TryPrepareMeshletDraw(ResourceManager& resources, IRenderer& renderer,
                           const math::Matrix4& viewProjection,
                           const math::Matrix4& world,
                           MeshletDrawPass pass, const RenderMeshItem& item,
                           bool skinned, DrawCall& inOut, bool frustumCullingEnabled,
                           bool forceMesh)
{
    if (!renderer.SupportsMeshShaders() || !IsMeshletDrawCandidate(item, skinned, pass)) return false;
    if (!forceMesh && !frustumCullingEnabled) return false;

    MeshletGeometry geometry;
    if (!resources.Meshlets().Prepare(resources, item.vertexBuffer, item.indexBuffer,
        item.vertexCount, item.vertexStride, item.indexCount, geometry)) return false;
    if (!geometry.blob.IsValid() || geometry.meshletCount == 0
        || geometry.meshletCount > kMeshletMaximumDispatchGroups
        || geometry.vertexCount != item.vertexCount || geometry.vertexStride != item.vertexStride
        || geometry.indexCount != item.indexCount) return false;

    BufferResourceMetadata vertexMetadata;
    BufferResourceMetadata blobMetadata;
    if (!resources.GetBufferMetadata(item.vertexBuffer, vertexMetadata)
        || !resources.GetBufferMetadata(geometry.blob, blobMetadata)
        || vertexMetadata.stride != item.vertexStride
        || blobMetadata.sizeBytes < static_cast<uint64_t>(geometry.meshletCount) * sizeof(MeshletDescriptor))
        return false;
    const uint64_t vertexBytes = static_cast<uint64_t>(item.vertexCount) * item.vertexStride;
    if (vertexBytes > vertexMetadata.sizeBytes || vertexBytes > std::numeric_limits<uint32_t>::max())
        return false;

    const math::Frustum frustum = math::Frustum::FromViewProjection(viewProjection);
    MeshletGeometryConstants constants;
    const bool validFrustum = BuildConstants(frustum, geometry.meshletCount,
        geometry.vertexStride, frustumCullingEnabled, constants);
    if (!forceMesh && (!validFrustum || !HasMeaningfulFrustumClipping(geometry, world, frustum)))
        return false;

    MeshletDrawResources& state = StateFor(resources);
    const ResourceHandle<ShaderTag> meshShader = MeshletShader(resources, state, pass);
    if (!meshShader.IsValid()) return false;
    if (!state.geometryConstants.IsValid())
        state.geometryConstants = resources.CreateConstantBuffer(sizeof(MeshletGeometryConstants));
    if (!state.geometryConstants.IsValid()) return false;
    resources.Update(state.geometryConstants, &constants, sizeof(constants));

    DrawCall meshCall = inOut;
    meshCall.vertexBuffer = {};
    meshCall.indexBuffer = {};
    meshCall.startIndex = 0;
    meshCall.baseVertex = 0;
    meshCall.shader = meshShader;
    meshCall.constantBuffers[13] = state.geometryConstants;
    meshCall.instanceBuffer = {};
    meshCall.vsBuffers = {};
    meshCall.vsRawBuffers[0] = item.vertexBuffer;
    meshCall.vsRawBuffers[1] = geometry.blob;
    meshCall.indirectArguments = {};
    meshCall.indirectArgumentOffset = 0;
    meshCall.indirectCommandCount = 1;
    meshCall.indirectInstanceOffset = false;
    meshCall.meshGroupCountX = geometry.meshletCount;
    meshCall.meshGroupCountY = 1;
    meshCall.meshGroupCountZ = 1;
    meshCall.topology = PrimitiveTopology::TRIANGLE_LIST;
    inOut = meshCall;
    return true;
}

} /// @note namespace fbzz::renderer
