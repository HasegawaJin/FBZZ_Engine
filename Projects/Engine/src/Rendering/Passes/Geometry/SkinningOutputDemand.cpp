/// @file    SkinningOutputDemand.cpp
/// @brief   GPU 変形需要を観測し、外部コールバックの前に準備済み出力を確定する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "SkinningOutputDemand.hpp"
#include "SkinningPreparedOutput.hpp"
#include "SkinningComputeCache.hpp"
#include "GeometryPasses.hpp"

#include <Engine/Core/Timing/Time.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Scripting/ScriptComponent.hpp>
#include <Engine/Scene/Managed/Runtime/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Components/Animation/AnimatorComponent.hpp>
#include <Engine/Scene/Components/VFX/FiberComponent.hpp>
#include <Engine/Scene/Components/Geometry/InstancedMeshComponent.hpp>
#include <Engine/Scene/Components/Lighting/LightProbeVolumeComponent.hpp>
#include <Engine/Scene/Components/Geometry/MaterialComponent.hpp>
#include <Engine/Scene/Components/Lighting/ReflectionProbeComponent.hpp>
#include <Engine/Scene/Components/Geometry/SkinnedMeshRenderer.hpp>
#include <Engine/Rendering/Extraction/RenderSceneExtractor.hpp>
#include <Graphics/Effects/Animation/PreparedSkinning.hpp>
#include <Graphics/Passes/Geometry/Animation/ComputedSkinningDemand.hpp>
#include <Graphics/RayTracing/RayTracingPipeline.hpp>
#include <Graphics/Renderer/Visibility/RenderVisibility.hpp>
#include <Graphics/Renderer/Geometry/SkinningPolicy.hpp>
#include <cmath>
#include <limits>
#include <utility>

