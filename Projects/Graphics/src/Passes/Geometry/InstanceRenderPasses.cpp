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
#include <Graphics/Pipeline/GpuHiZ.hpp>
#include <Graphics/Pipeline/OcclusionCuller.hpp>
#include <Math/Quaternion.hpp>
#include <Core/Logger.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include "InstanceShadowBatch.hpp"
#include "../../Renderer/GpuCullDrawQueue.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <limits>
#include <optional>

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
    math::Matrix4 terrainViewProjection{};
    std::array<uint32_t,4> terrainDepth{};
    std::array<uint32_t,4> vegetation{};
};
static_assert(sizeof(InstanceView)==304);
constexpr uint32_t TERRAIN_HIZ_SLOT=28;
constexpr uint32_t VEGETATION_SLOTS=27;
constexpr uint32_t MIN_GPU_PLACEMENTS=64;
constexpr uint32_t INSTANCE_GROUP_SIZE=64;
constexpr uint32_t MAX_DISPATCH_X=65535;
struct InstanceBoundsNode {
    math::Vector3 low{}, high{};
    uint32_t first = 0, count = 0, left = 0, right = 0;
};
struct VegetationCell {
    math::Vector3 low, high;
    uint32_t first, count;
};
struct VegetationCandidates {
    uint32_t active=0, frustum=0, distance=0;
    bool partitioned=false;
};
struct InstanceSource {
    ResourceHandle<StructuredBufferTag> gpu;
    ResourceHandle<StructuredBufferTag> vegetationGpu;
    std::vector<ResidentInstanceRecord> rows;
    std::shared_ptr<const InstanceSetData> data;
    math::Matrix4 world=math::Matrix4::Identity();
    math::Vector3 center{};
    float radius=0;
    uint64_t preparedFrame=UINT64_MAX, lastSeen=0;
    std::vector<uint32_t> motionSlots;
    bool moved=false;
    uint32_t activeCount=0;
    math::Vector3 boundsMin{}, boundsMax{};
    bool hasBounds=false, finiteBounds=true;
    std::vector<uint32_t> spatialSlots;
    std::vector<InstanceBoundsNode> spatialNodes;
    std::vector<uint32_t> vegetationSlots;
    std::vector<VegetationCell> vegetationCells;
    bool vegetationPrepared=false;
};
struct InstanceResources {
    uint64_t owner=0, epoch=UINT64_MAX;
    std::map<std::array<uint64_t,2>,InstanceSource> sources;
    std::array<ResourceHandle<ShaderTag>,10> shaders{};
    ResourceHandle<ShaderTag> cascadeCullShader;
    DynamicStructuredBufferPool views{1}, partLods{1}, cpuIds{64};
    DynamicStructuredBufferPool occlusion{OcclusionCuller::kGpuRows};
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
            for (const auto& [identity, source] : cache.sources) {
                owner->Release(source.gpu);
                owner->Release(source.vegetationGpu);
            }
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
        manager.Release(entry.second.gpu);
        manager.Release(entry.second.vegetationGpu);
        return true;
    });
    return cache;
}
bool SameMatrix(const math::Matrix4& a,const math::Matrix4& b) { return std::memcmp(&a,&b,sizeof(a))==0; }
bool SameVector(math::Vector3 a,math::Vector3 b) { return a.x==b.x&&a.y==b.y&&a.z==b.z; }
void UpdateWindFrame(RenderPassContext& ctx,InstanceResources& cache)
{
    if (cache.windStamp != ctx.resources.FrameStamp()) {
        cache.previousWindTime = cache.windStamp == UINT64_MAX ? ctx.time : cache.windTime;
        cache.windTime = ctx.time; cache.windStamp = ctx.resources.FrameStamp();
    }
    PostProcCB windFrame{}; windFrame.time = cache.windTime; windFrame.customParameters[0] = cache.previousWindTime;
    ctx.resources.Update(cache.windFrame,&windFrame,sizeof(windFrame));
}
float LeafWindStrength(const RenderMaterial& material)
{
    float strength = 0; std::memcpy(&strength, material.gbufferParams.data() + 68, sizeof(strength));
    return strength;
}
bool LeafAnimated(const RenderMaterial& material) { return LeafWindStrength(material)>0.0f; }
bool UsesAlphaShadow(const RenderMaterial& material)
{
    /// @note Instance sets carry the normalized material buffer even when surface inputs have not been extracted.
    float alphaCutoff = 0;
    std::memcpy(&alphaCutoff, material.gbufferParams.data() + 64, sizeof(alphaCutoff));
    return alphaCutoff>0.0f || material.surface.alphaCutoff>0.0f || LeafAnimated(material);
}
constexpr float POSITIVE_INFINITY=std::numeric_limits<float>::infinity();
constexpr double MAX_FLOAT=static_cast<double>((std::numeric_limits<float>::max)());
/// @note AᵀA の最大絶対行和は最大伸長の二乗の上界。float の外向き 1 ULP で集計と格納の丸めを吸収する。
/// @see https://nhigham.com/2021/10/12/what-is-a-matrix-norm/ Higham, spectral-radius bound (1) and the A* A norm bound.
float Stretch(const math::Matrix4& world)
{
    for (int row=0;row<3;++row) for (int col=0;col<3;++col)
        if (!std::isfinite(world.m[row][col])) return POSITIVE_INFINITY;
    double maximumRowSum=0;
    for (int a=0;a<3;++a) {
        double rowSum=0;
        for (int b=0;b<3;++b) {
            double gram=0;
            for (int row=0;row<3;++row)
                gram+=static_cast<double>(world.m[row][a])*static_cast<double>(world.m[row][b]);
            rowSum+=std::abs(gram);
        }
        maximumRowSum=std::max(maximumRowSum,rowSum);
    }
    const double stretch=std::sqrt(maximumRowSum);
    if (stretch>MAX_FLOAT) return POSITIVE_INFINITY;
    return stretch>0 ? std::nextafter(static_cast<float>(stretch),POSITIVE_INFINITY) : 0;
}

