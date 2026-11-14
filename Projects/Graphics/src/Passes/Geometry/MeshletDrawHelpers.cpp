/// @file    MeshletDrawHelpers.cpp
/// @brief   Owner-scoped meshlet shader state and direct mesh draw preparation.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Graphics/Pipeline/MeshletDrawHelpers.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>

#include <Graphics/Renderer/IShader.hpp>
#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include "../../Renderer/GpuCullDrawQueue.hpp"
#include <Core/Profiler/ProfileScope.hpp>
#include <Math/Frustum.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
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
    std::array<std::array<float, 4>, 4> clusterClipRows{};
    std::array<float, 2> clusterViewportHalfSize{};
    float clusterErrorPixels = 0;
    uint32_t clusterNodeOffsetBytes = 0;
    std::array<uint32_t, 2> hizSize{};
    uint32_t hizPadding = 0;
    float windPadding = 0.0f;
    std::array<float, 4> clipPadding{};
    uint32_t vertexSkinning = 0;
    uint32_t nodeCount = 0;
    uint32_t residentDepth = 0;
    uint32_t instanceMode = 0;
    uint32_t instanceCapacity = 0;
    uint32_t argumentIndex = 0;
    std::array<uint32_t, 2> instancePadding{};
};
static_assert(sizeof(MeshletGeometryConstants) == 256);

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
        state.maskedShadowShader = {};
        state.compactShader = {};
        state.resetShader = {};
        state.gbufferShaderAttempted = false;
        state.shadowShaderAttempted = false;
        state.maskedShadowShaderAttempted = false;
    }
    return state;
}

ResourceHandle<ShaderTag> MeshletShader(ResourceManager& resources, MeshletDrawResources& state,
                                         MeshletDrawPass pass)
{
    if (pass == MeshletDrawPass::SHADOW_MASKED) {
        if (!state.maskedShadowShaderAttempted) {
            state.maskedShadowShaderAttempted = true;
            state.maskedShadowShader = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/AlphaShadowMesh.ms.hlsl");
        }
        const auto* shader = resources.Get(state.maskedShadowShader);
        return shader && shader->IsMeshShader() ? state.maskedShadowShader : ResourceHandle<ShaderTag>{};
    }
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
    return (shader.vertex == ShaderVertexContract::STANDARD_SURFACE || shader.vertex == ShaderVertexContract::STANDARD_SKINNED)
        && shader.surface == ShaderSurfaceContract::STANDARD_PBR
        && shader.HasStandardVariants();
}

bool HasClusterLodMaterial(const RenderMeshItem& item)
{
    return item.clusterLod && item.material.capabilities.blend == BlendMode::OPAQUE_BLEND
        && item.material.depthBias == 0 && item.material.depthBiasSlope == 0.0f;
}

float MaterialGeometryParameter(const RenderMaterial& material, size_t byteOffset)
{
    float value = 0.0f;
    std::memcpy(&value, material.gbufferParams.data() + byteOffset, sizeof(value));
    return value;
}

