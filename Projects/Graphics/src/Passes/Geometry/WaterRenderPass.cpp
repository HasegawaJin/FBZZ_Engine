/// @file    WaterRenderPass.cpp
/// @brief   WaterComponent → GPU 水面メッシュ・泡マスク・波紋テクスチャ生成と描画 (IRenderPass 実装)。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include <Graphics/Passes/Geometry/WaterRenderPass.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/SurfacePatchMorph.hpp>
#include <Graphics/Effects/WaterGrid.hpp>
#include <Graphics/Renderer/DynamicBufferPool.hpp>
#include <Graphics/Renderer/GpuGeometrySelection.hpp>
#include <Graphics/Renderer/IShader.hpp>
#include <Graphics/Renderer/ShaderPathResolver.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Core/Logger.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include <Core/Util/StringUtils.hpp>
#include "../../Renderer/GpuCullDrawQueue.hpp"
#include "WaterNoiseBake.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace fbzz::renderer {
bool ResolveWaterHorizonRectangle(const RenderWaterInput& input, const Camera& camera,
    math::Vector2& outMin, math::Vector2& outMax)
{
    if (!input.farOceanEnabled || !input.adaptivePatchLod || !std::isfinite(input.farOceanCoverageScale)) return false;
    const auto& world = input.constants.worldMatrix;
    const math::Vector3 origin{world.m[0][3], world.m[1][3], world.m[2][3]};
    const math::Vector3 axisX{world.m[0][0], world.m[1][0], world.m[2][0]};
    const math::Vector3 axisZ{world.m[0][2], world.m[1][2], world.m[2][2]};
    const float axisXSquare = axisX.LengthSq(), axisZSquare = axisZ.LengthSq();
    if (!std::isfinite(axisXSquare) || !std::isfinite(axisZSquare) || axisXSquare <= 0.0f || axisZSquare <= 0.0f)
        return false;
    /// @note Water は水平 TRS。軸への投影で local XZ を求め、小さい scale の行列式に依存しない。
    auto localAt = [&](const math::Vector3& point) {
        const auto relative = point - origin;
        return math::Vector2{math::Vector3::Dot(relative, axisX) / axisXSquare,
            math::Vector3::Dot(relative, axisZ) / axisZSquare};
    };
    const auto localCamera = localAt(camera.m_position);
    const auto projection = camera.GetProjectionMatrix();
    const float projectionX = std::abs(projection.m[0][0]), projectionY = std::abs(projection.m[1][1]);
    if (!std::isfinite(localCamera.x) || !std::isfinite(localCamera.y)
        || !std::isfinite(projectionX) || !std::isfinite(projectionY) || projectionX <= 0.0f || projectionY <= 0.0f)
        return false;
    const auto forward = camera.GetForward(), right = camera.GetRight(), up = camera.GetUp();
    const float cameraMargin = (std::max)(input.medium.maxDistance, camera.m_near);
    if (!std::isfinite(cameraMargin) || cameraMargin <= 0.0f) return false;
    const math::Vector2 localMargin{cameraMargin / std::sqrt(axisXSquare), cameraMargin / std::sqrt(axisZSquare)};
    math::Vector2 minimum = localCamera - localMargin;
    math::Vector2 maximum = localCamera + localMargin;
    const float coverage = (std::clamp)(input.farOceanCoverageScale, 1.0f, 2.0f);
    /// @note near/far の全 corner を包む。大きな平行投影でも逆行列の行列式下限を踏まない。
    for (float depth : {camera.m_near, camera.m_far}) {
        for (float y : {-1.0f, 1.0f}) {
            for (float x : {-1.0f, 1.0f}) {
                const float perspectiveDepth = camera.m_projection == ProjectionMode::Orthographic ? 1.0f : depth;
                const auto corner = camera.m_position + forward * depth
                    + right * (x * perspectiveDepth / projectionX) + up * (y * perspectiveDepth / projectionY);
                const auto local = localAt(corner);
                const math::Vector2 covered{localCamera.x + (local.x - localCamera.x) * coverage,
                    localCamera.y + (local.y - localCamera.y) * coverage};
                if (!std::isfinite(covered.x) || !std::isfinite(covered.y)) return false;
                minimum.x = (std::min)(minimum.x, covered.x);
                minimum.y = (std::min)(minimum.y, covered.y);
                maximum.x = (std::max)(maximum.x, covered.x);
                maximum.y = (std::max)(maximum.y, covered.y);
            }
        }
    }
    outMin = minimum;
    outMax = maximum;
    return true;
}

