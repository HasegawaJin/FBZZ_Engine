/// @file    TerrainRenderPass.cpp
/// @brief   TerrainComponent → GPU チャンクメッシュ生成・描画 (IRenderPass 実装)。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include <Graphics/Passes/Geometry/TerrainRenderPass.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Renderer/IPipelineState.hpp>
#include <Graphics/Renderer/IShader.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Pipeline/DirectionalShadowBake.hpp>
#include <Graphics/Renderer/GpuGeometrySelection.hpp>
#include <Graphics/Pipeline/OcclusionCuller.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include <bit>
#include "InstanceShadowBatch.hpp"
#include "TerrainShadowBatch.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <map>
#include <optional>
#include <unordered_map>
#include <limits>
namespace fbzz::renderer {
namespace {
constexpr uint32_t kTerrainMeshletVertexStride = 44;
constexpr uint32_t kTerrainMeshletMaximumGroups = 65535;
constexpr uint32_t kTerrainMeshletMinimumIndices = 126 * 3;

struct TerrainGpuResources {
    uint64_t owner = 0, version = UINT64_MAX;
    GpuGeometrySelection selection;
    std::array<ResourceHandle<ShaderTag>, 3> shaders{};
    std::array<ResourceHandle<ShaderTag>, 3> meshletShaders{};
    ResourceHandle<ConstantBufferTag> meshletConstants;
};

/// @note The resource ledger validates the generation without waiting for native render recording.
bool HasTerrainBuffer(ResourceManager& resources, ResourceHandle<BufferTag> handle)
{
    BufferResourceMetadata metadata;
    return resources.GetBufferMetadata(handle, metadata) && metadata.live;
}

void BindTerrainStrip(const RenderPassContext& ctx, const RenderTerrainPatch& patch, int lod, DrawCall& call)
{
    if (lod != 0 || ctx.settings.rasterGeometryMode != RasterGeometryMode::TRIANGLE_STRIP
        || !patch.lod0Strip.buffer.IsValid() || patch.lod0Strip.count == 0) return;
    FBZZ_PROFILE_SCOPE("TerrainStrip::Bind");
    call.indexBuffer = patch.lod0Strip.buffer;
    call.indexCount = patch.lod0Strip.count;
    call.topology = PrimitiveTopology::TRIANGLE_STRIP;
    call.stripTriangleCount = patch.indexCountLOD[0] / 3u;
}

TerrainGpuResources& GetTerrainGpuResources(ResourceManager& resources)
{
    static TerrainGpuResources state;
    if (state.owner != resources.GetInstanceId() || state.version != resources.GetResetVersion()) {
        state.owner = resources.GetInstanceId();
        state.version = resources.GetResetVersion();
        state.shaders = {};
        state.meshletShaders = {};
        state.meshletConstants = {};
    }
    return state;
}
ResourceHandle<ShaderTag> TerrainGpuShader(ResourceManager& resources, size_t mode)
{
    constexpr std::array<const char*, 3> paths{
        "Assets/Shaders/Terrain/TerrainGpu.hlsl", "Assets/Shaders/Terrain/TerrainGpuGBuffer.hlsl",
        "Assets/Shaders/Terrain/TerrainGpuShadow.hlsl"};
    auto& handle = GetTerrainGpuResources(resources).shaders[mode];
    if (!resources.Get(handle)) handle = resources.LoadShader(paths[mode]);
    return handle;
}

bool UsesTerrainMeshlets(const RenderPassContext& ctx)
{
    return !ctx.gpuOcclusionTerrain && ctx.frustumCullingEnabled && !ctx.settings.IsWireframe()
        && ctx.settings.geometryDrawMode == GeometryDrawMode::MESH_SHADER && ctx.renderer.SupportsMeshShaders();
}

ResourceHandle<ShaderTag> TerrainMeshletShader(ResourceManager& resources, size_t mode)
{
    constexpr std::array<const char*, 3> paths{
        "Assets/Shaders/Terrain/TerrainMesh.ms.hlsl", "Assets/Shaders/Terrain/TerrainGBufferMesh.ms.hlsl",
        "Assets/Shaders/Terrain/TerrainShadowMesh.ms.hlsl"};
    auto& handle = GetTerrainGpuResources(resources).meshletShaders[mode];
    if (!resources.Get(handle)) handle = resources.LoadShader(paths[mode]);
    const auto* shader = resources.Get(handle);
    return shader && shader->IsMeshShader() ? handle : ResourceHandle<ShaderTag>{};
}

struct TerrainMeshletConstants {
    std::array<math::Vector4, math::Frustum::PLANE_COUNT> frustumPlanes{};
    /// @note Group count comes from the exact patch blob. The shader limit keeps these constants reusable within a view.
    uint32_t meshletCount = kTerrainMeshletMaximumGroups;
    uint32_t vertexStride = kTerrainMeshletVertexStride;
    uint32_t positionOffset = 0;
    uint32_t cullingEnabled = 1;
    math::Vector4 localEye{};
};
static_assert(offsetof(TerrainMeshletConstants, localEye) == 112);
static_assert(sizeof(TerrainMeshletConstants) == 128);

struct TerrainMeshletView {
    TerrainMeshletConstants constants;
    ResourceHandle<ShaderTag> shader;
    ResourceHandle<ConstantBufferTag> constantsHandle;
};

math::Vector3 MatrixRow3(const math::Matrix4& matrix, int row)
{
    return {matrix.m[row][0], matrix.m[row][1], matrix.m[row][2]};
}

/// @note Only clip x, y, and w determine projected winding. Derive the eye from these rows of the actual draw matrix.
/// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_rasterizer_desc FrontCounterClockwise.
math::Vector4 TerrainMeshletLocalEye(const math::Matrix4& world, const math::Matrix4& viewProjection)
{
    using math::Vector3;
    const auto x = MatrixRow3(world, 0);
    const auto y = MatrixRow3(world, 1);
    const auto z = MatrixRow3(world, 2);
    const float orientation = Vector3::Dot(Vector3::Cross(x, y), z);
    const float worldScale = x.Length() * y.Length() * z.Length();
    if (!std::isfinite(orientation) || !std::isfinite(worldScale) || orientation <= worldScale * 0.1f)
        return {};

    const math::Matrix4 localClip = viewProjection * world;
    const auto clipX = MatrixRow3(localClip, 0);
    const auto clipY = MatrixRow3(localClip, 1);
    const auto clipW = MatrixRow3(localClip, 3);
    const auto normal = Vector3::Cross(clipX, clipY);
    if (clipW.x == 0.0f && clipW.y == 0.0f && clipW.z == 0.0f) {
        const float length = normal.Length();
        if (!std::isfinite(localClip.m[3][3]) || localClip.m[3][3] <= 0.0f || !std::isfinite(length)
            || length <= clipX.Length() * clipY.Length() * 0.1f) return {};
        return {-normal / length, 2.0f};
    }

    /// @note The perspective eye is the common intersection of the clip x=0, y=0, and w=0 planes.
    const float determinant = Vector3::Dot(normal, clipW);
    const float clipScale = clipX.Length() * clipY.Length() * clipW.Length();
    if (!std::isfinite(determinant) || !std::isfinite(clipScale) || determinant <= clipScale * 0.1f) return {};
    const auto eye = (Vector3::Cross(clipY, clipW) * -localClip.m[0][3]
        + Vector3::Cross(clipW, clipX) * -localClip.m[1][3]
        + normal * -localClip.m[3][3]) / determinant;
    if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z)) return {};
    return {eye, 1.0f};
}

TerrainMeshletView MakeTerrainMeshletView(ResourceHandle<ShaderTag> shader, const math::Frustum& frustum,
    const math::Matrix4& world, const math::Matrix4& viewProjection)
{
    if (!shader.IsValid() || world.m[3][0] != 0.0f || world.m[3][1] != 0.0f
        || world.m[3][2] != 0.0f || world.m[3][3] != 1.0f) return {};
    TerrainMeshletView view;
    for (int index = 0; index < math::Frustum::PLANE_COUNT; ++index) {
        const auto plane = frustum.GetPlane(index);
        const float lengthSquared = plane.normal.LengthSq();
        if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-20f || !std::isfinite(plane.distance))
            return {};
        view.constants.frustumPlanes[index] = {plane.normal, plane.distance};
    }
    view.shader = shader;
    view.constants.localEye = TerrainMeshletLocalEye(world, viewProjection);
    return view;
}

bool BindTerrainMeshlet(ResourceManager& resources, TerrainMeshletView& view, DrawCall& call)
{
    if (!view.shader.IsValid() || call.indexCount < kTerrainMeshletMinimumIndices) return false;
    MeshletGeometry geometry;
    if (!resources.Meshlets().PrepareTerrain(resources, call.vertexBuffer, call.indexBuffer,
        call.indexCount, geometry) || geometry.meshletCount == 0 || geometry.meshletCount > kTerrainMeshletMaximumGroups)
        return false;
    if (!view.constantsHandle.IsValid()) {
        auto& handle = GetTerrainGpuResources(resources).meshletConstants;
        if (!resources.Get(handle)) handle = resources.CreateConstantBuffer(sizeof(TerrainMeshletConstants));
        if (!handle.IsValid()) return false;
        resources.Update(handle, &view.constants, sizeof(view.constants));
        view.constantsHandle = handle;
    }

    call.shader = view.shader;
    call.constantBuffers[13] = view.constantsHandle;
    call.vsRawBuffers[0] = call.vertexBuffer;
    call.vsRawBuffers[1] = geometry.blob;
    call.vertexBuffer = {};
    call.indexBuffer = {};
    call.meshGroupCountX = geometry.meshletCount;
    call.meshGroupCountY = 1;
    call.meshGroupCountZ = 1;
    return true;
}

std::array<math::Vector3, 2> TerrainWorldBounds(const math::Matrix4& world,
    const math::Vector3& low, const math::Vector3& high)
{
    const auto localCenter = (low + high) * 0.5f;
    const auto localExtents = (high - low) * 0.5f;
    const auto center = world * math::Vector4{localCenter, 1.0f};
    const auto extent = [&](uint32_t row) {
        return std::abs(world.m[row][0]) * localExtents.x + std::abs(world.m[row][1]) * localExtents.y
            + std::abs(world.m[row][2]) * localExtents.z;
    };
    const math::Vector3 worldCenter{center.x, center.y, center.z};
    const math::Vector3 worldExtents{extent(0), extent(1), extent(2)};
    return {worldCenter - worldExtents, worldCenter + worldExtents};
}
bool PreserveFiberTerrainDetail(const RenderTerrainInput& input, const RenderTerrainPatch& patch,
    const math::Vector3& cameraPosition)
{
    if (!input.fiberSurface) return false;
    if (!input.fiberDistanceLod) return true;
    const auto bounds = TerrainWorldBounds(input.constants.worldMatrix,patch.aabbMin,patch.aabbMax);
    const math::Vector3 nearest{std::clamp(cameraPosition.x,bounds[0].x,bounds[1].x),
        std::clamp(cameraPosition.y,bounds[0].y,bounds[1].y),std::clamp(cameraPosition.z,bounds[0].z,bounds[1].z)};
    return (nearest-cameraPosition).LengthSq() <= input.fiberLodFar*input.fiberLodFar;
}

float TerrainLODDistanceSquared(const RenderTerrainInput& input, const RenderTerrainPatch& patch,
    const math::Vector3& cameraPosition)
{
    const auto localCenter = (patch.aabbMin + patch.aabbMax) * 0.5f;
    const auto worldCenter = input.constants.worldMatrix * math::Vector4{localCenter, 1.0f};
    return (math::Vector3{worldCenter.x, worldCenter.y, worldCenter.z} - cameraPosition).LengthSq();
}

