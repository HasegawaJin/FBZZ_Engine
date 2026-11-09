/// @file    FiberRenderPass.cpp
/// @brief   共通 RHI で表面繊維を描き、Fin の GPU データをメッシュごとに共有する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <Graphics/Passes/Geometry/FiberRenderPass.hpp>
#include <Graphics/Effects/ComputedSkinningOutput.hpp>
#include "RenderScenePassHelpers.hpp"
#include <Graphics/Renderer/FiberGeometry.hpp>
#include <algorithm>
#include <cstring>
#include <map>
#include <limits>
namespace fbzz::renderer {
namespace {
struct FiberMotionCB {
    math::Matrix4 m_previousWorld;
    math::Matrix4 m_previousNormal;
    math::Vector4 m_previousWindTime;
    math::Vector4 m_previousShape;
    math::Vector4 m_previousGust;
    math::Vector4 m_motion;
};
static_assert(sizeof(FiberMotionCB) == 192);
enum class FiberPassMode { COLOR, GBUFFER, VELOCITY, SHADOW, SELECTION };
enum class FiberShape { SHELL, FIN, BLADE };

struct FiberShaderSlot {
    renderer::ResourceHandle<renderer::ShaderTag> m_handle;
    uint64_t m_attempt = (std::numeric_limits<uint64_t>::max)();
};
template<typename T>
struct FiberUpload {
    T m_value{};
    bool m_valid = false;
};
struct FiberResources {
    ResourceManager* owner = nullptr;
    uint64_t version = UINT64_MAX;
    uint64_t ownerIdentity = 0;
    ResourceHandle<ConstantBufferTag> m_material, m_frame, m_motion, m_contacts;
    ResourceHandle<PipelineStateTag> m_pipeline, m_deferredPipeline;
    std::array<ResourceHandle<BufferTag>, 6> m_bladeIndices;
    std::array<FiberShaderSlot, 30> m_shaders;
    FiberUpload<FiberMaterialSettings> m_uploadedMaterial;
    FiberUpload<FiberFrameCB> m_uploadedFrame;
    FiberUpload<FiberMotionCB> m_uploadedMotion;
    FiberUpload<FiberContactCB> m_uploadedContacts;
    ResourceHandle<ConstantBufferTag> m_appearanceMaterial;
    std::vector<uint8_t> m_appearanceBytes;
    std::map<std::array<uint64_t,3>, FiberMotionHistory> histories;
};
FiberResources& GetFiberResources(ResourceManager& resources) {
    static FiberResources cache;
    if (cache.owner != &resources || cache.ownerIdentity != resources.GetInstanceId()
        || cache.version != resources.GetResetVersion()) {
        if (auto* owner = ResourceManager::FindOwner(cache.ownerIdentity)) {
            for (auto handle : {cache.m_material, cache.m_frame, cache.m_motion, cache.m_contacts, cache.m_appearanceMaterial})
                owner->Release(handle);
            owner->Release(cache.m_pipeline);
            owner->Release(cache.m_deferredPipeline);
            for (auto handle : cache.m_bladeIndices) owner->Release(handle);
        }
        cache = {}; cache.owner = &resources; cache.ownerIdentity = resources.GetInstanceId();
        cache.version = resources.GetResetVersion();
    }
    if (!cache.m_material.IsValid()) {
        cache.m_material = resources.CreateConstantBuffer(sizeof(FiberMaterialSettings));
        cache.m_frame = resources.CreateConstantBuffer(sizeof(FiberFrameCB));
        cache.m_contacts = resources.CreateConstantBuffer(sizeof(FiberContactCB));
        cache.m_motion = resources.CreateConstantBuffer(sizeof(FiberMotionCB));
        cache.m_deferredPipeline = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_SKY });
        cache.m_pipeline = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
    }
    std::erase_if(cache.histories, [&](const auto& item) { return resources.FrameStamp() > item.second.m_frame && resources.FrameStamp() - item.second.m_frame > 120; });
    return cache;
}

