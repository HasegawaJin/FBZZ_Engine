/// @file    ViewPipeline.cpp
/// @brief   ビューの資源宣言と描画・ポスト処理パスの構成。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <Graphics/Pipeline/ViewPipeline.hpp>
#include <Graphics/Pipeline/GeometryPipeline.hpp>
#include <Graphics/Pipeline/GpuHiZ.hpp>
#include <Graphics/Pipeline/ViewPreparation.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Passes/Geometry/WaterRenderPass.hpp>
#include <Graphics/Passes/Geometry/MeshTrailRenderPass.hpp>
#include <Graphics/Passes/Geometry/TrailRenderPass.hpp>
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Renderer/ITexture.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include <Core/Logger.hpp>
#include "../Passes/PostProcess/UnderwaterPassHelpers.hpp"
#include "../Renderer/BufferStateReadScope.hpp"
#include "TaaDepthWitness.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <string_view>

namespace fbzz::renderer {
namespace {

void AppendTaaFloat(std::vector<uint64_t>& witness, float value)
{
    witness.push_back(std::bit_cast<uint32_t>(value));
}

template<typename Tag>
void AppendTaaHandle(std::vector<uint64_t>& witness, ResourceHandle<Tag> handle)
{
    witness.push_back((static_cast<uint64_t>(handle.gen) << 32u) | handle.id);
}

bool AppendTaaWorld(std::vector<uint64_t>& witness, const math::Matrix4& matrix)
{
    for (const auto& row : matrix.m) {
        for (const float value : row) {
            if (!std::isfinite(value)) return false;
            AppendTaaFloat(witness, value);
        }
    }
    return true;
}

bool SameTaaCamera(const math::Matrix4& previous, const math::Matrix4& current)
{
    for (uint32_t row = 0; row < 4u; ++row)
        for (uint32_t column = 0; column < 4u; ++column)
            if (previous.m[row][column] != current.m[row][column]) return false;
    return true;
}

/// @note 葉の風を使わない標準頂点だけを静止扱いし、alpha clip は全定数と実テクスチャ内容版まで比較する。
/// @see Assets/Shaders/Pipeline/Deferred/GBuffer.hlsl GBufferVS / PSMain; Assets/Shaders/Material/Surface/PBR.hlsl VSMain / PSMain.
bool AppendTaaDepthMaterial(std::vector<uint64_t>& witness, const RenderMaterial& material,
    ResourceManager& resources)
{
    const auto& contract = material.shaderCapabilities;
    const bool standardOpaque = contract.vertex == ShaderVertexContract::STANDARD_SURFACE
        && contract.HasOpaqueOutput();
    const bool standardClip = contract.vertex == ShaderVertexContract::STANDARD_SURFACE
        && contract.SupportsGBuffer() && material.capabilities.gbufferEquivalentShader;
    if (!material.valid || (!standardOpaque && !standardClip)) return false;
    float leafWindStrength;
    std::memcpy(&leafWindStrength, material.gbufferParams.data()+68, sizeof(leafWindStrength));
    /// @note 同じ材質値でも風は時刻で位置を変えるため、静止した深度履歴の証明には使えない。
    /// @see Assets/Shaders/Rendering/LeafWind.hlsli FbzzLeafWind sampleTime.
    if (!std::isfinite(leafWindStrength) || leafWindStrength!=0.0f) return false;
    AppendTaaHandle(witness, material.shader);
    witness.push_back(static_cast<uint64_t>(contract.vertex));
    witness.push_back(static_cast<uint64_t>(contract.surface));
    witness.push_back(static_cast<uint64_t>(contract.opacity));
    witness.push_back(static_cast<uint64_t>(contract.variants));
    witness.push_back(static_cast<uint64_t>(contract.preview));
    witness.push_back(static_cast<uint64_t>(material.capabilities.blend));
    witness.push_back(material.capabilities.gbufferEquivalentShader);
    witness.push_back(material.capabilities.advancedLobe);
    witness.push_back(material.doubleSided);
    witness.push_back(static_cast<uint32_t>(material.renderQueue));
    witness.push_back(static_cast<uint32_t>(material.depthBias));
    AppendTaaFloat(witness, material.depthBiasSlope);
    const auto parameters = std::bit_cast<std::array<uint64_t, 12>>(material.gbufferParams);
    witness.insert(witness.end(), parameters.begin(), parameters.end());
    const auto& surface = material.surface;
    witness.push_back(surface.standardSurfaceSupported);
    witness.push_back(static_cast<uint64_t>(surface.issue));
    AppendTaaFloat(witness, surface.baseColor.x);
    AppendTaaFloat(witness, surface.baseColor.y);
    AppendTaaFloat(witness, surface.baseColor.z);
    AppendTaaFloat(witness, surface.baseColor.w);
    AppendTaaFloat(witness, surface.alphaCutoff);
    for (const float value : surface.uvTiling) AppendTaaFloat(witness, value);
    for (const float value : surface.uvOffset) AppendTaaFloat(witness, value);
    witness.push_back(surface.textureMask);
    for (const auto handle : material.textures) {
        AppendTaaHandle(witness, handle);
        if (!handle.IsValid()) { witness.push_back(0u); continue; }
        const auto* texture = resources.Get(handle);
        if (!texture) return false;
        const auto version = texture->GetContentVersion();
        if (version == 0) return false;
        witness.push_back(version);
    }
    return true;
}

} /// @note namespace
namespace taa_depth_detail {
/// @note 不透明深度の入力だけを証明する。透明色の静止や新しい多層履歴の保証は含めない。
/// @see Docs/design/shader-capabilities.md
bool BuildDepthWitness(RenderPassContext& context, std::vector<uint64_t>& witness)
{
    witness.clear();
    const auto input = context.renderScene;
    if (!input || input->sceneGeneration == 0 || input->snapshotSerial == 0
        || !input->terrains.empty() || !input->water.empty() || !input->fibers.empty()
        || !input->instances.empty() || !input->customPost.empty() || !input->particles.empty()
        || !input->trails.empty() || !input->meshTrails.empty() || !input->decals.empty()) return false;
    auto& resources = context.resources;
    const auto owner = resources.GetInstanceId();
    const auto epoch = resources.GetResetVersion();
    const auto shaders = resources.GetShaderVersion();
    witness.push_back(owner);
    witness.push_back(epoch);
    witness.push_back(shaders);
    witness.push_back(input->sceneGeneration);
    witness.push_back(context.cullingMask);
    witness.push_back(context.width);
    witness.push_back(context.height);
    witness.push_back(static_cast<uint64_t>(context.settings.viewMode));
    witness.push_back(static_cast<uint64_t>(context.settings.geometryDrawMode));
    witness.push_back(context.settings.gpuInstancing);
    witness.push_back(context.frustumCullingEnabled);
    witness.push_back(context.occlusionCullingEnabled);
    witness.push_back(context.cullDistanceSpherical);
    witness.push_back(context.hasLayerCullDistances);
    AppendTaaFloat(witness, context.cullingBoundsPadding);
    AppendTaaFloat(witness, context.cullMaxDistance);
    AppendTaaFloat(witness, context.smallObjectScreenHeight);
    for (const float distance : context.cullLayerDistances) AppendTaaFloat(witness, distance);
    const auto objects = input->Objects();
    witness.push_back(objects.size());
    witness.push_back(input->items.size());
    for (const auto& object : objects) {
        if (object.skinned || object.firstItem > input->items.size()
            || object.itemCount > input->items.size() - object.firstItem) return false;
        witness.push_back(object.sourceIndex);
        witness.push_back(object.sourceGeneration);
        witness.push_back(object.firstItem);
        witness.push_back(object.itemCount);
        witness.push_back(object.layer);
        witness.push_back(object.lodVisible);
        witness.push_back(object.colorEligible);
        witness.push_back(object.castShadows);
        AppendTaaFloat(witness, object.lodDither);
        AppendTaaFloat(witness, object.boundsCenter.x);
        AppendTaaFloat(witness, object.boundsCenter.y);
        AppendTaaFloat(witness, object.boundsCenter.z);
        AppendTaaFloat(witness, object.boundsRadius);
        AppendTaaFloat(witness, object.leafWindBoundsPadding);
        if (!std::isfinite(object.leafWindBoundsPadding) || object.leafWindBoundsPadding!=0.0f) return false;
        AppendTaaFloat(witness, object.sortPosition.x);
        AppendTaaFloat(witness, object.sortPosition.y);
        AppendTaaFloat(witness, object.sortPosition.z);
        if (!AppendTaaWorld(witness, object.world)) return false;
    }
    buffer_state_detail::BufferStateReadScope readScope(resources, context.renderer);
    for (const auto& item : input->items) {
        if (item.objectIndex >= objects.size()) return false;
        witness.push_back(item.objectIndex);
        witness.push_back(item.materialSlot);
        witness.push_back(item.sourceSubmesh);
        witness.push_back(item.visible);
        witness.push_back(item.slotVisible);
        witness.push_back(static_cast<uint64_t>(item.material.capabilities.blend));
        const bool transparent = item.material.capabilities.blend != BlendMode::OPAQUE_BLEND;
        if (transparent) continue;
        if (item.deformedVertexBuffer.IsValid() || item.preparedSkinning) return false;
        AppendTaaHandle(witness, item.vertexBuffer);
        AppendTaaHandle(witness, item.indexBuffer);
        witness.push_back(item.vertexCount);
        witness.push_back(item.indexCount);
        witness.push_back(item.vertexStride);
        witness.push_back(item.vertexPositionOffset);
        witness.push_back(item.reliableOccluder);
        AppendTaaFloat(witness, item.boundsCenter.x);
        AppendTaaFloat(witness, item.boundsCenter.y);
        AppendTaaFloat(witness, item.boundsCenter.z);
        AppendTaaFloat(witness, item.boundsRadius);
        AppendTaaFloat(witness, item.occluderRadius);
        const std::array<ResourceHandle<BufferTag>, 2> handles{item.vertexBuffer, item.indexBuffer};
        std::array<BufferResourceState, 2> states{};
        for (const auto handle : handles) {
            BufferResourceMetadata metadata;
            if (!resources.GetBufferMetadata(handle, metadata) || metadata.gpuWritable) return false;
        }
        /// @note 一度の CPU drain 境界だけを再利用し、借用 IBuffer::Update を含む実版は各入力から読み直す。
        readScope.Read(handles, states);
        for (const auto& state : states) {
            if (!state.live || state.contentVersion == 0 || state.bindlessUavIndex != UINT32_MAX) return false;
            witness.push_back(state.sizeBytes);
            witness.push_back(state.stride);
            witness.push_back(state.contentVersion);
        }
        const bool deferred = context.settings.UsesGBuffer()
            && ResolveGeometryRoute(item.material.capabilities, true) == GeometryRoute::GBuffer;
        const auto& material = deferred ? item.material : item.forwardMaterial;
        if (!AppendTaaDepthMaterial(witness, material, resources)) return false;
    }
    return input == context.renderScene && owner == resources.GetInstanceId()
        && epoch == resources.GetResetVersion() && shaders == resources.GetShaderVersion();
}

} /// @note namespace taa_depth_detail
namespace {
bool PrepareTaaStaticView(RenderPassContext& context, RenderViewResources& view)
{
    if (!view.taaHistoryValid) {
        view.taaStaticViewPrevious.clear();
        view.taaStaticViewCurrent.clear();
    }
    if (!taa_depth_detail::BuildDepthWitness(context, view.taaStaticViewCurrent)) {
        view.taaStaticViewPrevious.clear();
        view.taaStaticViewCurrent.clear();
        return false;
    }
    const bool cameraUnchanged = view.advancedGraphicsSnapshotValid
        && SameTaaCamera(view.advancedGraphicsSnapshot.prevViewProjection, context.camera.GetGpuViewProjection());
    const bool staticView = cameraUnchanged && !view.taaStaticViewPrevious.empty()
        && view.taaStaticViewPrevious == view.taaStaticViewCurrent;
    view.taaStaticViewPrevious.swap(view.taaStaticViewCurrent);
    view.taaStaticViewCurrent.clear();
    return staticView;
}

} /// @note namespace

void BuildViewPipeline(RenderPipeline& pipeline, RenderPassContext& passCtx,
    RenderViewResources& viewTargets, RenderSharedResources& shared,
    const ViewPipelineOptions& options, const ViewPipelineExtensions& extensions)
{
    const profiler::ProfileScope buildPipelineScope(
        profiler::ProfilerMarker("RenderSystem::BuildPipeline", "Rendering"));
    pipeline.BeginBuild();
    passCtx.rayPathViewActive = false;
    passCtx.ssrPassActive = false;
    passCtx.velocityWritten = false;
    passCtx.underwaterLightPassActive = false;
    passCtx.hybridReflectionResolveActive = false;
    passCtx.hybridReflectionSourcePass = false;
    passCtx.hybridReflectionSsrPlanned = false;
    if (!options.renderPlan.IsValid()) {
        FBZZ_LOG_ERROR("ViewPipeline: 実行できない描画構成です");
        return;
    }
    auto& resources = passCtx.resources;
    auto& passHandles = passCtx.handles;
    const auto& rs = passCtx.settings;
    passCtx.waterMediumPassActive = false;
    passCtx.waterInterfacePassActive = false;
    passCtx.waterMediumQualityLimited = false;
    passHandles.waterMediumAtlasRT = &viewTargets.waterMediumAtlasRT;
    passHandles.waterInterfaceRT = &viewTargets.waterInterfaceRT;
    passHandles.waterMediumTable = {};
    passHandles.waterMediumBodyCount = 0;
    passHandles.waterMediumTileResolution = 0;
    passHandles.waterMediumBodies.clear();
    passHandles.waterMediumDisabledOwnerKeys.clear();
    passHandles.waterMediumQualityLimitedOwnerKeys.clear();
    if (!rs.IsTaaActive()) {
        viewTargets.taaHistoryValid = false;
        viewTargets.taaProviderHistory.valid = false;
        viewTargets.taaStaticViewPrevious.clear();
        viewTargets.taaStaticViewCurrent.clear();
    }
    const auto rayViewPreparationToken = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("RenderSystem::RayViewPreparation", "Rendering"));
    const bool rayDebugReady = PrepareRayDebugView(passCtx, viewTargets, shared);
    const bool rayReflectionReady = PrepareRayReflectionView(passCtx, viewTargets, shared,
        options.renderPlan.rasterPlan);
    const bool rayPathReady = PrepareRayPathView(passCtx, viewTargets, shared);
    const auto renderPlan = PrepareViewRenderPlan(resources, passCtx.renderer, rs, viewTargets, shared, passHandles,
        passCtx.experimentalRayTracingEnabled);
    profiler::Profiler::EndScope(rayViewPreparationToken);
    passCtx.rayReflectionPassActive = rayReflectionReady && renderPlan.reflection.enabled;
    /// @note RT の縮退理由は保持し、使える Deferred 表面の SSR / IBL まで旧 HDR 合成へ戻さない。
    passCtx.hybridReflectionResolveActive = passCtx.experimentalRayTracingEnabled
        && rs.modeRequest.mode == RenderMode::HYBRID
        && renderPlan.rasterPlan.UsesDeferredLighting() && !rs.IsUnlit() && !rs.IsWireframe()
        && !IsRayDebugView(rs.viewMode);
    if (!passCtx.rayReflectionPassActive) {
        viewTargets.rayReflection.reconstruction.historyValid = false;
        viewTargets.rayReflection.reconstruction.prepared = false;
        passCtx.rayReflectionReconstructionPrepared = false;
    }
    if (!renderPlan.IsValid()) return;
    if (rayPathReady && renderPlan.effectiveMode == RenderMode::PATH_TRACING) {
        BuildRayPathViewPipeline(pipeline, passCtx, viewTargets, shared, extensions);
        return;
    }
    viewTargets.rayPath.history.Reset();
    PrepareWaterMediumResources(passCtx, viewTargets);
    const auto passOverriddenOff = [&](std::string_view name) {
        return std::any_of(rs.passOverrides.begin(), rs.passOverrides.end(),
            [&](const RenderPassOverride& override) { return override.name == name && !override.enabled; });
    };
    if (passOverriddenOff("WaterMediumAtlas")) {
        for (const auto& body : passHandles.waterMediumBodies) {
            if (passCtx.renderScene && body.waterInputIndex < passCtx.renderScene->water.size()) {
                const uint64_t ownerKey = passCtx.renderScene->water[body.waterInputIndex].medium.stableOwnerKey;
                if (std::find(passHandles.waterMediumDisabledOwnerKeys.begin(),
                              passHandles.waterMediumDisabledOwnerKeys.end(), ownerKey)
                    == passHandles.waterMediumDisabledOwnerKeys.end())
                    passHandles.waterMediumDisabledOwnerKeys.push_back(ownerKey);
            }
        }
        passCtx.waterMediumPassActive = false;
        passCtx.waterInterfacePassActive = false;
        passHandles.waterMediumTable = {};
        passHandles.waterMediumBodyCount = 0;
        passHandles.waterMediumTileResolution = 0;
        passHandles.waterMediumBodies.clear();
    }
    if (passOverriddenOff("WaterInterface"))
        passCtx.waterInterfacePassActive = false;
    const auto outputRT = passCtx.outputRT;
    const uint32_t nativeW = passCtx.outputWidth;
    const uint32_t nativeH = passCtx.outputHeight;
    const bool needsUpscale = viewTargets.needsUpscale;
    const auto& opaquePlan = renderPlan.rasterPlan;
    const bool screenSpaceReady = opaquePlan.HasScreenSpaceInputs();
    const bool ssrOverriddenOff = std::any_of(rs.passOverrides.begin(), rs.passOverrides.end(),
        [](const RenderPassOverride& override) { return override.name == "SSR" && !override.enabled; });
    /// @note A persistent GBuffer is graph-readable after its producer is disabled, but cannot prove a new Hybrid surface or SSR receipt.
    const bool hybridGBufferOverriddenOff = passCtx.hybridReflectionResolveActive
        && std::any_of(rs.passOverrides.begin(), rs.passOverrides.end(),
            [](const RenderPassOverride& override) { return override.name == "DeferredGBuffer" && !override.enabled; });
    const bool ssrAvailable = rs.ssr.enabled && screenSpaceReady && !ssrOverriddenOff && !hybridGBufferOverriddenOff
        && resources.Get(passHandles.ssrShader) && resources.Get(viewTargets.ssrResult)
        && resources.Get(viewTargets.gbuffer);
    const bool ssrSourceOverriddenOff = std::any_of(rs.passOverrides.begin(), rs.passOverrides.end(),
        [](const RenderPassOverride& override) {
            return override.name == "ReflectionSourceLighting" && !override.enabled;
        });
    /// @note source を止めたフレームは未照明 HDR を SSR として採用せず、RT-only resolve に戻す。
    passCtx.hybridReflectionSsrPlanned = passCtx.hybridReflectionResolveActive
        && ssrAvailable && !ssrSourceOverriddenOff;
    const bool ssaoEnabled = passCtx.ssaoEnabled;
    const bool selectionOutlineEnabled = passCtx.selectionOutlineEnabled;
    const bool objectMaskEnabled = passCtx.objectMaskEnabled;
    const bool clusteredEnabled = renderPlan.clusteredLighting;
    const auto& customAfterOpaqueIndices = options.customAfterOpaqueIndices;
    const auto& customSceneHdrIndices = options.customSceneHdrIndices;
    const auto& customPostProcessIndices = options.customPostProcessIndices;
    const uint32_t punctualShadowRes = passCtx.punctualShadowResolution;
    auto& hdrRT                   = viewTargets.hdr;
    auto& ldrRT                   = viewTargets.ldr;
    auto& selectionMaskRT         = viewTargets.selectionMask;
    auto& outlineRT               = viewTargets.outline;
    auto& objectMaskRT           = viewTargets.objectMask;
    auto& customPostProcessRT     = viewTargets.customPostProcess;
    auto& upscaleSrcRT            = viewTargets.upscaleSrc;
    auto& gbufferRT               = viewTargets.gbuffer;
    auto& velocityRT              = viewTargets.velocity;
    auto& decalDepthRT            = viewTargets.decalDepth;
    auto& bloomHalf               = viewTargets.bloomHalf;
    auto& bloomFull               = viewTargets.bloomFull;
    auto& ssaoBlur                = viewTargets.ssaoBlur;
    auto& ssrResult               = viewTargets.ssrResult;
    auto& volumetricResult        = viewTargets.volumetricResult;
    auto& motionBlurResult        = viewTargets.motionBlurResult;
    auto& gtaoBlur                = viewTargets.gtaoBlur;
    auto& contactShadowResult     = viewTargets.contactShadowResult;
    uint32_t& sHdrW               = viewTargets.width;
    uint32_t& sHdrH               = viewTargets.height;
    const auto shadowMapRT = shared.shadowMapRT.Handle();
    auto& punctualShadowRT = shared.punctualShadowRT;
    auto& lightCookieRT = shared.lightCookieRT;
    /// @note エディターが編集した «このパスは載せない / これを待つ» をこのフレームへ効かせる。
    pipeline.SetPassOverrides(rs.passOverrides);
    pipeline.SetSchedulePolicy(rs.schedulePolicy);
    /// @note 申告を条件で組み立てるパス用。initializer_list には if を書けないので、
    /// @note 読むものが構成で変わるパスは vector を渡す。
    using RA = renderer::RenderGraph::ResourceAccess;
    using RU = renderer::RenderGraph::ResourceUsage;
    /// @note 論理リソースの宣言
    /// @note 申告 (依存解析に使う «形») と実体 (名前 → ハンドル) を同じ 1 行で渡す。宣言と登録を
    /// @note 分けると、片方だけ足しても Plan が通り «申告したのに実体が無い» が静かに成立してしまう。
    /// @note 登録簿は RenderPipeline がここから組み立てる。
    using RK = renderer::RenderGraph::ResourceKind;
    constexpr auto kResFormat = renderer::Format::RGBA16F;