bool UseFarTerrainLOD(const RenderTerrainInput& input, const RenderTerrainPatch& patch, float distanceSquared)
{
    if (input.farLodDistance <= 0.0f || !patch.indexBufferLOD[TERRAIN_FAR_LOD_INDEX].IsValid()) return false;
    /// @note 穴を含む粗い格子は細分化して保持するため、三角形が減らない場合もある。
    if (patch.indexCountLOD[TERRAIN_FAR_LOD_INDEX] >= patch.indexCountLOD[TERRAIN_FAR_LOD_INDEX - 1]) return false;
    const float distance = std::max(input.farLodDistance, input.chunkWorldSize * 6.0f);
    return distanceSquared >= distance * distance;
}

/// @note The viewing camera controls LOD in color and shadow passes; only visibility uses the light frustum.
static bool PrepareTerrainGpu(RenderPassContext& ctx, const RenderTerrainInput& input,
    const math::Frustum& frustum, GpuGeometryDispatch& output, bool selection = false, ComputeCall* deferred = nullptr)
{
    if ((ctx.settings.geometryDrawMode != GeometryDrawMode::GPU_INDIRECT
        && ctx.settings.geometryDrawMode != GeometryDrawMode::MESH_SHADER) || !ctx.renderer.SupportsIndirectDraw()
        || input.patches.empty() || !ctx.resources.Get(input.gpuHeights)
        || !ctx.resources.Get(input.gpuPatches)
        || (deferred ? !HasTerrainBuffer(ctx.resources, input.gpuIndices) : !ctx.resources.Get(input.gpuIndices))) return false;
    /// @note All visible and shadow variants must be ready before selecting a GPU LOD for either pass.
    for (size_t mode = 0; mode < 3; ++mode)
        if (!ctx.resources.Get(TerrainGpuShader(ctx.resources, mode))) return false;
    GpuGeometrySelectionView view;
    for (int i = 0; i < math::Frustum::PLANE_COUNT; ++i) {
        const auto& plane = frustum.GetPlane(i);
        view.frustumPlanes[i] = {plane.normal.x, plane.normal.y, plane.normal.z, plane.distance};
    }
    const float projectionScale = std::abs(ctx.camera.GetProjectionMatrix().m[1][1])
        * static_cast<float>(std::max(ctx.height, 1u)) * 0.5f;
    view.cameraAndProjectionScale = {ctx.camera.m_position.x, ctx.camera.m_position.y,
        ctx.camera.m_position.z, projectionScale};
    view.projectionMetrics = {projectionScale, 16.0f, std::max(ctx.camera.m_near, 0.001f),
        ctx.camera.m_projection == ProjectionMode::Orthographic ? 1.0f : 0.0f};
    const auto& world = input.constants.worldMatrix;
    /// @note The Frobenius norm bounds stretch under nonuniform scale, negative scale and shear.
    const float stretch = std::sqrt(world.m[0][0]*world.m[0][0] + world.m[0][2]*world.m[0][2]
        + world.m[1][0]*world.m[1][0] + world.m[1][2]*world.m[1][2]
        + world.m[2][0]*world.m[2][0] + world.m[2][2]*world.m[2][2]);
    std::vector<GpuGeometryCandidate> candidates;
    candidates.reserve(input.patches.size());
    static_assert(TERRAIN_LOD_STEPS.size() <= kGpuGeometryMaxLods);
    for (const auto& patch : input.patches) {
        const auto bounds = TerrainWorldBounds(world, patch.aabbMin, patch.aabbMax);
        GpuGeometryCandidate candidate;
        candidate.boundsMin = {bounds[0].x, bounds[0].y, bounds[0].z, 0};
        candidate.boundsMax = {bounds[1].x, bounds[1].y, bounds[1].z, 0};
        candidate.lodCount = 1;
        if (!selection && !PreserveFiberTerrainDetail(input, patch, ctx.camera.m_position)) {
            candidate.lodCount = static_cast<uint32_t>(TERRAIN_FAR_LOD_INDEX);
            if (input.farLodDistance > 0.0f && UseFarTerrainLOD(input, patch,
                TerrainLODDistanceSquared(input, patch, ctx.camera.m_position)))
                ++candidate.lodCount;
        }
        for (uint32_t lod = 0; lod < candidate.lodCount; ++lod)
            candidate.lods[lod] = {patch.indexCountLOD[lod], patch.gpuStartIndexLOD[lod], 0,
                patch.gpuPatch.columns * patch.gpuPatch.rows,
                std::max(patch.gpuPatch.cellSize * stretch * static_cast<float>(TERRAIN_LOD_STEPS[lod]), 0.0001f), {}};
        candidates.push_back(candidate);
    }
    auto& service = GetTerrainGpuResources(ctx.resources).selection;
    if (deferred)
        return service.PrepareDispatch(ctx.renderer, ctx.resources, view, candidates,
            input.gpuIndexCount, input.gpuVertexCapacity, *deferred, output);
    return service.Prepare(ctx.renderer, ctx.resources, view, candidates,
        input.gpuIndexCount, input.gpuVertexCapacity, output);
}
void BindTerrainGpu(DrawCall& call, const RenderTerrainInput& input,
    const GpuGeometryDispatch& dispatch, size_t patchIndex, ResourceHandle<ShaderTag> shader)
{
    call.vertexBuffer = {};
    call.indexBuffer = input.gpuIndices;
    call.shader = shader;
    call.vsBuffers = {input.gpuHeights, input.gpuPatches};
    call.indirectArguments = dispatch.indirectArguments;
    call.indirectArgumentOffset = dispatch.indirectArgumentOffset + patchIndex * kGpuGeometryIndexedArgumentStride;
    call.indirectInstanceOffset = true;
}
} /// @note namespace
struct TerrainCameraFrameCB {
    math::Matrix4 view;
    math::Matrix4 projection;
    math::Matrix4 viewProjection;
    math::Matrix4 invViewProjection;
    math::Vector3 cameraPos; float nearZ;
    /// @note LAYOUT: PerFrameCB / Constants.hlsli の CameraConstants と一致させること。
    float         farZ;      float _reserved;
    float         isOrthographic; float _pad;
};
static_assert(sizeof(TerrainCameraFrameCB) == 288, "PerFrameCB size mismatch");
static bool IsChunkVisible(
    const math::Frustum&  frustum,
    const math::Matrix4&  world,
    const math::Vector3&  localMin,
    const math::Vector3&  localMax)
{
    math::Vector3 worldMin = {  1e30f,  1e30f,  1e30f };
    math::Vector3 worldMax = { -1e30f, -1e30f, -1e30f };
    const math::Vector3 corners[8] = {
        { localMin.x, localMin.y, localMin.z }, { localMax.x, localMin.y, localMin.z },
        { localMin.x, localMax.y, localMin.z }, { localMax.x, localMax.y, localMin.z },
        { localMin.x, localMin.y, localMax.z }, { localMax.x, localMin.y, localMax.z },
        { localMin.x, localMax.y, localMax.z }, { localMax.x, localMax.y, localMax.z },
    };
    for (const auto& c : corners) {
        const math::Vector4 wp = world * math::Vector4{ c.x, c.y, c.z, 1.0f };
        worldMin.x = std::min(worldMin.x, wp.x); worldMax.x = std::max(worldMax.x, wp.x);
        worldMin.y = std::min(worldMin.y, wp.y); worldMax.y = std::max(worldMax.y, wp.y);
        worldMin.z = std::min(worldMin.z, wp.z); worldMax.z = std::max(worldMax.z, wp.z);
    }
    const math::Vector3 center  = { (worldMin.x + worldMax.x) * 0.5f,
                                     (worldMin.y + worldMax.y) * 0.5f,
                                     (worldMin.z + worldMax.z) * 0.5f };
    const math::Vector3 extents = { (worldMax.x - worldMin.x) * 0.5f,
                                     (worldMax.y - worldMin.y) * 0.5f,
                                     (worldMax.z - worldMin.z) * 0.5f };
    return frustum.IntersectsAABB(center, extents);
}
/// @brief 地形の描画と影で同じチャンク LOD を選ぶ。
/// @note 異なるメッシュで自己影を描くと粗い三角形が表示面を貫き、斜面に偽の影が出る。
static int SelectTerrainLOD(const RenderTerrainInput& input, const RenderTerrainPatch& chunk,
                            const math::Vector3& cameraPosition)
{
    if (PreserveFiberTerrainDetail(input,chunk,cameraPosition)) return 0;
    const float distSq = TerrainLODDistanceSquared(input, chunk, cameraPosition);
    if (UseFarTerrainLOD(input, chunk, distSq)) return static_cast<int>(TERRAIN_FAR_LOD_INDEX);
    const float nearDistance = input.chunkWorldSize * 2.0f;
    const float midDistance = input.chunkWorldSize * 6.0f;
    return distSq < nearDistance * nearDistance ? 0
         : distSq < midDistance * midDistance ? 1 : 2;
}

namespace {
/// @note 正向きの整数 XZ 面積と内辺の逆向き接続で、格子矩形の一重被覆を証明する。
/// @note 境界 fan は許可し、穴・重複・反転・T 字接続など証明できない形状は実三角形へ戻す。
/// @see Docs/design/occlusion-culling.md 地形の完全な被覆条件。
bool HasFullTerrainTopology(const TerrainOcclusionGeometry& geometry, const TerrainGpuPatch& grid,
    std::span<const uint16_t> indices)
{
    if (grid.columns < 2 || grid.rows < 2 || geometry.heights.size() > UINT16_MAX
        || uint64_t(grid.columns) * grid.rows != geometry.heights.size()
        || indices.empty() || indices.size() % 3 != 0) return false;
    const uint64_t expectedArea = uint64_t(grid.columns - 1) * (grid.rows - 1) * 2;
    if (indices.size() / 3 > expectedArea) return false;
    uint64_t area = 0;
    thread_local std::vector<uint64_t> edges;
    edges.clear(); edges.reserve(indices.size());
    for (size_t i = 0; i < indices.size(); i += 3) {
        const uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= geometry.heights.size() || b >= geometry.heights.size() || c >= geometry.heights.size()) return false;
        const int64_t ax = a % grid.columns, az = a / grid.columns;
        const int64_t bx = b % grid.columns, bz = b / grid.columns;
        const int64_t cx = c % grid.columns, cz = c / grid.columns;
        const int64_t triangleArea = (bz - az) * (cx - ax) - (bx - ax) * (cz - az);
        if (triangleArea <= 0 || uint64_t(triangleArea) > expectedArea - area) return false;
        area += uint64_t(triangleArea);
        const std::array<uint32_t, 3> triangle{a,b,c};
        for (size_t e = 0; e < triangle.size(); ++e) {
            const auto from = triangle[e], to = triangle[(e + 1) % triangle.size()];
            const uint64_t key = (uint64_t(std::min(from,to)) << 32) | std::max(from,to);
            edges.push_back((key << 1) | uint64_t(from > to));
        }
    }
    if (area != expectedArea) return false;
    std::sort(edges.begin(), edges.end());
    for (size_t first = 0; first < edges.size();) {
        const uint64_t key = edges[first] >> 1;
        size_t end = first + 1;
        while (end < edges.size() && (edges[end] >> 1) == key) ++end;
        if (end - first == 2) {
            if ((edges[first] & 1u) == (edges[first + 1] & 1u)) return false;
        } else if (end - first == 1) {
            const auto a = uint32_t(key >> 32), b = uint32_t(key);
            const auto ax = a % grid.columns, az = a / grid.columns;
            const auto bx = b % grid.columns, bz = b / grid.columns;
            if (!((ax == 0 && bx == 0) || (ax + 1 == grid.columns && bx + 1 == grid.columns)
                || (az == 0 && bz == 0) || (az + 1 == grid.rows && bz + 1 == grid.rows))) return false;
        } else return false;
        first = end;
    }
    return true;
}