void PrepareVegetationCells(InstanceSource& source)
{
    source.vegetationPrepared=true;
    if (!source.finiteBounds || !source.hasBounds) return;
    struct CellSlot {
        std::array<float,2> cell;
        uint32_t priority, slot;
    };
    constexpr float CELL_SIZE=128.0f;
    std::vector<CellSlot> sorted;
    sorted.reserve(source.activeCount);
    for (uint32_t slot=0;slot<source.rows.size();++slot) {
        const auto& row=source.rows[slot];
        if (row.bounds.w<0.0f) continue;
        const std::array cell{std::floor(row.world.m[0][3]/CELL_SIZE), std::floor(row.world.m[2][3]/CELL_SIZE)};
        if (!std::isfinite(cell[0]) || !std::isfinite(cell[1])) return;
        /// @note 安定 ID の順位は視点や入力順を問わず、連番の配置を空間的に偏らせない。
        uint32_t priority=source.data->placements[slot].id;
        priority=(priority^(priority>>16u))*0x7feb352du;
        priority=(priority^(priority>>15u))*0x846ca68bu;
        priority^=priority>>16u;
        sorted.push_back({cell,priority,slot});
    }
    std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b) {
        if (a.cell!=b.cell) return a.cell<b.cell;
        if (a.priority!=b.priority) return a.priority<b.priority;
        return a.slot<b.slot;
    });
    source.vegetationSlots.reserve(sorted.size());
    const auto negative=[](float value) { return std::nextafter(value,-POSITIVE_INFINITY); };
    const auto positive=[](float value) { return std::nextafter(value,POSITIVE_INFINITY); };
    for (size_t i=0;i<sorted.size();++i) {
        const auto slot=sorted[i].slot;
        const auto& sphere=source.rows[slot].bounds;
        const math::Vector3 low{negative(sphere.x-sphere.w),negative(sphere.y-sphere.w),negative(sphere.z-sphere.w)};
        const math::Vector3 high{positive(sphere.x+sphere.w),positive(sphere.y+sphere.w),positive(sphere.z+sphere.w)};
        if (i==0 || sorted[i-1].cell!=sorted[i].cell) source.vegetationCells.push_back({low,high,static_cast<uint32_t>(i),1});
        else {
            auto& cell=source.vegetationCells.back();
            cell.low={std::min(cell.low.x,low.x),std::min(cell.low.y,low.y),std::min(cell.low.z,low.z)};
            cell.high={std::max(cell.high.x,high.x),std::max(cell.high.y,high.y),std::max(cell.high.z,high.z)};
            ++cell.count;
        }
        source.vegetationSlots.push_back(slot);
    }
}
template<typename SubmitPending>
InstanceSource* Source(RenderPassContext& ctx,InstanceResources& cache,const RenderInstanceInput& input,SubmitPending&& submitPending)
{
    auto& source=cache.sources[input.identity]; source.lastSeen=ctx.resources.FrameStamp();
    const bool unchanged=source.data && source.data.get()==input.data.get() && source.data->revision==input.data->revision && SameMatrix(source.world,input.world)
        && SameVector(source.center,input.localBoundsCenter) && source.radius==input.localBoundsRadius;
    if (source.preparedFrame==ctx.resources.FrameStamp()) {
        if (unchanged) {
            if (input.vegetationBudget.enabled && !source.vegetationPrepared) PrepareVegetationCells(source);
            return &source;
        }
        /// @note Finish any same-frame use before replacing the resident data for this identity.
        if (!submitPending()) return nullptr;
    }
    std::vector<uint32_t> changed=source.motionSlots; source.motionSlots.clear(); source.moved=false;
    for (uint32_t slot : changed) source.rows[slot].previousWorld=source.rows[slot].world;
    const bool partial=source.data && input.data->parentRevision==source.data->revision && source.rows.size()<=input.data->placements.size()
        && SameMatrix(source.world,input.world) && SameVector(source.center,input.localBoundsCenter) && source.radius==input.localBoundsRadius;
    if (!unchanged) {
        source.spatialSlots.clear(); source.spatialNodes.clear();
        source.vegetationSlots.clear(); source.vegetationCells.clear(); source.vegetationPrepared=false;
        ctx.resources.Release(source.vegetationGpu); source.vegetationGpu={};
        if (partial) changed.insert(changed.end(),input.data->dirtySlots.begin(),input.data->dirtySlots.end());
        else { changed.resize(input.data->placements.size()); for (size_t i=0;i<changed.size();++i) changed[i]=static_cast<uint32_t>(i); }
        std::sort(changed.begin(),changed.end()); changed.erase(std::unique(changed.begin(),changed.end()),changed.end());
        const size_t previousCount=source.rows.size();
        if (!partial) { source.activeCount=0; source.hasBounds=false; source.finiteBounds=true; }
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
            float radius=-1;
            if (placement.active) {
                /// @note float 同士の積も外接境界を縮めないよう double で求め、格納時だけ外向きへ丸める。
                const double exactRadius=static_cast<double>(input.localBoundsRadius)*static_cast<double>(Stretch(world));
                radius=!std::isfinite(exactRadius) || exactRadius<0 || exactRadius>MAX_FLOAT
                    ? POSITIVE_INFINITY : static_cast<float>(exactRadius);
                if (static_cast<double>(radius)<exactRadius) radius=std::nextafter(radius,POSITIVE_INFINITY);
            }
            row.bounds={center.x,center.y,center.z,radius};
            if (placement.active) {
                if (!std::isfinite(row.bounds.x) || !std::isfinite(row.bounds.y)
                    || !std::isfinite(row.bounds.z) || !std::isfinite(row.bounds.w) || row.bounds.w<=0) source.finiteBounds=false;
                else {
                    const math::Vector3 low{center.x-row.bounds.w,center.y-row.bounds.w,center.z-row.bounds.w};
                    const math::Vector3 high{center.x+row.bounds.w,center.y+row.bounds.w,center.z+row.bounds.w};
                    if (!source.hasBounds) { source.boundsMin=low; source.boundsMax=high; source.hasBounds=true; }
                    else {
                        source.boundsMin={std::min(source.boundsMin.x,low.x),std::min(source.boundsMin.y,low.y),std::min(source.boundsMin.z,low.z)};
                        source.boundsMax={std::max(source.boundsMax.x,high.x),std::max(source.boundsMax.y,high.y),std::max(source.boundsMax.z,high.z)};
                    }
                }
            }
            if (placement.active && !SameMatrix(old,world)) { source.motionSlots.push_back(slot); source.moved=true; }
        }
        source.world=input.world; source.center=input.localBoundsCenter; source.radius=input.localBoundsRadius; source.data=input.data;
    }
    if (input.vegetationBudget.enabled && !source.vegetationPrepared) PrepareVegetationCells(source);
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
            used-=ctx.resources.Get(oldest->second.gpu)->GetSize();
            ctx.resources.Release(oldest->second.gpu); oldest->second.gpu={};
            ctx.resources.Release(oldest->second.vegetationGpu); oldest->second.vegetationGpu={};
        }
        if (used+required<=RESIDENT_GPU_BUDGET) source.gpu=ctx.resources.CreateRWStructuredBuffer(nullptr,capacity,sizeof(ResidentInstanceRecord));
        else FBZZ_LOG_WARN("Instance rendering: resident GPU budget exceeded; using CPU fallback");
        changed.resize(source.rows.size()); for (size_t i=0;i<changed.size();++i) changed[i]=static_cast<uint32_t>(i);
    }
    std::sort(changed.begin(),changed.end()); changed.erase(std::unique(changed.begin(),changed.end()),changed.end());
    if (!ctx.resources.Get(source.gpu)) return &source;
    for (size_t i=0;i<changed.size();) {
        size_t end=i+1; while (end<changed.size() && changed[end]==changed[end-1]+1) ++end;
        const size_t bytes=(end-i)*sizeof(ResidentInstanceRecord), offset=changed[i]*sizeof(ResidentInstanceRecord);
        if (!ctx.resources.UpdateRegion(source.gpu,source.rows.data()+changed[i],bytes,offset)) { ctx.resources.Release(source.gpu); source.gpu={}; break; }
        ctx.statsInstanceUploadBytes+=bytes; i=end;
    }
    return &source;
}
void SetFrustumPlanes(InstanceView& view,const math::Frustum& frustum)
{
    for (size_t i=0;i<view.planes.size();++i) {
        const auto& plane=frustum.GetPlane(static_cast<int>(i));
        view.planes[i]={plane.normal.x,plane.normal.y,plane.normal.z,plane.distance};
    }
}
InstanceView View(const RenderPassContext& ctx,const RenderInstanceInput& input,const math::Frustum& frustum,bool shadow)
{
    InstanceView result;
    SetFrustumPlanes(result,frustum);
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
    result.culling={std::max(ctx.cullingBoundsPadding,0.0f),shadow ? 0.0f : ctx.smallObjectScreenHeight,ctx.cullDistanceSpherical ? 0.0f : 1.0f,
        !shadow && IsViewOcclusionCurrent(ctx) ? 1.0f : 0.0f};
    if (input.vegetationBudget.enabled) {
        const auto& budget=input.vegetationBudget;
        result.vegetation=shadow ? std::array{0u,0u,budget.shadowDetailPerCell,0u}
            : std::array{budget.drawDetailPerCell,budget.nearDetailPerCell,0u,0u};
    }
    for (uint32_t i=0;i<input.parts.size();++i) if (result.firstParts[input.parts[i].lod]==UINT32_MAX) result.firstParts[input.parts[i].lod]=i;
    return result;
}
/// @note Partial edits only expand the cached union; retaining old extents cannot hide a live placement.
/// @see Docs/design/instanced-mesh.md
VisibilityResult BoundsVisibility(math::Vector3 lowBound,math::Vector3 highBound,const InstanceView& view,const math::Frustum& frustum)
{
    const auto center=lowBound*0.5f+highBound*0.5f;
    const float padding=view.culling.x;
    const auto extents=(highBound-lowBound)*0.5f+math::Vector3{padding,padding,padding};
    if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z)
        || !std::isfinite(extents.x) || !std::isfinite(extents.y) || !std::isfinite(extents.z)) return VisibilityResult::VISIBLE;
    if (view.counts[3] && !frustum.IntersectsAABB(center,extents)) return VisibilityResult::FRUSTUM_CULLED;
    if (view.metrics.z<=0) return VisibilityResult::VISIBLE;
    const math::Vector3 camera{view.cameraProjection.x,view.cameraProjection.y,view.cameraProjection.z};
    if (view.culling.z>0.5f) {
        const math::Vector3 forward{view.forward.x,view.forward.y,view.forward.z};
        const float reach=std::abs(forward.x)*extents.x+std::abs(forward.y)*extents.y+std::abs(forward.z)*extents.z;
        return math::Vector3::Dot(center-camera,forward)-reach>view.metrics.z
            ? VisibilityResult::DISTANCE_CULLED : VisibilityResult::VISIBLE;
    }
    const auto low=center-extents,high=center+extents;
    const math::Vector3 nearest{std::clamp(camera.x,low.x,high.x),std::clamp(camera.y,low.y,high.y),std::clamp(camera.z,low.z,high.z)};
    return (nearest-camera).Length()>view.metrics.z ? VisibilityResult::DISTANCE_CULLED : VisibilityResult::VISIBLE;
}
VisibilityResult SourceVisibility(const InstanceSource& source,const InstanceView& view,const math::Frustum& frustum)
{
    return source.hasBounds && source.finiteBounds
        ? BoundsVisibility(source.boundsMin,source.boundsMax,view,frustum) : VisibilityResult::VISIBLE;
}