    /// @note 内部解像度・フレーム内だけ生きる RT。違うのは MRT 枚数と深度の有無だけ。
    const auto declareViewTarget = [&](const char* name,
                                       renderer::ResourceHandle<renderer::RenderTargetTag> handle,
                                       uint32_t colorCount, bool withDepth) {
        pipeline.DeclareTarget(name, handle,
            { RK::RenderTarget, sHdrW, sHdrH, kResFormat, colorCount, withDepth, false, true });
    };
    /// @note CS 出力のテクスチャ。解像度以外の «形» は全部同じ。
    const auto declareViewTexture = [&](const char* name,
                                        renderer::ResourceHandle<renderer::TextureTag> handle,
                                        uint32_t width, uint32_t height) {
        pipeline.DeclareTexture(name, handle,
            { RK::Texture, width, height, kResFormat, 1, false, false, true });
    };

    /// @note Output だけは出力先そのものなので実寸で申告する (中間 RT は内部解像度)。
    pipeline.DeclareTarget("Output", outputRT,
        { RK::RenderTarget, nativeW, nativeH, kResFormat, 1, true, true, false });
    pipeline.DeclareTarget("ShadowMap", shadowMapRT,
        { RK::RenderTarget, rs.shadow.mapResolution, rs.shadow.mapResolution, kResFormat, 0, true, false, false });
    if (passCtx.staticShadowCache.atlas.IsValid())
        pipeline.DeclareTarget("StaticShadowMap", passCtx.staticShadowCache.atlas,
            { RK::RenderTarget, rs.shadow.mapResolution, rs.shadow.mapResolution, kResFormat, 0, true, false, false });
    pipeline.DeclareTarget("PunctualShadowMap", punctualShadowRT,
        { RK::RenderTarget, punctualShadowRes, punctualShadowRes, kResFormat, 0, true, false, false });
    pipeline.DeclareTarget("LightCookieAtlas", lightCookieRT,
        { RK::RenderTarget, kLightCookieAtlasWidth, kLightCookieAtlasHeight, kResFormat, 1, true, false, false });