struct TerrainVolume { math::Vector3 low, high; };

/// @note Sweep by X; active Z intervals are disjoint until the first overlap. Ends precede starts on shared edges.
bool TerrainTilesDisjoint(std::span<const TerrainVolume> tiles)
{
    FBZZ_PROFILE_SCOPE("ViewOcclusion::TerrainTileOverlap");
    struct Event { float x; size_t tile; bool starts; };
    std::vector<Event> events;
    events.reserve(tiles.size() * 2);
    for (size_t i = 0; i < tiles.size(); ++i) {
        const auto& tile = tiles[i];
        if (tile.low.x >= tile.high.x || tile.low.z >= tile.high.z) continue;
        events.push_back({tile.low.x, i, true});
        events.push_back({tile.high.x, i, false});
    }
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        return a.x == b.x ? a.starts < b.starts : a.x < b.x;
    });
    std::map<float, float> active;
    for (const auto& event : events) {
        const auto& tile = tiles[event.tile];
        if (!event.starts) {
            active.erase(tile.low.z);
            continue;
        }
        const auto next = active.lower_bound(tile.low.z);
        if ((next != active.end() && next->first < tile.high.z)
            || (next != active.begin() && std::prev(next)->second > tile.low.z)) return false;
        active.emplace_hint(next, tile.low.z, tile.high.z);
    }
    return true;
}

struct TerrainVolumeNode {
    TerrainVolume bounds;
    uint32_t first = 0, count = 0, left = 0, right = 0;
};
struct TerrainColumnVolume {
    TerrainVolume bounds;
    uint32_t sourceIndex = 0;
};
struct PatchTopologyProof {
    std::shared_ptr<const TerrainOcclusionGeometry> owner;
    std::array<ResourceHandle<BufferTag>, TERRAIN_LOD_STEPS.size()> indexBuffers{};
    uint32_t columns = 0, rows = 0;
    size_t lodCount = 0;
    float borderMin = 0, borderMax = 0;
};
struct PatchGridLocation {
    int32_t x = 0, z = 0;
    uint32_t columns = 0, rows = 0;
    bool operator==(const PatchGridLocation&) const = default;
};
struct SeamVertex {
    size_t patch = 0;
    uint32_t vertex = 0;
};
struct TerrainSeamNeighbor {
    size_t patch = 0;
    uint32_t vertex = 0, neighborVertex = 0;
};
struct TerrainSeamPatch {
    std::vector<TerrainSeamNeighbor> neighbors;
    std::shared_ptr<const TerrainOcclusionGeometry> validatedOwner;
    uint32_t validatedWorldY = 0;
    bool changed = true;
};
struct HeightFieldProof {
    std::vector<uint64_t> key, currentKey;
    std::vector<std::shared_ptr<const TerrainOcclusionGeometry>> owners;
    std::vector<PatchTopologyProof> topology;
    std::vector<PatchGridLocation> seamLayout, currentLayout;
    std::vector<float> worldHeights;
    std::vector<TerrainSeamPatch> seamPatches;
    std::vector<TerrainColumnVolume> volumes, currentVolumes;
    std::vector<TerrainVolumeNode> nodes;
    math::Vector3 low{}, high{};
    bool valid = false;
};

template<class Visit>
void VisitPatchBorder(uint32_t columns, uint32_t rows, Visit&& visit)
{
    for (uint32_t x = 0; x < columns; ++x) visit(x, 0);
    for (uint32_t z = 1; z + 1 < rows; ++z) {
        visit(0, z);
        visit(columns - 1, z);
    }
    for (uint32_t x = 0; x < columns; ++x) visit(x, rows - 1);
}

bool ProvePatchTopology(const RenderTerrainPatch& patch, size_t lodCount, PatchTopologyProof& proof)
{
    const auto& grid = patch.gpuPatch;
    if (proof.owner == patch.occlusion && proof.columns == grid.columns && proof.rows == grid.rows
        && proof.lodCount == lodCount && proof.indexBuffers == patch.indexBufferLOD) return true;
    FBZZ_PROFILE_SCOPE("ViewOcclusion::TerrainPatchTopology");
    proof = {};
    const auto& heights = patch.occlusion->heights;
    for (float height : heights) if (!std::isfinite(height)) return false;
    for (size_t lod = 1; lod < lodCount; ++lod) {
        if (!HasFullTerrainTopology(*patch.occlusion, grid, patch.occlusion->indices[lod])) return false;
    }
    /// @note Exact row-major LOD0 quads prove coverage and connectivity without sorting triangle edges.
    /// @note The near-plane test indexes this order by cell offset.
    const auto& levelZero = patch.occlusion->indices[0];
    for (uint32_t z = 0; z + 1 < grid.rows; ++z) for (uint32_t x = 0; x + 1 < grid.columns; ++x) {
        const uint32_t a = z * grid.columns + x, b = a + grid.columns, c = a + 1, d = b + 1;
        const size_t first = (size_t(z) * (grid.columns - 1) + x) * 6;
        if (levelZero[first] != a || levelZero[first + 1] != b || levelZero[first + 2] != c
            || levelZero[first + 3] != c || levelZero[first + 4] != b || levelZero[first + 5] != d) return false;
    }
    float borderMin = heights.front(), borderMax = borderMin;
    VisitPatchBorder(grid.columns, grid.rows, [&](uint32_t x, uint32_t z) {
        const float height = heights[z * grid.columns + x];
        borderMin = std::min(borderMin, height);
        borderMax = std::max(borderMax, height);
    });
    proof = {patch.occlusion, patch.indexBufferLOD, grid.columns, grid.rows, lodCount, borderMin, borderMax};
    return true;
}

bool ValidateTerrainSeams(HeightFieldProof& proof)
{
    FBZZ_PROFILE_SCOPE("ViewOcclusion::TerrainSeams");
    if (proof.currentLayout != proof.seamLayout) {
        FBZZ_PROFILE_SCOPE("ViewOcclusion::TerrainSeamLayout");
        proof.seamLayout = proof.currentLayout;
        proof.seamPatches.clear();
        proof.seamPatches.resize(proof.seamLayout.size());
        std::unordered_map<uint64_t, SeamVertex> border;
        border.reserve(16384);
        for (size_t i = 0; i < proof.seamLayout.size(); ++i) {
            const auto& grid = proof.seamLayout[i];
            VisitPatchBorder(grid.columns, grid.rows, [&](uint32_t x, uint32_t z) {
                const auto worldX = static_cast<uint32_t>(int64_t(grid.x) + x);
                const auto worldZ = static_cast<uint32_t>(int64_t(grid.z) + z);
                const uint64_t location = (uint64_t(worldX) << 32) | worldZ;
                const SeamVertex vertex{i, z * grid.columns + x};
                const auto [first, inserted] = border.emplace(location, vertex);
                if (inserted) return;
                const auto& adjacent = first->second;
                proof.seamPatches[i].neighbors.push_back({adjacent.patch, vertex.vertex, adjacent.vertex});
                proof.seamPatches[adjacent.patch].neighbors.push_back({i, adjacent.vertex, vertex.vertex});
            });
        }
    }
    for (size_t i = 0; i < proof.seamPatches.size(); ++i) {
        auto& patch = proof.seamPatches[i];
        patch.changed = patch.validatedOwner != proof.owners[i]
            || patch.validatedWorldY != std::bit_cast<uint32_t>(proof.worldHeights[i]);
    }
    for (size_t i = 0; i < proof.seamPatches.size(); ++i) {
        const auto& patch = proof.seamPatches[i];
        if (!patch.changed || patch.neighbors.empty()) continue;
        FBZZ_PROFILE_SCOPE("ViewOcclusion::TerrainSeamPatch");
        const auto& heights = proof.owners[i]->heights;
        const float worldY = proof.worldHeights[i];
        for (const auto& adjacent : patch.neighbors) {
            if (adjacent.patch < i && proof.seamPatches[adjacent.patch].changed) continue;
            const float height = heights[adjacent.vertex] + worldY;
            const float neighborHeight = proof.owners[adjacent.patch]->heights[adjacent.neighborVertex]
                + proof.worldHeights[adjacent.patch];
            if (height != neighborHeight) return false;
        }
    }
    /// @note A failed check leaves the last proven owners intact, so another edit cannot hide an unresolved crack.
    for (size_t i = 0; i < proof.seamPatches.size(); ++i) {
        auto& patch = proof.seamPatches[i];
        if (!patch.changed) continue;
        patch.validatedOwner = proof.owners[i];
        patch.validatedWorldY = std::bit_cast<uint32_t>(proof.worldHeights[i]);
    }
    return true;
}

bool TryRefitTerrainVolumeTree(HeightFieldProof& proof)
{
    if (proof.nodes.empty() || proof.volumes.size() != proof.currentVolumes.size()) return false;
    /// @note The original partition depends only on XZ and source order, including equal-center ties.
    const auto equalCoordinate = [](float a, float b) {
        return std::isfinite(a) && std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
    };
    for (const auto& volume : proof.volumes) {
        const auto& a = volume.bounds;
        const auto& b = proof.currentVolumes[volume.sourceIndex].bounds;
        if (!equalCoordinate(a.low.x, b.low.x) || !equalCoordinate(a.low.z, b.low.z)
            || !equalCoordinate(a.high.x, b.high.x) || !equalCoordinate(a.high.z, b.high.z)
            || !std::isfinite(b.high.y) || std::bit_cast<uint32_t>(b.high.y) == std::bit_cast<uint32_t>(-0.0f)) return false;
    }
    FBZZ_PROFILE_SCOPE("ViewOcclusion::TerrainVolumeRefit");
    for (auto& volume : proof.volumes) {
        volume.bounds.low.y = proof.low.y;
        volume.bounds.high.y = proof.currentVolumes[volume.sourceIndex].bounds.high.y;
    }
    /// @note Parents precede both children. Finite Y maxima without negative zero are independent of reduction order.
    for (auto it = proof.nodes.rbegin(); it != proof.nodes.rend(); ++it) {
        auto& node = *it;
        node.bounds.low.y = proof.low.y;
        if (node.count) {
            node.bounds.high.y = proof.volumes[node.first].bounds.high.y;
            for (uint32_t i = node.first + 1; i < node.first + node.count; ++i)
                node.bounds.high.y = std::max(node.bounds.high.y, proof.volumes[i].bounds.high.y);
        } else {
            node.bounds.high.y = std::max(proof.nodes[node.left].bounds.high.y, proof.nodes[node.right].bounds.high.y);
        }
    }
    return true;
}