VisibilityResult CellVisibility(const VegetationCell& cell,const InstanceView& view,std::span<const math::Frustum> frusta)
{
    for (const auto& frustum : frusta) {
        const auto visibility=BoundsVisibility(cell.low,cell.high,view,frustum);
        if (visibility!=VisibilityResult::FRUSTUM_CULLED) return visibility;
    }
    return VisibilityResult::FRUSTUM_CULLED;
}

VegetationCandidates PrepareVegetationDispatch(const InstanceSource& source,const InstanceView& view,
    std::span<const math::Frustum> frusta,std::vector<uint32_t>& parameters)
{
    FBZZ_PROFILE_SCOPE("InstanceVisibility::VegetationCells");
    VegetationCandidates result;
    result.partitioned=true;
    const size_t maxGroups=source.vegetationCells.size()+(source.activeCount+INSTANCE_GROUP_SIZE-1)/INSTANCE_GROUP_SIZE;
    parameters.reserve(parameters.size()+3*maxGroups);
    for (const auto& cell : source.vegetationCells) {
        const auto visibility=CellVisibility(cell,view,frusta);
        if (visibility==VisibilityResult::FRUSTUM_CULLED) result.frustum+=cell.count;
        else if (visibility==VisibilityResult::DISTANCE_CULLED) result.distance+=cell.count;
        else {
            result.active+=cell.count;
            for (uint32_t rank=0;rank<cell.count;rank+=INSTANCE_GROUP_SIZE) {
                parameters.push_back(cell.first+rank);
                parameters.push_back(std::min(INSTANCE_GROUP_SIZE,cell.count-rank));
                parameters.push_back(rank);
            }
        }
    }
    return result;
}

