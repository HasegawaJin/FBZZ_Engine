/// @file    GeometryPassHelpers.cpp
/// @brief   Graphics の共通 PSO・定数バッファとビュー単位の遮蔽準備。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include "RenderScenePassHelpers.hpp"
#include <Graphics/Pipeline/OcclusionCuller.hpp>
#include <Graphics/Pipeline/GpuHiZ.hpp>
#include <Graphics/Pipeline/MeshletDrawHelpers.hpp>
#include <Graphics/Passes/Geometry/InstanceRenderPasses.hpp>
#include <Graphics/Renderer/DynamicBufferPool.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
namespace fbzz::renderer {
namespace {
std::array<uint32_t, 96> CaptureOcclusionView(const RenderPassContext& ctx)
{
    std::array<uint32_t, 96> result{};
    size_t offset = 0;
    const auto put = [&](float value) { result[offset++] = std::bit_cast<uint32_t>(value); };
    const auto& camera = ctx.camera;
    for (float value : {camera.m_position.x, camera.m_position.y, camera.m_position.z,
        camera.m_rotation.x, camera.m_rotation.y, camera.m_rotation.z, camera.m_rotation.w,
        camera.m_fovY, camera.m_aspect, camera.m_near, camera.m_far, camera.m_orthoHeight,
        ctx.taaJitterNdcX, ctx.taaJitterNdcY, ctx.cullingBoundsPadding, ctx.cullMaxDistance,
        ctx.smallObjectScreenHeight, ctx.cullProjScaleY,
        ctx.cullCameraForward.x, ctx.cullCameraForward.y, ctx.cullCameraForward.z}) put(value);
    for (float distance : ctx.cullLayerDistances) put(distance);
    if (ctx.cameraFrustum) {
        for (int index = 0; index < math::Frustum::PLANE_COUNT; ++index) {
            const auto plane = ctx.cameraFrustum->GetPlane(index);
            put(plane.normal.x); put(plane.normal.y); put(plane.normal.z); put(plane.distance);
        }
    } else offset += math::Frustum::PLANE_COUNT * 4;
    result[offset++] = ctx.cullingMask;
    result[offset++] = ctx.width; result[offset++] = ctx.height;
    result[offset++] = static_cast<uint32_t>(camera.m_projection);
    result[offset++] = static_cast<uint32_t>(ctx.settings.pipeline);
    result[offset++] = static_cast<uint32_t>(ctx.settings.geometryDrawMode);
    result[offset++] = static_cast<uint32_t>(ctx.settings.rasterGeometryMode);
    result[offset++] = static_cast<uint32_t>(ctx.settings.viewMode);
    result[offset++] = static_cast<uint32_t>(ctx.frustumCullingEnabled)
        | (static_cast<uint32_t>(ctx.occlusionCullingEnabled) << 1)
        | (static_cast<uint32_t>(ctx.hasLayerCullDistances) << 2)
        | (static_cast<uint32_t>(ctx.cullDistanceSpherical) << 3)
        | (static_cast<uint32_t>(ctx.cullOrthographic) << 4)
        | (static_cast<uint32_t>(ctx.isDeferred) << 5)
        | (static_cast<uint32_t>(ctx.cameraFrustum != nullptr) << 6)
        | (static_cast<uint32_t>(ctx.waterMediumPassActive) << 7);
    return result;
}

struct MeshOcclusionCandidate {
    math::Vector4 bounds;
    uint32_t indexCount;
    uint32_t padding[3]{};
};
static_assert(sizeof(MeshOcclusionCandidate) == 32);

struct MeshOcclusionView {
    math::Matrix4 viewProjection;
    uint32_t width, height, candidateCount, padding = 0;
};
static_assert(sizeof(MeshOcclusionView) == 80);

const RenderMaterial* MeshOcclusionMaterial(const RenderPassContext& ctx, const RenderMeshItem& item)
{
    const auto route = ResolveGeometryRoute(item.material.capabilities, ctx.isDeferred);
    if (route != GeometryRoute::GBuffer && route != GeometryRoute::ForwardOpaque) return nullptr;
    const auto& material = route == GeometryRoute::GBuffer ? item.material : item.forwardMaterial;
    if (!HasColorGeometry(item) || !item.indexCount || !material.valid
        || material.capabilities.blend != BlendMode::OPAQUE_BLEND
        || material.shaderCapabilities.vertex != ShaderVertexContract::STANDARD_SURFACE
        || !material.shaderCapabilities.HasStandardGeometry()
        || item.material.depthBias != 0 || item.material.depthBiasSlope != 0.0f
        || material.depthBias != 0 || material.depthBiasSlope != 0.0f) return nullptr;
    return &material;
}

bool UsesClusteredDepth(const RenderPassContext& ctx, const RenderObject& object, const RenderMeshItem& item)
{
    return ctx.settings.geometryDrawMode == GeometryDrawMode::MESH_SHADER && ctx.frustumCullingEnabled
        && ctx.isDeferred && ctx.renderer.SupportsMeshShaders()
        && ResolveGeometryRoute(item.material.capabilities, true) == GeometryRoute::GBuffer
        && CanUseGBufferClusterLod(object, item);
}
} /// @note namespace

bool SupportsViewGpuOcclusion(const RenderPassContext& ctx)
{
    const auto mode = ctx.settings.geometryDrawMode;
    return ctx.occlusionCullingEnabled && ctx.occlusionCuller && !ctx.settings.IsWireframe()
        && ctx.width != 0 && ctx.height != 0 && ctx.renderScene && ctx.renderer.SupportsIndirectDraw()
        && (mode == GeometryDrawMode::GPU_INDIRECT || mode == GeometryDrawMode::MESH_SHADER)
        && (!ctx.renderScene->items.empty() || HasTerrainHiZInstanceCandidates(ctx));
}

bool IsViewOcclusionCurrent(const RenderPassContext& ctx)
{
    const auto& prepared = ctx.occlusionPreparedScene;
    if (!ctx.occlusionCullingEnabled || !ctx.occlusionCuller || !prepared || !ctx.renderScene
        || ctx.occlusionPreparedCuller != ctx.occlusionCuller
        || ctx.occlusionPreparedCullerVersion != ctx.occlusionCuller->GetResetVersion()
        || prepared.get() != ctx.renderScene.get()
        || prepared.owner_before(ctx.renderScene) || ctx.renderScene.owner_before(prepared)
        || ctx.renderScene->snapshotSerial != ctx.occlusionPreparedSerial
        || ctx.renderScene->sceneGeneration != ctx.occlusionPreparedSceneGeneration
        || ctx.renderScene->frameStamp != ctx.occlusionPreparedFrame
        || ctx.resources.GetResetVersion() != ctx.occlusionPreparedResourceEpoch
        || ctx.resources.FrameStamp() != ctx.occlusionPreparedResourceFrame
        || CaptureOcclusionView(ctx) != ctx.occlusionPreparedView
        || ctx.settings.passOverrides.size() != ctx.occlusionPreparedPassOverrides.size()) return false;
    for (size_t index = 0; index < ctx.settings.passOverrides.size(); ++index) {
        const auto& current = ctx.settings.passOverrides[index];
        const auto& prior = ctx.occlusionPreparedPassOverrides[index];
        if (current.name != prior.name || current.enabled != prior.enabled
            || current.allowCulling != prior.allowCulling || current.extraReads != prior.extraReads) return false;
    }
    return true;
}

bool IsViewOccluded(const RenderPassContext& ctx, const math::Vector3& center, float radius)
{
    return IsViewOcclusionCurrent(ctx)
        && !ctx.occlusionCuller->TestAndRaster(center, radius);
}

bool IsViewAabbOccluded(const RenderPassContext& ctx, const math::Matrix4& world,
    const math::Vector3& low, const math::Vector3& high, float worldMargin)
{
    if (!IsViewOcclusionCurrent(ctx)) return false;
    if (low.x > high.x || low.y > high.y || low.z > high.z) return false;
    const auto center = world * math::Vector4{(low + high) * 0.5f, 1};
    const auto local = (high - low) * 0.5f;
    const auto extent = [&](int r) {
        return std::abs(world.m[r][0]) * local.x + std::abs(world.m[r][1]) * local.y + std::abs(world.m[r][2]) * local.z;
    };
    const float padding = std::max(worldMargin, 0.0f) + std::max(ctx.cullingBoundsPadding, 0.0f);
    const math::Vector3 extents{extent(0) + padding, extent(1) + padding, extent(2) + padding};
    const math::Vector3 origin{center.x, center.y, center.z};
    return !ctx.occlusionCuller->TestAabb(origin - extents, origin + extents);
}

bool DrawOpaqueOccluderDepth(RenderPassContext& ctx)
{
    const auto scene = ctx.renderScene;
    if (!scene || scene->items.empty() || !IsViewOcclusionCurrent(ctx)) return false;
    auto& resources = ctx.resources;
    auto& shared = resources.Rendering().Shared();
    if (!resources.Get(shared.opaqueOccluderDepthShader))
        shared.opaqueOccluderDepthShader = resources.LoadShader("Assets/Shaders/Pipeline/OpaqueOccluderDepth.hlsl");
    if (ctx.isDeferred && !resources.Get(shared.opaqueOccluderMaterialCB))
        shared.opaqueOccluderMaterialCB = resources.CreateConstantBuffer(96);
    if (!resources.Get(shared.opaqueOccluderDepthShader) || !resources.Get(ctx.handles.frameCB)
        || !resources.Get(ctx.handles.objectCB)
        || (ctx.isDeferred && (!resources.Get(ctx.handles.gbufferShader)
            || !resources.Get(shared.opaqueOccluderMaterialCB)))) return false;

    const auto frame = MakeCameraFrameCB(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    resources.Update(ctx.handles.frameCB, &frame, sizeof(frame));
    ctx.gpuOccluderItems.assign(scene->items.size(), false);
    bool submitted = false;
    for (const auto& object : scene->Objects()) {
        if (!object.colorEligible || object.skinned || object.waterfallFarIndexStart || !MatchesView(ctx, object)
            || object.lodDither != 0.0f || object.leafWindBoundsPadding != 0.0f) continue;
        if (!object.itemCount || object.firstItem >= scene->items.size()
            || object.itemCount > scene->items.size() - object.firstItem) continue;
        const auto bounds = RenderBounds(ctx, object);
        const auto clip = frame.viewProjection * math::Vector4{bounds.center, 1.0f};
        /// @note Approximate diameter in pixels limits depth-only work to substantial occluders.
        constexpr float MIN_OCCLUDER_PIXELS = 64.0f;
        const float diameter = bounds.radius * std::abs(frame.projection.m[1][1]) * static_cast<float>(ctx.height) / clip.w;
        if (clip.w <= 0.0f || !std::isfinite(diameter) || diameter < MIN_OCCLUDER_PIXELS
            || EvaluateVisibility(CullingView(ctx), {bounds.center, bounds.radius, DrawDistance(ctx, object)})
                != VisibilityResult::VISIBLE
            || IsViewOccluded(ctx, bounds.center, bounds.radius)) continue;
        const auto& first = scene->items[object.firstItem];
        if (!HasColorGeometry(first) || (ctx.isDeferred
            && ResolveGeometryRoute(first.material.capabilities, true) != GeometryRoute::GBuffer)) continue;
        const auto constants = ObjectConstants(object);
        resources.Update(ctx.handles.objectCB, &constants, sizeof(constants));
        for (uint32_t index = object.firstItem; index < object.firstItem + object.itemCount; ++index) {
            const auto& item = scene->items[index];
            const auto* material = MeshOcclusionMaterial(ctx, item);
            if (!material || UsesClusteredDepth(ctx, object, item) || !material->shaderCapabilities.SupportsGBuffer()
                || (ctx.isDeferred && ResolveGeometryRoute(item.material.capabilities, true) != GeometryRoute::GBuffer)
                || !resources.Get(item.vertexBuffer) || !resources.Get(item.indexBuffer)) continue;
            float wind;
            std::memcpy(&wind, material->gbufferParams.data() + 68, sizeof(wind));
            if (wind != 0.0f) continue;
            uint32_t textureMask;
            std::memcpy(&textureMask, material->gbufferParams.data() + 80, sizeof(textureMask));
            if ((textureMask & 1u) != 0 && !resources.Get(material->textures[0])) continue;
            DrawCall draw;
            draw.vertexBuffer = item.vertexBuffer;
            draw.indexBuffer = item.indexBuffer;
            draw.indexCount = item.indexCount;
            draw.vertexCount = item.vertexCount;
            draw.shader = shared.opaqueOccluderDepthShader;
            draw.pipelineState = MaterialPipeline(ctx, item.material);
            draw.constantBuffers[0] = ctx.handles.frameCB;
            draw.constantBuffers[1] = ctx.handles.objectCB;
            draw.textures[0] = material->textures[0];
            if (ctx.isDeferred) {
                const auto params = MakeHybridGBufferParams(material->gbufferParams, material->surface);
                resources.Update(shared.opaqueOccluderMaterialCB, params.data(), params.size());
                draw.constantBuffers[2] = shared.opaqueOccluderMaterialCB;
            } else {
                if (!resources.Get(material->shader) || !resources.Get(material->paramsBuffer)) continue;
                draw.constantBuffers[2] = material->paramsBuffer;
            }
            if (!IsViewOcclusionCurrent(ctx)) return false;
            SubmitCounted(ctx, draw);
            ctx.gpuOccluderItems[index] = true;
            submitted = true;
        }
    }
    return submitted;
}

GpuHiZ ReadViewGpuHiZ(const RenderPassContext& ctx)
{
    if (!ctx.gpuOcclusionEnabled || !ctx.gpuOccluderDepthReady || !IsViewOcclusionCurrent(ctx)) return {};
    return {ctx.Res().StructuredBuffer(OCCLUSION_HIZ), ctx.width, ctx.height, true};
}

void CullOccludedMeshes(RenderPassContext& ctx, const GpuHiZ& hierarchy)
{
    const auto scene = ctx.renderScene;
    if (!scene || !hierarchy.depths.IsValid()) return;
    constexpr uint32_t GROUP_SIZE = 64;
    constexpr uint32_t MAX_CANDIDATES = 65535u * GROUP_SIZE;
    if (scene->items.size() > MAX_CANDIDATES) return;
    std::vector<MeshOcclusionCandidate> candidates;
    ctx.gpuOcclusionArguments.assign(scene->items.size(), UINT32_MAX);
    ctx.gpuOcclusionCounted.assign(scene->Objects().size(), false);
    for (const auto& object : scene->Objects()) {
        if (!object.colorEligible || object.skinned || object.waterfallFarIndexStart
            || !MatchesView(ctx, object) || object.leafWindBoundsPadding < 0.0f) continue;
        if (object.firstItem > scene->items.size() || object.itemCount > scene->items.size() - object.firstItem) continue;
        const auto bounds = RenderBounds(ctx, object);
        if (bounds.radius <= 0.0f
            || EvaluateVisibility(CullingView(ctx), {bounds.center, bounds.radius, DrawDistance(ctx, object)})
                != VisibilityResult::VISIBLE) continue;
        for (uint32_t index = object.firstItem; index < object.firstItem + object.itemCount; ++index) {
            const auto& item = scene->items[index];
            if (!MeshOcclusionMaterial(ctx, item) || UsesClusteredDepth(ctx, object, item)) continue;
            ctx.gpuOcclusionArguments[index] = static_cast<uint32_t>(candidates.size());
            candidates.push_back({{bounds.center, bounds.radius}, item.indexCount});
        }
    }
    if (candidates.empty()) return;

    auto& resources = ctx.resources;
    auto& shared = resources.Rendering().Shared();
    if (!resources.Get(shared.meshOcclusionShader))
        shared.meshOcclusionShader = resources.LoadShader("Assets/Shaders/Pipeline/MeshOcclusionCull.cs.hlsl");
    if (!resources.Get(shared.meshOcclusionViewCB))
        shared.meshOcclusionViewCB = resources.CreateConstantBuffer(sizeof(MeshOcclusionView));
    if (!resources.Get(shared.meshOcclusionShader) || !resources.Get(shared.meshOcclusionViewCB)) return;
    static DynamicStructuredBufferPool candidatePool;
    static DynamicRWStructuredBufferPool argumentPool;
    const auto inputs = candidatePool.Acquire(resources, candidates.size(), sizeof(MeshOcclusionCandidate));
    const auto arguments = argumentPool.Acquire(resources, candidates.size(), OCCLUSION_DRAW_ARGUMENT_STRIDE);
    if (!resources.Get(inputs) || !resources.Get(arguments) || !IsViewOcclusionCurrent(ctx)) return;
    const auto frame = MakeCameraFrameCB(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    const MeshOcclusionView view{frame.viewProjection, ctx.width, ctx.height, static_cast<uint32_t>(candidates.size())};
    resources.Update(shared.meshOcclusionViewCB, &view, sizeof(view));
    resources.Update(inputs, candidates.data(), candidates.size() * sizeof(MeshOcclusionCandidate));
    ComputeCall compute;
    compute.shader = shared.meshOcclusionShader;
    compute.constantBuffers[0] = shared.meshOcclusionViewCB;
    compute.srvBuffers[14] = inputs;
    compute.srvBuffers[28] = hierarchy.depths;
    compute.uavBuffers[0] = arguments;
    compute.dispatchX = (view.candidateCount + GROUP_SIZE - 1) / GROUP_SIZE;
    if (IsViewOcclusionCurrent(ctx) && ctx.renderer.TryDispatch(compute, resources) && IsViewOcclusionCurrent(ctx))
        ctx.resourceRegistry.BindStructuredBuffer(OCCLUSION_DRAW_ARGUMENTS, arguments);
}

bool BindMeshOcclusionArguments(RenderPassContext& ctx, const RenderScene& scene, size_t index,
    ResourceHandle<StructuredBufferTag> arguments, DrawCall& draw)
{
    if (!arguments.IsValid() || ctx.renderScene.get() != &scene || index >= ctx.gpuOcclusionArguments.size()
        || ctx.gpuOcclusionArguments[index] == UINT32_MAX || !IsViewOcclusionCurrent(ctx)) return false;
    const auto& item = scene.items[index];
    draw.indirectArguments = arguments;
    draw.indirectArgumentOffset = uint64_t{ctx.gpuOcclusionArguments[index]} * OCCLUSION_DRAW_ARGUMENT_STRIDE;
    if (item.objectIndex < ctx.gpuOcclusionCounted.size() && !ctx.gpuOcclusionCounted[item.objectIndex]) {
        ctx.gpuOcclusionCounted[item.objectIndex] = true;
        ++ctx.statsGpuVisibilityUnknownObjects;
    }
    return true;
}

void PrepareViewOcclusion(RenderPassContext& ctx, OcclusionPasses passes)
{
    ctx.occlusionGpuRows = {};
    ctx.occlusionPreparedScene.reset();
    ctx.occlusionPreparedCuller = nullptr;
    auto* const culler = ctx.occlusionCuller;
    if (!culler || !ctx.occlusionCullingEnabled) return;
    FBZZ_PROFILE_SCOPE("ViewOcclusion::Prepare");
    culler->Reset(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    const uint64_t cullerVersion = culler->GetResetVersion();
    /// @note Native reads may pump callbacks; a matching version on another culler does not prove this view.
    const auto sameCuller = [&] {
        return ctx.occlusionCuller == culler && culler->GetResetVersion() == cullerVersion;
    };
    ctx.occlusionPreparedCullerVersion = cullerVersion;
    const auto input = ctx.renderScene;
    if (!input || ctx.settings.IsWireframe()) return;
    ctx.occlusionPreparedScene = input;
    ctx.occlusionPreparedSerial = input->snapshotSerial;
    ctx.occlusionPreparedSceneGeneration = input->sceneGeneration;
    ctx.occlusionPreparedFrame = input->frameStamp;
    ctx.occlusionPreparedView = CaptureOcclusionView(ctx);
    ctx.occlusionPreparedPassOverrides = ctx.settings.passOverrides;
    ctx.occlusionPreparedResourceEpoch = ctx.resources.GetResetVersion();
    ctx.occlusionPreparedResourceFrame = ctx.resources.FrameStamp();
    auto& shared = ctx.resources.Rendering().Shared();
    if (!sameCuller()) return;
    const bool terrainEnabled = ctx.isDeferred ? passes.terrainGBuffer : passes.terrainForward;
    const auto terrainShader = ctx.isDeferred ? shared.terrainGBufferShader : shared.terrainShader;
    const bool terrainReady = terrainEnabled && ctx.resources.Get(terrainShader);
    if (!sameCuller()) return;
    if (terrainReady) {
        RasterTerrainOccluders(ctx);
        if (!sameCuller()) return;
    }
    FBZZ_PROFILE_SCOPE("ViewOcclusion::Meshes");
    for (const auto& object : input->Objects()) {
        if (!object.colorEligible || !MatchesView(ctx, object)) continue;
        if (object.firstItem > input->items.size() || object.itemCount > input->items.size() - object.firstItem) continue;
        for (uint32_t i = object.firstItem; i < object.firstItem + object.itemCount; ++i) {
            const auto& item = input->items[i];
            if (!HasColorGeometry(item) || item.indexCount == 0 || item.vertexCount == 0) continue;
            const auto route = ResolveGeometryRoute(item.material.capabilities, ctx.isDeferred);
            const bool gbuffer = route == GeometryRoute::GBuffer;
            if (route == GeometryRoute::ForwardTransparent || (gbuffer ? !passes.meshGBuffer : !passes.meshForward)) continue;
            const auto& material = gbuffer ? item.material : item.forwardMaterial;
            const float interior = OccluderRadius(ctx, object, item, material);
            if (interior <= 0) continue;
            const auto bounds = RenderBounds(ctx, object);
            if (EvaluateVisibility(CullingView(ctx), {bounds.center, bounds.radius, DrawDistance(ctx, object)})
                != VisibilityResult::VISIBLE) continue;
            /// @note Get は CPU 描画完了待機を伴うため、被覆を証明できない候補と画面外候補には呼ばない。
            const bool vertexReady = ctx.resources.Get(item.vertexBuffer) != nullptr;
            if (!sameCuller()) return;
            if (!vertexReady) continue;
            const bool indexReady = ctx.resources.Get(item.indexBuffer) != nullptr;
            if (!sameCuller()) return;
            if (!indexReady) continue;
            const auto shader = gbuffer ? ctx.handles.gbufferShader : material.shader;
            const bool shaderReady = ctx.resources.Get(shader) != nullptr;
            if (!sameCuller()) return;
            if (!shaderReady) continue;
            culler->TestAndRaster(bounds.center, bounds.radius, interior);
        }
    }
    if (ctx.occlusionPreparedScene == input && ctx.renderScene == input
        && sameCuller())
        ctx.occlusionPreparedCuller = culler;
}

renderer::ResourceHandle<renderer::PipelineStateTag> GetOrCreateMaterialPSO(
    renderer::ResourceManager& resources,
    renderer::BlendMode        blend,
    bool                       doubleSided,
    int32_t                    depthBias,
    float                      depthBiasSlope,
    bool                       wireframe)
{
    /// @note 両面描画はバックフェースカリングを無効化する。
    const renderer::RasterizerMode raster = wireframe
        ? (doubleSided ? renderer::RasterizerMode::WIREFRAME_NOCULL : renderer::RasterizerMode::WIREFRAME)
        : (doubleSided ? renderer::RasterizerMode::SOLID_NOCULL : renderer::RasterizerMode::SOLID);
    /// @note 半透明・加算は深度書き込みをオフにし、背後のオブジェクトが透けて見えるようにする。
    const renderer::DepthMode depth = (blend == renderer::BlendMode::OPAQUE_BLEND)
        ? renderer::DepthMode::DEPTH_ON
        : renderer::DepthMode::DEPTH_READ;

    /// @note ビットパッキングは enum 値追加時にサイレントなキー衝突が起きるため、構造体を直接比較する std::map を使う。
    struct DescLess {
        bool operator()(const renderer::PipelineStateDesc& a,
                        const renderer::PipelineStateDesc& b) const noexcept {
            if (a.rasterizer != b.rasterizer) return a.rasterizer < b.rasterizer;
            if (a.blend      != b.blend)      return a.blend      < b.blend;
            if (a.depth      != b.depth)      return a.depth      < b.depth;
            if (a.depthBias  != b.depthBias)  return a.depthBias  < b.depthBias;
            return a.depthBiasSlope < b.depthBiasSlope;
        }
    };
    static std::map<renderer::PipelineStateDesc,
                    renderer::ResourceHandle<renderer::PipelineStateTag>,
                    DescLess> s_cache;
    const renderer::PipelineStateDesc desc{ raster, blend, depth, depthBias, depthBiasSlope };
    auto it = s_cache.find(desc);
    if (it != s_cache.end()) {
        /// @note Reset や別 ResourceManager で同じ番号が再利用されても、失効・別設定の PSO を返さない。
        if (const auto* existing = resources.Get(it->second)) {
            const auto& cached = existing->GetDesc();
            if (!DescLess{}(cached, desc) && !DescLess{}(desc, cached)) return it->second;
        }
    }
    auto handle = resources.CreatePipelineState(desc);
    s_cache[desc] = handle;
    return handle;
}


ShadowConstantsCB MakeShadowConstants(const RenderPassContext& ctx)
{
    const auto& rs = ctx.settings;
    ShadowConstantsCB data{};

    /// @note 全カスケードが共有する 1 枚のアトラスなので、テクセルサイズはアトラス全体基準。
    /// @note カスケード内 UV → アトラス UV への写像は HLSL 側 (cascadeAtlasRect) が行う。
    const float texel = 1.0f / static_cast<float>((std::max)(rs.shadow.mapResolution, 1u));
    data.shadowMapTexelSize[0] = texel;
    data.shadowMapTexelSize[1] = texel;

    const int count = std::clamp(ctx.shadowCascadeCount, 1, renderer::kMaxShadowCascades);
    data.cascadeCount     = count;
    data.cascadeBlend     = std::clamp(rs.shadow.cascadeBlend, 0.0f, 0.5f);
    /// @note 可視化は分割している時だけ意味がある。1 分割で有効なままだと画面全体が
    /// @note カスケード 0 の色に染まるだけなので、ここで落とす。
    data.cascadeDebugView = (rs.shadow.debugVisualizeCascades && count > 1) ? 1 : 0;

    float bias[renderer::kMaxShadowCascades] = {};
    for (int i = 0; i < renderer::kMaxShadowCascades; ++i) {
        /// @note 未使用スロットは最遠カスケードで埋める。HLSL 側は cascadeCount までしか
        /// @note 見ないが、未初期化の行列が残ると RenderDoc 等で追うときに紛らわしい。
        const ShadowCascade& cascade = ctx.shadowCascades[(i < count) ? i : count - 1];
        data.cascadeViewProjection[i] = cascade.viewProjection;
        data.cascadeAtlasRect[i]      = cascade.atlasRect;
        bias[i]                       = cascade.biasNDC;
    }
    data.cascadeBias = { bias[0], bias[1], bias[2], bias[3] };
    data.cascadeSplitFar = {
        ctx.shadowCascades[0].splitFar,
        ctx.shadowCascades[(std::min)(1, count - 1)].splitFar,
        ctx.shadowCascades[(std::min)(2, count - 1)].splitFar,
        ctx.shadowCascades[(std::min)(3, count - 1)].splitFar
    };
    const math::Vector3 forward = ctx.camera.GetForward();
    data.shadowCameraPosition = { ctx.camera.m_position.x, ctx.camera.m_position.y, ctx.camera.m_position.z, 0.0f };
    data.shadowCameraForward = { forward.x, forward.y, forward.z, 0.0f };

    /// @note 単一のライト行列で足りるパス向け (= 最遠カスケード)。
    /// @note cascadeCount == 1 のときはカスケード 0 と同一なので、従来の単一シャドウマップ経路と一致する。
    data.lightViewProjection = ctx.lightVP;
    data.shadowBias          = ctx.shadowBiasNDC;
    data.shadowStrength      = ctx.shadowStrength;
    data.shadowPcfRadius     = rs.shadow.pcfRadius;

    data.cloudShadowStrength = ctx.cloudShadowStrength;
    data.cloudShadowCoverage = ctx.cloudShadowCoverage;
    data.cloudShadowScale    = ctx.cloudShadowScale;
    data.cloudShadowSpeed    = ctx.cloudShadowSpeed;
    data.cloudShadowTime     = ctx.cloudShadowTime;
    data.cloudShadowWindX    = ctx.cloudShadowWindX;
    data.cloudShadowWindZ    = ctx.cloudShadowWindZ;

    return data;
}

void UpdateShadowConstants(RenderPassContext& ctx)
{
    const auto data = MakeShadowConstants(ctx);
    ctx.resources.Update(ctx.handles.shadowCB, &data, sizeof(data));
}

PunctualShadowConstantsCB MakePunctualShadowConstants(const RenderPassContext& ctx)
{
    const auto& rs = ctx.settings;
    PunctualShadowConstantsCB data{};

    /// @note 全スロットが 1 枚のアトラスを共有するので、テクセルサイズはアトラス全体基準。
    /// @note タイル内 UV → アトラス UV への写像は HLSL 側 (punctualShadowRect) が行う。
    const float texel =
        1.0f / static_cast<float>((std::max)(ctx.punctualShadowResolution, 1u));
    data.punctualShadowTexel[0] = texel;
    data.punctualShadowTexel[1] = texel;
    data.punctualShadowPcf      = std::clamp(rs.shadow.punctualPcfRadius, 0, 3);

    /// @note 未使用スロットは 0 のまま残す。HLSL 側は punctualShadowCount までしか見ない。
    const int count = std::clamp(ctx.punctualShadowViewCount, 0, kMaxPunctualShadows);
    data.punctualShadowCount = count;
    for (int i = 0; i < count; ++i) {
        const PunctualShadowView& view = ctx.punctualShadowViews[i];
        data.punctualShadowVP[i]     = view.viewProjection;
        data.punctualShadowRect[i]   = view.atlasRect;
        data.punctualShadowParams[i] =
            { view.biasNDC, view.shadowStrength, view.penumbraTexels, 0.0f };
    }

    data.lightCookieTexel[0] = 1.0f / static_cast<float>(kLightCookieAtlasWidth);
    data.lightCookieTexel[1] = 1.0f / static_cast<float>(kLightCookieAtlasHeight);
    const int cookieCount = std::clamp(ctx.lightCookieViewCount, 0, kMaxLightCookies);
    data.lightCookieCount = cookieCount;
    for (int i = 0; i < cookieCount; ++i) {
        data.lightCookieVP[i]   = ctx.lightCookieViews[i].viewProjection;
        data.lightCookieRect[i] = ctx.lightCookieViews[i].atlasRect;
    }

    /// @note レガシー経路の「大きさを持つ光源」。b3 に型が無いので実体ごと載せる。
    /// @note レイアウトは PunctualShadowConstants.hlsli のコメントと FBZZ_PunctualAt が正本。
    const int shapedCount = std::clamp(ctx.legacyShapedLightCount, 0, kMaxLegacyShapedLights);
    data.legacyShapedLightCount = shapedCount;
    for (int i = 0; i < shapedCount; ++i) {
        const PunctualLightGPU& a = ctx.legacyShapedLights[i];
        math::Vector4* dst = &data.legacyShapedLight[i * kLegacyShapedLightStride];
        dst[0] = { a.position,  a.range };
        dst[1] = { a.color,     a.intensity };
        dst[2] = { a.direction, 0.0f };
        dst[3] = { a.tangent,   a.halfWidth };
        dst[4] = { a.bitangent, a.halfHeight };
        /// @note y は両面フラグ。PunctualLightGPU では outerCos の枠に載せてある。
        /// @note z は影のスロット番号。legacyPunctualSlots は b3 の 12 枠に紐付いた表なので、
        /// @note b3 に席の無い「大きさを持つ光源」はそこから引けない。空いている枠へ載せる。
        dst[5] = { static_cast<float>(a.type), a.outerCos,
                   static_cast<float>(a.shadowIndex), 0.0f };
    }

    /// @note レガシー経路のスロット番号と光源半径。「無し」は -1 (ctx 側の既定値がそう)。0 は「スロット 0」という有効な番号のため 0 埋めでは済ませられず、影を持たないライトが他のライトのシャドウマップを引いてしまう。
    for (int i = 0; i < kMaxLegacyPunctualLights; ++i) {
        data.legacyPunctualSlots[i] = {
            static_cast<float>(ctx.legacyShadowSlots[i]),
            static_cast<float>(ctx.legacyCookieSlots[i]),
            ctx.legacySourceRadius[i],
            0.0f
        };
    }

    return data;
}

void UpdatePunctualShadowConstants(RenderPassContext& ctx)
{
    if (!ctx.handles.punctualShadowCB.IsValid()) return;
    const auto data = MakePunctualShadowConstants(ctx);
    ctx.resources.Update(ctx.handles.punctualShadowCB, &data, sizeof(data));
}


math::Vector3 ComputeCameraFacingRibbonNormal(
    const math::Vector3& direction, const math::Vector3& cameraPos, const math::Vector3& point)
{
    /// @note 帯の面をカメラへ向けるには、幅方向を「進行方向 × 視線方向」に取る。
    math::Vector3 up = cameraPos - point;
    if (up.LengthSq() > math::EPSILON * math::EPSILON) up = up.Normalized();
    else up = math::Vector3::UP;
    /// @note 進行方向と視線がほぼ平行だと外積が退化して帯が消える。安定な軸へ逃がす。
    if (std::abs(math::Vector3::Dot(direction, up)) > 0.99f) up = math::Vector3::UP;
    if (std::abs(math::Vector3::Dot(direction, up)) > 0.99f) up = math::Vector3::RIGHT;

    const math::Vector3 normal = math::Vector3::Cross(direction, up);
    return normal.LengthSq() > math::EPSILON * math::EPSILON
        ? normal.Normalized() : math::Vector3::RIGHT;
}



}