    declareViewTarget("HDR",                hdrRT,                  1, true);
    if (SupportsViewGpuOcclusion(passCtx) && !passOverriddenOff(DEPTH_PREPASS)
        && !passOverriddenOff(BUILD_OCCLUSION_HIZ) && !passOverriddenOff(CULL_OCCLUDED_MESHES)) {
        viewTargets.occluderDepth.Ensure(resources, sHdrW, sHdrH, CameraDepthTargetDesc(0));
        if (viewTargets.occluderDepth.IsValid()) {
            declareViewTarget(OCCLUDER_DEPTH.data(), viewTargets.occluderDepth.Handle(), 0, true);
            RenderGraph::ResourceDesc buffer;
            buffer.allowAliasing = false;
            buffer.stride = sizeof(float);
            pipeline.DeclareStructuredBuffer(OCCLUSION_HIZ, {}, buffer);
            buffer.stride = OCCLUSION_DRAW_ARGUMENT_STRIDE;
            pipeline.DeclareStructuredBuffer(OCCLUSION_DRAW_ARGUMENTS, {}, buffer);
        }
    }
    declareViewTarget("LDR",                ldrRT,                  1, false);
    declareViewTarget("SelectionMask",      selectionMaskRT,        1, true);
    declareViewTarget("Outline",            outlineRT,              1, false);
    declareViewTarget("ObjectMask",         objectMaskRT,           1, true);
    declareViewTarget("Velocity",           velocityRT,             1, true);
    declareViewTarget("CustomPostProcess0", customPostProcessRT[0], 1, false);
    declareViewTarget("CustomPostProcess1", customPostProcessRT[1], 1, false);