/// @note 葉の変位を含む常駐球を包む。個体・owner・モデル境界の変更時に破棄し、ビューの可視結果は保存しない。
void PrepareSpatialBounds(InstanceSource& source)
{
    if (!source.spatialNodes.empty()) return;
    for (uint32_t i = 0; i < source.rows.size(); ++i) if (source.rows[i].bounds.w >= 0) source.spatialSlots.push_back(i);
    if (source.spatialSlots.empty()) return;
    source.spatialNodes.reserve(source.spatialSlots.size() / 8 + 1);
    const auto build = [&](auto&& self,uint32_t first,uint32_t count) -> uint32_t {
        const uint32_t index = uint32_t(source.spatialNodes.size());
        InstanceBoundsNode node; node.first = first; node.count = count;
        for (uint32_t i = first; i < first + count; ++i) {
            const auto b = source.rows[source.spatialSlots[i]].bounds;
            const math::Vector3 low{std::nextafter(b.x - b.w,-INFINITY),std::nextafter(b.y - b.w,-INFINITY),std::nextafter(b.z - b.w,-INFINITY)};
            const math::Vector3 high{std::nextafter(b.x + b.w,INFINITY),std::nextafter(b.y + b.w,INFINITY),std::nextafter(b.z + b.w,INFINITY)};
            if (i == first) { node.low = low; node.high = high; }
            else {
                node.low = {std::min(node.low.x,low.x),std::min(node.low.y,low.y),std::min(node.low.z,low.z)};
                node.high = {std::max(node.high.x,high.x),std::max(node.high.y,high.y),std::max(node.high.z,high.z)};
            }
        }
        source.spatialNodes.push_back(node);
        if (count > 32) {
            const auto extents = node.high - node.low;
            const int axis = extents.y > extents.x && extents.y > extents.z ? 1 : extents.z > extents.x ? 2 : 0;
            const auto coordinate = [&](uint32_t slot) { const auto b = source.rows[slot].bounds; return axis == 0 ? b.x : axis == 1 ? b.y : b.z; };
            const uint32_t half = count / 2;
            std::nth_element(source.spatialSlots.begin() + first,source.spatialSlots.begin() + first + half,source.spatialSlots.begin() + first + count,
                [&](uint32_t a,uint32_t b) { return coordinate(a) < coordinate(b); });
            const auto left = self(self,first,half), right = self(self,first + half,count - half);
            source.spatialNodes[index].left = left; source.spatialNodes[index].right = right;
        }
        return index;
    };
    build(build,0,uint32_t(source.spatialSlots.size()));
}
void CountRejected(RenderPassContext& ctx,VisibilityResult result,int count)
{
    switch (result) {
    case VisibilityResult::FRUSTUM_CULLED: ctx.statsFrustumCulled+=count; break;
    case VisibilityResult::DISTANCE_CULLED: ctx.statsDistanceCulled+=count; break;
    case VisibilityResult::SMALL_OBJECT_CULLED: ctx.statsSmallObjectCulled+=count; break;
    case VisibilityResult::VISIBLE: break;
    }
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
uint32_t VegetationLod(const InstanceView& view,uint32_t lod,uint32_t rank)
{
    const uint32_t last=view.counts[1]-1;
    const auto& limits=view.vegetation;
    /// @note LOD3 can be a distant billboard. Leave it to the projected-size test for shadow casters.
    if (limits[2]!=0 && rank>=limits[2]) return std::max(lod,std::min(2u,last));
    if (limits[0]!=0 && rank>=limits[0]) return std::max(lod,std::min(2u,last));
    if (limits[1]!=0 && rank>=limits[1]) return std::max(lod,std::min(1u,last));
    return lod;
}
bool HasValidInput(const RenderPassContext& ctx,const RenderInstanceInput& input)
{
    if (!input.data || input.lodCount==0 || input.lodCount>4 || input.layer>=32 || (ctx.cullingMask&(1u<<input.layer))==0
        || input.parts.empty() || input.parts.size()>64 || input.data->placements.size()>1048576) return false;
    uint32_t lodMask=0;
    for (const auto& part : input.parts) {
        if (part.lod>=input.lodCount) return false;
        lodMask|=1u<<part.lod;
    }
    return lodMask==(1u<<input.lodCount)-1;
}
bool HasHiZMaterials(const RenderInstanceInput& input)
{
    bool hasOpaque=false;
    for (const auto& part : input.parts) {
        const auto& material=part.mesh.material;
        if (!material.shaderCapabilities.SupportsGBuffer()) return false;
        if (material.capabilities.blend!=BlendMode::OPAQUE_BLEND) continue;
        /// @note A depth-biased receiver may rasterize nearer than its geometric bounds.
        if (material.depthBias!=0 || material.depthBiasSlope!=0.0f || !std::isfinite(LeafWindStrength(material))) return false;
        hasOpaque=true;
    }
    return hasOpaque;
}
bool IsTerrainHiZCandidate(const RenderInstanceInput& input)
{
    return !input.selectionOnly && input.data->placements.size()>=MIN_GPU_PLACEMENTS && HasHiZMaterials(input);
}
bool SupportsTerrainHiZMode(const RenderPassContext& ctx)
{
    const auto mode=ctx.settings.geometryDrawMode;
    return ctx.occlusionCullingEnabled && ctx.settings.bulkInstanceGpuCulling && ctx.settings.gpuInstancing
        && !ctx.settings.IsWireframe() && ctx.renderer.SupportsIndirectDraw()
        && (mode==GeometryDrawMode::GPU_INDIRECT || mode==GeometryDrawMode::MESH_SHADER);
}
bool CanPrepareGpu(RenderPassContext& ctx,const InstanceResources& cache,const RenderInstanceInput& input,
    const InstanceSource& source,uint32_t slotCount)
{
    if (!ctx.settings.bulkInstanceGpuCulling || !ctx.settings.gpuInstancing || !ctx.renderer.SupportsIndirectDraw()
        || !ctx.resources.Get(source.gpu) || slotCount<MIN_GPU_PLACEMENTS || !ctx.resources.Get(cache.shaders[6])) return false;
    /// @note Native Profiler measurements favor CPU-visible-ID instancing for small sets; explicit Indirect/Mesh mode keeps the GPU comparison available.
    if ((ctx.settings.geometryDrawMode==GeometryDrawMode::CPU
        || ctx.settings.geometryDrawMode==GeometryDrawMode::GPU_INSTANCE_INDIRECT) && slotCount<32768) return false;
    return std::all_of(input.parts.begin(),input.parts.end(),[](const auto& part) {
        return part.mesh.material.shaderCapabilities.SupportsGBuffer();
    });
}
std::optional<ComputeCall> PrepareGpuBuffers(RenderPassContext& ctx,InstanceResources& cache,const RenderInstanceInput& input,
    InstanceSource& source,InstanceView& view,std::span<const math::Frustum> frusta,ResourceHandle<ShaderTag> shader,
    VegetationCandidates* candidateCounts=nullptr)
{
    const uint32_t viewCount=static_cast<uint32_t>(frusta.size());
    std::vector<uint32_t> parameters;
    parameters.reserve(input.parts.size());
    for (const auto& part : input.parts) parameters.push_back(part.lod);
    if (input.vegetationBudget.enabled && !source.vegetationCells.empty()) {
        const auto candidates=PrepareVegetationDispatch(source,view,frusta,parameters);
        if (candidates.active==0) return {};
        if (!ctx.resources.Get(source.vegetationGpu))
            source.vegetationGpu=ctx.resources.CreateGpuLocalStructuredBuffer(source.vegetationSlots.data(),
                static_cast<uint32_t>(source.vegetationSlots.size()),sizeof(uint32_t));
        if (!ctx.resources.Get(source.vegetationGpu)) return {};
        if (candidateCounts) *candidateCounts=candidates;
        view.vegetation[3]=static_cast<uint32_t>((parameters.size()-input.parts.size())/3);
        view.counts[0]=candidates.active;
    }
    const uint32_t idsPerView=view.counts[0]*view.counts[1];
    std::vector<GpuGeometryIndexedArguments> initial;
    initial.reserve(input.parts.size()*viewCount);
    for (uint32_t viewIndex=0;viewIndex<viewCount;++viewIndex) {
        const uint32_t firstId=viewIndex*idsPerView;
        for (const auto& part : input.parts)
            initial.push_back({firstId+part.lod*view.counts[0],part.mesh.indexCount,0,0,0,0});
    }
    const auto ids=cache.ids.Acquire(ctx.resources,static_cast<size_t>(idsPerView)*viewCount,sizeof(uint32_t));
    const auto args=cache.arguments.Acquire(ctx.resources,initial.size(),sizeof(GpuGeometryIndexedArguments));
    const auto viewBuffer=cache.views.Acquire(ctx.resources,viewCount,sizeof(InstanceView));
    const auto partsBuffer=cache.partLods.Acquire(ctx.resources,parameters.size(),sizeof(uint32_t));
    if (!ctx.resources.Get(ids)||!ctx.resources.Get(args)||!ctx.resources.Get(viewBuffer)||!ctx.resources.Get(partsBuffer)) return {};
    ctx.resources.Update(partsBuffer,parameters.data(),parameters.size()*sizeof(uint32_t));
    ctx.resources.Update(args,initial.data(),initial.size()*sizeof(GpuGeometryIndexedArguments));
    ComputeCall compute;
    compute.shader=shader;
    compute.srvBuffers[14]=source.gpu; compute.srvBuffers[15]=viewBuffer; compute.srvBuffers[29]=partsBuffer;
    compute.uavBuffers={ids,args}; compute.dispatchX=(view.counts[0]+63)/64;
    if (view.vegetation[3]!=0) {
        compute.srvBuffers[VEGETATION_SLOTS]=source.vegetationGpu;
        compute.dispatchX=std::min(view.vegetation[3],MAX_DISPATCH_X);
        compute.dispatchY=(view.vegetation[3]+MAX_DISPATCH_X-1)/MAX_DISPATCH_X;
    }
    return compute;
}
std::optional<ComputeCall> PrepareGpu(RenderPassContext& ctx,InstanceResources& cache,const RenderInstanceInput& input,
    InstanceSource& source,InstanceView view,const math::Frustum& frustum,const GpuHiZ* hierarchy,const RenderScene* scene,
    const math::Matrix4& viewProjection,VegetationCandidates& candidateCounts)
{
    if (!CanPrepareGpu(ctx,cache,input,source,view.counts[0])) return {};
    auto compute=PrepareGpuBuffers(ctx,cache,input,source,view,std::span{&frustum,1},cache.shaders[6],&candidateCounts);
    if (!compute) return {};
    const auto currentHiZ=[&] {
        return hierarchy && hierarchy->reversedZ && hierarchy->width==ctx.width && hierarchy->height==ctx.height
            && hierarchy->width!=0 && hierarchy->height!=0 && SupportsTerrainHiZMode(ctx)
            && ctx.resources.Get(hierarchy->depths) && ctx.renderScene.get()==scene && IsViewOcclusionCurrent(ctx);
    };
    const bool useHiZ=hierarchy && HasHiZMaterials(input) && currentHiZ();
    if (useHiZ) {
        view.terrainViewProjection=viewProjection;
        view.terrainDepth={hierarchy->width,hierarchy->height,0,0};
        view.culling.w=0.0f;
        compute->srvBuffers[TERRAIN_HIZ_SLOT]=hierarchy->depths;
    } else if (view.culling.w > 0) {
        if (!IsViewOcclusionCurrent(ctx)) return {};
        if (!ctx.resources.Get(ctx.occlusionGpuRows)) {
            std::array<math::Vector4, OcclusionCuller::kGpuRows> rows;
            ctx.occlusionCuller->CopyGpuRows(rows);
            ctx.occlusionGpuRows=cache.occlusion.Acquire(ctx.resources,rows.size(),sizeof(math::Vector4));
            if (!ctx.resources.Get(ctx.occlusionGpuRows)) return {};
            ctx.resources.Update(ctx.occlusionGpuRows,rows.data(),sizeof(rows));
        }
        compute->srvBuffers[30]=ctx.occlusionGpuRows;
    }
    ctx.resources.Update(compute->srvBuffers[15],&view,sizeof(view));
    /// @note Buffer allocation/update may pump a callback that retires the prepared snapshot or view.
    if (useHiZ ? !currentHiZ() : view.culling.w>0 && !IsViewOcclusionCurrent(ctx)) return {};
    return compute;
}
struct InstanceSetSelection {
    ResourceHandle<StructuredBufferTag> ids,arguments;
    uint32_t firstArgument=0;
    VegetationCandidates vegetation;
};
struct InstanceSubmission {
    RenderPassContext& ctx;
    InstanceResources& cache;
    InstancePass pass;
    const math::Frustum& frustum;
    ResourceHandle<ConstantBufferTag> lightFrame;
    bool countStatistics;
};
void SubmitSet(const InstanceSubmission& submission,const RenderInstanceInput& input,InstanceSource& source,
    const InstanceView& view,const InstanceSetSelection* selection,OcclusionCuller* occlusion)
{
    auto& [ctx,cache,pass,frustum,lightFrame,countStatistics]=submission;
    const bool forwardPass=pass==InstancePass::FORWARD || pass==InstancePass::CAPTURE;
    const float cardWireframe = pass == InstancePass::FORWARD && ctx.settings.IsWireframe()
        ? (ctx.settings.IsUnlit() ? 2.0f : 1.0f) : 0.0f;
    const bool gpu=selection!=nullptr;
    /// @note GPU visibility stays on the GPU; unresolved candidates must not be reported as visible CPU objects.
    /// @see Docs/design/instanced-mesh.md カリング統計の完全性。
    if (gpu && countStatistics) {
        const auto& candidates=selection->vegetation;
        ctx.statsGpuVisibilityUnknownObjects+=static_cast<int>(candidates.partitioned ? candidates.active : source.activeCount);
        ctx.statsFrustumCulled+=static_cast<int>(candidates.frustum);
        ctx.statsDistanceCulled+=static_cast<int>(candidates.distance);
    }
    std::array<std::vector<uint32_t>,4> visible;
    std::array<ResourceHandle<StructuredBufferTag>,4> cpuIds{};
    if (!gpu) {
        FBZZ_PROFILE_SCOPE("InstanceVisibility::CpuSelection");
        const auto select = [&](uint32_t i,uint32_t rank=0) {
            const auto& sphere=source.rows[i].bounds; if (sphere.w<0) return;
            const float radius=sphere.w+view.culling.x;
            if (view.counts[3] && !frustum.IntersectsSphere({sphere.x,sphere.y,sphere.z},radius)) {
                if (countStatistics) CountRejected(ctx,VisibilityResult::FRUSTUM_CULLED,1);
                return;
            }
            if (view.metrics.z>0 || view.culling.y>0) {
                const auto delta=math::Vector3{sphere.x-view.cameraProjection.x,sphere.y-view.cameraProjection.y,sphere.z-view.cameraProjection.z};
                const float distance=view.culling.z>0.5f ? math::Vector3::Dot(delta,{view.forward.x,view.forward.y,view.forward.z}) : delta.Length();
                if (view.metrics.z>0 && distance-radius>view.metrics.z) {
                    if (countStatistics) CountRejected(ctx,VisibilityResult::DISTANCE_CULLED,1);
                    return;
                }
                const float screenHeight=2*radius*view.cameraProjection.w/view.metrics.x/(view.metrics.w>0.5f ? 1.0f : std::max(distance,view.metrics.y));
                if (view.culling.y>0 && distance>0 && screenHeight<view.culling.y) {
                    if (countStatistics) CountRejected(ctx,VisibilityResult::SMALL_OBJECT_CULLED,1);
                    return;
                }
            }
            if (occlusion && !occlusion->TestAndRaster({sphere.x,sphere.y,sphere.z}, radius)) {
                if (countStatistics) ++ctx.statsOcclusionCulled;
                return;
            }
            visible[VegetationLod(view,CpuLod(view,sphere),rank)].push_back(i);
        };
        if (input.vegetationBudget.enabled && !source.vegetationCells.empty()) {
            FBZZ_PROFILE_SCOPE("InstanceVisibility::VegetationCells");
            for (const auto& cell : source.vegetationCells) {
                const auto result=BoundsVisibility(cell.low,cell.high,view,frustum);
                if (result!=VisibilityResult::VISIBLE) {
                    if (countStatistics) CountRejected(ctx,result,static_cast<int>(cell.count));
                    continue;
                }
                for (uint32_t rank=0;rank<cell.count;++rank) select(source.vegetationSlots[cell.first+rank],rank);
            }
            for (auto& ids : visible) std::sort(ids.begin(),ids.end());
        } else if (occlusion && source.activeCount >= 512 && source.hasBounds && source.finiteBounds) {
            PrepareSpatialBounds(source);
            const math::Vector3 padding{view.culling.x,view.culling.x,view.culling.x};
            const auto visit = [&](auto&& self,uint32_t index) -> void {
                const auto& node = source.spatialNodes[index];
                const auto result = BoundsVisibility(node.low,node.high,view,frustum);
                if (result != VisibilityResult::VISIBLE) {
                    if (countStatistics) CountRejected(ctx,result,int(node.count));
                    return;
                }
                if (!occlusion->TestAabb(node.low - padding,node.high + padding)) {
                    if (countStatistics) ctx.statsOcclusionCulled += int(node.count);
                    return;
                }
                if (node.left) { self(self,node.left); self(self,node.right); }
                else for (uint32_t i = node.first; i < node.first + node.count; ++i) select(source.spatialSlots[i]);
            };
            visit(visit,0);
            constexpr uint32_t BITS_PER_WORD = std::numeric_limits<uint64_t>::digits;
            const size_t wordCount = (source.rows.size() + BITS_PER_WORD - 1) / BITS_PER_WORD;
            for (auto& ids : visible) {
                /// @note 少数の可視個体に対して全スロットのビット列を走査するコストを避ける。
                if (ids.size() < BITS_PER_WORD || ids.size() < wordCount) {
                    std::sort(ids.begin(),ids.end());
                    continue;
                }
                /// @note BVH の葉は各スロットを一度だけ列挙する。昇順を保ち、callback のない範囲で LOD 間の作業領域を再利用する。
                thread_local std::vector<uint64_t> words;
                words.assign(wordCount,0);
                for (uint32_t slot : ids) words[slot / BITS_PER_WORD] |= uint64_t{1} << (slot % BITS_PER_WORD);
                size_t output = 0;
                for (size_t word = 0; word < wordCount; ++word) {
                    for (uint64_t bits = words[word]; bits != 0; bits &= bits - 1)
                        ids[output++] = static_cast<uint32_t>(word * BITS_PER_WORD + std::countr_zero(bits));
                }
            }
        } else for (uint32_t i=0;i<source.rows.size();++i) select(i);
    }
    if (!gpu && ctx.settings.gpuInstancing && ctx.resources.Get(source.gpu)) for (uint32_t lod=0;lod<input.lodCount;++lod) if (!visible[lod].empty()) {
        cpuIds[lod]=cache.cpuIds.Acquire(ctx.resources,visible[lod].size(),sizeof(uint32_t));
        ctx.resources.Update(cpuIds[lod],visible[lod].data(),visible[lod].size()*sizeof(uint32_t));
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
        PerObjectCB identity{};
        identity.objectParams.y = cardWireframe;
        ctx.resources.Update(ctx.handles.objectCB,&identity,sizeof(identity));
        for (size_t i=0;i<material.textures.size();++i) draw.textures[i]=material.textures[i];
        ctx.resources.Update(cache.material,material.gbufferParams.data(),material.gbufferParams.size());
        draw.constantBuffers[2]=cache.material;
        size_t shader=pass==InstancePass::GBUFFER ? 0u : forwardPass ? 1u : pass==InstancePass::SELECTION ? 4u : pass==InstancePass::VELOCITY ? 5u : UsesAlphaShadow(material) ? 3u : 2u;
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
            draw.instanceBuffer=source.gpu; draw.vsBuffers[0]=gpu ? selection->ids : cpuIds[part.lod];
            draw.instanceCount=gpu ? view.counts[0] : static_cast<uint32_t>(visible[part.lod].size());
            if (gpu) { draw.indirectArguments=selection->arguments; draw.indirectArgumentOffset=(selection->firstArgument+partIndex)*sizeof(GpuGeometryIndexedArguments); draw.indirectInstanceOffset=true; }
            if (pass==InstancePass::SHADOW) SubmitCountedShadow(ctx,draw); else SubmitCounted(ctx,draw);
        } else {
            draw.shader=pass==InstancePass::GBUFFER ? ctx.handles.gbufferShader : forwardPass ? material.shader
                : pass==InstancePass::VELOCITY ? cache.shaders[9] : pass==InstancePass::SHADOW ? (UsesAlphaShadow(material) ? cache.shaders[7] : ctx.handles.shadowShader) : cache.shaders[8];
            for (uint32_t slot : visible[part.lod]) {
                PerObjectCB object{}; object.world=source.rows[slot].world;
                object.worldInvTranspose=pass==InstancePass::VELOCITY ? source.rows[slot].previousWorld : source.rows[slot].normal;
                object.objectParams.y = cardWireframe;
                ctx.resources.Update(ctx.handles.objectCB,&object,sizeof(object));
                if (pass==InstancePass::SHADOW) SubmitCountedShadow(ctx,draw); else SubmitCounted(ctx,draw);
            }
        }
    }
}
void Execute(RenderPassContext& ctx,InstancePass pass,const math::Frustum* lightFrustum=nullptr,ResourceHandle<ConstantBufferTag> lightFrame={},
    uint64_t excludedOwner=UINT64_MAX,const GpuHiZ* hierarchy=nullptr,bool enableGpu=true,bool preserveShadowDetail=false)
{
    const auto scene=ctx.renderScene;
    if (!scene || scene->instances.empty()) return;
    auto& cache=Resources(ctx.resources);
    const auto resourceOwner=cache.owner;
    const auto resourceEpoch=cache.epoch;
    const auto resourceFrame=ctx.resources.FrameStamp();
    const auto resourcesCurrent=[&] {
        return cache.owner==resourceOwner && cache.epoch==resourceEpoch && ctx.resources.GetResetVersion()==resourceEpoch
            && ctx.resources.FrameStamp()==resourceFrame;
    };
    UpdateWindFrame(ctx,cache);
    auto frame=MakeCameraFrameCB(ctx.camera,ctx.taaJitterNdcX,ctx.taaJitterNdcY);
    const bool forwardPass=pass==InstancePass::FORWARD || pass==InstancePass::CAPTURE;
    const bool splitTerrainHiZ=ctx.terrainHiZActive
        && ((pass==InstancePass::GBUFFER && ctx.isDeferred) || (pass==InstancePass::FORWARD && !ctx.isDeferred));
    if (pass==InstancePass::CAPTURE) {
        frame.projection=ctx.camera.GetProjectionMatrix();frame.viewProjection=frame.projection*frame.view;
        frame.invViewProjection=math::Matrix4::Inverse(frame.viewProjection);
    }
    const auto cameraFrustum=math::Frustum::FromViewProjection(ctx.camera.GetViewProjection());
    const auto& frustum=lightFrustum ? *lightFrustum : cameraFrustum;
    const bool countStatistics=pass==InstancePass::GBUFFER || (pass==InstancePass::FORWARD && !ctx.isDeferred);
    const auto* preparedCuller=ctx.occlusionPreparedCuller;
    const auto cullerVersion=ctx.occlusionPreparedCullerVersion;
    const auto occlusionFrame=ctx.occlusionPreparedResourceFrame;
    const auto occlusionEpoch=ctx.occlusionPreparedResourceEpoch;
    const auto sameOcclusionPublication=[&] {
        return ctx.renderScene.get()==scene.get() && ctx.occlusionPreparedCuller==preparedCuller
            && ctx.occlusionPreparedCullerVersion==cullerVersion && ctx.occlusionPreparedResourceFrame==occlusionFrame
            && ctx.occlusionPreparedResourceEpoch==occlusionEpoch;
    };
    const auto occlusionCurrent=[&] { return sameOcclusionPublication() && IsViewOcclusionCurrent(ctx); };
    if (pass!=InstancePass::SHADOW) ctx.resources.Update(ctx.handles.frameCB,&frame,sizeof(frame));
    const InstanceSubmission submission{ctx,cache,pass,frustum,lightFrame,countStatistics};
    const auto submit=[&](const RenderInstanceInput& input,InstanceSource& source,const InstanceView& view,
        const ComputeCall* selection,const VegetationCandidates& vegetation) {
        if (selection) {
            const InstanceSetSelection selected{selection->uavBuffers[0],selection->uavBuffers[1],0,vegetation};
            SubmitSet(submission,input,source,view,&selected,nullptr);
        } else {
            auto* occlusion=view.culling.w>0 && occlusionCurrent() ? ctx.occlusionCuller : nullptr;
            SubmitSet(submission,input,source,view,nullptr,occlusion);
        }
    };
    struct PendingSet {
        const RenderInstanceInput* input;
        InstanceSource* source;
        InstanceView view;
        size_t selection;
        VegetationCandidates vegetation;
    };
    constexpr size_t NO_SELECTION=SIZE_MAX;
    std::vector<PendingSet> pending;
    std::vector<ComputeCall> dispatches;
    const auto usesOcclusion=[](const ComputeCall& call) {
        return call.srvBuffers[TERRAIN_HIZ_SLOT].IsValid() || call.srvBuffers[30].IsValid();
    };
    const auto submitPending=[&] {
        if (!resourcesCurrent()) return false;
        if (dispatches.empty()) return true;
        const bool needsProof=std::any_of(dispatches.begin(),dispatches.end(),usesOcclusion);
        std::vector<uint8_t> recorded(dispatches.size());
        if (!needsProof || occlusionCurrent()) {
            const bool hasHiZ=std::any_of(dispatches.begin(),dispatches.end(),[](const auto& call) {
                return call.srvBuffers[TERRAIN_HIZ_SLOT].IsValid();
            });
            if (hasHiZ) {
                FBZZ_PROFILE_SCOPE("InstanceVisibility::GpuTerrainHiZ");
                gpu_cull_detail::TryDispatchSequence(ctx.renderer,ctx.resources,dispatches,recorded);
            } else gpu_cull_detail::TryDispatchSequence(ctx.renderer,ctx.resources,dispatches,recorded);
        }
        for (const auto& set : pending) {
            /// @note A retired resource frame cannot receive either the indirect draw or its fallback.
            if (!resourcesCurrent()) return false;
            const ComputeCall* selection=nullptr;
            if (set.selection!=NO_SELECTION && recorded[set.selection]) {
                const auto& call=dispatches[set.selection];
                /// @note The dispatch wait and earlier draw setup can pump callbacks that retire the view proof.
                if (!usesOcclusion(call) || occlusionCurrent()) selection=&call;
                if (selection && call.srvBuffers[TERRAIN_HIZ_SLOT].IsValid()) {
                    FBZZ_PROFILE_SCOPE("InstanceVisibility::GpuTerrainHiZSubmitted");
                }
            }
            submit(*set.input,*set.source,set.view,selection,set.vegetation);
        }
        dispatches.clear();
        pending.clear();
        return resourcesCurrent();
    };
    for (const auto& input : scene->instances) {
        if (input.selectionOnly && pass != InstancePass::SELECTION) continue;
        if (pass==InstancePass::CAPTURE && input.identity[1]==excludedOwner) continue;
        if (!HasValidInput(ctx,input)) continue;
        /// @note Keep the early/late partition independent of hierarchy creation and resource readiness so the late fallback still draws every deferred set.
        if (splitTerrainHiZ && IsTerrainHiZCandidate(input)!=(hierarchy!=nullptr)) continue;
        if (pass==InstancePass::SHADOW && !input.castShadows) continue;
        if (pass==InstancePass::SELECTION && !input.selected) continue;
        /// @note Deferred already consumed these parts; reject before preparing rows or synchronizing GPU visibility dispatches.
        if (pass==InstancePass::FORWARD && ctx.isDeferred && std::all_of(input.parts.begin(),input.parts.end(),
            [](const auto& part) { return part.mesh.material.shaderCapabilities.SupportsGBuffer(); })) continue;
        if (!resourcesCurrent()) return;
        auto* preparedSource=Source(ctx,cache,input,submitPending);
        if (!preparedSource || !resourcesCurrent()) return;
        auto& source=*preparedSource;
        if (countStatistics) ctx.statsTotalObjects+=static_cast<int>(source.activeCount);
        if (source.activeCount==0) continue;
        if (pass==InstancePass::VELOCITY && !source.moved && std::none_of(input.parts.begin(),input.parts.end(),
            [](const auto& part) { return LeafAnimated(part.mesh.material); })) continue;
        auto view=View(ctx,input,frustum,pass==InstancePass::SHADOW);
        if (preserveShadowDetail) view.vegetation[2]=0;
        if (pass==InstancePass::SELECTION) view.culling.w=0;
        const auto sourceVisibility=SourceVisibility(source,view,frustum);
        if (sourceVisibility!=VisibilityResult::VISIBLE) {
            if (countStatistics) CountRejected(ctx,sourceVisibility,static_cast<int>(source.activeCount));
            continue;
        }
        if (view.culling.w>0 && source.hasBounds && source.finiteBounds && sameOcclusionPublication()
            && IsViewAabbOccluded(ctx,math::Matrix4::Identity(),source.boundsMin,source.boundsMax)) {
            if (countStatistics) ctx.statsOcclusionCulled+=static_cast<int>(source.activeCount);
            continue;
        }
        bool residentShaders=true;
        for (const auto& part : input.parts) {
            const size_t shader=pass==InstancePass::GBUFFER ? 0u : forwardPass ? 1u : pass==InstancePass::SELECTION ? 4u : pass==InstancePass::VELOCITY ? 5u : UsesAlphaShadow(part.mesh.material) ? 3u : 2u;
            residentShaders=residentShaders && ctx.resources.Get(cache.shaders[shader]);
        }
        VegetationCandidates vegetation;
        auto selection=enableGpu && residentShaders
            ? PrepareGpu(ctx,cache,input,source,view,frustum,hierarchy,scene.get(),frame.viewProjection,vegetation) : std::nullopt;
        if (selection) {
            pending.push_back({&input,&source,view,dispatches.size(),vegetation});
            dispatches.push_back(std::move(*selection));
        } else if (!dispatches.empty()) pending.push_back({&input,&source,view,NO_SELECTION});
        else submit(input,source,view,nullptr,{});
    }
    submitPending();
}
} /// @note namespace