namespace {
/// @note The root origin moves by whole quarter-root tiles, preserving the world lattice of fine LODs.
/// @see https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry GPU Gems 2, Chapter 2, regular nested grids.
bool ResolveHorizonPatchDomain(math::Vector2 minimum, math::Vector2 maximum,
    math::Vector2 axisScale, SurfacePatchSurface& surface, math::Vector4& domain)
{
    const auto snapAxis = [](float low, float high, float scale) {
        const double span = (static_cast<double>(high) - low) * scale;
        const double width = std::exp2(std::ceil(std::log2(span * (4.0 / 3.0))));
        const double step = width * 0.25;
        const double middle = (static_cast<double>(low) + high) * (0.5 * scale);
        const double snapped = std::round(middle / step) * step;
        return math::Vector2{static_cast<float>((snapped - width * 0.5) / scale),
            static_cast<float>(width / scale)};
    };
    const auto x = snapAxis(minimum.x, maximum.x, axisScale.x);
    const auto z = snapAxis(minimum.y, maximum.y, axisScale.y);
    const math::Vector2 high{x.x + x.y, z.x + z.y};
    if (!std::isfinite(x.x) || !std::isfinite(z.x) || !std::isfinite(high.x) || !std::isfinite(high.y)
        || x.y <= 0.0f || z.y <= 0.0f || x.x > minimum.x || z.x > minimum.y
        || high.x < maximum.x || high.y < maximum.y) return false;
    surface.localMin = {x.x, z.x};
    surface.localMax = high;
    domain = {x.x, z.x, high.x - x.x, high.y - z.x};
    return true;
}

bool AabbVisible(const math::Frustum& frustum, const math::Matrix4& world,
                 const math::Vector3& localMin, const math::Vector3& localMax,
                 const WaterWaveMargin& margin = {})
{
    const math::Vector3 localCenter = {
        (localMin.x + localMax.x) * 0.5f,
        (localMin.y + localMax.y) * 0.5f,
        (localMin.z + localMax.z) * 0.5f,
    };
    const math::Vector3 localExtents = {
        (localMax.x - localMin.x) * 0.5f,
        (localMax.y - localMin.y) * 0.5f,
        (localMax.z - localMin.z) * 0.5f,
    };

    const math::Vector4 center =
        world * math::Vector4{ localCenter.x, localCenter.y, localCenter.z, 1.0f };
    /// @note 回転した箱を軸並行で包み直す = |M| を half-extent に掛ける。
    auto projectRow = [&](int row) {
        return std::abs(world.m[row][0]) * localExtents.x
             + std::abs(world.m[row][1]) * localExtents.y
             + std::abs(world.m[row][2]) * localExtents.z;
    };
    /// @note 波は world m の変位なので、Transform 後の箱に安全域を加える。
    const math::Vector3 extents = { projectRow(0) + margin.horizontal,
        projectRow(1) + margin.vertical, projectRow(2) + margin.horizontal };
    return frustum.IntersectsAABB({ center.x, center.y, center.z }, extents);
}

/// @brief 頂点シェーダーと同じ有限格子の写像をチャンクの境界へ適用する。
/// @note 格子の集中はビューごとに異なる。抽出時の等間隔 AABB では近景チャンクを誤って捨てる。
/// @see https://developer.nvidia.com/gpugems/gpugems2/part-ii-shading-lighting-and-shadows/chapter-18-using-vertex-texture-displacement GPU Gems 2 Chapter 18, camera-centered grid
void FocusWaterChunkBounds(const RenderWaterInput& input, const math::Vector2& focus,
                           math::Vector3& outMin, math::Vector3& outMax)
{
    if (input.constants.gridParams.x <= 0.5f) return;
    const auto& world = input.constants.worldMatrix;
    const math::Vector3 axisX = { world.m[0][0], world.m[1][0], world.m[2][0] };
    const math::Vector3 axisZ = { world.m[0][2], world.m[1][2], world.m[2][2] };
    auto mapAxis = [&](float minimum, float maximum, float extent, const math::Vector3& axis,
                       uint32_t resolution, float axisFocus) {
        const float axisScale = (std::max)(axis.Length(), 1.0e-4f);
        const float nearCellSize = input.constants.gridParams.y / axisScale;
        /// @note CPU/GPU の積和の丸めで中心の量子化が 1 セル違っても、見えるチャンクを捨てない。
        return math::Vector2{
            WaterGridAxisPosition(minimum / extent + 0.5f, extent, resolution, axisFocus, nearCellSize) - nearCellSize,
            WaterGridAxisPosition(maximum / extent + 0.5f, extent, resolution, axisFocus, nearCellSize) + nearCellSize
        };
    };
    const auto x = mapAxis(outMin.x, outMax.x, input.aabbMax.x - input.aabbMin.x, axisX,
                           static_cast<uint32_t>(input.constants.gridParams.z), focus.x);
    const auto z = mapAxis(outMin.z, outMax.z, input.aabbMax.z - input.aabbMin.z, axisZ,
                           static_cast<uint32_t>(input.constants.gridParams.w), focus.y);
    outMin.x = x.x; outMax.x = x.y;
    outMin.z = z.x; outMax.z = z.y;
}

struct WaterCameraCB {
    math::Matrix4 view, projection, viewProjection, invViewProjection;
    math::Vector3 cameraPos;
    float nearZ, farZ, waterSsrEnabled, isOrthographic, wireframeMode;
};
static_assert(sizeof(WaterCameraCB) == 288);

struct WaterDrawResources {
    ResourceHandle<ShaderTag> waterShader, selectionShader, copyColorShader, depthCopyShader;
    ResourceHandle<ShaderTag> mediumAtlasShader, interfaceShader, crestSprayShader;
    ResourceHandle<PipelineStateTag> waterPSO, wireframePSO, selectionPSO;
    ResourceHandle<PipelineStateTag> mediumAtlasPSO, interfacePSO, crestSprayPSO;
    ResourceHandle<ConstantBufferTag> cameraCB, waterCB, effectCB, mediumCaptureCB, zoneAppearanceCB;
    ResourceHandle<TextureTag> black, neutralRipple;
    DynamicStructuredBufferPool patchInstances{16};
    std::unique_ptr<GpuGeometrySelection> geometrySelection;
    bool indirectPathReported = false;
    std::unordered_set<uint64_t> rejectedGeometryWarnings;
    uint64_t appearanceShaderVersion = 0;
    std::unordered_map<uint64_t, bool> defaultWaterAppearances;
};

WaterDrawResources& GetWaterDrawResources(ResourceManager& resources)
{
    static uint64_t owner = 0;
    static uint64_t version = UINT64_MAX;
    static WaterDrawResources handles;
    if (owner == resources.GetInstanceId() && version == resources.GetResetVersion()) return handles;
    owner = resources.GetInstanceId();
    version = resources.GetResetVersion();
    handles = {};
    handles.waterShader = resources.LoadShader("Assets/Shaders/Water/Water.hlsl");
    handles.selectionShader = resources.LoadShader("Assets/Shaders/Water/WaterSelection.hlsl");
    handles.copyColorShader = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
    handles.depthCopyShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
    handles.mediumAtlasShader = resources.LoadShader("Assets/Shaders/Water/WaterMediumAtlas.hlsl");
    handles.interfaceShader = resources.LoadShader("Assets/Shaders/Water/WaterInterface.hlsl");
    handles.crestSprayShader = resources.LoadShader("Assets/Shaders/Water/WaterCrestSpray.hlsl");
    /// @note 波の裏側とチャンク順で水面を上塗りしないよう、本描画は深度を書く。
    handles.waterPSO = resources.CreatePipelineState({RasterizerMode::SOLID_NOCULL, BlendMode::ALPHA_BLEND, DepthMode::DEPTH_ON});
    handles.wireframePSO = resources.CreatePipelineState({RasterizerMode::WIREFRAME_NOCULL, BlendMode::OPAQUE_BLEND, DepthMode::DEPTH_ON});
    handles.selectionPSO = resources.CreatePipelineState({RasterizerMode::SOLID_NOCULL, BlendMode::OPAQUE_BLEND, DepthMode::DEPTH_ON});
    handles.mediumAtlasPSO = resources.CreatePipelineState({RasterizerMode::SOLID_NOCULL, BlendMode::OPAQUE_BLEND, DepthMode::DEPTH_ON});
    handles.interfacePSO = resources.CreatePipelineState({RasterizerMode::SOLID_NOCULL, BlendMode::OPAQUE_BLEND, DepthMode::DEPTH_ON});
    handles.crestSprayPSO = resources.CreatePipelineState({RasterizerMode::SOLID_NOCULL, BlendMode::ALPHA_BLEND, DepthMode::DEPTH_READ});
    handles.cameraCB = resources.CreateConstantBuffer(sizeof(WaterCameraCB));
    handles.waterCB = resources.CreateConstantBuffer(sizeof(WaterCB));
    handles.effectCB = resources.CreateConstantBuffer(sizeof(WaterEffectParams));
    handles.mediumCaptureCB = resources.CreateConstantBuffer(sizeof(math::Vector4));
    handles.zoneAppearanceCB = resources.CreateConstantBuffer(sizeof(WaterMaterialBlendTarget) * 32);
    handles.geometrySelection = std::make_unique<GpuGeometrySelection>();
    const uint8_t black[4] = {0, 0, 0, 255};
    const uint8_t ripple[4] = {128, 128, 128, 255};
    handles.black = resources.CreateTexture(black, 1, 1);
    handles.neutralRipple = resources.CreateTexture(ripple, 1, 1);
    return handles;
}

/// @note GUID and path loads can have different handles for the same standard water shader.
bool IsDefaultWaterAppearance(ResourceManager& resources, WaterDrawResources& handles,
    ResourceHandle<ShaderTag> appearance)
{
    if (appearance == handles.waterShader) return true;
    const uint64_t shaderVersion = resources.GetShaderVersion();
    if (handles.appearanceShaderVersion != shaderVersion) {
        handles.appearanceShaderVersion = shaderVersion;
        handles.defaultWaterAppearances.clear();
    }
    const uint64_t key = (static_cast<uint64_t>(appearance.gen) << 32) | appearance.id;
    const auto [cached, inserted] = handles.defaultWaterAppearances.try_emplace(key, false);
    if (!inserted) return cached->second;

    const auto* shader = resources.Get(appearance);
    const auto* standard = resources.Get(handles.waterShader);
    if (!shader || !standard) return false;
    std::error_code error;
    cached->second = shader->GetPath() == standard->GetPath() || std::filesystem::equivalent(
        ResolveShaderFilePath(util::StringUtils::ToWide(shader->GetPath())),
        ResolveShaderFilePath(util::StringUtils::ToWide(standard->GetPath())), error);
    return cached->second;
}

void UpdateWaterCamera(RenderPassContext& ctx, const WaterDrawResources& handles,
                       const Camera* cameraOverride = nullptr)
{
    const Camera& camera = cameraOverride ? *cameraOverride : ctx.camera;
    WaterCameraCB data{};
    data.view = camera.GetViewMatrix();
    data.projection = MakeJitteredProjection(camera, cameraOverride ? 0.0f : ctx.taaJitterNdcX,
        cameraOverride ? 0.0f : ctx.taaJitterNdcY);
    data.viewProjection = data.projection * data.view;
    if (cameraOverride && camera.m_projection == ProjectionMode::Orthographic) {
        /// @note 広域 atlas の正常な平行投影も determinant は小さい。対角投影を解析的に反転し、一般逆行列の特異判定を避ける。
        auto inverseProjection = data.projection;
        for (int axis = 0; axis < 3; ++axis) {
            inverseProjection.m[axis][axis] = 1.0f / data.projection.m[axis][axis];
            inverseProjection.m[axis][3] = -data.projection.m[axis][3] * inverseProjection.m[axis][axis];
        }
        data.invViewProjection = math::Matrix4::Inverse(data.view) * inverseProjection;
    } else {
        data.invViewProjection = math::Matrix4::Inverse(data.viewProjection);
    }
    data.cameraPos = camera.m_position;
    data.nearZ = camera.m_near;
    data.farZ = camera.m_far;
    data.waterSsrEnabled = cameraOverride ? 0.0f : (ctx.isDeferred && ctx.settings.ssr.enabled ? 1.0f : 0.0f);
    data.isOrthographic = camera.m_projection == ProjectionMode::Orthographic ? 1.0f : 0.0f;
    data.wireframeMode = cameraOverride ? 0.0f : (ctx.settings.IsWireframe() ? (ctx.settings.IsUnlit() ? 2.0f : 1.0f) : 0.0f);
    ctx.resources.Update(handles.cameraCB, &data, sizeof(data));
}

void SetWaterPatchConstants(WaterCB& cb, const SurfacePatch& patch, uint32_t cellsPerPatch)
{
    cb.patchRect = {patch.uvMin.x, patch.uvMin.y, patch.uvMax.x, patch.uvMax.y};
    cb.patchCellSize = {patch.cellSize.x, patch.cellSize.y, 1.0f, static_cast<float>(cellsPerPatch)};
    for (size_t edge = 0; edge < 4; ++edge) {
        cb.patchEdgeCellSize[edge] = {patch.edgeCellSize[edge].x, patch.edgeCellSize[edge].y, 0.0f, 0.0f};
        cb.patchCornerCellSize[edge] = {patch.cornerCellSize[edge].x, patch.cornerCellSize[edge].y, 0.0f, 0.0f};
    }
}

void SetWaterPatchMorphConstants(WaterCB& cb, const SurfacePatch& patch,
    const SurfacePatchLodSettings& settings, bool enabled, uint8_t axisMask)
{
    cb.patchMorphParams = {enabled ? 1.0f : 0.0f, settings.morphTransitionWidth,
        static_cast<float>(patch.depth), static_cast<float>(axisMask)};
    cb.patchMorphCellSize = {patch.morphCellSize.x, patch.morphCellSize.y, 0.0f, 0.0f};
}

struct WaterRasterIndices {
    ResourceHandle<BufferTag> buffer;
    uint32_t count = 0;
    uint32_t stripTriangleCount = 0;
    uint32_t startIndex = 0;

    bool operator==(const WaterRasterIndices&) const = default;