bool BuildHeightFieldProof(RenderPassContext& ctx, HeightFieldProof& proof)
{
    FBZZ_PROFILE_SCOPE("ViewOcclusion::TerrainProof");
    auto& key = proof.currentKey;
    size_t ownerCount = 0;
    bool sameOwners = true;
    /// @note The filtered terrain inputs and all LOD geometry define this proof; view-only choices must not rebuild it.
    key.clear();
    key.push_back(ctx.resources.GetInstanceId()); key.push_back(ctx.resources.GetResetVersion());
    for (const auto& input : ctx.renderScene->terrains) {
        if (input.layer >= 32 || !(ctx.cullingMask & (1u << input.layer))) continue;
        key.push_back(input.patches.size());
        key.push_back(input.LODCount());
        for (const auto& row : input.constants.worldMatrix.m) for (float value : row) key.push_back(std::bit_cast<uint32_t>(value));
        for (const auto& patch : input.patches) {
            if (!patch.occlusion || patch.occlusion->columns.empty() || !HasTerrainBuffer(ctx.resources, patch.vertexBuffer)) return false;
            sameOwners = sameOwners && ownerCount < proof.owners.size()
                && proof.owners[ownerCount].get() == patch.occlusion.get();
            ++ownerCount;
            key.push_back(patch.vertexBuffer.id); key.push_back(patch.vertexBuffer.gen);
            const auto& g = patch.gpuPatch;
            key.insert(key.end(), {g.originX,g.originZ,g.columns,g.rows,g.terrainColumns,g.terrainRows,std::bit_cast<uint32_t>(g.cellSize)});
            const int selectedLod = SelectTerrainLOD(input, patch, ctx.camera.m_position);
            const auto& selectedIndices = patch.occlusion->indices[selectedLod];
            /// @note LOD の閾値やカメラが変わっても、実際に描かない level の列を cache hit で復活させない。
            /// @note Only the selected GPU LOD must remain live; the cached column proof still covers every CPU LOD.
            if (!HasTerrainBuffer(ctx.resources, patch.indexBufferLOD[selectedLod])
                || selectedIndices.empty() || selectedIndices.size() % 3 != 0
                || selectedIndices.size() != patch.indexCountLOD[selectedLod]) return false;
            for (size_t lod = 0; lod < input.LODCount(); ++lod) {
                key.push_back(patch.indexBufferLOD[lod].id); key.push_back(patch.indexBufferLOD[lod].gen);
                key.push_back(patch.indexCountLOD[lod]);
                key.push_back(patch.occlusion->indices[lod].size());
            }
            for (float v : {patch.aabbMin.y,patch.aabbMax.y}) key.push_back(std::bit_cast<uint32_t>(v));
        }
    }
    /// @note A view with no terrain must not evict the proof another view uses in the same frame.
    if (ownerCount == 0) return false;
    if (sameOwners && ownerCount == proof.owners.size() && key == proof.key) return proof.valid;
    FBZZ_PROFILE_SCOPE("ViewOcclusion::TerrainProofBuild");
    /// @note Pin new geometry only on a miss, including negative proofs; old owners prevent address reuse while comparing.
    if (proof.key.empty() || proof.key[0] != key[0] || proof.key[1] != key[1]) {
        proof.seamLayout.clear();
        proof.nodes.clear();
    }
    proof.key = key; proof.owners.clear(); proof.owners.reserve(ownerCount);
    proof.topology.resize(ownerCount);
    for (const auto& input : ctx.renderScene->terrains) {
        if (input.layer >= 32 || !(ctx.cullingMask & (1u << input.layer))) continue;
        for (const auto& patch : input.patches) proof.owners.push_back(patch.occlusion);
    }
    proof.valid = false; proof.currentVolumes.clear();
    std::vector<TerrainVolume> tiles;
    proof.currentLayout.clear(); proof.currentLayout.reserve(ownerCount);
    proof.worldHeights.clear(); proof.worldHeights.reserve(ownerCount);
    float commonCell = 0;
    double area = 0;
    size_t patchIndex = 0;
    for (const auto& input : ctx.renderScene->terrains) {
        if (input.layer >= 32 || !(ctx.cullingMask & (1u << input.layer))) continue;
        if (input.patches.empty()) return false;
        const auto& world = input.constants.worldMatrix;
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
            if (c != 3 && world.m[r][c] != (r == c ? 1.0f : 0.0f)) return false;
        if (world.m[3][3] != 1) return false;
        const auto& base = input.patches.front().gpuPatch;
        if (base.terrainColumns < 2 || base.terrainRows < 2) return false;
        int exponent = 0;
        if (std::frexp(base.cellSize,&exponent) != 0.5f || (commonCell && commonCell != base.cellSize)) return false;
        commonCell = base.cellSize;
        const double rootX = world.m[0][3] / commonCell, rootZ = world.m[2][3] / commonCell;
        if (!std::isfinite(rootX) || !std::isfinite(rootZ) || std::floor(rootX) != rootX || std::floor(rootZ) != rootZ
            || std::abs(rootX) + base.terrainColumns > 4000000 || std::abs(rootZ) + base.terrainRows > 4000000) return false;
        TerrainVolume tile{{world.m[0][3],0,world.m[2][3]},
            {world.m[0][3] + float(base.terrainColumns - 1) * commonCell,0,world.m[2][3] + float(base.terrainRows - 1) * commonCell}};
        area += double(tile.high.x - tile.low.x) * (tile.high.z - tile.low.z);
        uint32_t nextX = 0, nextZ = 0, rowHeight = 0;
        const size_t lodCount = input.LODCount();
        const auto largestStep = static_cast<uint32_t>(TERRAIN_LOD_STEPS[lodCount - 1]);
        for (const auto& patch : input.patches) {
            const auto& g = patch.gpuPatch;
            if (g.columns < 2 || g.rows < 2 || g.originX != nextX || g.originZ != nextZ
                || g.originX % largestStep || g.originZ % largestStep
                || g.terrainColumns != base.terrainColumns || g.terrainRows != base.terrainRows || g.cellSize != commonCell
                || patch.indexCountLOD[0] != (g.columns - 1) * (g.rows - 1) * 6u
                || patch.occlusion->indices[0].size() != patch.indexCountLOD[0]
                || patch.occlusion->heights.size() != size_t(g.columns) * g.rows) return false;
            for (size_t lod = 0; lod < lodCount; ++lod)
                if (patch.occlusion->indices[lod].size() != patch.indexCountLOD[lod]) return false;
            auto& topology = proof.topology[patchIndex++];
            if (!ProvePatchTopology(patch, lodCount, topology)) return false;
            if (!nextX) rowHeight = g.rows - 1;
            if (rowHeight != g.rows - 1) return false;
            nextX += g.columns - 1;
            if (nextX == base.terrainColumns - 1) { nextX = 0; nextZ += rowHeight; }
            if (nextX >= base.terrainColumns || nextZ >= base.terrainRows) return false;
            const float minimum = patch.aabbMin.y + world.m[1][3], maximum = patch.aabbMax.y + world.m[1][3];
            if (!std::isfinite(minimum) || !std::isfinite(maximum)) return false;
            if (tiles.empty() && proof.currentVolumes.empty()) { proof.low = tile.low; proof.high = tile.high; proof.low.y = minimum; proof.high.y = maximum; }
            proof.low.y = std::min(proof.low.y,minimum); proof.high.y = std::max(proof.high.y,maximum);
            /// @note Shared samples are indexed by the integer XZ lattice. Boundary extrema prove every sample remains finite in world Y.
            if (!std::isfinite(topology.borderMin + world.m[1][3])
                || !std::isfinite(topology.borderMax + world.m[1][3])) return false;
            proof.currentLayout.push_back({static_cast<int32_t>(int64_t(rootX) + g.originX),
                static_cast<int32_t>(int64_t(rootZ) + g.originZ), g.columns, g.rows});
            proof.worldHeights.push_back(world.m[1][3]);
            for (const auto& column : patch.occlusion->columns) {
                const auto& r = column.rectangle;
                if (!std::isfinite(column.minimumHeight)) return false;
                const TerrainVolume bounds{{r.x + world.m[0][3],0,r.y + world.m[2][3]},
                    {r.z + world.m[0][3],column.minimumHeight + world.m[1][3] - 0.01f,r.w + world.m[2][3]}};
                proof.currentVolumes.push_back({bounds, static_cast<uint32_t>(proof.currentVolumes.size())});
            }
        }
        if (nextX || nextZ + 1 != base.terrainRows) return false;
        tiles.push_back(tile);
        proof.low.x = std::min(proof.low.x,tile.low.x); proof.low.z = std::min(proof.low.z,tile.low.z);
        proof.high.x = std::max(proof.high.x,tile.high.x); proof.high.z = std::max(proof.high.z,tile.high.z);
    }
    if (tiles.empty() || area != double(proof.high.x - proof.low.x) * (proof.high.z - proof.low.z)
        || !TerrainTilesDisjoint(tiles)) return false;
    if (!ValidateTerrainSeams(proof)) return false;
    FBZZ_PROFILE_SCOPE("ViewOcclusion::TerrainVolumeTree");
    if (TryRefitTerrainVolumeTree(proof)) { proof.valid = true; return true; }
    if (proof.currentVolumes.empty() || proof.currentVolumes.size() > UINT32_MAX / 2) return false;
    proof.volumes.swap(proof.currentVolumes);
    proof.nodes.clear();
    for (auto& volume : proof.volumes) volume.bounds.low.y = proof.low.y;
    /// @note 節の箱は検査対象の和集合を包むだけ。形状の証明と同時に作り、隠れた山裏の列を一括で省く。
    proof.nodes.reserve(proof.volumes.size() * 2);
    const auto build = [&](auto&& self, uint32_t first, uint32_t count) -> uint32_t {
        const uint32_t index = uint32_t(proof.nodes.size());
        TerrainVolumeNode node; node.first = first; node.count = count;
        node.bounds = proof.volumes[first].bounds;
        for (uint32_t i = first + 1; i < first + count; ++i) {
            const auto& box = proof.volumes[i].bounds;
            node.bounds.low = {std::min(node.bounds.low.x,box.low.x),std::min(node.bounds.low.y,box.low.y),std::min(node.bounds.low.z,box.low.z)};
            node.bounds.high = {std::max(node.bounds.high.x,box.high.x),std::max(node.bounds.high.y,box.high.y),std::max(node.bounds.high.z,box.high.z)};
        }
        proof.nodes.push_back(node);
        if (count > 8) {
            const bool splitX = node.bounds.high.x - node.bounds.low.x > node.bounds.high.z - node.bounds.low.z;
            const uint32_t half = count / 2;
            std::nth_element(proof.volumes.begin() + first,proof.volumes.begin() + first + half,proof.volumes.begin() + first + count,
                [=](const auto& a,const auto& b) {
                    const auto& left = a.bounds;
                    const auto& right = b.bounds;
                    return splitX ? left.low.x + left.high.x < right.low.x + right.high.x
                                  : left.low.z + left.high.z < right.low.z + right.high.z;
                });
            const auto left = self(self,first,half), right = self(self,first + half,count - half);
            proof.nodes[index].left = left; proof.nodes[index].right = right; proof.nodes[index].count = 0;
        }
        return index;
    };
    build(build,0,uint32_t(proof.volumes.size()));
    proof.valid = true;
    return true;
}