struct PreparedInstanceShadowBatch::State {
    struct ViewPolicy {
        std::array<float,46> values{};
        uint32_t height=0,mask=0;
        ProjectionMode projection=ProjectionMode::Perspective;
        bool layerDistances=false,sphericalDistance=false;
        bool operator==(const ViewPolicy&) const=default;
    };
    static ViewPolicy CapturePolicy(const RenderPassContext& ctx)
    {
        const auto& camera=ctx.camera;
        ViewPolicy result{{camera.m_position.x,camera.m_position.y,camera.m_position.z,
            camera.m_rotation.x,camera.m_rotation.y,camera.m_rotation.z,camera.m_rotation.w,
            camera.m_fovY,camera.m_aspect,camera.m_near,camera.m_far,camera.m_orthoHeight,
            ctx.cullMaxDistance,ctx.cullingBoundsPadding},ctx.height,ctx.cullingMask,camera.m_projection,
            ctx.hasLayerCullDistances,ctx.cullDistanceSpherical};
        std::copy_n(ctx.cullLayerDistances,32,result.values.begin()+14);
        return result;
    }
    struct SourceEntry {
        const RenderInstanceInput* input;
        InstanceSource* source;
        InstanceView view;
    };
    struct PendingSet {
        size_t sourceIndex;
        size_t receipt=SIZE_MAX;
        InstanceSetSelection selection;
        bool firstGroupedCascade=false;
    };
    struct LightView {
        InstanceShadowView input;
        std::vector<PendingSet> sets;
    };
    std::shared_ptr<const RenderScene> scene;
    InstanceResources* cache=nullptr;
    uint64_t owner=0,epoch=0,frame=0;
    ViewPolicy policy;
    std::vector<SourceEntry> sources;
    std::vector<LightView> views;
    bool cpuReplay=false;