    const bool taaRawHistoryReady = rs.IsTaaActive() && resources.Get(passHandles.taaCycleCB)
        && std::all_of(passHandles.taaRawHistory, passHandles.taaRawHistory + 8,
            [&](const auto history) { return resources.Get(history) != nullptr; });
    if (taaRawHistoryReady) {
        /// @note 各位相は前フレーム以前の外部履歴。未初期化の画素は TAA の有効長で拒否し、transient として貸し回さない。
        for (uint32_t phase = 0; phase < 8u; ++phase)
            pipeline.DeclareTarget("TaaRawHistory" + std::to_string(phase), passHandles.taaRawHistory[phase],
                { RK::RenderTarget, sHdrW, sHdrH, kResFormat, 1, false, true, false });
    }

    /// @note 実体は upscaleSrcRT。等倍のフレームは誰も触らないので申告もしない。
    const bool upscaleActive = needsUpscale && upscaleSrcRT.IsValid();
    if (upscaleActive)
        declareViewTarget("UpscaleSrc", upscaleSrcRT, 1, false);

    declareViewTexture("Bloom",            bloomFull,        sHdrW, sHdrH);
    declareViewTexture("SSRResult",        ssrResult,        sHdrW, sHdrH);
    declareViewTexture("MotionBlurResult", motionBlurResult, sHdrW, sHdrH);
    declareViewTexture("VolumetricResult", volumetricResult, sHdrW, sHdrH);
    declareViewTexture("VolumetricRaw", viewTargets.volumetricRaw, (sHdrW + 1u) / 2u, (sHdrH + 1u) / 2u);