ResourceHandle<BufferTag> GetFiberBladeIndexBuffer(FiberResources& cache, ResourceManager& resources, uint32_t segments)
{
    auto& buffer = cache.m_bladeIndices[segments - 1u];
    if (buffer.IsValid()) return buffer;

    /// @note 高さ段ごとに左右を並べ、ストリップと同じ対角線と表裏を使う。
    std::array<uint32_t, 36> indices{};
    for (uint32_t segment = 0; segment < segments; ++segment) {
        const uint32_t left = segment * 2u;
        const std::array corners{left, left + 1u, left + 2u, left + 2u, left + 1u, left + 3u};
        std::copy(corners.begin(), corners.end(), indices.begin() + segment * 6u);
    }
    buffer = resources.CreateIndexBuffer(indices.data(), segments * 6u);
    return buffer;
}

template<typename T>
void UploadIfChanged(renderer::ResourceManager& resources, renderer::ResourceHandle<renderer::ConstantBufferTag> handle,
                     FiberUpload<T>& last, const T& value)
{
    if (last.m_valid && std::memcmp(&last.m_value, &value, sizeof(T)) == 0) return;
    resources.Update(handle, &value, sizeof(T));
    last.m_value = value;
    last.m_valid = true;
}

/// @note 失敗したパスは 1 フレーム 1 回だけ再試行する。ファイル修正後の復帰とログの連打防止を両立する。
renderer::ResourceHandle<renderer::ShaderTag> GetFiberShader(FiberResources& cache, renderer::ResourceManager& resources,
                                                             FiberPassMode mode, FiberShape shape, bool skinned,
                                                             const FiberAppearance* appearance = nullptr)
{
    if (appearance) {
        const auto& family = shape == FiberShape::FIN && appearance->geometry == ShaderVertexContract::FIBER_SHELL
            && appearance->supportsHybrid ? appearance->finShaders : appearance->shaders;
        const auto handle = family[static_cast<size_t>(mode) * 2 + (skinned ? 1 : 0)];
        if (handle.IsValid() && resources.Get(handle)) return handle;
    }
    constexpr const char* SHAPES[]{"Shell", "Fin", "Blade"};
    constexpr const char* SUFFIXES[]{"", "GBuffer", "Velocity", "Shadow", "Selection"};
    auto& slot = cache.m_shaders[(static_cast<size_t>(mode) * 3 + static_cast<size_t>(shape)) * 2 + (skinned ? 1 : 0)];
    if (slot.m_handle.IsValid() && resources.Get(slot.m_handle)) return slot.m_handle;
    if (slot.m_attempt == resources.FrameStamp()) return {};
    slot.m_attempt = resources.FrameStamp();
    slot.m_handle = resources.LoadShader(std::string("Assets/Shaders/Fiber/Fiber") + SHAPES[static_cast<int>(shape)]
        + (skinned ? "Skinned" : "") + SUFFIXES[static_cast<int>(mode)] + ".hlsl");
    return slot.m_handle;
}
FiberMotionHistory& AdvanceFiberHistory(FiberResources& cache, const RenderFiberInput& input, RenderPassContext& ctx,
    const math::Matrix4& world, const FiberFrameCB& frame, const FiberMaterialSettings& settings, uint64_t surfaceKey) {
    const auto target = ctx.Res().Target("Velocity");
    const uint64_t key = (static_cast<uint64_t>(target.gen) << 32) | target.id;
    const uint64_t stamp = ctx.frameStamp;
    auto it = cache.histories.try_emplace({input.identity, key, surfaceKey}).first;
    auto& history = it->second;
    renderer::FiberDeformationState state;
    state.m_world = world;
    state.m_wind = frame.m_wind;
    state.m_time = frame.m_time;
    state.m_turbulence = frame.m_turbulence;
    state.m_pulseFrequency = frame.m_pulseFrequency;
    state.m_length = settings.m_length;
    state.m_windResponse = settings.m_windResponse;
    state.m_maxBend = settings.m_maxBend;
    state.m_gravityBend = settings.m_gravityBend;
    state.m_mode = static_cast<int>(input.settings.m_mode);
    state.m_shellCount = static_cast<int>(frame.m_shellCount);
    history.Advance(stamp, state);
    return history;
}
void ExecuteFiberGeometry(RenderPassContext& ctx, FiberPassMode mode,
    const PerFrameCB* lightFrame = nullptr, const math::Frustum* lightFrustum = nullptr)
{
    if (!ctx.renderScene || ctx.renderScene->fibers.empty()) return;
    auto& resources = ctx.resources;
    auto& cache = GetFiberResources(resources);
    if (!cache.m_material.IsValid() || !cache.m_frame.IsValid() || !cache.m_pipeline.IsValid()) return;
    if (mode == FiberPassMode::VELOCITY && !cache.m_motion.IsValid()) return;
    if (mode == FiberPassMode::COLOR && ctx.isDeferred && !cache.m_deferredPipeline.IsValid()) return;
    const bool wireframe = mode == FiberPassMode::COLOR && ctx.settings.IsWireframe();
    const auto colorPipeline = wireframe
        ? GetOrCreateMaterialPSO(resources, BlendMode::OPAQUE_BLEND, true, 0, 0.0f, true)
        : (ctx.isDeferred ? cache.m_deferredPipeline : cache.m_pipeline);
    const auto cameraData = lightFrame ? *lightFrame
        : MakeCameraFrameCB(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    resources.Update(ctx.handles.frameCB, &cameraData, sizeof(cameraData));
    if (mode != FiberPassMode::SHADOW && mode != FiberPassMode::SELECTION) {
        const char* target = mode == FiberPassMode::GBUFFER ? "GBuffer"
            : mode == FiberPassMode::VELOCITY ? "Velocity" : "HDR";
        if (!ctx.Res().Target(target).IsValid()) return;
        ctx.renderer.SetRenderTarget(ctx.Res().Target(target), resources);
    }
    for (const auto& surface : ctx.renderScene->fibers) {
        if (surface.layer >= 32 || (ctx.cullingMask & (1u << surface.layer)) == 0) continue;
        if (mode == FiberPassMode::SELECTION && !surface.selected) continue;
        const auto& fiber = surface.settings;
        const auto& settings = surface.material;
        UploadIfChanged(resources, cache.m_contacts, cache.m_uploadedContacts, surface.contacts);
        const bool drawFins = fiber.m_mode == FiberRenderMode::FIN
            || (fiber.m_mode == FiberRenderMode::HYBRID && mode != FiberPassMode::SHADOW);
        const bool drawShells = fiber.m_mode == FiberRenderMode::SHELL || fiber.m_mode == FiberRenderMode::HYBRID;
        const bool drawBlades = fiber.m_mode == FiberRenderMode::BLADE;
        auto object = surface.object;
        if (wireframe) object.objectParams.y = ctx.settings.IsUnlit() ? 2.0f : 1.0f;
        if (mode == FiberPassMode::SHADOW && !surface.cast) continue;
        const bool skinned = surface.skinned;
        if (mode == FiberPassMode::COLOR) ++ctx.statsTotalObjects;
        if (surface.hasBounds) {
            const WorldBounds bounds{surface.boundsCenter, surface.boundsRadius};
            if (lightFrustum) {
                if (!lightFrustum->IntersectsSphere(bounds.center, bounds.radius)) continue;
            } else {
                if (!IsWithinDrawDistance(CullingView(ctx), {bounds.center, bounds.radius, ctx.hasLayerCullDistances && ctx.cullLayerDistances[surface.layer] > 0 ? ctx.cullLayerDistances[surface.layer] : ctx.cullMaxDistance})) {
                    if (mode == FiberPassMode::COLOR) ++ctx.statsDistanceCulled;
                    continue;
                }
                if (ctx.frustumCullingEnabled && ctx.cameraFrustum
                    && !ctx.cameraFrustum->IntersectsSphere(bounds.center, bounds.radius)) {
                    if (mode == FiberPassMode::COLOR) ++ctx.statsFrustumCulled;
                    continue;
                }
                if (mode != FiberPassMode::SELECTION
                    && IsViewOccluded(ctx, bounds.center, bounds.radius > 0
                        ? bounds.radius + std::max(ctx.cullingBoundsPadding, 0.0f) : 0)) {
                    if (mode == FiberPassMode::COLOR) ++ctx.statsOcclusionCulled;
                    continue;
                }
            }
        }
        FiberFrameCB frame = surface.frame;
        frame.m_shellCount = static_cast<float>(std::clamp(fiber.m_shellCount, 1, 64));
        const auto& lodCenter = surface.lodCenter;
        const float distance=(lodCenter-ctx.camera.m_position).Length();
        const float sparseStart = fiber.m_terrainSparseLodStart;
        const bool sparseTerrain = sparseStart > 0.0f && distance >= sparseStart;
        if (fiber.m_distanceLod) {
            const float nearDistance=std::max(fiber.m_lodNear,0.0f);
            const float farDistance=std::max(fiber.m_lodFar,nearDistance+0.1f);
            if (distance>=farDistance) continue;
            frame.m_shellCount=static_cast<float>(renderer::FiberLodShellCount(fiber.m_shellCount,fiber.m_minShellCount,distance,nearDistance,farDistance));
            const float t=std::clamp((distance-nearDistance)/(farDistance-nearDistance),0.0f,1.0f);
            frame.m_lod.x=1.0f-0.75f*t;
            frame.m_lod.y=std::clamp((farDistance-distance)/(0.1f*(farDistance-nearDistance)),0.0f,1.0f);
            if (sparseStart > 0.0f) {
                /// @note 生成済み根元の先頭を使い、疎なバッファへ切り替わる前後で同じ密度にする。
                const float start = sparseTerrain ? sparseStart : nearDistance;
                const float end = sparseTerrain ? farDistance : sparseStart;
                const float fraction = sparseTerrain ? 0.25f : TERRAIN_SPARSE_BLADE_DENSITY_SCALE;
                frame.m_lod.x = std::lerp(1.0f, fraction, std::clamp((distance - start) / (end - start), 0.0f, 1.0f));
            }
        }
        if (drawBlades) {
            const int segments = mode == FiberPassMode::SHADOW && fiber.m_shadowBladeSegments > 0
                ? fiber.m_shadowBladeSegments : fiber.m_bladeSegments;
            frame.m_lod.z = sparseTerrain ? 1.0f : static_cast<float>(std::clamp(segments, 1, 6));
        }
        /// @note 影は層の隙間が見えにくく、カスケード・光源の面の数だけ全層を描くので層数を別に絞る。層間隔は fiberShellCount から求まるので視差の上乗せも自動で太る。
        if (mode == FiberPassMode::SHADOW)
            frame.m_shellCount = std::min(frame.m_shellCount, static_cast<float>(std::clamp(fiber.m_shadowShellCount, 1, 64)));
        frame.m_hybrid = fiber.m_mode == FiberRenderMode::HYBRID ? 1.0f : 0.0f;
        auto skinnedVertices = surface.skinnedVertices;
        if (skinned && drawShells && surface.preparedSkinning && mode != FiberPassMode::VELOCITY
            && !ResolvePreparedSkinningOutput(surface.preparedSkinning, surface.preparedSkinningWrite, skinnedVertices)) continue;
        const bool shellPreSkinned = skinned && skinnedVertices.IsValid() && mode != FiberPassMode::VELOCITY;
        const auto shellShader = drawShells ? GetFiberShader(cache, resources, mode, FiberShape::SHELL, skinned && !shellPreSkinned, surface.appearance.get())
                                            : renderer::ResourceHandle<renderer::ShaderTag>{};
        const auto finShader = drawFins ? GetFiberShader(cache, resources, mode, FiberShape::FIN, skinned, surface.appearance.get())
                                        : renderer::ResourceHandle<renderer::ShaderTag>{};
        const auto bladeShader = drawBlades && !skinned ? GetFiberShader(cache, resources, mode, FiberShape::BLADE, false, surface.appearance.get())
                                                        : renderer::ResourceHandle<renderer::ShaderTag>{};
        if (!shellShader.IsValid() && !finShader.IsValid() && !bladeShader.IsValid()) continue;

        ResourceHandle<BufferTag> bladeIndices;
        bool bladeStrip = false;
        if (bladeShader.IsValid()) {
            frame.m_lod.w = 0.0f;
            const auto geometryMode = ctx.settings.rasterGeometryMode;
            if (geometryMode != RasterGeometryMode::TRIANGLE_LIST) {
                const bool customBladeShader = surface.appearance
                    && surface.appearance->shaders[static_cast<size_t>(mode) * 2] == bladeShader;
                if (!customBladeShader || resources.GetShaderCapabilities(bladeShader).SupportsInstancedFiberBlade()) {
                    bladeStrip = geometryMode == RasterGeometryMode::TRIANGLE_STRIP;
                    if (geometryMode == RasterGeometryMode::INDEXED_FIBER_LIST)
                        bladeIndices = GetFiberBladeIndexBuffer(cache, resources, static_cast<uint32_t>(frame.m_lod.z));
                    if (bladeStrip || bladeIndices.IsValid()) frame.m_lod.w = 1.0f;
                }
            }
        }
        resources.Update(ctx.handles.objectCB, &object, sizeof(object));
        UploadIfChanged(resources, cache.m_material, cache.m_uploadedMaterial, settings);
        UploadIfChanged(resources, cache.m_frame, cache.m_uploadedFrame, frame);
        renderer::DrawCall call;
        call.pipelineState = cache.m_pipeline;
        if (mode == FiberPassMode::COLOR) {
            /// @note Deferred は GBuffer の深度を転写済み。DEPTH_SKY (等しい深度も通す) / 読取専用で固有の繊維照明を上書きする。
            call.pipelineState = colorPipeline;
        }
        call.constantBuffers[0] = ctx.handles.frameCB;
        call.constantBuffers[1] = ctx.handles.objectCB;
        call.constantBuffers[2] = cache.m_material;
        if (surface.appearance && surface.appearance->params.size() >= sizeof(settings)) {
            const auto& appearance = *surface.appearance;
            if (appearance.materialCB.IsValid()
                && std::memcmp(appearance.params.data(), &settings, sizeof(settings)) == 0) {
                call.constantBuffers[2] = appearance.materialCB;
            } else {
                auto bytes = appearance.params;
                std::memcpy(bytes.data(), &settings, sizeof(settings));
                const auto* buffer = resources.Get(cache.m_appearanceMaterial);
                if (!buffer || buffer->GetSize() < bytes.size()) {
                    if (cache.m_appearanceMaterial.IsValid()) resources.Release(cache.m_appearanceMaterial);
                    cache.m_appearanceMaterial = resources.CreateConstantBuffer(static_cast<uint32_t>(bytes.size()));
                    cache.m_appearanceBytes.clear();
                }
                if (cache.m_appearanceBytes != bytes) {
                    resources.Update(cache.m_appearanceMaterial, bytes.data(), static_cast<uint32_t>(bytes.size()));
                    cache.m_appearanceBytes = std::move(bytes);
                }
                call.constantBuffers[2] = cache.m_appearanceMaterial;
            }
        }
        call.constantBuffers[5] = cache.m_frame;
        call.constantBuffers[10] = cache.m_contacts;
        /// @note 局所 FlowField は VS の t2 (vsBuffers[1])、Baked の速度場アトラスは t26。GPU 粒子と同じ組。
        call.vsBuffers[1] = surface.flows;
        call.textures[26] = surface.velocityField;
        if (skinned) {
            call.constantBuffers[7]=surface.skin;
            call.constantBuffers[9]=surface.previousSkin;
        }
        if (mode==FiberPassMode::SELECTION) call.pipelineState=cache.m_pipeline;
        if (mode == FiberPassMode::COLOR) {
            call.constantBuffers[3] = ctx.handles.lightCB;
            call.constantBuffers[4] = ctx.handles.shadowCB;
            call.constantBuffers[8] = ctx.handles.advancedGraphicsCB;
            BindForwardShadingResources(call, ctx);
            call.textures[8] = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
            call.textures[16] = ctx.handles.iblIrradiance;
            call.textures[17] = ctx.handles.iblPrefilter;
            call.textures[18] = ctx.handles.iblBrdfLut;
        }
        if (mode == FiberPassMode::VELOCITY) {
            const uint64_t geometryIdentity = drawBlades
                ? (static_cast<uint64_t>(surface.blades.m_blades.gen)<<32)|surface.blades.m_blades.id
                : (static_cast<uint64_t>(surface.vertices.gen)<<32)|surface.vertices.id;
            auto& history = AdvanceFiberHistory(cache, surface, ctx, object.world, frame, settings,geometryIdentity);
            const auto& prev = history.m_previous;
            FiberMotionCB motion{};
            motion.m_previousWorld = prev.m_world;
            motion.m_previousNormal = math::Matrix4::InverseTransposeAffine(prev.m_world);
            motion.m_previousWindTime = {prev.m_wind.x, prev.m_wind.y, prev.m_wind.z, prev.m_time};
            motion.m_previousShape = {prev.m_length, prev.m_windResponse, prev.m_maxBend, prev.m_gravityBend};
            motion.m_previousGust = {prev.m_turbulence, prev.m_pulseFrequency, 0.0f, 0.0f};
            motion.m_motion = {ctx.taaJitterNdcX, ctx.taaJitterNdcY, history.m_valid && (!skinned || surface.validPreviousSkin) ? 1.0f : 0.0f, 0.0f};
            UploadIfChanged(resources, cache.m_motion, cache.m_uploadedMotion, motion);
            call.constantBuffers[6] = cache.m_motion;
            call.constantBuffers[8] = ctx.handles.advancedGraphicsCB;
            call.textures[7] = resources.GetDepthTexture(ctx.Res().Target("HDR"));
        }
        if (finShader.IsValid()) {
            const auto& fins = surface.fins;
            if (fins.m_vertices.IsValid() && fins.m_indices.IsValid()) {
                call.shader = finShader;
                call.vertexBuffer = fins.m_vertices;
                call.indexBuffer = fins.m_indices;
                call.indexCount = fins.m_indexCount;
                if (mode == FiberPassMode::SHADOW) SubmitCountedShadow(ctx, call);
                else SubmitCounted(ctx, call);
            }
        }
        if (shellShader.IsValid()) {
            call.shader = shellShader;
            call.vertexBuffer = shellPreSkinned ? skinnedVertices : surface.vertices;
            if (shellPreSkinned && surface.preparedSkinning) {
                call.preparedSkinning = surface.preparedSkinning;
                call.preparedSkinningRow = surface.preparedSkinningWrite;
            }
            call.indexBuffer = surface.indices;
            call.indexCount = surface.indexCount;
            call.instanceCount = static_cast<uint32_t>(frame.m_shellCount);
            if (mode == FiberPassMode::SHADOW) SubmitCountedShadow(ctx, call);
            else SubmitCounted(ctx, call);
        }
        if (bladeShader.IsValid()) {
            call.preparedSkinning.reset();
            call.preparedSkinningRow = 0;
            const auto& blades=surface.blades;
            if (blades.m_blades.IsValid() && blades.m_bladeCount > 0) {
                /// @note 頂点バッファなし。距離 LOD は先頭から提出する根元数を減らす。
                const auto bladeCount = static_cast<uint32_t>(static_cast<float>(blades.m_bladeCount) * frame.m_lod.x);
                if (bladeCount == 0) continue;
                const auto segments = static_cast<uint32_t>(frame.m_lod.z);
                call.shader = bladeShader;
                call.vertexBuffer = {};
                call.indexBuffer = bladeIndices;
                call.indexCount = bladeIndices.IsValid() ? segments * 6u : 0;
                call.vsBuffers[0] = blades.m_blades;
                if (bladeStrip) call.topology = PrimitiveTopology::TRIANGLE_STRIP;
                if (frame.m_lod.w != 0.0f) {
                    call.vertexCount = 2u * (segments + 1u);
                    call.instanceCount = 2u * bladeCount;
                } else {
                    call.vertexCount = bladeCount * segments * 12u;
                    call.instanceCount = 1;
                }
                if (mode==FiberPassMode::SHADOW) SubmitCountedShadow(ctx,call); else SubmitCounted(ctx,call);
            }
        }
    }
}
}
void FiberRenderPass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    builder.ReadWrite("HDR").Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas");
    if (ctx.waterMediumPassActive)
        builder.Read("WaterMediumAtlas");
    DeclareScreenSpaceOcclusionReads(builder, ctx);
}

bool FiberRenderPass::IsEnabled(const RenderPassContext& ctx) const { return ctx.renderScene && !ctx.renderScene->fibers.empty(); }
void ExecuteFiberSelectionMask(RenderPassContext& ctx) { ExecuteFiberGeometry(ctx,FiberPassMode::SELECTION); }
void ExecuteFiberPass(RenderPassContext& ctx) { ExecuteFiberGeometry(ctx, FiberPassMode::COLOR); }
void ExecuteFiberGBufferPass(RenderPassContext& ctx) { ExecuteFiberGeometry(ctx, FiberPassMode::GBUFFER); }
void ExecuteFiberVelocityPass(RenderPassContext& ctx) { ExecuteFiberGeometry(ctx, FiberPassMode::VELOCITY); }
void SubmitFiberShadowCasters(RenderPassContext& ctx, const PerFrameCB& frame, const math::Frustum& frustum)
{
    ExecuteFiberGeometry(ctx, FiberPassMode::SHADOW, &frame, &frustum);
}
}