namespace fbzz::scene {

namespace skinning_demand_detail {
namespace {
thread_local const RenderPassContext* s_demandContext=nullptr;
thread_local uint32_t s_demandDepth=0;
bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
bool Finite(const math::Matrix4& value)
{
    for(const auto& row:value.m)for(float element:row)if(!std::isfinite(element))return false;
    return true;
}
bool SafeWorldTransform(const Transform& transform)
{
    if(!Finite(transform.worldPosition) || !Finite(transform.worldScale)) {
        return false;
    }
    const auto& rotation=transform.worldRotation;
    if(!std::isfinite(rotation.x) || !std::isfinite(rotation.y)
        || !std::isfinite(rotation.z) || !std::isfinite(rotation.w)) {
        return false;
    }
    const float squared=rotation.x*rotation.x+rotation.y*rotation.y+rotation.z*rotation.z+rotation.w*rotation.w;
    // Matrix4::TRS は四元数を正規化するため、ゼロ長で契約違反になる前に除外する。
    return std::isfinite(squared) && squared>math::EPSILON*math::EPSILON;
}
bool StoredPoseBounds(GameObject& object,const SkinnedMeshRenderer& skin,WorldBounds& bounds)
{
    const AnimatorComponent* animator=nullptr;
    GameObject* owner=nullptr;
    for(GameObject* current=&object;current;current=current->GetParent()) {
        if(const auto* candidate=current->GetComponent<AnimatorComponent>()) { animator=candidate;owner=current;break; }
    }
    // 暫定診断でも、バインド姿勢の境界だけでは現在の CPU 姿勢が存在するとは判断できない。
    if(!animator || !owner || !Finite(animator->skinnedBoundsCenter)
        || !std::isfinite(animator->skinnedBoundsRadius) || animator->skinnedBoundsRadius<=0
        || !SafeWorldTransform(owner->transform)) {
        return false;
    }
    for(size_t slot=0;slot<skin.SubmeshCount();++slot) {
        const auto* mesh=skin.SubmeshMesh(slot);
        if(!mesh || !std::isfinite(mesh->boundsRadius)) {
            return false;
        }
    }
    if(!Finite(owner->transform.GetWorldMatrix())) {
        return false;
    }
    return ComputeSkinnedWorldBounds(object,skin,bounds) && Finite(bounds.center)
        && std::isfinite(bounds.radius) && bounds.radius>0;
}
uint32_t ObserveMorphContainersAndSelection(const SkinnedMeshRenderer& skin)
{
    uint32_t flags=0;
    if(!skin.morphWeights.empty()) {
        flags|=InputBit(InputFlag::WEIGHT_MAP_PRESENT);
    }
    if(!skin.appliedMorphWeights.empty()) {
        flags|=InputBit(InputFlag::APPLIED_MAP_PRESENT);
    }
    if(!skin.morphVertexBuffers.empty()) {
        flags|=InputBit(InputFlag::BUFFER_VECTOR_PRESENT);
    }
    const auto weights=[&](const auto& map,InputFlag nonzero,InputFlag nonfinite) {
        for(const auto& entry:map) {
            if(!std::isfinite(entry.second)) {
                flags|=InputBit(nonfinite);
            }
            else if(entry.second!=0.0f)flags|=InputBit(nonzero);
        }
    };
    weights(skin.morphWeights,InputFlag::WEIGHT_FINITE_NONZERO,InputFlag::WEIGHT_NONFINITE);
    weights(skin.appliedMorphWeights,InputFlag::APPLIED_WEIGHT_FINITE_NONZERO,InputFlag::APPLIED_WEIGHT_NONFINITE);
    if(skin.morphWeights!=skin.appliedMorphWeights) {
        flags|=InputBit(InputFlag::REQUESTED_APPLIED_DIFFER);
    }
    bool bitwiseDiffer=skin.morphWeights.size()!=skin.appliedMorphWeights.size();
    if(!bitwiseDiffer)for(const auto& entry:skin.morphWeights) {
        const auto found=skin.appliedMorphWeights.find(entry.first);
        if(found==skin.appliedMorphWeights.end() || std::bit_cast<uint32_t>(entry.second)!=std::bit_cast<uint32_t>(found->second)) {
            bitwiseDiffer=true;break;
        }
    }
    if(bitwiseDiffer) {
        flags|=InputBit(InputFlag::REQUESTED_APPLIED_BITWISE_DIFFER);
    }
    for(const auto& handle:skin.morphVertexBuffers)
        if(handle.IsValid()) {flags|=InputBit(InputFlag::LOGICAL_BUFFER_HANDLE_PRESENT);break;}
    for(size_t slot=0;slot<skin.SubmeshCount();++slot) {
        const auto* mesh=skin.SubmeshMesh(slot);
        if(!mesh) {
            continue;
        }
        const uint32_t index=skin.SubmeshAt(slot);
        if(index<skin.morphVertexBuffers.size() && skin.morphVertexBuffers[index].IsValid()) {
            flags|=InputBit(InputFlag::SELECTED_MORPH_PATH);
            if(skin.morphVertexBuffers[index]!=mesh->vertexBuffer) {
                flags|=InputBit(InputFlag::SELECTED_SOURCE_DIFFERS_FROM_BASE);
            }
        }
        if(!mesh->morphTargets.empty()) {
            flags|=InputBit(InputFlag::SELECTED_TARGET_PRESENT);
        }
        for(const auto& target:mesh->morphTargets) {
            const auto classify=[&](const auto& map,InputFlag nonzero,InputFlag nonfinite) {
                const auto found=map.find(target.name);
                if(found==map.end()) {
                    return;
                }
                if(!std::isfinite(found->second)) {
                    flags|=InputBit(nonfinite);
                }
                else if(found->second!=0.0f)flags|=InputBit(nonzero);
            };
            classify(skin.morphWeights,InputFlag::SELECTED_TARGET_REQUESTED_NONZERO,InputFlag::SELECTED_TARGET_REQUESTED_NONFINITE);
            classify(skin.appliedMorphWeights,InputFlag::SELECTED_TARGET_APPLIED_NONZERO,InputFlag::SELECTED_TARGET_APPLIED_NONFINITE);
        }
    }
    constexpr uint32_t maps=InputBit(InputFlag::WEIGHT_MAP_PRESENT)|InputBit(InputFlag::APPLIED_MAP_PRESENT);
    constexpr uint32_t nonzeroOrNonfinite=InputBit(InputFlag::WEIGHT_FINITE_NONZERO)|InputBit(InputFlag::WEIGHT_NONFINITE)
        |InputBit(InputFlag::APPLIED_WEIGHT_FINITE_NONZERO)|InputBit(InputFlag::APPLIED_WEIGHT_NONFINITE);
    if((flags&maps)!=0 && (flags&nonzeroOrNonfinite)==0) {
        flags|=InputBit(InputFlag::MAPS_ALL_FINITE_ZERO);
    }
    if((flags&maps)==0 && (flags&InputBit(InputFlag::BUFFER_VECTOR_PRESENT))!=0
        && (flags&InputBit(InputFlag::LOGICAL_BUFFER_HANDLE_PRESENT))==0) {
        flags|=InputBit(InputFlag::VECTOR_PRESENCE_ONLY);
    }
    return flags;
}

bool FiniteCullingView(const RenderPassContext& context)
{
    if(!Finite(context.camera.m_position) || !Finite(context.cullCameraForward)
        || !std::isfinite(context.cullingBoundsPadding) || !std::isfinite(context.cullMaxDistance)
        || !std::isfinite(context.cullProjScaleY) || !std::isfinite(context.smallObjectScreenHeight)) {
        return false;
    }
    if(context.hasLayerCullDistances)
        for(float distance:context.cullLayerDistances)if(!std::isfinite(distance))return false;
    if(context.frustumCullingEnabled && context.cameraFrustum)
        for(int index=0;index<math::Frustum::PLANE_COUNT;++index) {
            const auto plane=context.cameraFrustum->GetPlane(index);
            if(!Finite(plane.normal) || !std::isfinite(plane.distance)) {
                return false;
            }
        }
    return true;
}
}
OutputDemandScope::OutputDemandScope(const RenderPassContext& context):m_previous(s_demandContext)
{
    s_demandContext=&context;++s_demandDepth;
}
OutputDemandScope::~OutputDemandScope()
{
    s_demandContext=m_previous;--s_demandDepth;
}
bool IsOutputDemandScoped(const RenderPassContext& context)
{
    return s_demandContext==&context && s_demandDepth==1;
}
SceneDemandSnapshot ObserveSceneDemand(const RenderPassContext& context)
{
    SceneDemandSnapshot result;
    auto& scene=context.scene;
    result.sceneObjects=scene.GameObjectCount();
    result.animatorComponents=scene.GetEntities<AnimatorComponent>().size();
    result.skinnedRendererComponents=scene.GetEntities<SkinnedMeshRenderer>().size();
    result.scopeActive=IsOutputDemandScoped(context);
    if(!result.scopeActive) {
        result.globalReasons|=Bit(Reason::UNSCOPED_OR_REENTRY);
    }
    result.shadowRequested=context.settings.shadowEnabled
        && (context.shadowStrength>0 || context.settings.volumetricLight.enabled || context.punctualShadowViewCount>0);
    result.rayRequested=context.experimentalRayTracingEnabled
        && (context.settings.modeRequest.mode!=renderer::RenderMode::RASTER || renderer::IsRayDebugView(context.settings.viewMode));
    if(result.rayRequested) {
        result.globalReasons|=Bit(Reason::RAY);
    }
    if(context.objectMaskEnabled || !context.settings.objectMaskRequests.empty()) {
        result.globalReasons|=Bit(Reason::OBJECT_MASK);
    }
    for(EntityID id:scene.GetEntities<ReflectionProbeComponent>()) {
        const auto* object=scene.GetGameObject(id);const auto* probe=scene.GetComponent<ReflectionProbeComponent>(id);
        if(object && object->activeInHierarchy() && probe && probe->enabled) {
            ++result.activeReflectionProbes;
            if(probe->captureMode==ReflectionProbeCaptureMode::DynamicScene) {
                ++result.activeReflectionSceneProbes;
            }
        }
    }
    const bool canBake=context.handles.lightProbeProjectCS.IsValid() && context.handles.lightProbeProjectCB.IsValid()
        && context.handles.lightProbeCaptureFrameCB.IsValid() && context.handles.lightProbeCaptureAdvancedCB.IsValid();
    for(EntityID id:scene.GetEntities<LightProbeVolumeComponent>()) {
        const auto* object=scene.GetGameObject(id);const auto* volume=scene.GetComponent<LightProbeVolumeComponent>(id);
        if(object && object->activeInHierarchy() && volume && volume->enabled) {
            ++result.activeLightProbeVolumes;
            if(canBake) {
                ++result.activeLightProbeBakeConsumers;
            }
        }
    }
    if(result.activeReflectionSceneProbes || result.activeLightProbeBakeConsumers) {
        result.globalReasons|=Bit(Reason::PROBE_GI);
    }
    for(EntityID id:scene.GetEntities<InstancedMeshComponent>()) {
        const auto* object=scene.GetGameObject(id);const auto* instances=scene.GetComponent<InstancedMeshComponent>(id);
        if(object && object->activeInHierarchy() && instances && instances->m_enabled) {
            ++result.activeInstancedComponents;
        }
    }
    // 静的インスタンスの抽出はスキンドメッシュが所有する変形バッファを解決しないため、シーン内の存在と実際の需要を区別する。
    if(!scene.GetUserRenderPasses().empty()) {
        result.globalReasons|=Bit(Reason::NATIVE_SCRIPT_OR_PASS);
    }
    for(EntityID id:scene.GetEntities<ScriptComponent>()) {
        const auto* object=scene.GetGameObject(id);const auto* scripts=scene.GetComponent<ScriptComponent>(id);
        if(!object || !object->activeInHierarchy() || !scripts) {
            continue;
        }
        for(const auto& entry:scripts->scripts) {
            if(!entry.script || !entry.script->enabled) {
                continue;
            }
            if(ManagedScriptRuntime::Instance().FindAdapter(entry.script.get())) {
                ++result.activeManagedScriptEntries;
            }
            else {
                ++result.activeNativeScriptEntries;
            }
        }
    }
    if(result.activeNativeScriptEntries) {
        result.globalReasons|=Bit(Reason::NATIVE_SCRIPT_OR_PASS);
    }
    return result;
}
Observation ObserveRendererDemand(const RenderPassContext& context,GameObject& object,
    const SkinnedMeshRenderer& skin,const SceneDemandSnapshot& scene)
{
    Observation result;result.reasons=scene.globalReasons;
    if(scene.shadowRequested && skin.castShadows) {
        result.reasons|=Bit(Reason::SHADOW);
    }
    if(const auto* fiber=object.GetComponent<FiberComponent>();fiber && fiber->m_enabled) {
        result.reasons|=Bit(Reason::FIBER_OR_PROCEDURAL);
    }
    result.inputFlags=ObserveMorphContainersAndSelection(skin);
    if(result.HasInput(InputFlag::SELECTED_MORPH_PATH)) {
        result.reasons|=Bit(Reason::MORPH_OR_BAKE);
    }
    for(GameObject* current=&object;current;current=current->GetParent()) {
        if(const auto* animator=current->GetComponent<AnimatorComponent>()) {
            if(animator->boneBake.active) {
                result.reasons|=Bit(Reason::MORPH_OR_BAKE);
                result.inputFlags|=InputBit(InputFlag::BONE_BAKE_ACTIVE);
                if(animator->boneBake.source) {
                    result.inputFlags|=InputBit(InputFlag::BONE_BAKE_WITH_SOURCE);
                }
                if(animator->boneBakeMode>=static_cast<int>(BoneBakeMode::GPU_SHARED)) {
                    result.inputFlags|=InputBit(InputFlag::BONE_BAKE_GPU_MODE);
                }
            }
            break;
        }
    }
    constexpr uint32_t containers=InputBit(InputFlag::WEIGHT_MAP_PRESENT)|InputBit(InputFlag::APPLIED_MAP_PRESENT)|InputBit(InputFlag::BUFFER_VECTOR_PRESENT);
    if((result.inputFlags&containers)!=0 && !result.HasInput(InputFlag::SELECTED_MORPH_PATH) && !result.HasInput(InputFlag::BONE_BAKE_ACTIVE)) {
        // 存在しても入力元を選べなければ未確定として扱い、モーフの利用側には数えない。
        result.inputFlags|=InputBit(InputFlag::CONTAINERS_WITHOUT_SELECTED_MORPH_OR_BAKE);
        result.reasons|=Bit(Reason::INPUT_PROOF_PENDING);
    }
    constexpr uint32_t nonColorReasons=Bit(Reason::SHADOW)|Bit(Reason::RAY)|Bit(Reason::OBJECT_MASK)|Bit(Reason::PROBE_GI)
        |Bit(Reason::NATIVE_SCRIPT_OR_PASS)|Bit(Reason::FIBER_OR_PROCEDURAL)|Bit(Reason::MORPH_OR_BAKE)|Bit(Reason::UNSCOPED_OR_REENTRY);
    result.conservativeNonColorRequired=(result.reasons&nonColorReasons)!=0;
    const uint32_t layer=static_cast<uint32_t>(object.layer);
    bool color=skin.lodVisible && layer<32 && (context.cullingMask&(uint32_t{1}<<layer))!=0;
    const auto* materials=object.GetComponent<MaterialComponent>();
    bool visibleSlot=false;
    for(size_t slot=0;slot<skin.SubmeshCount();++slot)
        if(skin.SubmeshMesh(slot) && (!materials || (materials->enabled && materials->SlotAt(slot).visible))) {
            visibleSlot=true;break;
        }
    color &= visibleSlot;
    if(color) {
        WorldBounds bounds{};
        if(!FiniteCullingView(context) || !StoredPoseBounds(object,skin,bounds)) {
            result.reasons|=Bit(Reason::UNPROVEN_BOUNDS);
        }
        else {
            bounds.radius+=(std::max)(context.cullingBoundsPadding,0.0f);
            if(!std::isfinite(bounds.radius)) {
                result.reasons|=Bit(Reason::UNPROVEN_BOUNDS);
            }
            else {
                const float layerDistance=context.cullLayerDistances[layer];
                const float distance=context.hasLayerCullDistances && layerDistance>0 ? layerDistance:context.cullMaxDistance;
                const renderer::RenderCullingView view{context.camera.m_position,context.cullCameraForward,
                    context.frustumCullingEnabled?context.cameraFrustum:nullptr,context.cullProjScaleY,
                    context.smallObjectScreenHeight,context.cullDistanceSpherical,context.cullOrthographic};
                color=renderer::EvaluateVisibility(view,{bounds.center,bounds.radius,distance})==renderer::VisibilityResult::VISIBLE;
            }
        }
    }
    if(color) {
        result.reasons|=Bit(Reason::COLOR);
    }
    else {
        result.provisionalColorExcluded=true;
        // 診断件数のみ。現在の姿勢や GPU 入力の整合性はまだ検証していない。
        result.reasons|=Bit(Reason::UNPROVEN_BOUNDS)|Bit(Reason::INPUT_PROOF_PENDING)|Bit(Reason::MATERIAL_PROOF_PENDING);
    }
    if(skin.gpuSkinningContent.empty() || skin.skinnedBufferModel!=skin.model) {
        result.reasons|=Bit(Reason::INITIAL_OR_FAILED);
    }
    if(!scene.scopeActive) {
        result.reasons|=Bit(Reason::UNSCOPED_OR_REENTRY);
    }
    return result;
}

namespace {
thread_local GpuOutputDemandState* s_gpuDemandState = nullptr;
}

GpuOutputDemandState* FindGpuOutputDemand(RenderPassContext& context)
{
    auto* state = s_gpuDemandState;
    if (state && (&state->context != &context || state->cancelled || state->committed)) {
        return nullptr;
    }
    return state;
}

GpuOutputDemandState::GpuOutputDemandState(RenderPassContext& value)
    : context(value), observer(&BeforeCallback, this)
{
}

bool GpuOutputDemandState::BeforeCallback(void* value)
{
    return static_cast<GpuOutputDemandState*>(value)->Complete();
}

bool GpuOutputDemandState::SubmitTicket(const std::shared_ptr<renderer::PreparedSkinningTicket>& ticket)
{
    const auto writes = ticket->Writes();
    const bool previouslyRecorded = !writes.empty() && writes.front().recorded;
    const bool submitted = renderer::SubmitPreparedSkinning(ticket);
    if (submitted && !previouslyRecorded) {
        context.statsSkinningDemandSubmitted += writes.size();
        context.statsSkinningDemandSkipped -= (std::min)(context.statsSkinningDemandSkipped, static_cast<uint64_t>(writes.size()));
        uint64_t vertices = 0;
        for (const auto& write : writes) {
            vertices += write.vertexCount;
        }
        skinning_cache_detail::RecordDeferredSkinningDispatches(static_cast<uint32_t>(writes.size()), vertices);
        context.statsSkinningDispatchCount += static_cast<uint32_t>(writes.size());
        context.statsSkinningVertexCount += vertices;
    }
    return submitted;
}

bool GpuOutputDemandState::Complete()
{
    if (!committed) {
        cancelled = true;
        if (publications.empty()) {
            return true;
        }
    }
    bool success = true;
    std::array<const renderer::PreparedSkinningTicket*, 16> submittedTickets{};
    std::array<bool, 16> submittedResults{};
    size_t submittedCount = 0;
    // 全 native 結果を先に確定する。以後の所有値 COW は Logger/pump/Scene 再抽出を呼ばない。
    for (auto& publication : publications) {
        size_t index = 0;
        while (index < submittedCount && submittedTickets[index] != publication.ticket.get()) {
            ++index;
        }
        if (index < submittedCount) {
            publication.submitted = submittedResults[index];
        }
        else {
            publication.submitted = SubmitTicket(publication.ticket);
            if (submittedCount < submittedTickets.size()) {
                submittedTickets[submittedCount] = publication.ticket.get();
                submittedResults[submittedCount++] = publication.submitted;
            }
        }
        success = publication.submitted && success;
    }
    for (const auto& publication : publications) {
        auto* component = context.scene.GetComponent<SkinnedMeshRenderer>(publication.owner);
        if (!component || context.scene.GetRenderSceneGeneration() != publication.generation
            || context.scene.GetComponentLifetime<SkinnedMeshRenderer>(publication.owner) != publication.lifetime
            || component->model != publication.model || component->preparedSkinning != publication.ticket) {
            continue;
        }
        bool written = publication.submitted && publication.ticket->ResourceInstance() == context.resources.GetInstanceId()
            && publication.ticket->ResourceEpoch() == context.resources.GetResetVersion();
        const auto writes = publication.ticket->Writes();
        for (size_t slot = 0; slot < component->SubmeshCount(); ++slot) {
            const uint32_t submesh = component->SubmeshAt(slot);
            const uint32_t row = submesh < component->preparedSkinningRows.size() ? component->preparedSkinningRows[submesh] : UINT32_MAX;
            written = written && row < writes.size() && writes[row].recorded
                && writes[row].resultingVersion > writes[row].previousVersion && publication.ticket->IsWriteCurrent(row);
        }
        component->gpuSkinnedThisFrame = written;
        component->gpuSkinningFrame = written ? Time::frameCount : (std::numeric_limits<uint64_t>::max)();
        component->gpuSkinningVertexCount = written ? publication.vertices : 0;
        component->gpuSkinningDispatchCount = written ? publication.dispatches : 0;
        if (!written) {
            component->gpuSkinningContent.clear();
        }
    }
    if (auto previous = CompletePreparedSkinningSnapshot(context)) {
        retiredSnapshots.push_back(std::move(previous));
    }
    return success;
}

bool NeedsCurrentOutput(RenderPassContext& context, GameObject& object,
    const SkinnedMeshRenderer& skin, const AnimatorComponent* animator)
{
    // ベイク済みボーンやモーフ、表面の草描画が最新の変形を参照する場合は出力を省かない。
    if ((animator && animator->boneBake.active) || !skin.morphWeights.empty() || !skin.appliedMorphWeights.empty()) {
        return true;
    }
    const auto* fiber = object.GetComponent<FiberComponent>();
    if (fiber && fiber->m_enabled) {
        return true;
    }

    // カリングできないビューと選択・マスクの描画では、保存済み境界だけで省略を決めない。
    if (!FiniteCullingView(context) || context.selectionOutlineEnabled || !context.settings.objectMaskRequests.empty()) {
        return true;
    }
    for (const auto& handle : skin.morphVertexBuffers) {
        if (handle.IsValid()) {
            return true;
        }
    }

    WorldBounds bounds;
    if (!StoredPoseBounds(object, skin, bounds)) {
        return true;
    }
    renderer::RenderObject captured;
    captured.layer = static_cast<uint32_t>(object.layer);
    captured.lodVisible = skin.lodVisible;
    captured.castShadows = skin.castShadows;
    captured.colorEligible = true;
    captured.boundsCenter = bounds.center;
    captured.boundsRadius = bounds.radius;
    return renderer::RequiresComputedSkinning(context, captured);
}

GpuOutputDemandScope::GpuOutputDemandScope(RenderPassContext& context) : m_previous(s_gpuDemandState)
{
    s_gpuDemandState = nullptr;
    if (!renderer::GpuSkinningDemandEnabled() || m_previous || !context.scene.GetUserRenderPasses().empty()
        || !renderer::SupportsCapturedSkinning(context.renderer, context.resources)) {
        return;
    }
    for (EntityID id : context.scene.GetEntities<ScriptComponent>()) {
        const auto* object = context.scene.GetGameObject(id);
        const auto* scripts = context.scene.GetComponent<ScriptComponent>(id);
        if (!object || !object->activeInHierarchy() || !scripts) {
            continue;
        }
        for (const auto& entry : scripts->scripts)
            if (entry.script && entry.script->enabled && !ManagedScriptRuntime::Instance().FindAdapter(entry.script.get())) {
                return;
            }
    }
    m_state = std::make_unique<GpuOutputDemandState>(context);
    s_gpuDemandState = m_state.get();
}
GpuOutputDemandScope::~GpuOutputDemandScope()
{
    s_gpuDemandState = m_previous;
}
}

}