    if (passCtx.waterMediumPassActive && viewTargets.waterMediumAtlasRT.IsValid()) {
        pipeline.DeclareTarget("WaterMediumAtlas", viewTargets.waterMediumAtlasRT,
            { RK::RenderTarget, viewTargets.waterMediumAtlasRT.Width(), viewTargets.waterMediumAtlasRT.Height(),
              renderer::Format::RGBA32F, 1, true, false, true });
    }
    if (passCtx.waterInterfacePassActive && viewTargets.waterInterfaceRT.IsValid()) {
        pipeline.DeclareTarget("WaterInterface", viewTargets.waterInterfaceRT,
            { RK::RenderTarget, sHdrW, sHdrH, renderer::Format::RGBA32F, 1, true, false, true });
    }

    /// @note Forward もプリパスで GBuffer へ描くので、ここを Deferred 限定にすると
    /// @note 「宣言されていないリソース」への書き込みになり RenderGraph の検証が落ちる。
    if (screenSpaceReady)
        pipeline.DeclareTarget("GBuffer", gbufferRT,
            { RK::RenderTarget, sHdrW, sHdrH, kResFormat, GBUFFER_COLOR_COUNT, true, false, false });

    /// @note AO と接触影は半解像度で持つ (実体は curW/2 x curH/2)。ここをフル解像度で
    /// @note 申告していると、エイリアシングが全画面 RT と同じ枠を貸してしまう。
    const uint32_t halfW = (std::max)(1u, sHdrW / 2);
    const uint32_t halfH = (std::max)(1u, sHdrH / 2);
    declareViewTexture("LensFlareSource", bloomHalf, halfW, halfH);
    if (ssaoEnabled)
        declareViewTexture("SSAO", ssaoBlur, halfW, halfH);
    /// @note GTAO / ContactShadows は GBuffer を読んで独自の UAV へ書く。専用名で宣言しないと
    /// @note GBuffer への偽書き込みとみなされ、DeferredLighting との依存順が崩れる。
    if (screenSpaceReady && rs.IsGtaoActive())
        declareViewTexture("GTAOResult", gtaoBlur, halfW, halfH);
    if (screenSpaceReady && rs.contactShadow.enabled)
        declareViewTexture("ContactShadowResult", contactShadowResult, halfW, halfH);
    pipeline.SetOutputs({ "Output" });

    /// @note IBL BRDF LUT 焼き付け。512x512 の積分テーブルはシーンにも設定にも依存しない定数なので、
    /// @note 毎フレーム呼ぶが実処理は世代追跡で初回のみ走る。
    pipeline.AddPass<IBLBrdfBakePass>();

    if (extensions.begin) extensions.begin();

    /// @note HDR の段 (AfterOpaque / SceneHDR) のカスタムパスを積む。どちらも hdrRT を
    /// @note 読んで書くので、違うのは «パイプラインのどこへ挿すか» だけ。
    /// @note マスクを reads に入れないと、グラフは «マスクを描く前に» このパスを走らせてよいことになる。
    /// @note 読む宣言をしていない効果でも同じ段に読む効果が混ざれば順序は共有されるので、
    /// @note 有無で分けずまとめて宣言する。
    auto appendCustomHdrPasses = [&](const char* label, const std::vector<uint32_t>& indices) {
        for (uint32_t i = 0; i < static_cast<uint32_t>(indices.size()); ++i) {
            const uint32_t customIndex = indices[i];
            const auto body = [&, customIndex]() { ExecuteCustomHdrPass(passCtx, customIndex); };
            const std::string name = label + std::to_string(i);
            if (objectMaskEnabled)
                pipeline.AddRawPass(name, { "HDR", "ObjectMask" }, { "HDR" }, body);
            else
                pipeline.AddRawPass(name, { "HDR" }, { "HDR" }, body);
        }
    };