    void Apply(DrawCall& call) const
    {
        call.indexBuffer = buffer;
        call.indexCount = count;
        call.stripTriangleCount = stripTriangleCount;
        call.startIndex = startIndex;
        if (stripTriangleCount != 0) call.topology = PrimitiveTopology::TRIANGLE_STRIP;
    }
};

WaterRasterIndices SelectWaterRasterIndices(ResourceHandle<BufferTag> list, uint32_t listCount,
    const WaterStripIndices& strip, bool useStrip)
{
    if (useStrip && strip.buffer.IsValid() && strip.count >= 3u)
        return {strip.buffer, strip.count, listCount / 3u};
    return {list, listCount};
}

/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/water-meshing-system-and-surface-rendering-in-unreal-engine View-dependent water mesh density.
/// @note The error includes both samples of the shader displacement. A near-plane crossing always retains the fine cut.
bool WaterClusterErrorFits(const WaterSurfaceCluster& cluster, const math::Matrix4& localToClip,
    const math::Matrix4& worldToClip, const WaterWaveMargin& margin, uint32_t width, uint32_t height)
{
    const auto center = (cluster.boundsMin + cluster.boundsMax) * 0.5f;
    const auto extent = (cluster.boundsMax - cluster.boundsMin) * 0.5f;
    std::array<float, 4> clipCenter{}, clipExtent{}, clipError{};
    for (size_t row = 0; row < 4; ++row) {
        const float* local = localToClip.m[row];
        const float* world = worldToClip.m[row];
        const float wave = (std::abs(world[0]) + std::abs(world[2])) * margin.horizontal
            + std::abs(world[1]) * margin.vertical;
        clipCenter[row] = local[0] * center.x + local[1] * center.y + local[2] * center.z + local[3];
        clipExtent[row] = std::abs(local[0]) * extent.x + std::abs(local[1]) * extent.y
            + std::abs(local[2]) * extent.z;
        clipError[row] = math::Vector3{local[0], local[1], local[2]}.Length() * cluster.error + 2.0f * wave;
    }
    const float minW = clipCenter[3] - clipExtent[3];
    if (!(minW > clipError[3] + 1.0e-6f)
        || !(minW - clipCenter[2] > clipExtent[2] + clipError[2] + clipError[3])) return false;
    const auto pixelError = [&](size_t row, uint32_t size) {
        const float maxNdc = (std::abs(clipCenter[row]) + clipExtent[row]) / minW;
        return (clipError[row] + maxNdc * clipError[3]) * (0.5f * static_cast<float>(size))
            / (minW - clipError[3]);
    };
    const float error = std::hypot(pixelError(0, width), pixelError(1, height));
    return std::isfinite(error) && error <= 1.0f;
}

template<class Submit>
void VisitWaterPatches(const RenderWaterInput& input, RenderPassContext& ctx, bool countStatistics,
                       Submit&& submit, const Camera* cameraOverride = nullptr,
                       uint32_t viewportHeight = 0, bool allowFarOcean = true,
                       const Camera* lodCameraOverride = nullptr, bool gpuCulling = false, bool useStrip = false)
{
    const Camera& camera = cameraOverride ? *cameraOverride : ctx.camera;
    const Camera& lodCamera = lodCameraOverride ? *lodCameraOverride : camera;
    const bool frustumEnabled = cameraOverride != nullptr || ctx.frustumCullingEnabled;
    const uint32_t lodViewportHeight = viewportHeight == 0 ? ctx.height : viewportHeight;
    std::optional<math::Frustum> computedFrustum;
    const math::Frustum* frustum = !cameraOverride ? ctx.cameraFrustum : nullptr;
    if (!frustum) {
        computedFrustum.emplace(math::Frustum::FromViewProjection(camera.GetGpuViewProjection()));
        frustum = &*computedFrustum;
    }
    const auto projection = lodCamera.GetProjectionMatrix();
    auto cb = input.constants;
    const auto occluded = [&](const math::Vector3& low, const math::Vector3& high,
        float margin = 0.0f, bool includedInStatistics = true) {
        if (cameraOverride || !IsViewAabbOccluded(ctx, cb.worldMatrix, low, high, margin)) return false;
        if (countStatistics && includedInStatistics) ++ctx.statsOcclusionCulled;
        return true;
    };
    cb.wvpMatrix = MakeJitteredProjection(camera, cameraOverride ? 0.0f : ctx.taaJitterNdcX,
        cameraOverride ? 0.0f : ctx.taaJitterNdcY) * camera.GetViewMatrix() * cb.worldMatrix;
    if (ctx.settings.geometryDrawMode == GeometryDrawMode::MESH_SHADER && input.surfaceClusters
        && !input.surfaceClusters->empty() && input.surfaceClusterIndices.IsValid()
        && input.patches.size() == 1u && ctx.width > 0 && lodViewportHeight > 0) {
        FBZZ_PROFILE_SCOPE("WaterRender::ClusterSelection");
        const auto& patch = input.patches.front();
        const auto& clusters = *input.surfaceClusters;
        const auto vertexBuffer = input.surfaceClusterVertices.IsValid()
            ? input.surfaceClusterVertices : patch.vertexBuffer;
        const auto worldToClip = lodCamera.GetGpuViewProjection();
        const auto localToClip = worldToClip * cb.worldMatrix;
        const float waveRadius = std::hypot(input.margin.horizontal * std::sqrt(2.0f), input.margin.vertical);
        cb.patchCellSize = {};
        std::vector<const WaterSurfaceCluster*> selected;
        selected.reserve(clusters.size() / 2 + 1);
        uint32_t selectedFineIndices = 0;
        for (size_t end = clusters.size(); end > 0;) {
            const auto& cluster = clusters[end - 1];
            if (countStatistics) ++ctx.statsTotalObjects;
            if (frustumEnabled && !AabbVisible(*frustum, cb.worldMatrix,
                    cluster.boundsMin, cluster.boundsMax, input.margin)) {
                if (countStatistics) ++ctx.statsFrustumCulled;
            } else if (!occluded(cluster.boundsMin, cluster.boundsMax, waveRadius)) {
                const bool leaf = cluster.firstDescendant == end - 1;
                if (leaf || WaterClusterErrorFits(
                        cluster, localToClip, worldToClip, input.margin, ctx.width, lodViewportHeight)) {
                    selected.push_back(&cluster);
                    if (leaf) selectedFineIndices += cluster.indexCount;
                    end = cluster.firstDescendant;
                    continue;
                }
                --end;
                continue;
            }
            /// @note A rejected parent also encloses every finer candidate.
            end = cluster.firstDescendant;
        }
        if (selectedFineIndices == patch.indexCount) {
            /// @note The complete fine cut uses the original triangle order for overlapping displaced water.
            submit(patch.vertexBuffer, WaterRasterIndices{patch.indexBuffer, patch.indexCount},
                cb, patch.aabbMin, patch.aabbMax);
            return;
        }
        std::sort(selected.begin(), selected.end(), [](const auto* a, const auto* b) {
            return a->startIndex < b->startIndex;
        });
        WaterRasterIndices range{input.surfaceClusterIndices};
        for (const auto* cluster : selected) {
            if (range.count && range.startIndex + range.count != cluster->startIndex) {
                submit(vertexBuffer, range, cb, patch.aabbMin, patch.aabbMax);
                range.count = 0;
            }
            if (!range.count) range.startIndex = cluster->startIndex;
            range.count += cluster->indexCount;
        }
        if (range.count) submit(vertexBuffer, range, cb, patch.aabbMin, patch.aabbMax);
        return;
    }
    if (input.adaptivePatchLod) {
        if (!input.patchVertexBuffer.IsValid()) return;
        SurfacePatchSurface surface;
        surface.localMin = {input.aabbMin.x, input.aabbMin.z};
        surface.localMax = {input.aabbMax.x, input.aabbMax.z};
        surface.worldMatrix = cb.worldMatrix;
        const math::Vector3 axisY{cb.worldMatrix.m[0][1], cb.worldMatrix.m[1][1], cb.worldMatrix.m[2][1]};
        const float inverseHeightScale = 1.0f / (std::max)(axisY.Length(), 1.0e-6f);
        surface.minHeight = -input.margin.vertical * inverseHeightScale;
        surface.maxHeight = input.margin.vertical * inverseHeightScale;
        const math::Vector3 axisX{cb.worldMatrix.m[0][0], cb.worldMatrix.m[1][0], cb.worldMatrix.m[2][0]};
        const math::Vector3 axisZ{cb.worldMatrix.m[0][2], cb.worldMatrix.m[1][2], cb.worldMatrix.m[2][2]};
        const math::Vector2 axisScale{axisX.Length(), axisZ.Length()};
        surface.horizontalDisplacement = input.margin.horizontal
            / (std::max)((std::min)(axisScale.x, axisScale.y), 1.0e-6f);
        math::Vector2 outerMin, outerMax;
        const bool horizon = allowFarOcean && ResolveWaterHorizonRectangle(input, lodCamera, outerMin, outerMax);
        if (horizon && !ResolveHorizonPatchDomain(outerMin, outerMax, axisScale, surface, cb.horizonPatchDomain)) return;
        SurfacePatchView view;
        view.cameraPosition = lodCamera.m_position;
        view.cameraForward = lodCamera.GetForward().Normalized();
        view.projectionScalePixels = std::abs(projection.m[1][1]) * static_cast<float>((std::max)(lodViewportHeight, 1u)) * 0.5f;
        view.viewportHeight = lodViewportHeight;
        view.minProjectionDistance = (std::max)(lodCamera.m_near, 0.001f);
        view.orthographic = lodCamera.m_projection == ProjectionMode::Orthographic;
        view.frustum = frustumEnabled ? frustum : nullptr;
        SurfacePatchView selectionView = view;
        selectionView.includeCulledPatches = true;
        std::optional<math::Frustum> lodFrustum;
        if (horizon && lodCameraOverride) {
            selectionView.frustum = nullptr;
            if (ctx.frustumCullingEnabled) {
                lodFrustum.emplace(math::Frustum::FromViewProjection(lodCamera.GetGpuViewProjection()));
                selectionView.frustum = &*lodFrustum;
            }
        }
        SurfacePatchLodSettings patchLod = input.patchLod;
        patchLod.morphEnabled = input.patchMorphEnabled;
        auto selection = ctx.surfacePatchSelectionCache.FindOwned(surface, patchLod, selectionView);
        SurfacePatchSelection computedSelection;
        if (!selection) {
            if (!SelectSurfacePatches(surface, patchLod, selectionView, computedSelection)) return;
            selection = ctx.surfacePatchSelectionCache.Store(surface, patchLod, selectionView, std::move(computedSelection));
        }
        cb.gridParams.x = 0.0f;
        cb.farOceanParams = {horizon ? 1.0f : 0.0f, horizon ? 1.0f : 0.0f,
            input.farOceanRippleFadeDistance, 0.0f};
        cb.patchMorphView = {view.cameraPosition.x, view.cameraPosition.y, view.cameraPosition.z,
            view.projectionScalePixels};
        cb.patchMorphForward = {view.cameraForward.x, view.cameraForward.y, view.cameraForward.z,
            view.orthographic ? 1.0f : 0.0f};
        cb.patchMorphMetric = {view.minProjectionDistance, patchLod.targetCellPixels,
            patchLod.nearDistance, patchLod.nearCellSize};
        for (const auto& patch : selection->patches) {
            /// @note 選択と描画は変位・モーフ込みの同じ保守的な境界を使う。
            const bool frustumRejected = !IsSurfacePatchVisible(surface, patch, view);
            const bool selected = view.includeCulledPatches || !frustumRejected;
            if (!gpuCulling && !selected) continue;
            if (countStatistics && selected) ++ctx.statsTotalObjects;
            if (frustumRejected) {
                if (countStatistics && selected) ++ctx.statsFrustumCulled;
                if (!gpuCulling) continue;
            }
            const auto mask = patch.coarseEdgeMask;
            if (occluded(patch.aabbMin, patch.aabbMax, 0.0f, selected && !frustumRejected)) continue;
            if (mask >= input.patchIndexBuffers.size() || !input.patchIndexBuffers[mask].IsValid()) continue;
            SetWaterPatchConstants(cb, patch, patchLod.cellsPerPatch);
            SetWaterPatchMorphConstants(cb, patch, patchLod, input.patchMorphEnabled, SURFACE_MORPH_AXIS_BOTH);
            const auto indices = SelectWaterRasterIndices(input.patchIndexBuffers[mask],
                input.patchIndexCounts[mask], input.patchStrips[mask], useStrip);
            submit(input.patchVertexBuffer, indices, cb, patch.aabbMin, patch.aabbMax);
        }
        return;
    }
    math::Vector2 focus{};
    if (cb.gridParams.x > 0.5f) {
        focus = ResolveWaterGridFocus(cb.worldMatrix, lodCamera.m_position, lodCamera.GetForward(),
            {cb.gridParams.z, cb.gridParams.w}, cb.gridParams.y);
        cb.waveShapeParams.z = focus.x;
        cb.waveShapeParams.w = focus.y;
    }
    cb.patchCellSize = {};
    for (const auto& patch : input.patches) {
        if (countStatistics) ++ctx.statsTotalObjects;
        auto low = patch.aabbMin, high = patch.aabbMax;
        FocusWaterChunkBounds(input, focus, low, high);
        if (frustumEnabled && !AabbVisible(*frustum, cb.worldMatrix, low, high, input.margin)) {
            if (countStatistics) ++ctx.statsFrustumCulled;
            continue;
        }
        if (occluded(low, high, std::sqrt(2.0f * input.margin.horizontal * input.margin.horizontal
            + input.margin.vertical * input.margin.vertical))) continue;
        const auto indices = SelectWaterRasterIndices(patch.indexBuffer, patch.indexCount, patch.strip, useStrip);
        submit(patch.vertexBuffer, indices, cb, low, high);
    }
}

/// @note Water.hlsl の WaterPatchInstance と192 byte単位で同期する。body/viewの定数は共有WaterCBに残す。
struct WaterPatchInstance {
    math::Vector4 rect, cellSize;
    std::array<math::Vector4, 4> edgeCellSize, cornerCellSize;
    math::Vector4 morphParams, morphCellSize;
};
static_assert(sizeof(WaterPatchInstance) == 192);

struct WaterGpuDraw {
    ComputeCall compute;
    GpuGeometryDispatch indirect;
    uint32_t visibleInstancesForStatistics = 0;
};

/// @note A single LOD retains the balanced/stitch topology. The generic service can choose multiple LOD ranges for independent meshes.
/// @see Docs/design/gpu-driven-geometry.md
WaterGpuDraw PrepareWaterGpuDraw(const RenderWaterInput& input, RenderPassContext& ctx,
    const WaterCB& cb, std::span<const WaterPatchInstance> instances,
    std::span<const std::array<math::Vector3, 2>> bounds, uint32_t indexCount)
{
    WaterGpuDraw result;
    if (instances.empty() || !input.adaptivePatchLod
        || indexCount == 0 || indexCount % 3u != 0)
        return result;
    auto& handles = GetWaterDrawResources(ctx.resources);
    if (ctx.settings.geometryDrawMode != GeometryDrawMode::GPU_INDIRECT
        || bounds.size() != instances.size() || !ctx.renderer.SupportsIndirectDraw()
        || !ctx.frustumCullingEnabled || !handles.geometrySelection)
        return result;
    const auto frustum = ctx.cameraFrustum ? *ctx.cameraFrustum
        : math::Frustum::FromViewProjection(ctx.camera.GetGpuViewProjection());
    GpuGeometrySelectionView view;
    for (int planeIndex = 0; planeIndex < math::Frustum::PLANE_COUNT; ++planeIndex) {
        const auto plane = frustum.GetPlane(planeIndex);
        view.frustumPlanes[planeIndex] = {plane.normal.x, plane.normal.y, plane.normal.z, plane.distance};
    }
    const float projectionScale = std::abs(ctx.camera.GetProjectionMatrix().m[1][1])
        * static_cast<float>((std::max)(ctx.height, 1u)) * 0.5f;
    view.cameraAndProjectionScale = {ctx.camera.m_position.x, ctx.camera.m_position.y,
        ctx.camera.m_position.z, projectionScale};
    view.projectionMetrics = {projectionScale, input.patchLod.targetCellPixels,
        (std::max)(ctx.camera.m_near, 0.001f), ctx.camera.m_projection == ProjectionMode::Orthographic ? 1.0f : 0.0f};
    const auto& world = cb.worldMatrix;
    const uint32_t cells = input.patchLod.cellsPerPatch;
    if (cells < 2u || cells > 256u) return result;
    const uint32_t vertexCount = (cells + 1u) * (cells + 1u);
    std::vector<GpuGeometryCandidate> candidates;
    candidates.reserve(instances.size());
    for (size_t patchIndex = 0; patchIndex < instances.size(); ++patchIndex) {
        const auto& patch = instances[patchIndex];
        /// @note Reuse the selector's displacement and morph bounds. Reconstructing from UV loses the morph safety margin.
        const auto& low = bounds[patchIndex][0];
        const auto& high = bounds[patchIndex][1];
        const auto localCenter = (low + high) * 0.5f;
        const auto localExtents = (high - low) * 0.5f;
        const auto center = world * math::Vector4{localCenter, 1.0f};
        const auto projectRow = [&](uint32_t row) {
            return std::abs(world.m[row][0]) * localExtents.x
                + std::abs(world.m[row][1]) * localExtents.y + std::abs(world.m[row][2]) * localExtents.z;
        };
        const math::Vector3 safeExtents{projectRow(0), projectRow(1), projectRow(2)};
        /// @note Keep the old CPU visibility counters as a diagnostic estimate; GPU argument generation still owns the actual draw decision.
        if (frustum.IntersectsAABB({center.x, center.y, center.z}, safeExtents))
            ++result.visibleInstancesForStatistics;
        GpuGeometryCandidate candidate;
        candidate.boundsMin = {center.x - safeExtents.x, center.y - safeExtents.y, center.z - safeExtents.z, 0.0f};
        candidate.boundsMax = {center.x + safeExtents.x, center.y + safeExtents.y, center.z + safeExtents.z, 0.0f};
        candidate.lodCount = 1;
        candidate.lods[0] = {indexCount, 0u, 0, vertexCount,
            (std::max)(patch.cellSize.x, patch.cellSize.y), {}};
        candidates.push_back(candidate);
    }
    (void)handles.geometrySelection->PrepareDispatch(ctx.renderer, ctx.resources, view, candidates,
        indexCount, vertexCount, result.compute, result.indirect);
    if (result.indirect.indirectArguments.IsValid() && !handles.indirectPathReported) {
        handles.indirectPathReported = true;
        FBZZ_LOG_INFO("Water geometry path: GPU indexed indirect prepared (%u candidates)", result.indirect.candidateCount);
    }
    return result;
}

WaterPatchInstance MakeWaterPatchInstance(const WaterCB& cb)
{
    WaterPatchInstance instance{cb.patchRect, cb.patchCellSize, {}, {}, cb.patchMorphParams, cb.patchMorphCellSize};
    for (size_t edge = 0; edge < 4; ++edge) {
        instance.edgeCellSize[edge] = cb.patchEdgeCellSize[edge];
        instance.cornerCellSize[edge] = cb.patchCornerCellSize[edge];
    }
    return instance;
}

void ApplyWaterPatchInstance(WaterCB& cb, const WaterPatchInstance& instance)
{
    cb.patchRect = instance.rect;
    cb.patchCellSize = instance.cellSize;
    cb.patchMorphParams = instance.morphParams;
    cb.patchMorphCellSize = instance.morphCellSize;
    for (size_t edge = 0; edge < 4; ++edge) {
        cb.patchEdgeCellSize[edge] = instance.edgeCellSize[edge];
        cb.patchCornerCellSize[edge] = instance.cornerCellSize[edge];
    }
}

/// @brief 同じ索引を使う連続パッチだけを束ね、半透明の元のprimitive順を保つ。
/// @note バッファは束・パス・ビューごとに借り直し、GPUが読む前の上書きを防ぐ。custom shaderは従来のCBを使う。
/// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-drawindexedinstanced DrawIndexedInstanced
template<class Submit>
void VisitWaterPatchDraws(const RenderWaterInput& input, RenderPassContext& ctx, bool countStatistics,
    bool enableInstances, Submit&& submit, const Camera* cameraOverride = nullptr,
    uint32_t viewportHeight = 0, bool allowFarOcean = true, const Camera* lodCameraOverride = nullptr,
    bool enableGpuGeometry = false, bool allowStrip = true)
{
    ResourceHandle<BufferTag> vertices;
    WaterRasterIndices indices;
    WaterCB base{};
    std::vector<WaterPatchInstance> instances;
    std::vector<std::array<math::Vector3, 2>> bounds;
    /// @note Keep balanced invisible leaves as candidates only for the GPU-frustum path; other passes and custom shaders retain CPU visibility.
    const bool gpuCulling = enableGpuGeometry && enableInstances && input.adaptivePatchLod
        && ctx.settings.geometryDrawMode == GeometryDrawMode::GPU_INDIRECT
        && ctx.frustumCullingEnabled && ctx.renderer.SupportsIndirectDraw();
    /// @note カスタム shader と GPU 間接経路は既存の面順序と indexCount / 3 契約を保つ。
    const bool useStrip = allowStrip && enableInstances && ctx.settings.rasterGeometryMode == RasterGeometryMode::TRIANGLE_STRIP
        && ctx.settings.geometryDrawMode != GeometryDrawMode::GPU_INDIRECT;
    auto flush = [&] {
        if (instances.empty()) return;
        auto buffer = (instances.size() > 1 || (enableGpuGeometry
            && ctx.settings.geometryDrawMode == GeometryDrawMode::GPU_INDIRECT))
            ? GetWaterDrawResources(ctx.resources).patchInstances.Acquire(ctx.resources, instances.size(), sizeof(WaterPatchInstance))
            : ResourceHandle<StructuredBufferTag>{};
        if (buffer.IsValid()) {
            ctx.resources.Update(buffer, instances.data(), instances.size() * sizeof(WaterPatchInstance));
            auto cb = base;
            cb.patchCellSize.z = 2.0f;
            const auto gpu = gpuCulling
                ? PrepareWaterGpuDraw(input, ctx, cb, instances, bounds, indices.count) : WaterGpuDraw{};
            submit(vertices, indices, cb, buffer, static_cast<uint32_t>(instances.size()), gpu);
        } else {
            for (const auto& instance : instances) {
                auto cb = base;
                ApplyWaterPatchInstance(cb, instance);
                submit(vertices, indices, cb, ResourceHandle<StructuredBufferTag>{}, 1u, WaterGpuDraw{});
            }
        }
        instances.clear();
        bounds.clear();
    };
    VisitWaterPatches(input, ctx, countStatistics,
        [&](auto vertexBuffer, const WaterRasterIndices& patchIndices, const WaterCB& cb,
            const math::Vector3& low, const math::Vector3& high) {
            if (!enableInstances || cb.patchCellSize.z <= 0.5f) {
                flush();
                submit(vertexBuffer, patchIndices, cb, ResourceHandle<StructuredBufferTag>{}, 1u, WaterGpuDraw{});
                return;
            }
            if (!instances.empty() && (vertices != vertexBuffer || indices != patchIndices)) flush();
            if (instances.empty()) {
                vertices = vertexBuffer;
                indices = patchIndices;
                base = cb;
            }
            instances.push_back(MakeWaterPatchInstance(cb));
            if (gpuCulling) bounds.push_back({low, high});
        }, cameraOverride, viewportHeight, allowFarOcean, lodCameraOverride, gpuCulling, useStrip);
    flush();
}

/// @brief Tests whether this view has disabled a water owner after resource preparation.
bool IsWaterMediumOwnerDisabled(const RenderPassContext& ctx, uint64_t ownerKey)
{
    const auto& disabled = ctx.handles.waterMediumDisabledOwnerKeys;
    return std::find(disabled.begin(), disabled.end(), ownerKey) != disabled.end();
}

/// @note Color and interface consume the same immutable atlas owner indices for the current view.
math::Vector4 ResolveHorizonRiverMedia(const RenderWaterInput& ocean, const RenderScene& scene,
    const RenderPassContext& ctx)
{
    if (!ocean.farOceanEnabled || ocean.constants.horizonWaveSeams.front().w <= 0.0f) return {};
    std::array<float, 4> indices{};
    for (uint32_t i = 0; i < ctx.handles.waterMediumBodies.size(); ++i) {
        const auto index = ctx.handles.waterMediumBodies[i].waterInputIndex;
        if (index >= scene.water.size()) continue;
        const auto& river = scene.water[index];
        if (river.horizonReceiverKey != ocean.medium.stableOwnerKey
            || IsWaterMediumOwnerDisabled(ctx, river.medium.stableOwnerKey)) continue;
        for (std::size_t seam = 0; seam < indices.size(); ++seam) {
            const auto& a = ocean.constants.horizonWaveSeams[seam];
            const auto& b = river.constants.materialBlendMouth;
            if (a.w > 0.0f && a.x == b.x && a.y == b.y) indices[seam] = static_cast<float>(i + 1u);
        }
    }
    return {indices[0], indices[1], indices[2], indices[3]};
}

/// @brief Adds one owner to the view's fail-closed water list without duplicating its key.
void DisableWaterMediumOwner(RenderPassContext& ctx, uint64_t ownerKey)
{
    auto& disabled = ctx.handles.waterMediumDisabledOwnerKeys;
    if (!IsWaterMediumOwnerDisabled(ctx, ownerKey)) disabled.push_back(ownerKey);
}

/// @brief Disables every owner whose dry-volume clipping cannot be evaluated this frame.
void DisableWaterMediumExclusionOwners(RenderPassContext& ctx)
{
    if (!ctx.renderScene) return;
    for (const auto& input : ctx.renderScene->water)
        if (!input.medium.exclusions.empty()) DisableWaterMediumOwner(ctx, input.medium.stableOwnerKey);
}

/// @brief Disables one captured input when its view-local atlas tile cannot be produced.
void DisableWaterMediumExclusionOwner(RenderPassContext& ctx, uint32_t waterInputIndex)
{
    if (!ctx.renderScene || waterInputIndex >= ctx.renderScene->water.size())
    {
        DisableWaterMediumExclusionOwners(ctx);
        return;
    }
    const auto& input = ctx.renderScene->water[waterInputIndex];
    if (!input.medium.exclusions.empty()) DisableWaterMediumOwner(ctx, input.medium.stableOwnerKey);
}

/// @brief Clears stale atlas receipts and hides exclusion owners if atlas capture failed.
void InvalidateWaterMediumAtlas(RenderPassContext& ctx)
{
    DisableWaterMediumExclusionOwners(ctx);
    ctx.waterMediumPassActive = false;
    ctx.waterInterfacePassActive = false;
    ctx.handles.waterMediumTable = {};
    ctx.handles.waterMediumBodyCount = 0;
    ctx.handles.waterMediumTileResolution = 0;
    ctx.handles.waterMediumBodies.clear();
}
}
const WaterDetailNoise& GetWaterDetailNoise(renderer::ResourceManager& resources)
{
    static WaterDetailNoise s_noise;
    static uint64_t s_owner = 0;
    static uint64_t s_bakedAt = 0xFFFFFFFFFFFFFFFFull;

    /// @note 焼き直しを Reset に紐づける。デバイスロストで実体が消えてもハンドルは残るため、
    /// @note 世代が変わったときだけ焼き直せば失敗時に毎フレーム焼き続けずに済む。
    const uint64_t resetVersion = resources.GetResetVersion();
    if (s_owner == resources.GetInstanceId() && s_bakedAt == resetVersion)
        return s_noise;
    s_bakedAt = resetVersion;
    s_owner = resources.GetInstanceId();

    const waternoise::Tile tile = waternoise::BakeDetailTile();
    std::vector<renderer::TextureMipData> mips;
    mips.reserve(tile.mips.size());
    for (size_t level = 0; level < tile.mips.size(); ++level)
        mips.push_back({ tile.mips[level].data(), tile.sizes[level], tile.sizes[level] });

    s_noise.texture = resources.CreateTextureWithMips(mips.data(), static_cast<uint32_t>(mips.size()));
    s_noise.derivativeScale = tile.derivativeScale;
    s_noise.invTileCells = 1.0f / static_cast<float>(waternoise::kTileCells);
    return s_noise;
}