    bool ResourcesCurrent(const ResourceManager& resources) const
    {
        return cache->owner==owner && cache->epoch==epoch && resources.GetInstanceId()==owner
            && resources.GetResetVersion()==epoch && resources.FrameStamp()==frame;
    }
};

PreparedInstanceShadowBatch::PreparedInstanceShadowBatch()=default;
PreparedInstanceShadowBatch::~PreparedInstanceShadowBatch()=default;
PreparedInstanceShadowBatch::PreparedInstanceShadowBatch(PreparedInstanceShadowBatch&&) noexcept=default;
PreparedInstanceShadowBatch& PreparedInstanceShadowBatch::operator=(PreparedInstanceShadowBatch&&) noexcept=default;

PreparedInstanceShadowBatch PrepareInstanceShadowBatch(RenderPassContext& ctx,std::span<const InstanceShadowView> views,
    std::vector<ComputeCall>& commands)
{
    PreparedInstanceShadowBatch result;
    const auto scene=ctx.renderScene;
    if (!scene || scene->instances.empty() || views.empty()) return result;
    auto& cache=Resources(ctx.resources);
    result.m_state=std::make_unique<PreparedInstanceShadowBatch::State>();
    auto& state=*result.m_state;
    state.scene=scene;
    state.cache=&cache;
    state.owner=cache.owner;
    state.epoch=cache.epoch;
    state.frame=ctx.resources.FrameStamp();
    state.policy=PreparedInstanceShadowBatch::State::CapturePolicy(ctx);
    state.views.reserve(views.size());
    for (const auto& view : views) state.views.push_back({view,{}});
    const size_t firstCommand=commands.size();
    UpdateWindFrame(ctx,cache);

    for (const auto& input : scene->instances) {
        if (!state.ResourcesCurrent(ctx.resources)) break;
        if (input.selectionOnly || !input.castShadows || !HasValidInput(ctx,input)) continue;
        auto* source=Source(ctx,cache,input,[&] {
            return std::none_of(state.sources.begin(),state.sources.end(),[&](const auto& entry) {
                return entry.input->identity==input.identity;
            });
        });
        if (!source) {
            /// @note A duplicate identity with different resident data needs ordered CPU replay before its rows are replaced.
            state.cpuReplay=true;
            commands.resize(firstCommand);
            return result;
        }
        if (!state.ResourcesCurrent(ctx.resources)) break;
        if (source->activeCount==0) continue;
        const auto commonView=View(ctx,input,views.front().frustum,true);
        const size_t sourceIndex=state.sources.size();
        std::array<size_t,kMaxShadowCascades> cascadeIndices{};
        uint32_t cascadeCount=0;
        bool admitted=false;
        for (size_t viewIndex=0;viewIndex<state.views.size();++viewIndex) {
            auto& light=state.views[viewIndex];
            if (SourceVisibility(*source,commonView,light.input.frustum)!=VisibilityResult::VISIBLE) continue;
            light.sets.push_back({sourceIndex,SIZE_MAX,{},false});
            admitted=true;
            if (light.input.directional && cascadeCount<cascadeIndices.size()) cascadeIndices[cascadeCount++]=viewIndex;
        }
        if (!admitted) continue;
        state.sources.push_back({&input,source,commonView});
        if (!CanPrepareGpu(ctx,cache,input,*source,commonView.counts[0])) continue;
        const bool residentShaders=std::all_of(input.parts.begin(),input.parts.end(),[&](const auto& part) {
            const auto& material=part.mesh.material;
            return ctx.resources.Get(cache.shaders[UsesAlphaShadow(material) ? 3u : 2u])!=nullptr;
        });
        if (!residentShaders) continue;

        if (cascadeCount>1) {
            if (!ctx.resources.Get(cache.cascadeCullShader))
                cache.cascadeCullShader=ctx.resources.LoadShader("Assets/Shaders/Pipeline/InstanceCascadeLodCull.cs.hlsl");
            if (ctx.resources.Get(cache.cascadeCullShader)) {
                std::array<math::Frustum,kMaxShadowCascades> frusta;
                for (uint32_t i=0;i<cascadeCount;++i) frusta[i]=state.views[cascadeIndices[i]].input.frustum;
                auto gpuView=commonView;
                auto compute=PrepareGpuBuffers(ctx,cache,input,*source,gpuView,std::span{frusta}.first(cascadeCount),
                    cache.cascadeCullShader);
                if (compute) {
                    std::array<InstanceView,kMaxShadowCascades> cascadeViews;
                    uint32_t preserveDetailMask=0;
                    for (uint32_t i=0;i<cascadeCount;++i) {
                        auto& light=state.views[cascadeIndices[i]];
                        cascadeViews[i]=gpuView;
                        SetFrustumPlanes(cascadeViews[i],light.input.frustum);
                        if (light.input.preserveVegetationDetail) preserveDetailMask|=1u<<i;
                        auto& set=light.sets.back();
                        set.receipt=commands.size();
                        set.selection={compute->uavBuffers[0],compute->uavBuffers[1],i*commonView.counts[2]};
                        set.firstGroupedCascade=i==0;
                    }
                    /// @note The directional variant reads one resident sphere and loops over the compacted light views.
                    cascadeViews[0].terrainDepth[2]=preserveDetailMask;
                    cascadeViews[0].terrainDepth[3]=cascadeCount;
                    ctx.resources.Update(compute->srvBuffers[15],cascadeViews.data(),cascadeCount*sizeof(InstanceView));
                    commands.push_back(std::move(*compute));
                }
            }
        }
        for (auto& light : state.views) {
            if (light.sets.empty() || light.sets.back().sourceIndex!=sourceIndex || light.sets.back().receipt!=SIZE_MAX) continue;
            auto view=commonView;
            SetFrustumPlanes(view,light.input.frustum);
            if (light.input.preserveVegetationDetail) view.vegetation[2]=0;
            auto compute=PrepareGpuBuffers(ctx,cache,input,*source,view,std::span{&light.input.frustum,1},cache.shaders[6]);
            if (!compute) continue;
            ctx.resources.Update(compute->srvBuffers[15],&view,sizeof(view));
            auto& set=light.sets.back();
            set.receipt=commands.size();
            set.selection={compute->uavBuffers[0],compute->uavBuffers[1]};
            commands.push_back(std::move(*compute));
        }
    }
    if (!state.ResourcesCurrent(ctx.resources) || ctx.renderScene.get()!=scene.get()
        || state.policy!=PreparedInstanceShadowBatch::State::CapturePolicy(ctx)) {
        state.cpuReplay=true;
        commands.resize(firstCommand);
    }
    return result;
}