    BuildGeometryPreparation(pipeline, clusteredEnabled);
    BuildGeometryPipeline(pipeline, opaquePlan, [&]() {
        if (passCtx.rayReflectionPassActive)
            BuildRayReflectionPipeline(pipeline, viewTargets, shared, passCtx);
    }, passCtx.hybridReflectionSsrPlanned);

    if (extensions.setup) extensions.setup();

    if (extensions.userPasses) extensions.userPasses(UserRenderPassInjectionPoint::AfterOpaque);

    /// @note オブジェクトマスク。描くのはジオメトリなので不透明が出揃ったここで済ませる。
    /// @note 半透明より前でよい: マスクの中身は «不透明の形と、その時点の深度» で決まり、
    /// @note 深度を書かない半透明を待っても結果は変わらない。ここより後ろへ置くと、
    /// @note AfterOpaque 段のカスタムパスがマスクを読めなくなる (順序が閉じない)。
    pipeline.AddPass<ObjectMaskPass>();

    /// @note AfterOpaque 段のユーザーシェーダー
    /// @note 背景だけが描かれていて、デカール・トレイル・パーティクル・半透明はまだ乗っていない。
    /// @note 画面を歪める効果をこの後 (SceneHDR) に置くと、既に描かれたパーティクルごと曲がって
    /// @note «エフェクトだけ別の場所に居る» 絵になる。
    appendCustomHdrPasses("CustomAfterOpaque", customAfterOpaqueIndices);

    BuildGeometryTransparency(pipeline, opaquePlan);
    BuildWaterComposition(pipeline);

    /// @note デカール用深度スナップショット
    /// @note 深度専用 RT (colorCount = 0)。カラーを持つ RT と貸し回してはいけない。
    declareViewTarget("DecalDepth", decalDepthRT, 0, true);
    pipeline.AddPass<DecalDepthCopyPass>();

    /// @note Decal + Trail + Particle
    pipeline.AddPass<DecalPass>();

    pipeline.AddPass<MeshTrailRenderPass>();
    pipeline.AddPass<TrailRenderPass>();

    /// @note ShadowMap は粒子の自己影が読む (t8)。申告していないと影より前に走れてしまう。
    /// @note 粒子より前に登録するのは、粒子が同じフレームの霧を読んで «自分の奥行きの霧» を逆算する
    /// @note (ParticleLighting.hlsli の ApplyParticleFog) ため。依存を申告しあわない 2 パスは登録順に並ぶ。
    pipeline.AddPass<FroxelFogPass>();

    /// @note PunctualShadowMap / LightCookieAtlas は «点光源を受ける» .mat の粒子が読む (ParticleLighting.hlsli)。
    pipeline.AddPass<ParticlePass>();

    /// @note Overdraw 可視化は診断表示。有効なときだけ Particle の直後に HDR を上書きする。
    /// @note GPU 時間を Particle パスの実測値と混ぜないよう、別パスとして計測させる。
    pipeline.AddPass<ParticleOverdrawPass>();

    /// @note TAA の反応マスク。HDR へは書かないが、Particle の後・Composite (→ TAA) の前に並べるために
    /// @note HDR の書き手として申告する (Overdraw と同じ申告の仕方)。
    pipeline.AddPass<ParticleReactivePass>();

    if (extensions.userPasses) extensions.userPasses(UserRenderPassInjectionPoint::AfterTransparent);

    /// @note Selection / Debug
    if (extensions.selectionMask) extensions.selectionMask();

    /// @note SceneHDR 段のユーザーシェーダー
    /// @note 絵が出揃っていて、まだブルームにも露出にも触れていない唯一の場所。
    /// @note Bloom より後ろへ置くと «光っているのに滲まない»、AutoExposure より後ろへ
    /// @note 置くと «明るくしたのに露出が反応しない» という、段を選べる意味が消える並びになる。
    /// @note デバッグ描画より前なのは、ギズモを効果で歪ませないため。
    appendCustomHdrPasses("CustomSceneHDR", customSceneHdrIndices);

    /// @note 自動露出はデバッグ描画より前。測るのは «シーンの明るさ» で、グリッド・ギズモ・
    /// @note コライダー・NavMesh は Scene View にしか無い。後ろへ置くと Scene View だけ露出が
    /// @note Game View と食い違う (RenderGraph は登録順より前の書き手を読み手の世代とする)。
    /// @note MotionBlur / LensFlare より前なのは、フレアを測光へ入れると «明るい→露出が下がる→
    /// @note フレアが弱る» の輪ができるため。Bloom は HDR を書かないので位置に関わらず同じ。
    /// @note 出力は StructuredBuffer (Composite が t29 で読む) で論理リソースに乗らないため、
    /// @note FroxelFog と同じ理由でカリング対象から外す。
    pipeline.AddPass<AutoExposurePass>();

    if (extensions.depthDebug) extensions.depthDebug();

    if (extensions.userPasses) extensions.userPasses(UserRenderPassInjectionPoint::BeforePostProcess);

    /// @note モーションベクター
    /// @note TAA とモーションブラーは深度再投影だけでは「カメラの動き」しか復元できない。
    /// @note 不透明ジオメトリの実際の移動量を専用 RT へ描いて両者へ供給する。
    /// @note 消費側が 1 つも無いフレームは丸ごと省く (不透明をもう一度ラスタライズするため)。
    const bool velocityNeeded =
        velocityRT.IsValid() && rs.IsPassEnabled("Velocity")
        && (rs.motionBlur.enabled || rs.IsTaaActive());
    if (velocityNeeded) {
        pipeline.AddPass<VelocityPass>();
    }