bool WaterMediumPipelineResourcesReady(renderer::ResourceManager& resources)
{
    const auto& handles = GetWaterDrawResources(resources);
    return handles.mediumAtlasShader.IsValid() && handles.mediumAtlasPSO.IsValid()
        && handles.interfaceShader.IsValid() && handles.interfacePSO.IsValid();
}

void WaterMediumAtlasPass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    if (ctx.waterMediumPassActive)
        builder.Write("WaterMediumAtlas");
}

void WaterMediumAtlasPass::Execute(PassResources&, RenderPassContext& ctx)
{
    /// @note Resource and Submit callbacks may replace the context publication; keep this invocation's immutable inputs alive.
    const auto sceneOwner = ctx.renderScene;
    if (!ctx.waterMediumPassActive || !sceneOwner) return;
    if (ctx.handles.waterMediumBodies.empty() || !ctx.handles.waterMediumAtlasRT
        || !ctx.handles.waterMediumAtlasRT->IsValid() || !ctx.handles.waterMediumTable.IsValid()
        || !ctx.resources.Get(ctx.handles.waterMediumTable))
    {
        InvalidateWaterMediumAtlas(ctx);
        return;
    }

    const auto& handles = GetWaterDrawResources(ctx.resources);
    if (!handles.mediumAtlasShader.IsValid() || !handles.mediumAtlasPSO.IsValid()) {
        InvalidateWaterMediumAtlas(ctx);
        return;
    }

    ctx.renderer.SetRenderTarget(*ctx.handles.waterMediumAtlasRT, ctx.resources);
    ctx.renderer.Clear({0.0f, 0.0f, 0.0f, 0.0f});
    ctx.renderer.ClearDepth();
    const uint32_t tileResolution = ctx.handles.waterMediumTileResolution;
    if (tileResolution == 0u) {
        InvalidateWaterMediumAtlas(ctx);
        return;
    }

    for (uint32_t bodyIndex = 0; bodyIndex < ctx.handles.waterMediumBodies.size(); ++bodyIndex) {
        const auto& body = ctx.handles.waterMediumBodies[bodyIndex];
        if (body.waterInputIndex >= sceneOwner->water.size()) {
            InvalidateWaterMediumAtlas(ctx);
            return;
        }
        const auto& input = sceneOwner->water[body.waterInputIndex];
        const float spanX = body.worldBoundsXZ.z - body.worldBoundsXZ.x;
        const float spanZ = body.worldBoundsXZ.w - body.worldBoundsXZ.y;
        if (!(spanX > 1.0e-4f) || !(spanZ > 1.0e-4f)) {
            DisableWaterMediumExclusionOwner(ctx, body.waterInputIndex);
            continue;
        }

        float worldMinY = std::numeric_limits<float>::max();
        float worldMaxY = std::numeric_limits<float>::lowest();
        for (uint32_t corner = 0u; corner < 8u; ++corner) {
            const math::Vector3 local{
                (corner & 1u) != 0u ? input.aabbMax.x : input.aabbMin.x,
                (corner & 2u) != 0u ? input.aabbMax.y : input.aabbMin.y,
                (corner & 4u) != 0u ? input.aabbMax.z : input.aabbMin.z
            };
            const math::Vector4 world = input.constants.worldMatrix * math::Vector4{local.x, local.y, local.z, 1.0f};
            worldMinY = (std::min)(worldMinY, world.y);
            worldMaxY = (std::max)(worldMaxY, world.y);
        }
        if (!std::isfinite(worldMinY) || !std::isfinite(worldMaxY) || worldMaxY < worldMinY) {
            DisableWaterMediumExclusionOwner(ctx, body.waterInputIndex);
            continue;
        }
        const float verticalPadding = (std::max)(input.margin.vertical, 0.0f) + 1.0f;
        Camera atlasCamera;
        atlasCamera.m_position = {
            0.5f * (body.worldBoundsXZ.x + body.worldBoundsXZ.z),
            worldMinY - verticalPadding,
            0.5f * (body.worldBoundsXZ.y + body.worldBoundsXZ.w)
        };
        atlasCamera.m_rotation = math::Quaternion::LookRotation(
            {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
        atlasCamera.m_projection = ProjectionMode::Orthographic;
        atlasCamera.m_orthoHeight = spanZ;
        atlasCamera.m_aspect = spanX / spanZ;
        atlasCamera.m_near = 0.01f;
        atlasCamera.m_far = (worldMaxY - worldMinY) + verticalPadding * 2.0f + 0.1f;
        UpdateWaterCamera(ctx, handles, &atlasCamera);
        const math::Vector4 capture{static_cast<float>(bodyIndex + 1u),
            (std::max)(input.medium.depthLimit, 0.0f), 0.0f, 0.0f};
        ctx.resources.Update(handles.mediumCaptureCB, &capture, sizeof(capture));
        ctx.renderer.SetViewport(body.tileX * tileResolution, body.tileY * tileResolution,
            tileResolution, tileResolution);

        const bool connectedOcean = input.farOceanEnabled && input.constants.horizonWaveSeams.front().w > 0.0f;
        VisitWaterPatchDraws(input, ctx, connectedOcean, true,
            [&](auto vertexBuffer, const WaterRasterIndices& indices, const WaterCB& cb, auto instanceBuffer, uint32_t instanceCount, const WaterGpuDraw&) {
                ctx.resources.Update(handles.waterCB, &cb, sizeof(cb));
                renderer::DrawCall call;
                call.vertexBuffer = vertexBuffer;
                indices.Apply(call);
                call.instanceBuffer = instanceBuffer;
                call.instanceCount = instanceCount;
                call.shader = handles.mediumAtlasShader;
                call.pipelineState = handles.mediumAtlasPSO;
                call.constantBuffers[0] = handles.cameraCB;
                call.constantBuffers[1] = handles.waterCB;
                call.constantBuffers[10] = handles.mediumCaptureCB;
                call.textures[3] = input.foam;
                call.textures[4] = input.detailNoise;
                call.textures[8] = input.ripple.IsValid() ? input.ripple : handles.neutralRipple;
                call.textures[26] = input.velocityField;
                ctx.renderer.Submit(call, ctx.resources);
            }, &atlasCamera, ctx.height, true, &ctx.camera);
    }
    ctx.renderer.SetViewport(0, 0, ctx.width, ctx.height);
}

void WaterInterfacePass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    if (!ctx.waterMediumPassActive || !ctx.waterInterfacePassActive) return;
    builder.Read("HDR").Read("WaterMediumAtlas").Write("WaterInterface");
}

void WaterInterfacePass::Execute(PassResources&, RenderPassContext& ctx)
{
    /// @note A nested publication must not retire inputs still consumed by this invocation.
    const auto sceneOwner = ctx.renderScene;
    if (!ctx.waterMediumPassActive || !ctx.waterInterfacePassActive || !sceneOwner
        || ctx.handles.waterMediumBodies.empty() || !ctx.handles.waterInterfaceRT
        || !ctx.handles.waterInterfaceRT->IsValid()) return;

    const auto& handles = GetWaterDrawResources(ctx.resources);
    if (!handles.interfaceShader.IsValid() || !handles.interfacePSO.IsValid()) {
        ctx.waterInterfacePassActive = false;
        return;
    }
    const auto& atlasTarget = ctx.Res().Target("WaterMediumAtlas");
    const auto atlasTexture = ctx.resources.GetColorTexture(atlasTarget, 0);
    const auto sceneDepth = ctx.resources.GetDepthTexture(ctx.Res().Target("HDR"));
    if (!ctx.handles.waterMediumTable.IsValid() || !atlasTexture.IsValid() || !sceneDepth.IsValid()) {
        ctx.waterInterfacePassActive = false;
        return;
    }

    ctx.renderer.SetRenderTarget(*ctx.handles.waterInterfaceRT, ctx.resources);
    ctx.renderer.SetViewport(0, 0, ctx.width, ctx.height);
    ctx.renderer.Clear({0.0f, 0.0f, 0.0f, 0.0f});
    ctx.renderer.ClearDepth();
    UpdateWaterCamera(ctx, handles);
    for (uint32_t bodyIndex = 0; bodyIndex < ctx.handles.waterMediumBodies.size(); ++bodyIndex) {
        const auto& body = ctx.handles.waterMediumBodies[bodyIndex];
        if (body.waterInputIndex >= sceneOwner->water.size()) continue;
        const auto& input = sceneOwner->water[body.waterInputIndex];
        if (input.layer >= 32 || (ctx.cullingMask & (1u << input.layer)) == 0) continue;

        const auto linkedRivers = ResolveHorizonRiverMedia(input, *sceneOwner, ctx);
        const bool connectedOcean = input.farOceanEnabled && input.constants.horizonWaveSeams.front().w > 0.0f;
        VisitWaterPatchDraws(input, ctx, connectedOcean, true,
            [&](auto vertexBuffer, const WaterRasterIndices& indices, WaterCB cb, auto instanceBuffer, uint32_t instanceCount, const WaterGpuDraw&) {
                cb.farOceanParams.w = static_cast<float>(bodyIndex + 1u);
                cb.horizonLinkedRiverMedia = linkedRivers;
                ctx.resources.Update(handles.waterCB, &cb, sizeof(cb));
                renderer::DrawCall call;
                call.vertexBuffer = vertexBuffer;
                indices.Apply(call);
                call.instanceBuffer = instanceBuffer;
                call.instanceCount = instanceCount;
                call.shader = handles.interfaceShader;
                call.pipelineState = handles.interfacePSO;
                call.constantBuffers[0] = handles.cameraCB;
                call.constantBuffers[1] = handles.waterCB;
                call.auxiliaryPsBuffers[0] = ctx.handles.waterMediumTable;
                call.textures[3] = input.foam;
                call.textures[4] = input.detailNoise;
                call.textures[5] = sceneDepth;
                call.textures[8] = input.ripple.IsValid() ? input.ripple : handles.neutralRipple;
                call.textures[15] = atlasTexture;
                call.textures[26] = input.velocityField;
                ctx.renderer.Submit(call, ctx.resources);
            }, nullptr, ctx.height, true);
    }
}

void WaterSelectionMaskSystem(RenderPassContext& ctx)
{
    /// @note Selection consumes the same immutable publication for all water inputs, including across Submit callbacks.
    const auto sceneOwner = ctx.renderScene;
    if (!ctx.selectionOutlineEnabled) return;
    auto& h = ctx.handles;
    if (!h.selectionMaskPSO.IsValid()) return;

    if (!sceneOwner) return;
    const auto& handles = GetWaterDrawResources(ctx.resources);
    if (!handles.selectionShader.IsValid() || !handles.selectionPSO.IsValid()) return;
    renderer::ResourceHandle<renderer::TextureTag> mediumAtlasTexture;
    bool mediumBindingsReady = ctx.waterMediumPassActive && h.waterMediumTable.IsValid()
        && ctx.resourceRegistry.Target("WaterMediumAtlas").IsValid()
        && ctx.resources.Get(h.waterMediumTable);
    if (mediumBindingsReady) {
        mediumAtlasTexture = ctx.resources.GetColorTexture(ctx.Res().Target("WaterMediumAtlas"), 0);
        mediumBindingsReady = mediumAtlasTexture.IsValid();
    }
    UpdateWaterCamera(ctx, handles);
    for (uint32_t inputIndex = 0u; inputIndex < sceneOwner->water.size(); ++inputIndex) {
        const auto& input = sceneOwner->water[inputIndex];
        if (IsWaterMediumOwnerDisabled(ctx, input.medium.stableOwnerKey)) continue;
        if (!input.selected || input.layer >= 32 || (ctx.cullingMask & (1u << input.layer)) == 0) continue;
        const auto body = std::find_if(h.waterMediumBodies.begin(), h.waterMediumBodies.end(),
            [inputIndex](const auto& candidate) { return candidate.waterInputIndex == inputIndex; });
        const bool clipExclusions = mediumBindingsReady && !input.medium.exclusions.empty()
            && body != h.waterMediumBodies.end();
        if (!input.medium.exclusions.empty() && !clipExclusions) continue;
        const uint32_t bodyIndex = clipExclusions
            ? static_cast<uint32_t>(body - h.waterMediumBodies.begin()) : 0u;
        VisitWaterPatchDraws(input, ctx, false, true, [&](auto vertexBuffer, const WaterRasterIndices& indices, const WaterCB& cb, auto instanceBuffer, uint32_t instanceCount, const WaterGpuDraw&) {
            auto selectionCB = cb;
            selectionCB.crestFoamParams.z = 0.0f;
            selectionCB.farOceanParams.w = clipExclusions ? static_cast<float>(bodyIndex + 1u) : 0.0f;
            ctx.resources.Update(handles.waterCB, &selectionCB, sizeof(selectionCB));
            renderer::DrawCall dc;
            dc.vertexBuffer       = vertexBuffer;
            indices.Apply(dc);
            dc.instanceBuffer     = instanceBuffer;
            dc.instanceCount      = instanceCount;
            dc.shader             = handles.selectionShader;
            dc.pipelineState      = handles.selectionPSO;
            dc.constantBuffers[0] = handles.cameraCB;
            dc.constantBuffers[1] = handles.waterCB;
            if (clipExclusions) {
                dc.auxiliaryPsBuffers[0] = h.waterMediumTable;
                dc.textures[15] = mediumAtlasTexture;
            }
            dc.textures[8] = input.ripple.IsValid() ? input.ripple : handles.neutralRipple;
            dc.textures[26] = input.velocityField;
            ctx.renderer.Submit(dc, ctx.resources);
        });
    }
}

/// @name IRenderPass

std::string_view WaterRenderPass::Name() const { return "WaterForward"; }

void WaterRenderPass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    /// @note 平行光の影 (t9) に加え、BindForwardShadingResources が Spot/Point の影 (t28) と
    /// @note Cookie (t31) を束縛する。申告しないと Shadow / LightCookie より先に走ってよいことになる。
    ///
    /// @note 屈折用のシーンカラー / 深度のコピーはこのパスの中で作って読み切る作業用で、
    /// @note 他のパスからは見えない。元の HDR は下の ReadWrite で押さえてある。
    /// @note SetAutoTarget は呼ばない。屈折用のコピーを作る間に束縛を 3 回切り替えるので、
    /// @note 描き先は Execute の中で自分で張る。
    builder.ReadWrite("HDR").Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas");
    if (ctx.waterMediumPassActive)
        builder.Read("WaterMediumAtlas");
}
void WaterRenderPass::Execute(PassResources&, RenderPassContext& ctx)
{
    /// @note Pin before any resource callback so replacement of ctx.renderScene cannot invalidate this pass's water references.
    const auto sceneOwner = ctx.renderScene;
    renderer::IRenderer& renderer = ctx.renderer;
    renderer::ResourceManager& resources = ctx.resources;
    const renderer::Camera& camera = ctx.camera;
    const renderer::RenderSettings* settings = &ctx.settings;
    const auto lightCB = ctx.handles.lightCB;
    const auto shadowDepthTexture = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
    const auto shadowCB = ctx.handles.shadowCB;
    auto& handles = GetWaterDrawResources(resources);
    const auto copyColorShader = handles.copyColorShader;
    const auto depthCopyShader = handles.depthCopyShader;
    /// @note カリング錐台はジッター無しのまま。半ピクセルのために可視判定を揺らす意味がない。
    const math::Frustum frustum = math::Frustum::FromViewProjection(camera.GetGpuViewProjection());

    /// @note 描画用の行列だけ TAA ジッターを乗せる。乗せないと水面だけ AA が効かず、
    /// @note b0 経由で描く不透明物とサブピクセルずれた深度になって TAA の再投影が濁る。
    UpdateWaterCamera(ctx, handles);

    if (!sceneOwner) return;
    bool hasVisibleWater = false;
    for (const auto& input : sceneOwner->water) {
        if (IsWaterMediumOwnerDisabled(ctx, input.medium.stableOwnerKey)) continue;
        if (input.layer >= 32 || (ctx.cullingMask & (1u << input.layer)) == 0) continue;
        auto low = input.aabbMin, high = input.aabbMax;
        math::Vector2 outerMin, outerMax;
        if (ResolveWaterHorizonRectangle(input, ctx.camera, outerMin, outerMax)) {
            low.x = outerMin.x; low.z = outerMin.y;
            high.x = outerMax.x; high.z = outerMax.y;
        }
        if (!ctx.frustumCullingEnabled || AabbVisible(frustum, input.constants.worldMatrix, low, high, input.margin)) {
            hasVisibleWater = true; break;
        }
    }
    if (!hasVisibleWater) return;
    /// @note 作業 RT はビューが持つ (理由は RenderPassHandles::waterSceneColorRT を参照)。
    if (!ctx.handles.waterSceneColorRT || !ctx.handles.waterSceneDepthRT) return;
    renderer::SizedRenderTarget& sceneColorRT = *ctx.handles.waterSceneColorRT;
    renderer::SizedRenderTarget& sceneDepthRT = *ctx.handles.waterSceneDepthRT;

    (void)sceneColorRT.Ensure(resources, ctx.width, ctx.height, 1);
    (void)sceneDepthRT.Ensure(resources, ctx.width, ctx.height, renderer::CameraDepthTargetDesc(0));
    renderer.SetRenderTarget(sceneColorRT, resources);
    if (copyColorShader.IsValid()) {
        renderer::DrawCall copyDC;
        copyDC.shader = copyColorShader;
        copyDC.pipelineState = ctx.handles.postprocPSO;
        copyDC.vertexCount = 3;
        copyDC.textures[5] = resources.GetColorTexture(ctx.Res().Target("HDR"), 0);
        renderer.Submit(copyDC, resources);
    }
    const auto sceneColor = resources.GetColorTexture(sceneColorRT, 0);

    /// @note HDR の depth を Water 専用の深度 RT へコピーし、PS ではその SRV (t5) を読む。
    /// @note hdrRT を RTV/DSV として使いながら同じ depth を SRV(t5) で読むと DX11 の
    /// @note read/write 競合で SRV が解除され、背景判定・水深・泡が破綻する。
    renderer.SetRenderTarget(sceneDepthRT, resources);
    renderer.ClearDepth();
    if (depthCopyShader.IsValid()) {
        renderer::DrawCall depthDC;
        depthDC.shader = depthCopyShader;
        depthDC.pipelineState = ctx.handles.defaultPSO;
        depthDC.vertexCount = 3;
        depthDC.textures[7] = resources.GetDepthTexture(ctx.Res().Target("HDR"));
        renderer.Submit(depthDC, resources);
    }
    const auto sceneDepth = resources.GetDepthTexture(sceneDepthRT);
    renderer::ResourceHandle<renderer::TextureTag> mediumAtlasTexture;
    bool mediumBindingsReady = ctx.waterMediumPassActive && ctx.handles.waterMediumTable.IsValid()
        && ctx.resourceRegistry.Target("WaterMediumAtlas").IsValid()
        && resources.Get(ctx.handles.waterMediumTable);
    if (mediumBindingsReady) {
        mediumAtlasTexture = resources.GetColorTexture(ctx.Res().Target("WaterMediumAtlas"), 0);
        mediumBindingsReady = mediumAtlasTexture.IsValid();
    }
    std::vector<uint32_t> mediumBodyByWater(sceneOwner->water.size(), 0xFFFFFFFFu);
    if (mediumBindingsReady) {
        for (uint32_t bodyIndex = 0u; bodyIndex < ctx.handles.waterMediumBodies.size(); ++bodyIndex) {
            const uint32_t inputIndex = ctx.handles.waterMediumBodies[bodyIndex].waterInputIndex;
            if (inputIndex < mediumBodyByWater.size()) mediumBodyByWater[inputIndex] = bodyIndex;
        }
    }

    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);

    /// @note しぶきの GameObject は UpdateWaterSplashes (WaterSystem) が作って消す。
    /// @note 描画中にシーンを書き換えると、後続パスが握るエミッターがすり替わる。

    /// @note 輪の寿命は WaterSystem が進める。ここで進めると SceneView と GameView で
    /// @note 2 回進み、ビューを 2 つ開いた瞬間に波紋が倍速で消える。

    const bool crestSprayEnabled = !settings->IsWireframe() && handles.crestSprayShader.IsValid()
        && handles.crestSprayPSO.IsValid();
    std::vector<uint32_t> crestSprayOwners;
    std::vector<uint32_t> connectedDrawOrder;
    if (std::any_of(sceneOwner->water.begin(), sceneOwner->water.end(), [](const auto& input) {
            return input.horizonReceiverKey != 0;
        })) {
        connectedDrawOrder.reserve(sceneOwner->water.size());
        for (uint32_t i = 0; i < sceneOwner->water.size(); ++i)
            if (sceneOwner->water[i].farOceanEnabled) connectedDrawOrder.push_back(i);
        for (uint32_t i = 0; i < sceneOwner->water.size(); ++i)
            if (!sceneOwner->water[i].farOceanEnabled) connectedDrawOrder.push_back(i);
    }
    uint32_t receivingOceanIndex = static_cast<uint32_t>(sceneOwner->water.size());
    if (std::any_of(sceneOwner->water.begin(), sceneOwner->water.end(), [](const RenderWaterInput& water) {
        return water.constants.riverHorizonCaps[0].z > 0.0f || water.constants.riverHorizonCaps[1].z > 0.0f;
    })) {
        const auto ocean = std::find_if(sceneOwner->water.begin(), sceneOwner->water.end(),
            [](const RenderWaterInput& water) { return water.farOceanEnabled; });
        receivingOceanIndex = static_cast<uint32_t>(ocean - sceneOwner->water.begin());
    }
    bool receivingOceanDrawn = false;
    for (uint32_t drawIndex = 0u; drawIndex < sceneOwner->water.size(); ++drawIndex) {
        uint32_t inputIndex = drawIndex;
        /// @note Composite the receiving ocean first without changing the published medium order or indices.
        if (!connectedDrawOrder.empty()) {
            inputIndex = connectedDrawOrder[drawIndex];
        } else if (receivingOceanIndex < sceneOwner->water.size()) {
            if (drawIndex == 0) inputIndex = receivingOceanIndex;
            else if (drawIndex <= receivingOceanIndex) inputIndex = drawIndex - 1;
        }
        const auto& input = sceneOwner->water[inputIndex];
        if (IsWaterMediumOwnerDisabled(ctx, input.medium.stableOwnerKey)) continue;
        if (input.layer >= 32 || (ctx.cullingMask & (1u << input.layer)) == 0) continue;
        resources.Update(handles.effectCB, &input.effects, sizeof(input.effects));
        if (!input.zoneMaterials.empty()) {
            std::array<WaterMaterialBlendTarget, 32> profiles{};
            std::copy(input.zoneMaterials.begin(), input.zoneMaterials.end(), profiles.begin());
            resources.Update(handles.zoneAppearanceCB, profiles.data(), sizeof(profiles));
        }
        const auto depthTex = sceneDepth;
        const auto colorTex = sceneColor.IsValid() ? sceneColor : handles.black;
        const auto rippleTex = input.ripple.IsValid() ? input.ripple : handles.neutralRipple;
        const auto requestedShader = input.shader.IsValid() ? input.shader : handles.waterShader;
        const bool acceptsAppearance = resources.GetShaderCapabilities(requestedShader).SupportsWaterGeometry();
        /// @note CPU surface queries, selection and medium geometry cannot follow arbitrary project vertex code.
        const auto activeShader = acceptsAppearance ? requestedShader : handles.waterShader;
        if (!acceptsAppearance && input.shader.IsValid()) {
            const uint64_t key = (static_cast<uint64_t>(requestedShader.gen) << 32) | requestedShader.id;
            if (handles.rejectedGeometryWarnings.insert(key).second) {
                const auto* shader = resources.Get(requestedShader);
                FBZZ_LOG_WARN("Water shader '%s' requires water_surface_v1 + water_v1; using shared Water geometry and legacy appearance. Custom vertex displacement is not synchronized with CPU surface queries.",
                    shader ? shader->GetPath().c_str() : "<unavailable>");
            }
        }
        const bool standardWaterShader = acceptsAppearance
            || resources.GetShaderCapabilities(handles.waterShader).SupportsWaterGeometry();
        const uint32_t bodyIndex = mediumBodyByWater[inputIndex];
        const bool hasExclusions = !input.medium.exclusions.empty();
        const auto linkedRivers = mediumBindingsReady ? ResolveHorizonRiverMedia(input, *sceneOwner, ctx) : math::Vector4{};
        const bool bindOwnerMedium = mediumBindingsReady && bodyIndex != 0xFFFFFFFFu
            && (hasExclusions || input.horizonReceiverKey != 0
                || (input.farOceanEnabled && input.constants.opticalParams.x > 0.5f));
        if (hasExclusions && !bindOwnerMedium) continue;
        const bool crestSprayEligible = crestSprayEnabled && input.farOceanEnabled && input.constants.opticalParams.x > 0.5f
            && input.constants.crestFoamParams.z > 0.001f && input.constants.reflectParams.w > 0.1f
            && std::count_if(std::begin(input.constants.waveDir), std::end(input.constants.waveDir),
                [](const math::Vector4& wave) { return wave.w > 0.5f; }) >= 2
            && IsDefaultWaterAppearance(resources, handles, requestedShader);
        bool drewHorizon = false;
        VisitWaterPatchDraws(input, ctx, true, standardWaterShader,
            [&](auto vertexBuffer, const WaterRasterIndices& indices, WaterCB cb, auto instanceBuffer, uint32_t instanceCount, const WaterGpuDraw& gpu) {
            if (!receivingOceanDrawn) {
                cb.riverHorizonCaps[0] = {};
                cb.riverHorizonCaps[1] = {};
            }
            if (inputIndex == receivingOceanIndex) receivingOceanDrawn = true;
            drewHorizon |= cb.farOceanParams.x > 0.5f;
            cb.horizonLinkedRiverMedia = bindOwnerMedium ? linkedRivers : math::Vector4{};
            if (!ctx.handles.iblPrefilter.IsValid()) cb.reflectParams.x = 0.0f;
            /// @note A negative owner ID exposes the camera's medium without enabling per-fragment exclusion tests.
            cb.farOceanParams.w = bindOwnerMedium
                ? (hasExclusions ? 1.0f : -1.0f) * static_cast<float>(bodyIndex + 1u) : 0.0f;
            resources.Update(handles.waterCB, &cb, sizeof(cb));
            renderer::DrawCall call;
            call.vertexBuffer = vertexBuffer;
            indices.Apply(call);
            call.shader       = activeShader;
            call.pipelineState = settings->IsWireframe() ? handles.wireframePSO : handles.waterPSO;
            call.instanceBuffer = instanceBuffer;
            call.instanceCount = instanceCount;
            call.layer        = renderer::RenderLayer::TRANSPARENT_LAYER;
            call.constantBuffers[0] = handles.cameraCB;
            call.constantBuffers[1] = handles.waterCB;
            call.constantBuffers[2] = acceptsAppearance && input.appearanceParameters.IsValid()
                ? input.appearanceParameters : handles.effectCB;
            call.constantBuffers[3] = lightCB;
            call.constantBuffers[4] = shadowCB;
            call.constantBuffers[8] = ctx.handles.advancedGraphicsCB;
            if (!input.zoneMaterials.empty()) call.constantBuffers[11] = handles.zoneAppearanceCB;
            BindForwardShadingResources(call, ctx);
            if (bindOwnerMedium) {
                call.auxiliaryPsBuffers[0] = ctx.handles.waterMediumTable;
                call.textures[15] = mediumAtlasTexture;
            }
            /// @note Legacy register samplers may use t0/t1/t2/t7. All eight project slots remain available through bindless indices.
            if (acceptsAppearance)
                for (const uint32_t slot : {0u, 1u, 2u, 7u}) call.textures[slot] = input.appearanceTextures[slot];
            call.textures[3]  = input.foam;
            /// @note さざ波タイル (全水面で共有)
            call.textures[4]  = input.detailNoise;
            call.textures[5]  = depthTex;
            call.textures[6]  = colorTex;
            call.textures[8]  = rippleTex;
            call.textures[9]  = shadowDepthTexture;
            /// @note TEX_IBL_PREFILTER: 空反射
            call.textures[17] = ctx.handles.iblPrefilter;
            /// @note TEX_VELOCITY_FIELD: 焼いた速度場のアトラス。場が 1 枚も無くても **必ず束縛する**
            /// @note — DX12 の null ディスクリプタは Texture2D 固定で、Texture3D を宣言した
            /// @note スロットを空にすると次元が食い違う。
            call.textures[26] = input.velocityField;
            if (gpu.indirect.indirectArguments.IsValid()) {
                const auto fallback = call;
                call.indirectArguments = gpu.indirect.indirectArguments;
                call.indirectArgumentOffset = gpu.indirect.indirectArgumentOffset;
                call.indirectCommandCount = gpu.indirect.candidateCount;
                call.indirectInstanceOffset = true;
                /// @note The shared constants, compute and draw stay ordered with other transparent water groups.
                gpu_cull_detail::SubmitCullDraw(renderer, resources, gpu.compute, call, fallback);
                ++ctx.statsDrawCalls;
                ctx.statsVertexCount += DrawCallVertexCount(call);
                ctx.statsTriangleCount += static_cast<int>(static_cast<uint64_t>(indices.count / 3u) * gpu.visibleInstancesForStatistics);
            } else {
                SubmitCounted(ctx, call);
                if (instanceCount > 1u) {
                    ++ctx.statsInstancedBatches;
                    ctx.statsInstancedDrawsSaved += static_cast<int>(instanceCount) - 1;
                }
            }
        }, nullptr, 0, true, nullptr, standardWaterShader, activeShader == handles.waterShader);
        if (crestSprayEligible && drewHorizon) crestSprayOwners.push_back(inputIndex);
    }

    /// @note All water surfaces must write depth before any transparent spray tests against them.
    /// @note The procedural instance grid matches WaterCrestSpray.hlsl.
    constexpr uint32_t CREST_SPRAY_GRID_WIDTH = 21;
    for (const uint32_t inputIndex : crestSprayOwners) {
        const auto& input = sceneOwner->water[inputIndex];
        const bool hasExclusions = !input.medium.exclusions.empty();
        auto constants = input.constants;
        constants.farOceanParams = {1.0f, 0.0f, input.farOceanRippleFadeDistance,
            hasExclusions ? static_cast<float>(mediumBodyByWater[inputIndex] + 1u) : 0.0f};
        resources.Update(handles.waterCB, &constants, sizeof(constants));
        DrawCall call;
        call.shader = handles.crestSprayShader;
        call.pipelineState = handles.crestSprayPSO;
        call.vertexCount = 3;
        call.instanceCount = CREST_SPRAY_GRID_WIDTH * CREST_SPRAY_GRID_WIDTH;
        call.layer = RenderLayer::TRANSPARENT_LAYER;
        call.constantBuffers[0] = handles.cameraCB;
        call.constantBuffers[1] = handles.waterCB;
        call.constantBuffers[3] = lightCB;
        call.textures[3] = input.foam;
        if (hasExclusions) {
            call.auxiliaryPsBuffers[0] = ctx.handles.waterMediumTable;
            call.textures[15] = mediumAtlasTexture;
        }
        SubmitCounted(ctx, call);
    }
}

} /// @note namespace fbzz::renderer
