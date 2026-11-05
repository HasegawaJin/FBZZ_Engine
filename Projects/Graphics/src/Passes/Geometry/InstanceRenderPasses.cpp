/// @file    InstanceRenderPasses.cpp
/// @brief   Reuse GPU placement rows; views build visible IDs and mesh/material/LOD draw commands.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Graphics/Passes/Geometry/InstanceRenderPasses.hpp>
#include "RenderScenePassHelpers.hpp"
#include <Graphics/Renderer/DynamicBufferPool.hpp>
#include <Graphics/Renderer/GpuGeometrySelection.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include <Math/Quaternion.hpp>
#include <Core/Logger.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <limits>

namespace fbzz::renderer {
namespace {
enum class InstancePass { GBUFFER, FORWARD, SHADOW, SELECTION, VELOCITY, CAPTURE };
struct ResidentInstanceRecord {
    math::Matrix4 world, normal, previousWorld;
    math::Vector4 bounds;
};
static_assert(sizeof(ResidentInstanceRecord)==208);
struct InstanceView {
    std::array<math::Vector4,6> planes{};
    math::Vector4 cameraProjection{}, metrics{}, heights{};
    std::array<uint32_t,4> counts{}, firstParts{};
    math::Vector4 forward{}, culling{};
};
static_assert(sizeof(InstanceView)==208);
struct InstanceSource {
    ResourceHandle<StructuredBufferTag> gpu;
    std::vector<ResidentInstanceRecord> rows;
    std::shared_ptr<const InstanceSetData> data;
    math::Matrix4 world=math::Matrix4::Identity();
    math::Vector3 center{};
    float radius=0;
    uint64_t preparedFrame=UINT64_MAX, lastSeen=0;
    std::vector<uint32_t> motionSlots;
    bool moved=false;
    uint32_t activeCount=0;
};
struct InstanceResources {
    uint64_t owner=0, epoch=UINT64_MAX;
    std::map<std::array<uint64_t,2>,InstanceSource> sources;
    std::array<ResourceHandle<ShaderTag>,10> shaders{};
    DynamicStructuredBufferPool views{1}, partLods{1}, cpuIds{64};
    DynamicRWStructuredBufferPool ids{64}, arguments{1};
    ResourceHandle<ConstantBufferTag> material;
    ResourceHandle<ConstantBufferTag> windFrame;
    uint64_t windStamp = UINT64_MAX;
    float windTime = 0.0f, previousWindTime = 0.0f;
    std::set<std::array<uint32_t,2>> warnedCustom;
};
InstanceResources& Resources(ResourceManager& manager)
{
    static InstanceResources cache;
    if (cache.owner!=manager.GetInstanceId() || cache.epoch!=manager.GetResetVersion()) {
        if (auto* owner = ResourceManager::FindOwner(cache.owner)) {
            for (const auto& [identity, source] : cache.sources) owner->Release(source.gpu);
            owner->Release(cache.material);
            owner->Release(cache.windFrame);
        }
        cache={}; cache.owner=manager.GetInstanceId(); cache.epoch=manager.GetResetVersion();
    }
    constexpr std::array<const char*,10> paths{"Assets/Shaders/Pipeline/Deferred/GBufferResident.hlsl",
        "Assets/Shaders/Material/Surface/PBRResident.hlsl","Assets/Shaders/Pipeline/Shadow/ShadowResident.hlsl",
        "Assets/Shaders/Pipeline/Shadow/AlphaShadowResident.hlsl","Assets/Shaders/Pipeline/InstanceSelection.hlsl",
        "Assets/Shaders/Motion/VelocityResident.hlsl","Assets/Shaders/Pipeline/InstanceLodCull.cs.hlsl",
        "Assets/Shaders/Pipeline/Shadow/AlphaShadow.hlsl","Assets/Shaders/Pipeline/AlphaSelection.hlsl","Assets/Shaders/Motion/VelocityAlpha.hlsl"};
    for (size_t i=0;i<paths.size();++i) if (!manager.Get(cache.shaders[i])) cache.shaders[i]=manager.LoadShader(paths[i]);
    if (!manager.Get(cache.material)) cache.material=manager.CreateConstantBuffer(96);
    if (!manager.Get(cache.windFrame)) cache.windFrame=manager.CreateConstantBuffer(sizeof(PostProcCB));
    std::erase_if(cache.sources,[&](auto& entry) {
        if (manager.FrameStamp()<=entry.second.lastSeen+120) return false;
        manager.Release(entry.second.gpu); return true;
    });
    return cache;
}
bool SameMatrix(const math::Matrix4& a,const math::Matrix4& b) { return std::memcmp(&a,&b,sizeof(a))==0; }
bool SameVector(math::Vector3 a,math::Vector3 b) { return a.x==b.x&&a.y==b.y&&a.z==b.z; }
bool LeafAnimated(const RenderMaterial& material)
{
    float strength = 0; std::memcpy(&strength, material.gbufferParams.data() + 68, sizeof(strength));
    return strength > 0.0f;
}
float Stretch(const math::Matrix4& world)
{
    const std::array<math::Vector3,3> axes{{{world.m[0][0],world.m[1][0],world.m[2][0]},
        {world.m[0][1],world.m[1][1],world.m[2][1]},{world.m[0][2],world.m[1][2],world.m[2][2]}}};
    const std::array<float,3> lengths{axes[0].LengthSq(),axes[1].LengthSq(),axes[2].LengthSq()};
    bool orthogonal=true;
    for (size_t a=0;a<3;++a) for (size_t b=a+1;b<3;++b)
        if (std::abs(math::Vector3::Dot(axes[a],axes[b]))>0.000001f*std::sqrt(lengths[a]*lengths[b])) orthogonal=false;
    /// @note Orthogonal TRS has exact maximum-axis stretch; shear uses a conservative Frobenius bound.
    return std::sqrt(orthogonal ? std::max({lengths[0],lengths[1],lengths[2]}) : lengths[0]+lengths[1]+lengths[2]);
}
InstanceSource& Source(RenderPassContext& ctx,InstanceResources& cache,const RenderInstanceInput& input)
{
    auto& source=cache.sources[input.identity]; source.lastSeen=ctx.resources.FrameStamp();
    const bool unchanged=source.data && source.data.get()==input.data.get() && source.data->revision==input.data->revision && SameMatrix(source.world,input.world)
        && SameVector(source.center,input.localBoundsCenter) && source.radius==input.localBoundsRadius;
    if (source.preparedFrame==ctx.resources.FrameStamp() && unchanged) return source;
    std::vector<uint32_t> changed=source.motionSlots; source.motionSlots.clear(); source.moved=false;
    for (uint32_t slot : changed) source.rows[slot].previousWorld=source.rows[slot].world;
    const bool partial=source.data && input.data->parentRevision==source.data->revision && source.rows.size()<=input.data->placements.size()
        && SameMatrix(source.world,input.world) && SameVector(source.center,input.localBoundsCenter) && source.radius==input.localBoundsRadius;
    if (!unchanged) {
        if (partial) changed.insert(changed.end(),input.data->dirtySlots.begin(),input.data->dirtySlots.end());
        else { changed.resize(input.data->placements.size()); for (size_t i=0;i<changed.size();++i) changed[i]=static_cast<uint32_t>(i); }
        std::sort(changed.begin(),changed.end()); changed.erase(std::unique(changed.begin(),changed.end()),changed.end());
        const size_t previousCount=source.rows.size();
        if (!partial) source.activeCount=0;
        source.rows.resize(input.data->placements.size());
        for (uint32_t slot : changed) {
            const auto& placement=input.data->placements[slot]; auto& row=source.rows[slot];
            const bool previouslyActive=slot<previousCount && source.data && source.data->placements[slot].active;
            if (!partial) source.activeCount+=placement.active ? 1u : 0u;
            else if (placement.active!=previouslyActive) { if (placement.active) ++source.activeCount; else --source.activeCount; }
            const auto world=input.world*math::Matrix4::TRS(placement.position,math::Quaternion::FromEuler(placement.eulerRadians),placement.scale);
            const bool sameId=slot<previousCount && source.data && source.data->placements[slot].id==placement.id;
            const auto old=sameId ? row.world : world;
            row.world=world; row.normal=math::Matrix4::InverseTransposeAffine(world); row.previousWorld=old;
            const auto center=world*math::Vector4{input.localBoundsCenter,1};
            row.bounds={center.x,center.y,center.z,placement.active ? input.localBoundsRadius*Stretch(world) : -1};
            if (placement.active && !SameMatrix(old,world)) { source.motionSlots.push_back(slot); source.moved=true; }
        }
        source.world=input.world; source.center=input.localBoundsCenter; source.radius=input.localBoundsRadius; source.data=input.data;
    }
    source.preparedFrame=ctx.resources.FrameStamp();
    if (!source.rows.empty() && (!ctx.resources.Get(source.gpu) || ctx.resources.Get(source.gpu)->GetElementCount()<source.rows.size())) {
        ctx.resources.Release(source.gpu);
        uint32_t capacity=64;
        while (capacity<source.rows.size()) capacity*=2;
        constexpr uint64_t RESIDENT_GPU_BUDGET=256ull*1024*1024;
        const uint64_t required=static_cast<uint64_t>(capacity)*sizeof(ResidentInstanceRecord);
        uint64_t used=0;
        for (const auto& entry : cache.sources) if (const auto* buffer=ctx.resources.Get(entry.second.gpu)) used+=buffer->GetSize();
        while (used+required>RESIDENT_GPU_BUDGET) {
            auto oldest=cache.sources.end();
            for (auto it=cache.sources.begin();it!=cache.sources.end();++it)
                if (ctx.resources.Get(it->second.gpu) && it->second.preparedFrame!=ctx.resources.FrameStamp()
                    && (oldest==cache.sources.end() || it->second.lastSeen<oldest->second.lastSeen)) oldest=it;
            if (oldest==cache.sources.end()) break;
            used-=ctx.resources.Get(oldest->second.gpu)->GetSize(); ctx.resources.Release(oldest->second.gpu); oldest->second.gpu={};
        }
        if (used+required<=RESIDENT_GPU_BUDGET) source.gpu=ctx.resources.CreateRWStructuredBuffer(nullptr,capacity,sizeof(ResidentInstanceRecord));
        else FBZZ_LOG_WARN("Instance rendering: resident GPU budget exceeded; using CPU fallback");
        changed.resize(source.rows.size()); for (size_t i=0;i<changed.size();++i) changed[i]=static_cast<uint32_t>(i);
    }
    std::sort(changed.begin(),changed.end()); changed.erase(std::unique(changed.begin(),changed.end()),changed.end());
    if (!ctx.resources.Get(source.gpu)) return source;
    for (size_t i=0;i<changed.size();) {
        size_t end=i+1; while (end<changed.size() && changed[end]==changed[end-1]+1) ++end;
        const size_t bytes=(end-i)*sizeof(ResidentInstanceRecord), offset=changed[i]*sizeof(ResidentInstanceRecord);
        if (!ctx.resources.UpdateRegion(source.gpu,source.rows.data()+changed[i],bytes,offset)) { ctx.resources.Release(source.gpu); source.gpu={}; break; }
        ctx.statsInstanceUploadBytes+=bytes; i=end;
    }
    return source;
}
InstanceView View(const RenderPassContext& ctx,const RenderInstanceInput& input,const math::Frustum& frustum,bool shadow)
{
    InstanceView result;
    for (int i=0;i<6;++i) { const auto& p=frustum.GetPlane(i); result.planes[i]={p.normal.x,p.normal.y,p.normal.z,p.distance}; }
    const float scale=std::abs(ctx.camera.GetProjectionMatrix().m[1][1])*static_cast<float>(std::max(ctx.height,1u))*0.5f;
    result.cameraProjection={ctx.camera.m_position.x,ctx.camera.m_position.y,ctx.camera.m_position.z,scale};
    float distance=ctx.hasLayerCullDistances && ctx.cullLayerDistances[input.layer]>0 ? ctx.cullLayerDistances[input.layer] : ctx.cullMaxDistance;
    if (input.maxDistance>0) distance=distance>0 ? std::min(distance,input.maxDistance) : input.maxDistance;
    result.metrics={static_cast<float>(std::max(ctx.height,1u)),std::max(ctx.camera.m_near,0.001f),distance,
        ctx.camera.m_projection==ProjectionMode::Orthographic ? 1.0f : 0.0f};
    result.heights={input.lodHeights[0],input.lodHeights[1],input.lodHeights[2],input.lodHeights[3]};
    result.counts={static_cast<uint32_t>(input.data->placements.size()),input.lodCount,static_cast<uint32_t>(input.parts.size()),shadow||ctx.frustumCullingEnabled ? 1u : 0u};
    result.firstParts.fill(UINT32_MAX);
    const auto forward=ctx.camera.GetForward(); result.forward={forward.x,forward.y,forward.z,0};
    result.culling={std::max(ctx.cullingBoundsPadding,0.0f),shadow ? 0.0f : ctx.smallObjectScreenHeight,ctx.cullDistanceSpherical ? 0.0f : 1.0f,0};
    for (uint32_t i=0;i<input.parts.size();++i) if (result.firstParts[input.parts[i].lod]==UINT32_MAX) result.firstParts[input.parts[i].lod]=i;
    return result;
}
uint32_t CpuLod(const InstanceView& view,const math::Vector4& bounds)
{
    const auto distance=view.metrics.w>0.5f ? 1.0f
        : (math::Vector3{bounds.x,bounds.y,bounds.z}-math::Vector3{view.cameraProjection.x,view.cameraProjection.y,view.cameraProjection.z}).Length();
    const float height=2*bounds.w*view.cameraProjection.w/view.metrics.x
        /(view.metrics.w>0.5f ? 1.0f : std::max(distance-bounds.w,view.metrics.y));
    const std::array<float,4> heights{view.heights.x,view.heights.y,view.heights.z,view.heights.w};
    for (uint32_t i=0;i<view.counts[1];++i) if (height>=heights[i]) return i;
    return view.counts[1]-1;
}
bool PrepareGpu(RenderPassContext& ctx,InstanceResources& cache,const RenderInstanceInput& input,
    const InstanceSource& source,const InstanceView& view,ResourceHandle<StructuredBufferTag>& ids,ResourceHandle<StructuredBufferTag>& args)
{
    if (!ctx.settings.bulkInstanceGpuCulling || !ctx.settings.gpuInstancing || !ctx.renderer.SupportsIndirectDraw()
        || !ctx.resources.Get(source.gpu) || view.counts[0]<64 || !ctx.resources.Get(cache.shaders[6])) return false;
    /// @note Native Profiler measurements favor CPU-visible-ID instancing for small sets; explicit Indirect/Mesh mode keeps the GPU comparison available.
    if ((ctx.settings.geometryDrawMode==GeometryDrawMode::CPU
        || ctx.settings.geometryDrawMode==GeometryDrawMode::GPU_INSTANCE_INDIRECT) && view.counts[0]<32768) return false;
    for (const auto& part : input.parts) if (!part.mesh.material.shaderCapabilities.SupportsGBuffer()) return false;
    std::vector<GpuGeometryIndexedArguments> initial;
    std::vector<uint32_t> lods;
    for (const auto& part : input.parts) { initial.push_back({part.lod*view.counts[0],part.mesh.indexCount,0,0,0,0}); lods.push_back(part.lod); }
    ids=cache.ids.Acquire(ctx.resources,static_cast<size_t>(view.counts[0])*view.counts[1],sizeof(uint32_t));
    args=cache.arguments.Acquire(ctx.resources,input.parts.size(),sizeof(GpuGeometryIndexedArguments));
    const auto viewBuffer=cache.views.Acquire(ctx.resources,1,sizeof(InstanceView));
    const auto partsBuffer=cache.partLods.Acquire(ctx.resources,lods.size(),sizeof(uint32_t));
    if (!ctx.resources.Get(ids)||!ctx.resources.Get(args)||!ctx.resources.Get(viewBuffer)||!ctx.resources.Get(partsBuffer)) return false;
    ctx.resources.Update(viewBuffer,&view,sizeof(view)); ctx.resources.Update(partsBuffer,lods.data(),lods.size()*sizeof(uint32_t));
    ctx.resources.Update(args,initial.data(),initial.size()*sizeof(GpuGeometryIndexedArguments));
    ComputeCall compute; compute.shader=cache.shaders[6]; compute.srvBuffers[14]=source.gpu; compute.srvBuffers[15]=viewBuffer;
    compute.srvBuffers[29]=partsBuffer; compute.uavBuffers={ids,args}; compute.dispatchX=(view.counts[0]+63)/64;
    return ctx.renderer.TryDispatch(compute,ctx.resources);
}
void Execute(RenderPassContext& ctx,InstancePass pass,const math::Frustum* lightFrustum=nullptr,ResourceHandle<ConstantBufferTag> lightFrame={},
    uint64_t excludedOwner=UINT64_MAX)
{
    if (!ctx.renderScene || ctx.renderScene->instances.empty()) return;
    auto& cache=Resources(ctx.resources);
    if (cache.windStamp != ctx.resources.FrameStamp()) {
        cache.previousWindTime = cache.windStamp == UINT64_MAX ? ctx.time : cache.windTime;
        cache.windTime = ctx.time; cache.windStamp = ctx.resources.FrameStamp();
    }
    PostProcCB windFrame{}; windFrame.time = cache.windTime; windFrame.customParameters[0] = cache.previousWindTime;
    ctx.resources.Update(cache.windFrame, &windFrame, sizeof(windFrame));
    auto frame=MakeCameraFrameCB(ctx.camera,ctx.taaJitterNdcX,ctx.taaJitterNdcY);
    const bool forwardPass=pass==InstancePass::FORWARD || pass==InstancePass::CAPTURE;
    if (pass==InstancePass::CAPTURE) {
        frame.projection=ctx.camera.GetProjectionMatrix();frame.viewProjection=frame.projection*frame.view;
        frame.invViewProjection=math::Matrix4::Inverse(frame.viewProjection);
    }
    const auto cameraFrustum=math::Frustum::FromViewProjection(ctx.camera.GetViewProjection());
    if (pass!=InstancePass::SHADOW) ctx.resources.Update(ctx.handles.frameCB,&frame,sizeof(frame));
    for (const auto& input : ctx.renderScene->instances) {
        if (input.selectionOnly && pass != InstancePass::SELECTION) continue;
        if (pass==InstancePass::CAPTURE && input.identity[1]==excludedOwner) continue;
        if (!input.data || input.lodCount==0 || input.lodCount>4 || input.layer>=32 || (ctx.cullingMask&(1u<<input.layer))==0) continue;
        if (input.parts.empty() || input.parts.size()>64 || input.data->placements.size()>1048576) continue;
        std::array<bool,4> levels{}; bool valid=true;
        for (const auto& part : input.parts) { if (part.lod>=input.lodCount) { valid=false; break; } levels[part.lod]=true; }
        for (uint32_t level=0;level<input.lodCount;++level) if (!levels[level]) valid=false;
        if (!valid) continue;
        if (pass==InstancePass::SHADOW && !input.castShadows) continue;
        if (pass==InstancePass::SELECTION && !input.selected) continue;
        /// @note Deferred already consumed these parts; reject before preparing rows or synchronizing GPU visibility dispatches.
        if (pass==InstancePass::FORWARD && ctx.isDeferred && std::all_of(input.parts.begin(),input.parts.end(),
            [](const auto& part) { return part.mesh.material.shaderCapabilities.SupportsGBuffer(); })) continue;
        auto& source=Source(ctx,cache,input);
        if (pass==InstancePass::GBUFFER || (pass==InstancePass::FORWARD && !ctx.isDeferred)) ctx.statsTotalObjects+=static_cast<int>(source.activeCount);
        const bool animated = std::any_of(input.parts.begin(), input.parts.end(), [](const auto& part) { return LeafAnimated(part.mesh.material); });
        if (pass==InstancePass::VELOCITY && !source.moved && !animated) continue;
        const auto view=View(ctx,input,lightFrustum ? *lightFrustum : cameraFrustum,pass==InstancePass::SHADOW);
        ResourceHandle<StructuredBufferTag> gpuIds,gpuArgs;
        bool residentShaders=true;
        for (const auto& part : input.parts) {
            const size_t shader=pass==InstancePass::GBUFFER ? 0u : forwardPass ? 1u : pass==InstancePass::SELECTION ? 4u : pass==InstancePass::VELOCITY ? 5u : (part.mesh.material.surface.alphaCutoff>0 || LeafAnimated(part.mesh.material)) ? 3u : 2u;
            residentShaders=residentShaders && ctx.resources.Get(cache.shaders[shader]);
        }
        const bool gpu=residentShaders && PrepareGpu(ctx,cache,input,source,view,gpuIds,gpuArgs);
        std::array<std::vector<uint32_t>,4> visible;
        std::array<ResourceHandle<StructuredBufferTag>,4> cpuIds{};
        if (!gpu) {
            const auto& frustum=lightFrustum ? *lightFrustum : cameraFrustum;
            for (uint32_t i=0;i<source.rows.size();++i) {
                const auto& sphere=source.rows[i].bounds; if (sphere.w<0) continue;
                const float radius=sphere.w+view.culling.x;
                if (view.counts[3] && !frustum.IntersectsSphere({sphere.x,sphere.y,sphere.z},radius)) continue;
                if (view.metrics.z>0 || view.culling.y>0) {
                    const auto delta=math::Vector3{sphere.x,sphere.y,sphere.z}-ctx.camera.m_position;
                    const float distance=view.culling.z>0.5f ? math::Vector3::Dot(delta,{view.forward.x,view.forward.y,view.forward.z}) : delta.Length();
                    if (view.metrics.z>0 && distance-radius>view.metrics.z) continue;
                    const float screenHeight=2*radius*view.cameraProjection.w/view.metrics.x/(view.metrics.w>0.5f ? 1.0f : std::max(distance,view.metrics.y));
                    if (view.culling.y>0 && distance>0 && screenHeight<view.culling.y) continue;
                }
                visible[CpuLod(view,sphere)].push_back(i);
            }
            if (ctx.settings.gpuInstancing && ctx.resources.Get(source.gpu)) for (uint32_t lod=0;lod<input.lodCount;++lod) if (!visible[lod].empty()) {
                cpuIds[lod]=cache.cpuIds.Acquire(ctx.resources,visible[lod].size(),sizeof(uint32_t));
                ctx.resources.Update(cpuIds[lod],visible[lod].data(),visible[lod].size()*sizeof(uint32_t));
            }
        }
        for (uint32_t partIndex=0;partIndex<input.parts.size();++partIndex) {
            const auto& part=input.parts[partIndex]; const auto& material=part.mesh.material;
            if ((pass == InstancePass::FORWARD || pass == InstancePass::GBUFFER)
                && material.capabilities.blend != BlendMode::OPAQUE_BLEND) continue;
            const bool standard=material.shaderCapabilities.SupportsGBuffer();
            if (forwardPass && !standard && cache.warnedCustom.insert({material.shader.id,material.shader.gen}).second)
                FBZZ_LOG_WARN("Instance rendering: custom shader uses individual CPU draws to preserve its vertex/material contract");
            if (pass==InstancePass::GBUFFER && !standard) continue;
            if (forwardPass && ctx.isDeferred && standard) continue;
            if (!gpu && visible[part.lod].empty()) continue;
            DrawCall draw; draw.vertexBuffer=part.mesh.vertexBuffer; draw.indexBuffer=part.mesh.indexBuffer;
            draw.indexCount=part.mesh.indexCount; draw.vertexCount=part.mesh.vertexCount;
            draw.pipelineState=pass==InstancePass::SHADOW || pass==InstancePass::CAPTURE
                ? GetOrCreateMaterialPSO(ctx.resources,BlendMode::OPAQUE_BLEND,material.doubleSided,material.depthBias,material.depthBiasSlope,false)
                : MaterialPipeline(ctx,material);
            draw.constantBuffers[0]=pass==InstancePass::SHADOW ? lightFrame : ctx.handles.frameCB;
            draw.constantBuffers[1]=ctx.handles.objectCB; draw.constantBuffers[8]=ctx.handles.advancedGraphicsCB;
            draw.constantBuffers[5]=cache.windFrame;
            PerObjectCB identity{}; ctx.resources.Update(ctx.handles.objectCB,&identity,sizeof(identity));
            for (size_t i=0;i<material.textures.size();++i) draw.textures[i]=material.textures[i];
            ctx.resources.Update(cache.material,material.gbufferParams.data(),material.gbufferParams.size());
            draw.constantBuffers[2]=cache.material;
            size_t shader=pass==InstancePass::GBUFFER ? 0u : forwardPass ? 1u : pass==InstancePass::SELECTION ? 4u : pass==InstancePass::VELOCITY ? 5u : (material.surface.alphaCutoff>0 || LeafAnimated(material)) ? 3u : 2u;
            draw.shader=cache.shaders[shader];
            if (forwardPass) {
                draw.constantBuffers[2]=material.paramsBuffer; draw.constantBuffers[3]=ctx.handles.lightCB; draw.constantBuffers[4]=ctx.handles.shadowCB;
                BindForwardShadingResources(draw,ctx); draw.textures[8]=ctx.resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
                draw.textures[16]=ctx.handles.iblIrradiance; draw.textures[17]=ctx.handles.iblPrefilter; draw.textures[18]=ctx.handles.iblBrdfLut;
                if (pass==InstancePass::CAPTURE) {
                    draw.constantBuffers[4]=ctx.handles.reflectionProbeCaptureShadowCB;
                    BindForwardShadingResources(draw,ctx,true);draw.constantBuffers[12]=ctx.handles.reflectionProbeCapturePunctualCB;
                    for (uint32_t slot : {8u,16u,17u,18u,23u,24u,28u,31u}) draw.textures[slot]={};
                }
            }
            if (pass==InstancePass::VELOCITY) draw.textures[7]=ctx.resources.GetDepthTexture(ctx.Res().Target("HDR"));
            const bool instanced=ctx.resources.Get(source.gpu) && ctx.resources.Get(draw.shader)
                && (gpu || ctx.resources.Get(cpuIds[part.lod])) && (!forwardPass || standard);
            if (instanced) {
                draw.instanceBuffer=source.gpu; draw.vsBuffers[0]=gpu ? gpuIds : cpuIds[part.lod];
                draw.instanceCount=gpu ? view.counts[0] : static_cast<uint32_t>(visible[part.lod].size());
                if (gpu) { draw.indirectArguments=gpuArgs; draw.indirectArgumentOffset=partIndex*sizeof(GpuGeometryIndexedArguments); draw.indirectInstanceOffset=true; }
                if (pass==InstancePass::SHADOW) SubmitCountedShadow(ctx,draw); else SubmitCounted(ctx,draw);
            } else {
                draw.shader=pass==InstancePass::GBUFFER ? ctx.handles.gbufferShader : forwardPass ? material.shader
                    : pass==InstancePass::VELOCITY ? cache.shaders[9] : pass==InstancePass::SHADOW ? ((material.surface.alphaCutoff>0 || LeafAnimated(material)) ? cache.shaders[7] : ctx.handles.shadowShader) : cache.shaders[8];
                for (uint32_t slot : visible[part.lod]) {
                    PerObjectCB object{}; object.world=source.rows[slot].world;
                    object.worldInvTranspose=pass==InstancePass::VELOCITY ? source.rows[slot].previousWorld : source.rows[slot].normal;
                    ctx.resources.Update(ctx.handles.objectCB,&object,sizeof(object));
                    if (pass==InstancePass::SHADOW) SubmitCountedShadow(ctx,draw); else SubmitCounted(ctx,draw);
                }
            }
        }
    }
}
} /// @note namespace
void ExecuteInstanceGBuffer(RenderPassContext& ctx) { Execute(ctx,InstancePass::GBUFFER); }
void ExecuteInstanceForward(RenderPassContext& ctx) { Execute(ctx,InstancePass::FORWARD); }
void ExecuteInstanceSelection(RenderPassContext& ctx) { Execute(ctx,InstancePass::SELECTION); }
void ExecuteInstanceVelocity(RenderPassContext& ctx) { Execute(ctx,InstancePass::VELOCITY); }
void ExecuteInstanceCapture(RenderPassContext& ctx,const Camera& camera,uint32_t resolution,uint32_t excludedIndex,uint32_t excludedGeneration)
{
    RenderPassContext capture{static_cast<const RenderLightingInput&>(ctx),ctx.renderer,ctx.resources,camera,ctx.settings,ctx.outputRT,ctx.cullingMask,ctx.handles};
    capture.renderScene=ctx.renderScene;capture.resourceRegistry=ctx.resourceRegistry;capture.width=capture.height=resolution;
    capture.time=ctx.time;capture.frameStamp=ctx.frameStamp;
    Execute(capture,InstancePass::CAPTURE,nullptr,{},(static_cast<uint64_t>(excludedGeneration)<<32)|excludedIndex);
}
void SubmitInstanceShadows(RenderPassContext& ctx,const math::Frustum& frustum,ResourceHandle<ConstantBufferTag> frame) { Execute(ctx,InstancePass::SHADOW,&frustum,frame); }
} /// @note namespace fbzz::renderer