    /// @note PostProcess チェーン
    /// @note MotionBlur CS — HDR 空間で計算し motionBlurResult へ書く (Composite が hdrRT の代わりに読む)。
    /// @note Bloom の前に走らせるので blur 後の輝度が Bloom に乗る。
    if (rs.motionBlur.enabled) {
        /// @note Velocity は誰も書かないフレームがある。書かれないものを読むと申告した瞬間に
        /// @note «producer が居ない» で Plan が落ちるので、要るときだけ足す。
        std::vector<RA> motionBlurAccesses = { { "MotionBlurResult", RU::Write }, { "HDR", RU::ReadWrite } };
        if (velocityNeeded) motionBlurAccesses.push_back({ "Velocity", RU::Read });
        pipeline.AddRawPass("MotionBlur", std::move(motionBlurAccesses), [&]() {
            ExecuteMotionBlurPass(passCtx);
        });
    }
    /// @note LensFlare PS — 輝度抽出した光源を ADDITIVE で HDR へ合成する。
    /// @note Bloom の前に置くのでフレアも Bloom に乗るが、その順序では bloomHalf に今フレームの
    /// @note 輝点がまだ無い。パス自身が bloomHalf へ焼いてから読む (LensFlareSource がこの出力)。
    pipeline.AddPass<LensFlarePass>();
    pipeline.AddPass<BloomPass>();

    /// @note フロクセル霧。シャドウマップを読むので Shadow より後、Composite より前。
    /// @note 霧はライトとシャドウだけから作るので HDR の完成を待つ必要はない。
    /// @note 無効でも積むのは、パス側が b13 へ「無効」を書き戻さないと前フレームの定数が残り
    /// @note 画面が真っ黒になるため (FroxelFogPass 参照)。
    /// @note 出力先の 3D ボリュームは論理リソースに乗らないので、writes が空でもカリングさせない。

    const bool customPostProcessEnabled =
        !customPostProcessIndices.empty() &&
        customPostProcessRT[0].IsValid() &&
        customPostProcessRT[1].IsValid();
    /// @note hasPostCompositeEffects: Composite の出力先が "LDR" かチェーン終端かを決める。
    /// @note このフラグが true なら Composite は ldrRT に書き、後続エフェクトがチェーンを形成する。
    const bool hasPostCompositeEffects =
        rs.IsTaaActive() || customPostProcessEnabled || selectionOutlineEnabled || rs.postProcess.fxaaEnabled;

    /// @note ここが «Composite がどこへ書くか» の唯一の正本。グラフへの申告 (下の writes) と
    /// @note パスが実際に束縛するハンドルを、同じ 1 つの判定から配る。
    passCtx.compositeOutputRT = hasPostCompositeEffects ? ldrRT : passCtx.chainOutputRT;

    /// @note LDR チェーンの終端リソース。passCtx.chainOutputRT の «グラフ側の名前» で、
    /// @note 実寸へ引き伸ばすのは UpscalePass だけという対応を保つ。
    const char* const chainOutRes = upscaleActive ? "UpscaleSrc" : "Output";

    {
        /// @note Bloom を読むのは bloom.enabled のときだけ (CompositePass の bloomWritten と同条件)。
        std::vector<RA> compositeAccesses = {
            { "HDR", RU::Read },
            { hasPostCompositeEffects ? "LDR" : chainOutRes, RU::Write },
        };
        if (rs.postProcess.bloom.enabled) compositeAccesses.push_back({ "Bloom", RU::Read });
        const auto& underwater = passCtx.environment.underwater;
        const bool sceneHasOpticalWater = !passCtx.rayPathViewActive && passCtx.renderScene
            && HasOpticalWaterMedium(*passCtx.renderScene);
        const bool sharedWaterOptics = sceneHasOpticalWater && passCtx.waterMediumPassActive
            && passCtx.waterInterfacePassActive
            && viewTargets.waterMediumAtlasRT.IsValid()
            && viewTargets.waterInterfaceRT.IsValid();
        if (sharedWaterOptics) {
            compositeAccesses.push_back({ "WaterMediumAtlas", RU::Read });
            compositeAccesses.push_back({ "WaterInterface", RU::Read });
        }
        const bool sharedVolumeNeeded = sharedWaterOptics
            && HasOpticalWaterMedium(*passCtx.renderScene, true) && rs.IsVolumetricLightActive()
            && viewTargets.volumetricResult.IsValid();
        const bool legacyVolumeNeeded = !sceneHasOpticalWater && UsesOpticalUnderwater(underwater)
            && underwater.lightStrength > 0.0f && rs.IsVolumetricLightActive()
            && viewTargets.volumetricResult.IsValid();
        /// @note 光を切った構成では一時結果の書き手が存在せず、未生成のread宣言がグラフ全体を無効にする。
        if (sharedVolumeNeeded || legacyVolumeNeeded)
            compositeAccesses.push_back({ "VolumetricResult", RU::Read });
        if (passCtx.rayReflectionPassActive) compositeAccesses.push_back({ "RayReflectionResult", RU::Read });
        if (passCtx.rayReflectionReconstructionPrepared) compositeAccesses.push_back({ "RayReflectionRaw", RU::Read });
        pipeline.AddRawPass("Composite", std::move(compositeAccesses), [&]() {
            ExecuteCompositePass(passCtx);
        });
    }

    /// @note Post-composite チェーン
    /// @note ppCurrent は「LDR 空間の最新フレームを持つリソース名」。これを進めるだけで
    /// @note TAA/CustomPP/SelectionOutline/FXAA の任意の組み合わせが 1 本の直列チェーンになる。
    std::string ppCurrent  = hasPostCompositeEffects ? "LDR" : chainOutRes;
    /// @note customPostProcessRT の ping-pong インデックス
    int         ppPingPong = 0;