bool NearPlaneAboveHeightField(RenderPassContext& ctx, const HeightFieldProof& proof,
    const math::Vector3& nearLow, const math::Vector3& nearHigh)
{
    FBZZ_PROFILE_SCOPE("ViewOcclusion::TerrainNear");
    const auto camera = ctx.camera.m_position;
    if (nearLow.y > proof.high.y + 0.01f) return true;
    double maximum = -std::numeric_limits<double>::infinity();
    for (const auto& input : ctx.renderScene->terrains) {
        if (input.layer >= 32 || !(ctx.cullingMask & (1u << input.layer))) continue;
        const auto& world = input.constants.worldMatrix;
        for (const auto& patch : input.patches) {
            const auto& g = patch.gpuPatch;
            if (float(g.originX) * g.cellSize + world.m[0][3] > nearHigh.x
                || float(g.originX + g.columns - 1) * g.cellSize + world.m[0][3] < nearLow.x
                || float(g.originZ) * g.cellSize + world.m[2][3] > nearHigh.z
                || float(g.originZ + g.rows - 1) * g.cellSize + world.m[2][3] < nearLow.z) continue;
            const auto& geometry = *patch.occlusion;
            const int lod = SelectTerrainLOD(input,patch,camera);
            const auto& indices = geometry.indices[lod];
            const auto triangle = [&](size_t i) {
                if (i + 2 >= indices.size()) return false;
                struct NearPoint { double x, y, z; };
                std::array<NearPoint,8> polygon{}, clipped{}; size_t count = 3;
                for (size_t v = 0; v < 3; ++v) {
                    const auto index = indices[i + v];
                    if (index >= geometry.heights.size()) return false;
                    polygon[v] = {float(g.originX + index % g.columns) * g.cellSize + world.m[0][3],
                        geometry.heights[index] + world.m[1][3],float(g.originZ + index / g.columns) * g.cellSize + world.m[2][3]};
                }
                for (int plane = 0; plane < 4 && count; ++plane) {
                    size_t output = 0;
                    const auto distance = [&](const NearPoint& p) {
                        if (plane == 0) return p.x - nearLow.x;
                        if (plane == 1) return nearHigh.x - p.x;
                        if (plane == 2) return p.z - nearLow.z;
                        return nearHigh.z - p.z;
                    };
                    for (size_t v = 0; v < count; ++v) {
                        const auto a = polygon[v], b = polygon[(v + 1) % count];
                        const double da = distance(a), db = distance(b);
                        if (da >= 0) {
                            if (output == clipped.size()) return false;
                            clipped[output++] = a;
                        }
                        if ((da > 0 && db < 0) || (da < 0 && db > 0)) {
                            if (output == clipped.size()) return false;
                            const double t = da / (da - db);
                            clipped[output++] = {a.x + (b.x - a.x) * t,a.y + (b.y - a.y) * t,a.z + (b.z - a.z) * t};
                        }
                    }
                    count = output; polygon = clipped;
                }
                for (size_t v = 0; v < count; ++v) maximum = std::max(maximum,polygon[v].y);
                return true;
            };
            if (lod == 0) {
                /// @note 穴のない LOD0 は row-major の 2 面/セル。near の footprint と交差するセルだけを調べる。
                const auto cell = [&](float value,float offset,uint32_t origin,uint32_t cells) {
                    return std::clamp(int(std::floor((double(value) - offset) / g.cellSize)) - int(origin),0,int(cells) - 1);
                };
                const int x0 = cell(nearLow.x,world.m[0][3],g.originX,g.columns - 1);
                const int x1 = cell(nearHigh.x,world.m[0][3],g.originX,g.columns - 1);
                const int z0 = cell(nearLow.z,world.m[2][3],g.originZ,g.rows - 1);
                const int z1 = cell(nearHigh.z,world.m[2][3],g.originZ,g.rows - 1);
                for (int z = z0; z <= z1; ++z) for (int x = x0; x <= x1; ++x) {
                    const size_t first = (size_t(z) * (g.columns - 1) + x) * 6;
                    if (!triangle(first) || !triangle(first + 3)) return false;
                }
            } else {
                for (size_t i = 0; i + 2 < indices.size(); i += 3) if (!triangle(i)) return false;
            }
        }
    }
    return std::isfinite(maximum) && nearLow.y > maximum + 0.01;
}

/// @note 連続した高さ場の上から列内へ入るレイは、列より手前で実表面を横切る。穴・段差・near 交差は証明しない。
/// @see https://www.pbr-book.org/4ed/Shapes/Basic_Shape_Interface Conservative bounds must enclose the represented surface.
bool RasterHeightField(RenderPassContext& ctx)
{
    /// @note 正投影の平行レイは camera を共通始点とする領域進入の証明を満たさない。
    if (ctx.camera.m_projection == ProjectionMode::Orthographic) return false;
    thread_local HeightFieldProof proof;
    if (!BuildHeightFieldProof(ctx,proof)) return false;
    const auto projection = MakeJitteredProjection(ctx.camera,ctx.taaJitterNdcX,ctx.taaJitterNdcY);
    const auto view = ctx.camera.GetViewMatrix();
    const auto inverseView = math::Matrix4::Inverse(view);
    math::Vector3 nearLow{INFINITY,INFINITY,INFINITY}, nearHigh{-INFINITY,-INFINITY,-INFINITY};
    const float nearZ = ctx.camera.m_near, nearW = projection.m[3][2] * nearZ + projection.m[3][3];
    for (int corner = 0; corner < 4; ++corner) {
        const auto p = inverseView * math::Vector4{(((corner & 1) ? 1.0f : -1.0f) * nearW - projection.m[0][2] * nearZ - projection.m[0][3]) / projection.m[0][0],
            (((corner & 2) ? 1.0f : -1.0f) * nearW - projection.m[1][2] * nearZ - projection.m[1][3]) / projection.m[1][1],nearZ,1};
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return true;
        nearLow = {std::min(nearLow.x,p.x - 0.01f),std::min(nearLow.y,p.y - 0.01f),std::min(nearLow.z,p.z - 0.01f)};
        nearHigh = {std::max(nearHigh.x,p.x + 0.01f),std::max(nearHigh.y,p.y + 0.01f),std::max(nearHigh.z,p.z + 0.01f)};
    }
    const auto camera = ctx.camera.m_position;
    const bool inside = camera.x >= proof.low.x && camera.x <= proof.high.x && camera.z >= proof.low.z && camera.z <= proof.high.z;
    if ((!inside && (nearLow.y <= proof.high.y + 0.01f || camera.y <= proof.high.y + 0.01f))
        || (inside && !NearPlaneAboveHeightField(ctx,proof,nearLow,nearHigh))) return true;
    FBZZ_PROFILE_SCOPE("ViewOcclusion::TerrainRaster");
    /// @note 透視の外向き面は眼点が面の外側にある場合だけ表面。不確実な境界面は両方残す。
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_rasterizer_desc FrontCounterClockwise and back-face culling.
    constexpr double VIEW_TOLERANCE = 64.0 * std::numeric_limits<float>::epsilon();
    bool selectFaces = ctx.camera.m_projection == ProjectionMode::Perspective
        && ctx.camera.m_near > 0 && ctx.camera.m_far > ctx.camera.m_near
        && projection.m[0][0] > 0 && projection.m[1][1] > 0
        && projection.m[0][1] == 0 && projection.m[0][3] == 0
        && projection.m[1][0] == 0 && projection.m[1][3] == 0
        && projection.m[2][0] == 0 && projection.m[2][1] == 0
        && projection.m[2][2] > 0 && projection.m[2][3] < 0
        && projection.m[3][0] == 0 && projection.m[3][1] == 0
        && projection.m[3][2] == 1 && projection.m[3][3] == 0
        && view.m[3][0] == 0 && view.m[3][1] == 0 && view.m[3][2] == 0 && view.m[3][3] == 1
        && std::abs(inverseView.m[3][0]) <= VIEW_TOLERANCE && std::abs(inverseView.m[3][1]) <= VIEW_TOLERANCE
        && std::abs(inverseView.m[3][2]) <= VIEW_TOLERANCE && std::abs(inverseView.m[3][3] - 1) <= VIEW_TOLERANCE;
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
        selectFaces = selectFaces && std::isfinite(view.m[r][c])
            && std::isfinite(inverseView.m[r][c]) && std::isfinite(projection.m[r][c]);
    if (selectFaces) {
        for (int a = 0; a < 3; ++a) for (int b = a; b < 3; ++b) {
            double dot = 0;
            for (int c = 0; c < 3; ++c) dot += double(view.m[a][c]) * view.m[b][c];
            if (std::abs(dot - (a == b ? 1.0 : 0.0)) > VIEW_TOLERANCE) selectFaces = false;
        }
        const double determinant = double(view.m[0][0]) * (double(view.m[1][1]) * view.m[2][2] - double(view.m[1][2]) * view.m[2][1])
            - double(view.m[0][1]) * (double(view.m[1][0]) * view.m[2][2] - double(view.m[1][2]) * view.m[2][0])
            + double(view.m[0][2]) * (double(view.m[1][0]) * view.m[2][1] - double(view.m[1][1]) * view.m[2][0]);
        selectFaces = selectFaces && determinant > 0;
    }
    std::array<double,3> eyeLow{}, eyeHigh{};
    if (selectFaces) {
        const auto& bounds = proof.nodes[0].bounds;
        const std::array<float,3> position{camera.x,camera.y,camera.z};
        const std::array<float,3> low{bounds.low.x,bounds.low.y,bounds.low.z}, high{bounds.high.x,bounds.high.y,bounds.high.z};
        for (size_t axis = 0; axis < 3; ++axis) {
            const double inverseEye = double(inverseView.m[axis][3]) / inverseView.m[3][3];
            if (!std::isfinite(position[axis]) || !std::isfinite(inverseEye)
                || !std::isfinite(low[axis]) || !std::isfinite(high[axis]) || low[axis] > high[axis]) {
                selectFaces = false; break;
            }
            /// @note Camera と float View の眼点差、および列座標の丸めをビューごとの外向き区間へ含める。
            const double margin = VIEW_TOLERANCE * std::max({1.0,std::abs(double(position[axis])),std::abs(inverseEye),
                std::abs(double(low[axis])),std::abs(double(high[axis]))});
            eyeLow[axis] = std::nextafter(std::min(double(position[axis]),inverseEye) - margin,-std::numeric_limits<double>::infinity());
            eyeHigh[axis] = std::nextafter(std::max(double(position[axis]),inverseEye) + margin,std::numeric_limits<double>::infinity());
        }
    }
    const auto draw = [&](TerrainVolume box) {
        if (!inside) {
            float ratio = 0;
            if (camera.x < proof.low.x) ratio = std::max(ratio,(proof.low.x - camera.x) / (box.low.x - camera.x));
            if (camera.x > proof.high.x) ratio = std::max(ratio,(camera.x - proof.high.x) / (camera.x - box.high.x));
            if (camera.z < proof.low.z) ratio = std::max(ratio,(proof.low.z - camera.z) / (box.low.z - camera.z));
            if (camera.z > proof.high.z) ratio = std::max(ratio,(camera.z - proof.high.z) / (camera.z - box.high.z));
            if (ratio > 0) box.low.y = std::max(box.low.y,camera.y - (camera.y - proof.high.y - 0.02f) / ratio);
        }
        if (box.low.y > box.high.y) return;
        const auto& a = box.low; const auto& b = box.high;
        const std::array<bool,6> visible{{!selectFaces || eyeLow[2] <= a.z,!selectFaces || eyeHigh[2] >= b.z,
            !selectFaces || eyeLow[0] <= a.x,!selectFaces || eyeHigh[0] >= b.x,
            !selectFaces || eyeLow[1] <= a.y,!selectFaces || eyeHigh[1] >= b.y}};
        uint8_t faceMask = 0;
        for (size_t index = 0; index < visible.size(); ++index)
            if (visible[index]) faceMask |= uint8_t(1u << index);
        ctx.occlusionCuller->RasterBox(box.low,box.high,faceMask);
    };
    const auto visit = [&](auto&& self, uint32_t index) -> void {
        const auto& node = proof.nodes[index]; const auto& box = node.bounds;
        /// @note All columns share the terrain minimum; an inverted union cannot contain a drawable column.
        if (box.low.y > box.high.y) return;
        if (ctx.frustumCullingEnabled && ctx.cameraFrustum
            && !ctx.cameraFrustum->IntersectsAABB((box.low + box.high) * 0.5f,(box.high - box.low) * 0.5f)) return;
        if (!ctx.occlusionCuller->TestAabb(box.low,box.high)) return;
        if (node.count) {
            for (uint32_t i = node.first; i < node.first + node.count; ++i) draw(proof.volumes[i].bounds);
        } else {
            const auto distance = [&](uint32_t child) { const auto& b = proof.nodes[child].bounds; return ((b.low + b.high) * 0.5f - camera).LengthSq(); };
            const bool leftFirst = distance(node.left) < distance(node.right);
            self(self,leftFirst ? node.left : node.right); self(self,leftFirst ? node.right : node.left);
        }
    };
    visit(visit,0);
    return true;
}