bool PrepareClusterPages(ResourceManager& resources, IRenderer& renderer, MeshletDrawResources& state, const RenderMeshItem& item,
    uint32_t desiredDepth, DrawCall& draw, MeshletGeometryConstants& constants)
{
    FBZZ_PROFILE_SCOPE("Meshlet::PreparePages");
    constexpr size_t STREAM_BUDGET = 64u * 1024u * 1024u;
    constexpr size_t UPLOAD_BUDGET = 512u * 1024u;
    const auto frame = resources.FrameStamp();
    if (state.streamFrame != frame) {
        state.streamFrame = frame;
        state.uploadedBytes = 0;
        for (auto it = state.streams.begin(); it != state.streams.end();) {
            if (!it->second.source.expired() && frame - it->second.lastFrame < 90u) {
                ++it;
                continue;
            }
            for (const auto page : it->second.pages) resources.Release(page);
            if (it->second.table.IsValid()) resources.Release(it->second.table);
            state.streamBytes -= it->second.bytes;
            it = state.streams.erase(it);
        }
        /// @note Under pressure, recycle previous-frame entries before admitting new pages.
        if (state.streamBytes >= STREAM_BUDGET - UPLOAD_BUDGET) {
            for (auto it = state.streams.begin(); it != state.streams.end();) {
                if (frame - it->second.lastFrame < 3u) {
                    ++it;
                    continue;
                }
                for (const auto page : it->second.pages) resources.Release(page);
                if (it->second.table.IsValid()) resources.Release(it->second.table);
                state.streamBytes -= it->second.bytes;
                it = state.streams.erase(it);
                if (state.streamBytes < STREAM_BUDGET - UPLOAD_BUDGET) break;
            }
        }
    }
    const auto& source = *item.clusterLod;
    if (source.streamPages.empty()) return false;
    const uint64_t key = (static_cast<uint64_t>(item.vertexBuffer.gen) << 32u) | item.vertexBuffer.id;
    auto& stream = state.streams[key];
    if (stream.source.lock() != item.clusterLod) {
        for (const auto page : stream.pages) resources.Release(page);
        if (stream.table.IsValid()) resources.Release(stream.table);
        state.streamBytes -= stream.bytes;
        stream = {};
        stream.source = item.clusterLod;
    }
    stream.lastFrame = frame;
    while (stream.nextPage < source.streamPages.size()) {
        const auto& page = source.streamPages[stream.nextPage];
        if (page.depth > desiredDepth) break;
        const size_t bytes = page.words.size() * sizeof(uint32_t);
        if (bytes > STREAM_BUDGET - state.streamBytes || bytes > UPLOAD_BUDGET - state.uploadedBytes) break;
        const auto handle = resources.CreateGpuLocalStructuredBuffer(page.words.data(),
            static_cast<uint32_t>(page.words.size()), sizeof(uint32_t));
        if (!handle.IsValid()) break;
        stream.pages.push_back(handle);
        ++stream.nextPage;
        stream.bytes += bytes;
        state.streamBytes += bytes;
        state.uploadedBytes += bytes;
    }
    if (stream.nextPage == 0) return false;
    uint32_t residentDepth = source.streamPages[stream.nextPage - 1].depth;
    if (stream.nextPage < source.streamPages.size() && source.streamPages[stream.nextPage].depth == residentDepth) {
        if (residentDepth == 0) return false;
        --residentDepth;
    }
    const auto nodeCount = static_cast<uint32_t>(source.streamTable.size() - source.meshletCount * 3u);
    if (stream.tableDepth != residentDepth) {
        renderer.ExecuteResourceOperation([&] {
            while (stream.descriptors.size() < stream.pages.size()) {
                const auto* buffer = resources.Get(stream.pages[stream.descriptors.size()]);
                const auto descriptor = buffer ? buffer->GetBindlessIndex() : INVALID_BINDLESS_INDEX;
                if (descriptor == INVALID_BINDLESS_INDEX) break;
                stream.descriptors.push_back(descriptor);
            }
        });
        if (stream.descriptors.size() != stream.pages.size()) return false;
        auto rows = source.streamTable;
        for (size_t i = nodeCount; i < rows.size(); i += 3) {
            const uint32_t page = rows[i];
            rows[i] = page < stream.descriptors.size() && source.streamPages[page].depth <= residentDepth
                ? stream.descriptors[page] : INVALID_BINDLESS_INDEX;
        }
        const auto table = resources.CreateGpuLocalStructuredBuffer(rows.data(),
            static_cast<uint32_t>(rows.size()), sizeof(uint32_t));
        if (!table.IsValid()) return false;
        if (stream.table.IsValid()) resources.Release(stream.table);
        stream.table = table;
        stream.tableDepth = residentDepth;
    }
    constants.nodeCount = nodeCount;
    constants.residentDepth = residentDepth;
    draw.auxiliaryPsBuffers[0] = stream.table;
    draw.meshletPages = stream.pages;
    return true;
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
    if ((skinned ? !item.clusterLod || item.vertexStride != sizeof(SkinnedVertex)
                 : item.deformedVertexBuffer.IsValid())
        || !item.vertexBuffer.IsValid() || !item.indexBuffer.IsValid()
        || !item.material.valid
        || item.vertexCount == 0 || item.indexCount < (item.clusterLod ? CLUSTER_MIN_TRIANGLES : kMeshletMinimumTriangleCount) * 3u
        || item.indexCount % 3u != 0 || item.vertexPositionOffset != 0
        || !HasStandardPbrVariant(item)) return false;

    if (pass == MeshletDrawPass::GBUFFER) {
        return (skinned || item.vertexStride == sizeof(Vertex))
            && item.material.capabilities.gbufferEquivalentShader
            && !item.material.capabilities.advancedLobe
            && item.material.shaderCapabilities.SupportsGBuffer();
    }

    const bool alphaOrWind = item.material.surface.alphaCutoff > 0.0f
        || MaterialGeometryParameter(item.material, 64u) > 0.0f
        || MaterialGeometryParameter(item.material, 68u) > 0.0f;
    return item.vertexStride >= sizeof(math::Vector3) && item.vertexStride % sizeof(uint32_t) == 0
        && item.material.capabilities.blend == BlendMode::OPAQUE_BLEND
        && (pass == MeshletDrawPass::SHADOW_MASKED
            ? item.clusterLod && alphaOrWind && (skinned || item.vertexStride == sizeof(Vertex)) : !alphaOrWind);
}