void SubmitInstanceShadowBatchView(RenderPassContext& ctx,const PreparedInstanceShadowBatch& batch,size_t viewIndex,
    std::span<const uint8_t> receipts,ResourceHandle<ConstantBufferTag> lightFrame)
{
    if (!batch.m_state || !batch.m_state->ResourcesCurrent(ctx.resources) || viewIndex>=batch.m_state->views.size()) return;
    const auto& state=*batch.m_state;
    const auto& view=state.views[viewIndex];
    if (state.cpuReplay || ctx.renderScene.get()!=state.scene.get()
        || state.policy!=PreparedInstanceShadowBatch::State::CapturePolicy(ctx)) {
        Execute(ctx,InstancePass::SHADOW,&view.input.frustum,lightFrame,UINT64_MAX,nullptr,false,
            view.input.preserveVegetationDetail);
        return;
    }
    const InstanceSubmission submission{ctx,*state.cache,InstancePass::SHADOW,view.input.frustum,lightFrame,false};
    for (const auto& work : view.sets) {
        if (!state.ResourcesCurrent(ctx.resources)) return;
        const auto& entry=state.sources[work.sourceIndex];
        const bool recorded=work.receipt<receipts.size() && receipts[work.receipt]!=0;
        if (recorded && work.firstGroupedCascade) {
            FBZZ_PROFILE_SCOPE("ShadowVisibility::DirectionalCascadeDispatch");
        }
        if (!recorded && view.input.preserveVegetationDetail) {
            auto selectionView=entry.view;
            selectionView.vegetation[2]=0;
            SubmitSet(submission,*entry.input,*entry.source,selectionView,nullptr,nullptr);
        } else {
            SubmitSet(submission,*entry.input,*entry.source,entry.view,recorded ? &work.selection : nullptr,nullptr);
        }
    }
}

bool HasTerrainHiZInstanceCandidates(const RenderPassContext& ctx)
{
    if (!ctx.renderScene || !SupportsTerrainHiZMode(ctx)) return false;
    return std::any_of(ctx.renderScene->instances.begin(),ctx.renderScene->instances.end(),[&](const auto& input) {
        return HasValidInput(ctx,input) && IsTerrainHiZCandidate(input);
    });
}
void ExecuteInstanceGBuffer(RenderPassContext& ctx) { Execute(ctx,InstancePass::GBUFFER); }
void ExecuteInstanceForward(RenderPassContext& ctx) { Execute(ctx,InstancePass::FORWARD); }
void ExecuteInstanceGBuffer(RenderPassContext& ctx,const GpuHiZ& hierarchy) { Execute(ctx,InstancePass::GBUFFER,nullptr,{},UINT64_MAX,&hierarchy); }
void ExecuteInstanceForward(RenderPassContext& ctx,const GpuHiZ& hierarchy) { Execute(ctx,InstancePass::FORWARD,nullptr,{},UINT64_MAX,&hierarchy); }
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