/// @note 穴のないパッチの全表面が正面を向く場合だけ、投影境界の kernel 内にある矩形を使う。
/// @note 矩形の深度は全表面より遠い。内部 tessellation の継ぎ目に生じる未被覆セルを遮蔽漏れにしない。
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3d11/conservative-rasterization Underestimated coverage.
bool RasterTerrainKernel(RenderPassContext& ctx, const RenderTerrainPatch& patch, const math::Matrix4& world,
    std::span<const uint16_t> indices, const math::Matrix4& view, const math::Matrix4& projection,
    const math::Matrix4& inverseView)
{
    const auto& grid = patch.gpuPatch;
    if (grid.columns < 2 || grid.rows < 2 || patch.indexCountLOD[0] != (grid.columns - 1) * (grid.rows - 1) * 6u) return false;
    const auto& heights = patch.occlusion->heights;
    if (heights.size() != size_t(grid.columns) * grid.rows) return false;
    if (!HasFullTerrainTopology(*patch.occlusion, grid, indices)) return false;
    struct Point { double x, y; };
    /// @note CPU 作業領域は容量だけを再利用し、ビューや LOD の投影結果は毎回捨てる。
    thread_local std::vector<Point> points;
    thread_local std::vector<uint8_t> projected;
    points.resize(heights.size()); projected.assign(heights.size(), 0);
    float farthest = 0;
    const auto worldView = view * world;
    const auto project = [&](size_t i) {
        if (i >= heights.size()) return false;
        if (projected[i]) return true;
        const auto position = worldView * math::Vector4{float(grid.originX + i % grid.columns) * grid.cellSize,
            heights[i], float(grid.originZ + i / grid.columns) * grid.cellSize, 1};
        const auto clip = projection * position;
        if (!std::isfinite(position.z) || position.z <= ctx.camera.m_near || !std::isfinite(clip.w) || clip.w <= 0) return false;
        points[i] = {(double(clip.x) / clip.w + 1) * 0.5 * OcclusionCuller::kWidth,
            (1 - double(clip.y) / clip.w) * 0.5 * OcclusionCuller::kHeight};
        if (!std::isfinite(points[i].x) || !std::isfinite(points[i].y)) return false;
        farthest = std::max(farthest, position.z);
        projected[i] = 1;
        return true;
    };
    const auto edge = [](const Point& a, const Point& b, const Point& p) {
        return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
    };
    if (indices.empty() || indices.size() % 3) return false;
    for (size_t i = 0; i < indices.size(); i += 3) {
        if (!project(indices[i]) || !project(indices[i + 1]) || !project(indices[i + 2])
            || edge(points[indices[i]], points[indices[i + 1]], points[indices[i + 2]]) <= 1.0e-8) return false;
    }
    thread_local std::vector<uint32_t> boundary;
    boundary.clear();
    for (uint32_t z = 0; z < grid.rows; ++z) boundary.push_back(z * grid.columns);
    for (uint32_t x = 1; x < grid.columns; ++x) boundary.push_back((grid.rows - 1) * grid.columns + x);
    for (uint32_t z = grid.rows - 1; z > 0; --z) boundary.push_back((z - 1) * grid.columns + grid.columns - 1);
    for (uint32_t x = grid.columns - 1; x > 1; --x) boundary.push_back(x - 1);
    thread_local std::vector<Point> polygon, clipped;
    polygon = {{0,0},{double(OcclusionCuller::kWidth),0},
        {double(OcclusionCuller::kWidth),double(OcclusionCuller::kHeight)},{0,double(OcclusionCuller::kHeight)}};
    for (size_t i = 0; i < boundary.size(); ++i) {
        if (!projected[boundary[i]] || !projected[boundary[(i + 1) % boundary.size()]]) return false;
        const auto a = points[boundary[i]], b = points[boundary[(i + 1) % boundary.size()]];
        const double inset = 0.01 * (std::abs(b.x - a.x) + std::abs(b.y - a.y));
        clipped.clear();
        for (size_t j = 0; j < polygon.size(); ++j) {
            const auto p = polygon[j], q = polygon[(j + 1) % polygon.size()];
            const double dp = edge(a, b, p) - inset, dq = edge(a, b, q) - inset;
            if (dp >= 0) clipped.push_back(p);
            if ((dp >= 0) != (dq >= 0)) {
                const double t = dp / (dp - dq);
                clipped.push_back({p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t});
            }
        }
        polygon.swap(clipped);
        if (polygon.size() < 3 || polygon.size() > 128) return false;
    }
    Point center{};
    double minX = polygon[0].x, maxX = minX, minY = polygon[0].y, maxY = minY;
    for (const auto& p : polygon) {
        center.x += p.x; center.y += p.y;
        minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
    }
    center.x /= polygon.size(); center.y /= polygon.size();
    double halfX = (maxX - minX) * 0.5, halfY = (maxY - minY) * 0.5, scale = 1;
    for (size_t i = 0; i < polygon.size(); ++i) {
        const auto a = polygon[i], b = polygon[(i + 1) % polygon.size()];
        const double reach = std::abs(b.x - a.x) * halfY + std::abs(b.y - a.y) * halfX;
        if (reach > 0) scale = std::min(scale, std::max(0.0, edge(a, b, center) / reach));
    }
    halfX *= scale * 0.98; halfY *= scale * 0.98;
    if (halfX < 1 || halfY < 1) return false;
    const double planeZ = farthest + 0.05 + farthest * 0.00001;
    const double planeW = projection.m[3][2] * planeZ + projection.m[3][3];
    const auto unproject = [&](double x, double y) {
        /// @note far の NDC 深度を逆投影すると精度を失うため、既知の view-Z の平面を直接解く。
        const double px = ((x * 2 / OcclusionCuller::kWidth - 1) * planeW
            - projection.m[0][2] * planeZ - projection.m[0][3]) / projection.m[0][0];
        const double py = ((1 - y * 2 / OcclusionCuller::kHeight) * planeW
            - projection.m[1][2] * planeZ - projection.m[1][3]) / projection.m[1][1];
        const auto p = inverseView * math::Vector4{float(px), float(py), float(planeZ), 1};
        return math::Vector3{p.x, p.y, p.z};
    };
    ctx.occlusionCuller->RasterQuad(unproject(center.x - halfX, center.y + halfY), unproject(center.x - halfX, center.y - halfY),
        unproject(center.x + halfX, center.y - halfY), unproject(center.x + halfX, center.y + halfY));
    return true;
}
} /// @note namespace

void RasterTerrainOccluders(RenderPassContext& ctx)
{
    if (!ctx.renderScene || !ctx.occlusionCuller || !ctx.occlusionCullingEnabled || ctx.settings.IsWireframe()) return;
    FBZZ_PROFILE_SCOPE("ViewOcclusion::Terrain");
    if (RasterHeightField(ctx)) return;
    struct Candidate { const RenderTerrainInput* input; const RenderTerrainPatch* patch; int lod; };
    std::array<std::vector<Candidate>, TERRAIN_LOD_STEPS.size()> candidates;
    for (const auto& input : ctx.renderScene->terrains) {
        if (input.layer >= 32 || (ctx.cullingMask & (1u << input.layer)) == 0) continue;
        const auto& world = input.constants.worldMatrix;
        bool affine = world.m[3][0] == 0 && world.m[3][1] == 0 && world.m[3][2] == 0 && world.m[3][3] == 1;
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col) affine = affine && std::isfinite(world.m[row][col]);
        if (!affine) continue;
        for (const auto& patch : input.patches) {
            if (!patch.occlusion || !HasTerrainBuffer(ctx.resources, patch.vertexBuffer)
                || (ctx.frustumCullingEnabled && ctx.cameraFrustum && !IsChunkVisible(*ctx.cameraFrustum,
                    world, patch.aabbMin, patch.aabbMax))) continue;
            const int lod = SelectTerrainLOD(input, patch, ctx.camera.m_position);
            const auto& indices = patch.occlusion->indices[lod];
            const auto& grid = patch.gpuPatch;
            /// @note Only the selected drawable LOD proves coverage; stale CPU topology or an incomplete grid must fail open.
            if (!HasTerrainBuffer(ctx.resources, patch.indexBufferLOD[lod]) || indices.empty() || indices.size() % 3 != 0
                || indices.size() != patch.indexCountLOD[lod] || !grid.columns || !grid.rows
                || uint64_t(grid.columns) * grid.rows != patch.occlusion->heights.size()
                || !std::isfinite(grid.cellSize) || grid.cellSize <= 0
                || grid.originX > UINT32_MAX - (grid.columns - 1) || grid.originZ > UINT32_MAX - (grid.rows - 1)) continue;
            candidates[lod].push_back({&input, &patch, lod});
        }
    }
    /// @note 遠方の粗い実形状を先に処理する。予算超過は遮蔽不足になるだけで可視物を消さない。
    /// @note Fixed LOD buckets retain insertion order within each priority.
    size_t triangleBudget = 100000;
    const auto view = ctx.camera.GetViewMatrix();
    const auto projection = MakeJitteredProjection(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    const auto inverseView = math::Matrix4::Inverse(view);
    for (auto lod = candidates.rbegin(); lod != candidates.rend(); ++lod) for (const auto& candidate : *lod) {
        const auto& patch = *candidate.patch;
        const auto& geometry = *patch.occlusion;
        const auto& indices = geometry.indices[candidate.lod];
        const auto& grid = patch.gpuPatch;
        if (!grid.columns || !grid.rows) continue;
        const auto& world = candidate.input->constants.worldMatrix;
        if (indices.size() / 3 <= triangleBudget
            && RasterTerrainKernel(ctx, patch, world, indices, view, projection, inverseView)) {
            triangleBudget -= indices.size() / 3;
            if (!triangleBudget) return;
            continue;
        }
        const auto position = [&](uint16_t index) {
            const auto point = world * math::Vector4{float(grid.originX + index % grid.columns) * grid.cellSize,
                geometry.heights[index], float(grid.originZ + index / grid.columns) * grid.cellSize, 1};
            return math::Vector3{point.x, point.y, point.z};
        };
        for (size_t i = 0; i + 2 < indices.size() && triangleBudget; i += 3, --triangleBudget) {
            if (indices[i] >= geometry.heights.size() || indices[i + 1] >= geometry.heights.size()
                || indices[i + 2] >= geometry.heights.size()) continue;
            if (i + 5 < indices.size() && triangleBudget > 1 && indices[i + 3] == indices[i + 2]
                && indices[i + 4] == indices[i + 1] && indices[i + 5] < geometry.heights.size()) {
                ctx.occlusionCuller->RasterQuad(position(indices[i + 1]), position(indices[i + 5]),
                    position(indices[i + 2]), position(indices[i]));
                i += 3; --triangleBudget;
                continue;
            }
            ctx.occlusionCuller->RasterTriangle(position(indices[i]), position(indices[i + 1]), position(indices[i + 2]));
        }
        if (!triangleBudget) return;
    }
}
bool DrawTerrainOccluderDepth(RenderPassContext& ctx)
{
    const auto scene = ctx.renderScene;
    if (!ctx.gpuOcclusionTerrain || !scene || scene->terrains.empty()) return false;
    auto& resources = ctx.resources;
    auto& shared = resources.Rendering().Shared();
    auto& colorShader = ctx.isDeferred ? shared.terrainGBufferShader : shared.terrainShader;
    if (!resources.Get(colorShader))
        colorShader = resources.LoadShader(ctx.isDeferred ? "Assets/Shaders/Terrain/TerrainGBuffer.hlsl"
            : "Assets/Shaders/Terrain/Terrain.hlsl");
    if (!resources.Get(shared.terrainDepthShader))
        shared.terrainDepthShader = resources.LoadShader("Assets/Shaders/Terrain/TerrainDepth.hlsl");
    if (!resources.Get(shared.terrainPSO))
        shared.terrainPSO = resources.CreatePipelineState({RasterizerMode::SOLID, BlendMode::OPAQUE_BLEND, DepthMode::DEPTH_ON});
    if (!resources.Get(shared.terrainObjectCB))
        shared.terrainObjectCB = resources.CreateConstantBuffer(sizeof(TerrainObjectCB));
    if (!resources.Get(colorShader) || !resources.Get(shared.terrainDepthShader)
        || !resources.Get(shared.terrainPSO) || !resources.Get(shared.terrainObjectCB)) return false;

    const auto viewProjection = MakeJitteredProjection(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY)
        * ctx.camera.GetViewMatrix();
    const auto frustum = math::Frustum::FromViewProjection(viewProjection);
    bool submitted = false;
    for (const auto& input : scene->terrains) {
        if (input.layer >= 32 || (ctx.cullingMask & (1u << input.layer)) == 0) continue;
        auto constants = input.constants;
        const auto& world = constants.worldMatrix;
        constants.wvpMatrix = viewProjection * world;
        resources.Update(shared.terrainObjectCB, &constants, sizeof(constants));
        for (const auto& patch : input.patches) {
            if (!IsViewOcclusionCurrent(ctx)) return false;
            if (ctx.frustumCullingEnabled && !IsChunkVisible(frustum, world, patch.aabbMin, patch.aabbMax)) continue;
            if (IsViewAabbOccluded(ctx, world, patch.aabbMin, patch.aabbMax)) continue;
            const int lod = SelectTerrainLOD(input, patch, ctx.camera.m_position);
            if (patch.indexCountLOD[lod] == 0) continue;
            DrawCall draw;
            draw.vertexBuffer = patch.vertexBuffer;
            draw.indexBuffer = patch.indexBufferLOD[lod];
            draw.indexCount = patch.indexCountLOD[lod];
            draw.shader = shared.terrainDepthShader;
            draw.pipelineState = shared.terrainPSO;
            draw.constantBuffers[1] = shared.terrainObjectCB;
            BindTerrainStrip(ctx, patch, lod, draw);
            if (HasTerrainBuffer(resources, draw.vertexBuffer) && HasTerrainBuffer(resources, draw.indexBuffer)) {
                SubmitCounted(ctx, draw);
                submitted = true;
            }
        }
    }
    return submitted;
}