bool CanUseGBufferClusterLod(const RenderObject& object, const RenderMeshItem& item)
{
    return object.lodDither == 0.0f && object.leafWindBoundsPadding >= 0.0f
        && item.vertexContentVersion == 1u && item.indexContentVersion == 1u
        && HasClusterLodMaterial(item) && IsMeshletDrawCandidate(item, object.skinned, MeshletDrawPass::GBUFFER);
}

namespace {

bool PrepareMeshletDraw(ResourceManager& resources, IRenderer& renderer,
                           const math::Matrix4& viewProjection,
                           const math::Matrix4& world,
                           MeshletDrawPass pass, const RenderMeshItem& item,
                           bool skinned, DrawCall& inOut, bool frustumCullingEnabled,
                           bool forceMesh, const MeshletClusterView* clusterView, const GpuHiZ* hierarchy,
                           const MeshletClusterInstances* instances)
{
    if (!renderer.SupportsMeshShaders() || !IsMeshletDrawCandidate(item, skinned, pass)) return false;
    if (!forceMesh && !frustumCullingEnabled) return false;

    const bool tryClusters = clusterView && clusterView->width > 0 && clusterView->height > 0
        && inOut.startIndex == 0 && inOut.baseVertex == 0 && HasClusterLodMaterial(item);
    if (skinned && (!tryClusters || !(clusterView->skinDisplacement >= 0.0f)
        || !std::isfinite(clusterView->skinDisplacement) || item.skinningVertexBuffer != item.vertexBuffer)) return false;
    const float leafWindStrength = MaterialGeometryParameter(item.material, 68u);
    if (!std::isfinite(leafWindStrength) || (pass == MeshletDrawPass::GBUFFER && leafWindStrength > 0.0f && !tryClusters))
        return false;
    MeshletDrawResources& state = StateFor(resources);
    DrawCall meshCall = inOut;
    MeshletGeometryConstants pageConstants;
    bool paged = false;
    const bool usePages = tryClusters && !instances && !skinned && item.material.surface.alphaCutoff <= 0.0f
        && MaterialGeometryParameter(item.material, 64u) <= 0.0f && leafWindStrength <= 0.0f
        && !forceMesh && !item.clusterLod->streamPages.empty();
    if (usePages) {
        const auto localToClip = clusterView->viewProjection * world;
        const auto center = localToClip * math::Vector4{item.clusterLod->streamBoundsCenter, 1.0f};
        const auto rowLength = [&](size_t row) {
            const float* values = localToClip.m[row];
            return math::Vector3{values[0], values[1], values[2]}.Length();
        };
        const float screenScale = (std::max)(rowLength(0) * clusterView->width, rowLength(1) * clusterView->height);
        const float depth = center.w - rowLength(3) * item.clusterLod->streamBoundsRadius;
        const uint32_t desiredDepth = depth > 1.0e-6f && screenScale * item.clusterLod->streamRootError / depth < 0.5f
            ? 0u : UINT32_MAX;
        paged = PrepareClusterPages(resources, renderer, state, item, desiredDepth, meshCall, pageConstants);
        if (!paged) return false;
    }
    MeshletGeometry geometry;
    const bool clustered = tryClusters && resources.Meshlets().Prepare(resources, item.vertexBuffer, item.indexBuffer,
        item.vertexCount, item.vertexStride, item.indexCount, geometry, item.clusterLod.get(), paged);
    if (paged && !clustered) return false;
    if (!clustered && (instances || skinned || pass == MeshletDrawPass::SHADOW_MASKED
        || (pass == MeshletDrawPass::GBUFFER && leafWindStrength > 0.0f))) return false;
    if (!clustered && item.indexCount < kMeshletMinimumTriangleCount * 3u) return false;
    if (!clustered && !resources.Meshlets().Prepare(resources, item.vertexBuffer, item.indexBuffer,
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
        skinned && inOut.vertexBuffer != item.vertexBuffer ? sizeof(Vertex) : geometry.vertexStride,
        frustumCullingEnabled, constants);
    if (!forceMesh && (!validFrustum || (!clustered && !HasMeaningfulFrustumClipping(geometry, world, frustum))))
        return false;
    if (clustered) {
        const auto localToClip = clusterView->viewProjection * world;
        for (size_t row = 0; row < 4; ++row) {
            for (size_t column = 0; column < 4; ++column) {
                if (!std::isfinite(localToClip.m[row][column])) return false;
                constants.clusterClipRows[row][column] = localToClip.m[row][column];
            }
        }
        constants.clusterViewportHalfSize = {clusterView->width * 0.5f, clusterView->height * 0.5f};
        constants.clusterErrorPixels = 1.0f;
        constants.clusterNodeOffsetBytes = geometry.clusterNodeOffsetBytes;
        constants.nodeCount = pageConstants.nodeCount;
        constants.residentDepth = pageConstants.residentDepth;
        constants.windPadding = 2.0f * (std::max)(leafWindStrength, 0.0f);
        if (instances) {
            constants.instanceMode = instances->indexedArguments.IsValid() ? 2u : 1u;
            constants.instanceCapacity = instances->capacity;
            constants.argumentIndex = instances->argumentIndex;
        }
        if (skinned) {
            constants.cullingEnabled = 0u;
            const auto& vp = clusterView->viewProjection;
            for (size_t row = 0; row < constants.clipPadding.size(); ++row)
                constants.clipPadding[row] = math::Vector3{vp.m[row][0], vp.m[row][1], vp.m[row][2]}.Length()
                    * clusterView->skinDisplacement;
            constants.vertexSkinning = inOut.vertexBuffer == item.vertexBuffer ? 1u : 0u;
        }
    }
    const bool useHiZ = clustered && !skinned && pass == MeshletDrawPass::GBUFFER && hierarchy
        && hierarchy->reversedZ && hierarchy->width && hierarchy->height && resources.Get(hierarchy->depths);
    if (useHiZ) constants.hizSize = {hierarchy->width, hierarchy->height};

    const ResourceHandle<ShaderTag> meshShader = MeshletShader(resources, state, pass);
    if (!meshShader.IsValid()) return false;
    if (!state.geometryConstants.IsValid())
        state.geometryConstants = resources.CreateConstantBuffer(sizeof(MeshletGeometryConstants));
    if (!state.geometryConstants.IsValid()) return false;
    resources.Update(state.geometryConstants, &constants, sizeof(constants));

    meshCall.vertexBuffer = {};
    meshCall.indexBuffer = {};
    meshCall.startIndex = 0;
    meshCall.baseVertex = 0;
    meshCall.shader = meshShader;
    meshCall.constantBuffers[13] = state.geometryConstants;
    meshCall.instanceBuffer = constants.vertexSkinning ? inOut.vsBuffers[0] : ResourceHandle<StructuredBufferTag>{};
    meshCall.vsBuffers = {};
    if (useHiZ) meshCall.auxiliaryPsBuffers[1] = hierarchy->depths;
    if (instances) meshCall.instanceBuffer = instances->rows;
    meshCall.vsRawBuffers[0] = skinned ? inOut.vertexBuffer : item.vertexBuffer;
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

bool QueueCompactedMeshletDraw(RenderPassContext& ctx, const DrawCall& draw, MeshletDrawPass pass,
    const DrawCall& fallback, const MeshletClusterInstances* instances)
{
    FBZZ_PROFILE_SCOPE("Meshlet::Compact");
    auto& resources = ctx.resources;
    auto& state = StateFor(resources);
    if (!ctx.renderer.SupportsIndirectDraw() || (!instances && draw.meshGroupCountX < 32u)) return false;
    const uint32_t candidates = draw.meshGroupCountX * (instances ? instances->capacity : 1u);
    if (!state.compactShader.IsValid())
        state.compactShader = resources.LoadShader("Assets/Shaders/Pipeline/ClusterMeshletCompact.cs.hlsl");
    if (!state.resetShader.IsValid())
        state.resetShader = resources.LoadShader("Assets/Shaders/Pipeline/ClusterMeshletReset.cs.hlsl");
    const auto ids = state.compactIds.Acquire(resources, instances ? 1u + candidates * 2u : candidates, sizeof(uint32_t));
    const auto args = state.compactArguments.Acquire(resources, 1, 3 * sizeof(uint32_t));
    if (!state.compactShader.IsValid() || !state.resetShader.IsValid() || !ids.IsValid() || !args.IsValid()) return false;
    ComputeCall reset;
    reset.shader = state.resetShader;
    reset.constantBuffers[13] = draw.constantBuffers[13];
    reset.uavBuffers[1] = args;
    if (instances) reset.uavBuffers[0] = ids;
    ComputeCall compact;
    compact.shader = state.compactShader;
    compact.constantBuffers[0] = draw.constantBuffers[0];
    compact.constantBuffers[1] = draw.constantBuffers[1];
    compact.constantBuffers[13] = draw.constantBuffers[13];
    compact.srvRawBuffers[14] = draw.vsRawBuffers[1];
    compact.srvBuffers[15] = draw.auxiliaryPsBuffers[0];
    if (instances) {
        compact.srvBuffers[15] = instances->rows;
        compact.srvBuffers[29] = instances->ids;
        compact.srvBuffers[30] = instances->indexedArguments;
    } else if (pass == MeshletDrawPass::GBUFFER) compact.srvBuffers[29] = draw.auxiliaryPsBuffers[1];
    compact.uavBuffers = {ids, args};
    compact.dispatchX = (candidates + 31u) / 32u;
    DrawCall indirect = draw;
    indirect.meshletSelection = ids;
    indirect.indirectArguments = args;
    gpu_cull_detail::SubmitCullDraw(ctx.renderer, resources, compact, indirect, fallback, &reset);
    return true;
}

void CountMeshletDraw(RenderPassContext& ctx, const DrawCall& draw, MeshletDrawPass pass)
{
    if (pass == MeshletDrawPass::GBUFFER) {
        ++ctx.statsDrawCalls;
        ctx.statsVertexCount += DrawCallVertexCount(draw);
        ctx.statsTriangleCount += DrawCallTriangleCount(draw);
    } else {
        ++ctx.statsShadowDrawCalls;
        ctx.statsShadowTriangleCount += DrawCallTriangleCount(draw);
    }
}

} /// @note namespace

bool TryPrepareMeshletDraw(ResourceManager& resources, IRenderer& renderer,
    const math::Matrix4& viewProjection, const math::Matrix4& world, MeshletDrawPass pass,
    const RenderMeshItem& item, bool skinned, DrawCall& inOut, bool frustumCullingEnabled,
    bool forceMesh, const MeshletClusterView* clusterView, const GpuHiZ* hierarchy)
{
    return PrepareMeshletDraw(resources, renderer, viewProjection, world, pass, item, skinned,
        inOut, frustumCullingEnabled, forceMesh, clusterView, hierarchy, nullptr);
}

ResourceHandle<ConstantBufferTag> MeshletWindFrame(RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& state = StateFor(resources);
    if (!state.windFrame.IsValid()) state.windFrame = resources.CreateConstantBuffer(sizeof(PostProcCB));
    if (state.windFrame.IsValid() && state.windFrameStamp != resources.FrameStamp()) {
        PostProcCB wind{};
        wind.time = ctx.time;
        resources.Update(state.windFrame, &wind, sizeof(wind));
        state.windFrameStamp = resources.FrameStamp();
    }
    return state.windFrame;
}

void SubmitMeshletDraw(RenderPassContext& ctx, const DrawCall& draw, MeshletDrawPass pass)
{
    if (QueueCompactedMeshletDraw(ctx, draw, pass, draw, nullptr)) CountMeshletDraw(ctx, draw, pass);
    else if (pass == MeshletDrawPass::GBUFFER) SubmitCounted(ctx, draw);
    else SubmitCountedShadow(ctx, draw);
}

bool TrySubmitInstancedClusterDraw(RenderPassContext& ctx, const math::Matrix4& rasterViewProjection,
    const MeshletClusterView& view, const RenderMeshItem& item, MeshletDrawPass pass,
    const DrawCall& indexedDraw, const MeshletClusterInstances& instances)
{
    /// @note Bound transient memory and preserve the existing resident draw for oversized Cartesian products.
    constexpr uint32_t MAX_PAIR_CANDIDATES = 512u * 1024u;
    if (!item.clusterLod || !instances.capacity || !ctx.renderer.SupportsIndirectDraw()
        || !instances.rows.IsValid() || !instances.ids.IsValid()
        || item.clusterLod->meshletCount > MAX_PAIR_CANDIDATES / instances.capacity) return false;
    auto draw = indexedDraw;
    if (!PrepareMeshletDraw(ctx.resources, ctx.renderer, rasterViewProjection, math::Matrix4::Identity(),
            pass, item, false, draw, ctx.frustumCullingEnabled, false, &view, nullptr, &instances)) return false;
    if (!QueueCompactedMeshletDraw(ctx, draw, pass, indexedDraw, &instances)) return false;
    CountMeshletDraw(ctx, indexedDraw, pass);
    return true;
}

} /// @note namespace fbzz::renderer