    /// @note TAA — 最初に適用することで後続の CustomPP/SelectionOutline が TAA 済み映像に乗る。
    /// @note 登録順が RenderGraph のタイブレークになる (Kahn's algorithm)。
    if (rs.IsTaaActive()) {
        const auto taaBody = [&]() {
            PrepareTaaProviderHistory(passCtx, viewTargets);
            const bool staticView = PrepareTaaStaticView(passCtx, viewTargets);
            if (!ExecuteTAAPass(passCtx, staticView)) {
                viewTargets.taaHistoryValid = false;
                viewTargets.taaStaticViewPrevious.clear();
                viewTargets.taaStaticViewCurrent.clear();
                return;
            }
            viewTargets.taaHistoryValid = true;
            /// @note taaFlip は ExecuteTAAPass 内で反転済み — 反転後のフラグで「書いた方」を特定する。
            auto& taaOut = passHandles.taaFlip ? passHandles.taaHistoryB : passHandles.taaHistoryA;
            passHandles.fxaaInput = resources.GetColorTexture(taaOut, 0);
            /// @note TAA 後は履歴バッファが最新フレーム。更新しないと後続が TAA 前の ldrRT を読む。
            passHandles.postProcessInput = passHandles.fxaaInput;
            /// @note LDR の論理的な最新世代は履歴 RT に移る。診断も後続パスも同じ実体を引く。
            passCtx.resourceRegistry.BindTarget("LDR", taaOut);
        };
        /// @note MotionBlur と同じ理由で Velocity は要るときだけ足す。
        /// @note HDR は深度 (t7) を読むための申告。速度の無い画素の再投影に使う。
        std::vector<RA> taaAccesses = { { ppCurrent, RU::ReadWrite }, { "HDR", RU::Read } };
        if (rs.UsesGBuffer() && !rs.IsUnlit() && !rs.IsWireframe())
            taaAccesses.push_back({ "GBuffer", RU::Read });
        if (velocityNeeded) taaAccesses.push_back({ "Velocity", RU::Read });
        if (taaRawHistoryReady) {
            const auto writePhase = static_cast<uint32_t>(passCtx.frameStamp % 8u);
            for (uint32_t phase = 0; phase < 8u; ++phase)
                taaAccesses.push_back({ "TaaRawHistory" + std::to_string(phase),
                    phase == writePhase ? RU::Write : RU::Read });
        }
        pipeline.AddRawPass("TAA", std::move(taaAccesses), taaBody);
    }

    /// @note Custom PostProcess チェーン
    for (uint32_t i = 0; i < static_cast<uint32_t>(customPostProcessIndices.size()); ++i) {
        const uint32_t customIndex  = customPostProcessIndices[i];
        const bool     isLastEffect = (i + 1 == static_cast<uint32_t>(customPostProcessIndices.size()))
                                       && !selectionOutlineEnabled
                                       && !rs.postProcess.fxaaEnabled;
        /// @note outputIndex == 2 → ExecuteCustomPostProcessPass が ctx.outputRT に直書きする規約
        const uint32_t    outputIndex = isLastEffect ? 2u : static_cast<uint32_t>(ppPingPong % 2);
        const std::string outRes      = isLastEffect
            ? chainOutRes
            : ("CustomPostProcess" + std::to_string(outputIndex));
        const auto customBody = [&, customIndex, outputIndex]() {
            ExecuteCustomPostProcessPass(passCtx, customIndex, outputIndex);
        };
        /// @note 輪郭マスクはユーザーシェーダーの入力になりうる。読むと宣言しないと、
        /// @note グラフはマスクを描く前にこのパスを走らせてよいことになる (reads が
        /// @note initializer_list なので MotionBlur と同じく 2 通りに分ける)。
        if (objectMaskEnabled) {
            pipeline.AddRawPass(
                "CustomPostProcess" + std::to_string(i),
                { ppCurrent, "ObjectMask" }, { outRes }, customBody);
        } else {
            pipeline.AddRawPass(
                "CustomPostProcess" + std::to_string(i),
                { ppCurrent }, { outRes }, customBody);
        }
        ppCurrent = outRes;
        ++ppPingPong;
    }

    /// @note SelectionOutline
    if (selectionOutlineEnabled) {
        const bool        isLastEffect = !rs.postProcess.fxaaEnabled;
        const std::string outRes       = isLastEffect ? chainOutRes : "Outline";
        /// @note HDR は輪郭の深度比較が読む (t7)。
        pipeline.AddRawPass("SelectionOutline",
            { ppCurrent, "SelectionMask", "HDR" },
            { outRes },
            [execute = extensions.selectionOutline](PassResources& res) { if (execute) execute(res); });
        ppCurrent = outRes;
    }

    /// @note FXAA
    if (rs.postProcess.fxaaEnabled) {
        pipeline.AddRawPass("FXAA", { ppCurrent }, { chainOutRes }, [&]() { ExecuteFxaaPass(passCtx); });
        ppCurrent = chainOutRes;
    }

    /// @note TAA_Blit — TAA は ping-pong 履歴にしか書かないので、後続エフェクトが 1 つも無いときは
    /// @note ppCurrent が "LDR" のまま残る。ここでチェーン終端へ届ける。
    if (ppCurrent != chainOutRes) {
        pipeline.AddRawPass("TAA_Blit", { ppCurrent }, { chainOutRes }, [&]() {
            ExecuteTAABlitPass(passCtx);
        });
        ppCurrent = chainOutRes;
    }

    /// @note デバッグ線 (深度なし) は LDR チェーンの終端へ、Upscale より前に内部解像度で重ねる。
    /// @note HDR に描くと MotionBlur / Bloom / LensFlare / 露出 / トーンマップ / TAA で線の色と形が変わる。
    /// @note 終端 RT はシーン深度を持たないので、深度テストが要る線は上の HDR 段に残してある。
    /// @note 同じ描き先を ReadWrite するので登録順がそのまま描画順。コライダーは破線で最後に描き、
    /// @note 同じ形のスクリプト Gizmo (実線) と重なっても両方読めるようにする。
    if (rayDebugReady) BuildRayDebugPipeline(pipeline, viewTargets, shared, resources, chainOutRes);
    if (extensions.overlayDebug) extensions.overlayDebug(chainOutRes);

    /// @note Upscale — 内部解像度で仕上がった絵を出力先の実寸へ解像する。
    /// @note UI より «前» に置くのが要点。後ろに回すと UI まで引き伸ばされて滲む。
    if (upscaleActive) {
        pipeline.AddRawPass("Upscale", { ppCurrent }, { "Output" }, [&]() {
            ExecuteUpscalePass(passCtx);
        });
    }

    if (extensions.ui) extensions.ui();
}
}