std::string_view TerrainRenderPass::Name() const
{
    return m_mode == TerrainDrawMode::GBuffer ? "TerrainGBuffer" : "TerrainForward";
}

void TerrainRenderPass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    /// @note 影と Cookie の束縛は GBuffer / Forward で分岐していない (Execute の t13 / t28 / t31)。
    /// @note 以前は Deferred のときだけ申告から抜けていて、依存辺が張られないまま
    /// @note «同じ GBuffer を書く DeferredGBuffer が先に走るから» という偶然で順序が保たれていた。
    builder.Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas");

    /// @note GBuffer へ書けば DeferredLighting/GTAO/SSAO/SSR/ContactShadows に地形が含まれる。
    /// @note Forward では HDR へ直接ライティング結果を描く。
    const char* const target = m_mode == TerrainDrawMode::GBuffer ? "GBuffer" : "HDR";
    builder.ReadWrite(target).SetAutoTarget(target);
    if (m_mode == TerrainDrawMode::Forward && ctx.waterMediumPassActive)
        builder.Read("WaterMediumAtlas");
}

void TerrainRenderPass::Execute(PassResources&, RenderPassContext& ctx)
{
    /// @note 描き先の束縛は Setup の SetAutoTarget が済ませている。

    /// @note Forward + GBuffer 前段では地形が GBuffer と Forward の 2 回走る。チャンクの統計は Forward 側だけで数える。
    std::optional<CullStatsRollback> rollback;
    if (m_mode == TerrainDrawMode::GBuffer && !ctx.isDeferred) rollback.emplace(ctx);

    /// @note エイリアス: TerrainRenderSystem の旧シグネチャ変数名を ctx から引く
    renderer::IRenderer&       renderer            = ctx.renderer;
    renderer::ResourceManager& resources           = ctx.resources;
    const renderer::Camera&    camera              = ctx.camera;
    const renderer::RenderSettings* settings       = &ctx.settings;
    auto shadowDepthTexture = ctx.resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
    auto shadowCB           = ctx.handles.shadowCB;
    auto lightCB            = ctx.handles.lightCB;

    auto& shared = resources.Rendering().Shared();
    if (!resources.Get(shared.terrainShader))
        shared.terrainShader = resources.LoadShader("Assets/Shaders/Terrain/Terrain.hlsl");
    if (!resources.Get(shared.terrainGBufferShader))
        shared.terrainGBufferShader = resources.LoadShader("Assets/Shaders/Terrain/TerrainGBuffer.hlsl");
    if (!resources.Get(shared.terrainPSO))
        shared.terrainPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
    if (!resources.Get(shared.terrainWireframePSO))
        shared.terrainWireframePSO = resources.CreatePipelineState({ renderer::RasterizerMode::WIREFRAME,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
    if (!resources.Get(shared.terrainCameraCB))
        shared.terrainCameraCB = resources.CreateConstantBuffer(sizeof(TerrainCameraFrameCB));
    if (!resources.Get(shared.terrainObjectCB))
        shared.terrainObjectCB = resources.CreateConstantBuffer(sizeof(TerrainObjectCB));
    const auto terrainShader = shared.terrainShader;
    const auto terrainGBufferShader = shared.terrainGBufferShader;
    const auto terrainPSO = shared.terrainPSO;
    const auto terrainWireframePSO = shared.terrainWireframePSO;
    const auto cameraCBH = shared.terrainCameraCB;
    const auto terrainCBH = shared.terrainObjectCB;
    /// @note TAA ジッターを地形にも乗せる。乗せないと地形だけ AA されないうえ、b0 経由で描く
    /// @note 他の不透明物とサブピクセル単位でずれた深度になり、TAA の再投影が濁る。
    const math::Matrix4 jitteredProj =
        MakeJitteredProjection(camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    const math::Matrix4 jitteredVP = jitteredProj * camera.GetViewMatrix();

    {
        TerrainCameraFrameCB camData{};
        camData.view              = camera.GetViewMatrix();
        camData.projection        = jitteredProj;
        camData.viewProjection    = jitteredVP;
        camData.invViewProjection = math::Matrix4::Inverse(jitteredVP);
        camData.cameraPos         = camera.m_position;
        camData.nearZ             = camera.m_near;
        camData.farZ              = camera.m_far;
        camData.isOrthographic    =
            camera.m_projection == renderer::ProjectionMode::Orthographic ? 1.0f : 0.0f;
        resources.Update(cameraCBH, &camData, sizeof(camData));
    }

    const math::Frustum frustum = math::Frustum::FromViewProjection(jitteredVP);
    if (!ctx.renderScene) return;
    const bool terrainMeshlets = UsesTerrainMeshlets(ctx);
    const size_t shaderMode = m_mode == TerrainDrawMode::GBuffer ? 1u : 0u;
    const auto meshShader = terrainMeshlets ? TerrainMeshletShader(resources, shaderMode) : ResourceHandle<ShaderTag>{};
    for (const auto& input : ctx.renderScene->terrains) {
        if (input.layer >= 32 || (ctx.cullingMask & (1u << input.layer)) == 0) continue;
        const auto& world = input.constants.worldMatrix;
        auto terrainCBData = input.constants;
        terrainCBData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(world);
        terrainCBData.wvpMatrix = jitteredVP * world;
        resources.Update(terrainCBH, &terrainCBData, sizeof(terrainCBData));
        auto meshView = MakeTerrainMeshletView(meshShader, frustum, world, jitteredVP);
        /// @note The depth prepass and CPU proof use the same indexed terrain LOD as the color pass.
        const auto gpuShader = !terrainMeshlets && ctx.frustumCullingEnabled && !ctx.occlusionCullingEnabled
            && (ctx.settings.geometryDrawMode == GeometryDrawMode::GPU_INDIRECT || ctx.settings.geometryDrawMode == GeometryDrawMode::MESH_SHADER)
            ? TerrainGpuShader(resources, shaderMode) : ResourceHandle<ShaderTag>{};
        GpuGeometryDispatch gpuDispatch;
        const bool gpu = resources.Get(gpuShader)
            && PrepareTerrainGpu(ctx, input, frustum, gpuDispatch);
        size_t patchIndex = 0;
        for (const auto& chunk : input.patches) {
                const size_t currentPatch = patchIndex++;
                /// @note 地形チャンクはそれぞれ独立にカリングされる描画候補なので、
                /// @note メッシュと同じ粒度で統計に数える。
                ++ctx.statsTotalObjects;
                /// @note カメラの Frustum Culling を切っている間はチャンクも落とさない。
                /// @note メッシュだけ全部出て地形だけ消えると、切り分けの道具として成立しない。
                if (ctx.frustumCullingEnabled &&
                    !IsChunkVisible(frustum, world, chunk.aabbMin, chunk.aabbMax)) {
                    ++ctx.statsFrustumCulled;
                    if (!gpu) continue;
                }

                if (IsViewAabbOccluded(ctx, world, chunk.aabbMin, chunk.aabbMax)) {
                    ++ctx.statsOcclusionCulled;
                    continue;
                }

                const int lod = SelectTerrainLOD(input, chunk, camera.m_position);
                /// @note 全セルが穴のチャンクはインデックスが 0 本。
                if (chunk.indexCountLOD[lod] == 0) continue;

                renderer::DrawCall call;
                call.vertexBuffer  = chunk.vertexBuffer;
                call.indexBuffer   = chunk.indexBufferLOD[lod];
                /// @note Deferred: GBuffer 書き込みシェーダ。Forward: 自前ライティングシェーダ。
                call.shader        = m_mode == TerrainDrawMode::GBuffer ? terrainGBufferShader
                                                                        : terrainShader;
                call.pipelineState = (settings && settings->IsWireframe()) ? terrainWireframePSO : terrainPSO;
                call.indexCount    = chunk.indexCountLOD[lod];
                call.layer         = renderer::RenderLayer::OPAQUE_LAYER;
                call.topology      = renderer::PrimitiveTopology::TRIANGLE_LIST;

                call.constantBuffers[0] = cameraCBH;
                call.constantBuffers[1] = terrainCBH;
                call.constantBuffers[3] = lightCB;
                call.constantBuffers[4] = shadowCB;
                /// @note b8: 画面空間 AO / 接触影の強度。Forward の地形がこれを読む。
                call.constantBuffers[8] = ctx.handles.advancedGraphicsCB;
                BindForwardShadingResources(call, ctx);
                if (m_mode == TerrainDrawMode::Forward) {
                    call.textures[16] = ctx.handles.iblIrradiance;
                    call.textures[17] = ctx.handles.iblPrefilter;
                    call.textures[18] = ctx.handles.iblBrdfLut;
                }

                call.textures[0]  = input.splatIndices;
                call.textures[1]  = input.splatWeights;
                call.textures[13] = shadowDepthTexture;

                if (BindTerrainMeshlet(resources, meshView, call)) {
                    FBZZ_PROFILE_SCOPE("TerrainMeshlet::Submit");
                    SubmitCounted(ctx, call);
                } else {
                    if (gpu) BindTerrainGpu(call, input, gpuDispatch, currentPatch, gpuShader);
                    else BindTerrainStrip(ctx, chunk, lod, call);
                    SubmitCounted(ctx, call);
                }
        }
    }
}
namespace {

bool UsesGpuShadowTerrain(const RenderPassContext& ctx)
{
    /// @note Color and shadow use the same camera-driven LOD policy with different frusta.
    return ctx.frustumCullingEnabled && !ctx.occlusionCullingEnabled
        && (ctx.settings.geometryDrawMode == GeometryDrawMode::GPU_INDIRECT || ctx.settings.geometryDrawMode == GeometryDrawMode::MESH_SHADER);
}

bool IsStaticShadowTerrain(const RenderPassContext& ctx, const RenderTerrainInput& input)
{
    return IsDirectionalShadowBakeTerrain(input) && input.shadowBakeGeometryHash != 0
        && (!UsesGpuShadowTerrain(ctx) || UsesTerrainMeshlets(ctx));
}

void SubmitTerrainShadowCastersImpl(
    RenderPassContext&                            ctx,
    const math::Frustum&                          lightFrustum,
    renderer::ResourceHandle<renderer::ShaderTag> shadowShader,
    renderer::ResourceHandle<renderer::PipelineStateTag> pipelineState,
    renderer::ResourceHandle<renderer::ConstantBufferTag> frameCB,
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB,
    const PreparedTerrainShadowBatch* batch, size_t viewIndex, std::span<const uint8_t> receipts,
    const math::Matrix4* lightViewProjection, bool usesBakedStaticDepth, TerrainShadowCasterFilter filter)
{
    /// @note ライト種別・shadow atlas・cascade の選択は ShadowPass 側に閉じ込め、
    /// @note こちらは「指定されたライト視錐台へ描けるチャンクを提出する」だけにする。
    if (!shadowShader.IsValid() || !pipelineState.IsValid())
        return;

    const auto scene = ctx.renderScene;
    if (!scene) return;
    auto& resources = ctx.resources;
    const bool terrainMeshlets = lightViewProjection && UsesTerrainMeshlets(ctx);
    const auto* pipeline = terrainMeshlets ? resources.Get(pipelineState) : nullptr;
    const auto meshShader = pipeline && pipeline->GetDesc().rasterizer == RasterizerMode::SOLID
        ? TerrainMeshletShader(resources, 2u) : ResourceHandle<ShaderTag>{};
    for (size_t index = 0; index < scene->terrains.size(); ++index) {
        const auto& input = scene->terrains[index];
        if (input.layer >= 32 || (ctx.cullingMask & (1u << input.layer)) == 0) continue;
        if (usesBakedStaticDepth && IsDirectionalShadowBakeTerrain(input)) continue;
        if (filter != TerrainShadowCasterFilter::ALL
            && (filter == TerrainShadowCasterFilter::STATIC_ONLY) != IsStaticShadowTerrain(ctx, input)) continue;
        const auto& world = input.constants.worldMatrix;
        PerObjectCB object{};
        object.world = world;
        object.worldInvTranspose = math::Matrix4::InverseTransposeAffine(world);
        resources.Update(objectCB, &object, sizeof(object));
        TerrainMeshletView meshView;
        if (meshShader.IsValid()) meshView = MakeTerrainMeshletView(meshShader, lightFrustum, world, *lightViewProjection);
        ResourceHandle<ShaderTag> gpuShader;
        GpuGeometryDispatch gpuDispatch;
        bool gpu = false;
        if (!terrainMeshlets && UsesGpuShadowTerrain(ctx)) {
            if (!batch) {
                gpuShader = TerrainGpuShader(resources, 2u);
                gpu = resources.Get(gpuShader) && PrepareTerrainGpu(ctx, input, lightFrustum, gpuDispatch);
            } else if (batch->scene == scene && batch->resourceEpoch == resources.GetResetVersion()
                && batch->resourceFrame == resources.FrameStamp()) {
                const size_t entry = viewIndex * scene->terrains.size() + index;
                if (entry < batch->sets.size()) {
                    const auto& prepared = batch->sets[entry];
                    gpuShader = prepared.shader;
                    gpuDispatch = prepared.dispatch;
                    gpu = prepared.commandIndex < receipts.size() && receipts[prepared.commandIndex]
                        && resources.Get(gpuShader) && resources.Get(gpuDispatch.indirectArguments)
                        && resources.Get(input.gpuHeights) && resources.Get(input.gpuPatches)
                        && HasTerrainBuffer(resources, input.gpuIndices);
                }
            }
        }
        size_t patchIndex = 0;
        for (const auto& chunk : input.patches) {
                const size_t currentPatch = patchIndex++;
                if (!gpu && !IsChunkVisible(lightFrustum, world, chunk.aabbMin, chunk.aabbMax))
                    continue;

                const int lod = SelectTerrainLOD(input, chunk, ctx.camera.m_position);
                if (chunk.indexCountLOD[lod] == 0) continue;

                renderer::DrawCall dc;
                dc.vertexBuffer       = chunk.vertexBuffer;
                dc.indexBuffer        = chunk.indexBufferLOD[lod];
                dc.indexCount         = chunk.indexCountLOD[lod];
                dc.shader             = shadowShader;
                dc.pipelineState      = pipelineState;
                dc.layer              = renderer::RenderLayer::OPAQUE_LAYER;
                dc.topology           = renderer::PrimitiveTopology::TRIANGLE_LIST;
                dc.constantBuffers[0] = frameCB;
                dc.constantBuffers[1] = objectCB;
                if (BindTerrainMeshlet(resources, meshView, dc)) {
                    FBZZ_PROFILE_SCOPE("TerrainMeshlet::Submit");
                    SubmitCountedShadow(ctx, dc);
                } else {
                    if (gpu) BindTerrainGpu(dc, input, gpuDispatch, currentPatch, gpuShader);
                    else BindTerrainStrip(ctx, chunk, lod, dc);
                    SubmitCountedShadow(ctx, dc);
                }
        }
    }
}

} /// @note namespace

uint64_t TerrainShadowCacheContentKey(const RenderPassContext& ctx)
{
    uint64_t key = 14695981039346656037ull;
    if (!ctx.renderScene) return key;
    const auto append = [&key](uint64_t value) { key = (key ^ value) * 1099511628211ull; };
    for (const auto& input : ctx.renderScene->terrains) {
        if (input.layer >= 32 || (ctx.cullingMask & (1u << input.layer)) == 0
            || !IsStaticShadowTerrain(ctx, input)) continue;
        append(input.shadowBakeGeometryHash);
        for (const auto& row : input.constants.worldMatrix.m)
            for (const float value : row) append(std::bit_cast<uint32_t>(value));
        /// @note 色と影で同じ LOD を使う。カメラ移動で選んだ面が変わったときは深度を作り直す。
        uint64_t lodKey = 0;
        for (const auto& patch : input.patches)
            lodKey = (lodKey ^ (1 + SelectTerrainLOD(input, patch, ctx.camera.m_position))) * 1099511628211ull;
        append(lodKey);
    }
    return key;
}

PreparedTerrainShadowBatch PrepareTerrainShadowBatch(RenderPassContext& ctx,
    std::span<const InstanceShadowView> views, std::vector<ComputeCall>& commands, size_t bakedViewIndex)
{
    PreparedTerrainShadowBatch batch;
    batch.scene = ctx.renderScene;
    batch.resourceEpoch = ctx.resources.GetResetVersion();
    batch.resourceFrame = ctx.resources.FrameStamp();
    if (!batch.scene || batch.scene->terrains.empty() || !UsesGpuShadowTerrain(ctx)
        || UsesTerrainMeshlets(ctx)
        || !ctx.handles.shadowShader.IsValid() || !ctx.handles.defaultPSO.IsValid()) return batch;
    const auto shader = TerrainGpuShader(ctx.resources, 2u);
    if (!ctx.resources.Get(shader)) return batch;
    const auto& terrains = batch.scene->terrains;
    batch.sets.resize(views.size() * terrains.size());
    for (size_t view = 0; view < views.size(); ++view) {
        for (size_t index = 0; index < terrains.size(); ++index) {
            const auto& input = terrains[index];
            if (input.layer >= 32 || (ctx.cullingMask & (1u << input.layer)) == 0) continue;
            if (view == bakedViewIndex && IsDirectionalShadowBakeTerrain(input)) continue;
            auto& prepared = batch.sets[view * terrains.size() + index];
            ComputeCall compute;
            if (!PrepareTerrainGpu(ctx, input, views[view].frustum, prepared.dispatch, false, &compute)) continue;
            prepared.shader = shader;
            prepared.commandIndex = commands.size();
            commands.push_back(std::move(compute));
        }
    }
    return batch;
}

void SubmitTerrainShadowBatchView(RenderPassContext& ctx, const math::Frustum& frustum,
    const math::Matrix4& lightViewProjection,
    ResourceHandle<ShaderTag> shadowShader, ResourceHandle<PipelineStateTag> pipelineState,
    ResourceHandle<ConstantBufferTag> frameCB, ResourceHandle<ConstantBufferTag> objectCB,
    const PreparedTerrainShadowBatch& batch, size_t viewIndex, std::span<const uint8_t> receipts,
    bool usesBakedStaticDepth, TerrainShadowCasterFilter filter)
{
    SubmitTerrainShadowCastersImpl(ctx, frustum, shadowShader, pipelineState, frameCB, objectCB, &batch, viewIndex,
        receipts, &lightViewProjection, usesBakedStaticDepth, filter);
}

void SubmitTerrainShadowCasters(RenderPassContext& ctx, const math::Frustum& lightFrustum,
    ResourceHandle<ShaderTag> shadowShader, ResourceHandle<PipelineStateTag> pipelineState,
    ResourceHandle<ConstantBufferTag> frameCB, ResourceHandle<ConstantBufferTag> objectCB)
{
    SubmitTerrainShadowCastersImpl(ctx, lightFrustum, shadowShader, pipelineState, frameCB, objectCB,
        nullptr, 0, {}, nullptr, false, TerrainShadowCasterFilter::ALL);
}

void TerrainSelectionMaskSystem(RenderPassContext& ctx)
{
    if (!ctx.selectionOutlineEnabled) return;
    auto& h = ctx.handles;
    if (!h.selectionMaskShader.IsValid() || !h.selectionMaskPSO.IsValid()) return;

    if (!ctx.renderScene) return;
    for (const auto& input : ctx.renderScene->terrains) {
        if (!input.selected || input.layer >= 32 || (ctx.cullingMask & (1u << input.layer)) == 0) continue;
        const auto& world = input.constants.worldMatrix;
        PerObjectCB objData{};
        objData.world             = world;
        objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(world);
        ctx.resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));
        for (const auto& chunk : input.patches) {
            if (chunk.indexCountLOD[0] == 0) continue;
            renderer::DrawCall dc;
            dc.vertexBuffer       = chunk.vertexBuffer;
            dc.indexBuffer        = chunk.indexBufferLOD[0];
            dc.indexCount         = chunk.indexCountLOD[0];
            dc.shader             = h.selectionMaskShader;
            dc.pipelineState      = h.selectionMaskPSO;
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[1] = h.objectCB;
            ctx.renderer.Submit(dc, ctx.resources);
        }
    }
}

} /// @note namespace fbzz::renderer
