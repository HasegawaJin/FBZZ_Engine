/// @file    RenderSceneMaterialDemand.cpp
/// @brief   ビューの材質需要を判定し、条件変更時は遅延バインディングを補完する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "RenderSceneExtraction.hpp"
#include "RenderSceneMaterialDemand.hpp"

#include <Core/Profiler/ProfileScope.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Scene/Components/Lighting/LightProbeVolumeComponent.hpp>
#include <Engine/Scene/Components/Lighting/ReflectionProbeComponent.hpp>
#include <Engine/Scene/Managed/Runtime/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Scripting/ScriptComponent.hpp>
#include <Engine/Rendering/Passes/RenderPassContext.hpp>
#include <Graphics/RayTracing/RayTracingPipeline.hpp>
#include <Graphics/Renderer/Materials/MaterialTransferScope.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <iterator>
#include <utility>

namespace fbzz::scene {

namespace material_demand_detail {
namespace {
/// @note 描画スコープは同一スレッドでスタック順に復元する。context はスコープの終了まで生存する。
thread_local const RenderPassContext* s_bindingDemandContext = nullptr;
thread_local BindingDemandScope* s_bindingDemandScope = nullptr;
thread_local uint32_t s_bindingDemandDepth = 0;
thread_local uint32_t s_extractionDepth = 0;
thread_local uint32_t s_bindingCompletionDepth = 0;
}

/// @note 同一アドレスで編集された視錐台も検出するため、判定入力の値のみをビット単位で保持する。
using BindingDemandWitness = std::array<uint32_t, 44 + math::Frustum::PLANE_COUNT * 4>;

BindingDemandWitness CaptureBindingDemandWitness(const View& view)
{
    BindingDemandWitness result{};
    size_t offset = 0;
    const auto put = [&](float value) { result[offset++] = std::bit_cast<uint32_t>(value); };
    for (const auto& vector : {view.culling.position, view.culling.forward}) {
        put(vector.x); put(vector.y); put(vector.z);
    }
    put(view.culling.projectionScaleY); put(view.culling.smallObjectScreenHeight);
    put(view.maxDistance); put(view.boundsPadding);
    for (float distance : view.layerDistances) {
        put(distance);
    }
    result[offset++] = view.mask;
    result[offset++] = static_cast<uint32_t>(view.culling.frustum != nullptr)
        | (static_cast<uint32_t>(view.culling.distanceSpherical) << 1)
        | (static_cast<uint32_t>(view.culling.orthographic) << 2)
        | (static_cast<uint32_t>(view.hasLayerDistances) << 3)
        | (static_cast<uint32_t>(view.staticGpuFrustumPossible) << 4)
        | (static_cast<uint32_t>(view.directionalShadowPotential) << 5)
        | (static_cast<uint32_t>(view.punctualShadowPotential) << 6)
        | (static_cast<uint32_t>(view.reflectionProbePotential) << 7)
        | (static_cast<uint32_t>(view.lightProbePotential) << 8)
        | (static_cast<uint32_t>(view.rayPotential) << 9)
        | (static_cast<uint32_t>(view.consumerScopeCertified) << 10)
        | (static_cast<uint32_t>(view.materialCoverageKnown) << 11);
    if (view.culling.frustum) {
        for (int index = 0; index < math::Frustum::PLANE_COUNT; ++index) {
            const auto plane = view.culling.frustum->GetPlane(index);
            put(plane.normal.x); put(plane.normal.y); put(plane.normal.z); put(plane.distance);
        }
    }
    return result;
}

/// @note 現在の描画スコープにだけ保持し、補完時は公開済みスナップショットを複製する。
struct PendingBindings {
    std::shared_ptr<const renderer::RenderScene> snapshot;
    std::vector<uint32_t> items;
    uint64_t resourceOwner = 0;
    uint64_t resourceEpoch = 0;
    uint64_t resourceFrame = 0;
    int assetGeneration = 0;
    std::optional<BindingDemandWitness> noDemandWitness;
};

BindingDemandScope::BindingDemandScope(const RenderPassContext& context) : m_previous(s_bindingDemandContext)
{
    m_previousScope = s_bindingDemandScope;
    s_bindingDemandScope = this;
    s_bindingDemandContext = &context;
    ++s_bindingDemandDepth;
}

BindingDemandScope::~BindingDemandScope()
{
    s_bindingDemandScope = m_previousScope;
    s_bindingDemandContext = m_previous;
    --s_bindingDemandDepth;
}

ExtractionDepthScope::ExtractionDepthScope() { ++s_extractionDepth; }
ExtractionDepthScope::~ExtractionDepthScope() { --s_extractionDepth; }

/// @note 補完中の資源コールバックから再入した抽出を全同期へ戻す。
class CompletionDepthScope {
public:
    CompletionDepthScope() { ++s_bindingCompletionDepth; }
    ~CompletionDepthScope() { --s_bindingCompletionDepth; }
    CompletionDepthScope(const CompletionDepthScope&) = delete;
    CompletionDepthScope& operator=(const CompletionDepthScope&) = delete;
};

bool BindingScopeCertified(const RenderPassContext& context, bool completing = false)
{
    return s_bindingDemandContext == &context && s_bindingDemandDepth == 1 && s_bindingCompletionDepth == 0
        && (s_extractionDepth == 1 || (completing && s_extractionDepth == 0));
}

void RetainPendingBindings(const RenderPassContext& context, std::shared_ptr<const renderer::RenderScene> snapshot,
                           std::vector<uint32_t> items)
{
    if (!BindingScopeCertified(context) || !s_bindingDemandScope || items.empty()) {
        return;
    }
    auto pending = std::make_unique<PendingBindings>();
    pending->snapshot = std::move(snapshot);
    pending->items = std::move(items);
    pending->resourceOwner = context.resources.GetInstanceId();
    pending->resourceEpoch = context.resources.GetResetVersion();
    pending->resourceFrame = context.resources.FrameStamp();
    pending->assetGeneration = asset::AssetManager::GetAssetGeneration();
    s_bindingDemandScope->m_pending = std::move(pending);
}

void RetargetBindings(const std::shared_ptr<const renderer::RenderScene>& previous,
    const std::shared_ptr<const renderer::RenderScene>& completed)
{
    if (s_bindingDemandScope && s_bindingDemandScope->m_pending
        && s_bindingDemandScope->m_pending->snapshot == previous)
    {
        s_bindingDemandScope->m_pending->snapshot = completed;
        s_bindingDemandScope->m_pending->noDemandWitness.reset();
    }
}
}

namespace render_extraction_detail {

namespace {

bool NeedsMaterialBindings(const material_demand_detail::View& view, const renderer::RenderScene& snapshot,
                           std::span<const uint32_t> omittedItemIndices, Scene& scene)
{
    for (uint32_t itemIndex : omittedItemIndices) {
        if (itemIndex >= snapshot.items.size()) {
            return true;
        }
        const auto& item = snapshot.items[itemIndex];
        if (item.objectIndex >= snapshot.Objects().size()) {
            return true;
        }
        if (LeafWindBoundsMayHaveExpanded(scene, snapshot.Objects()[item.objectIndex])) {
            return true;
        }
        const auto result = material_demand_detail::Evaluate(view, snapshot.Objects()[item.objectIndex], item);
        if (result.reasons != 0 || (!result.cameraEvaluated && result.matchesView)) {
            return true;
        }
    }
    return false;
}

}

material_demand_detail::View CurrentMaterialDemandView(const RenderPassContext& context, bool rayTracingInputs, bool completing)
{
    material_demand_detail::View view;
    view.culling = {context.camera.m_position, context.cullCameraForward,
        context.frustumCullingEnabled ? context.cameraFrustum : nullptr,
        context.cullProjScaleY, context.smallObjectScreenHeight, context.cullDistanceSpherical, context.cullOrthographic};
    std::copy(std::begin(context.cullLayerDistances), std::end(context.cullLayerDistances), view.layerDistances.begin());
    view.maxDistance = context.cullMaxDistance;
    view.boundsPadding = context.cullingBoundsPadding;
    view.mask = context.cullingMask;
    view.hasLayerDistances = context.hasLayerCullDistances;
    view.staticGpuFrustumPossible = context.settings.gpuInstancing
        && (context.settings.geometryDrawMode == renderer::GeometryDrawMode::GPU_INDIRECT
            || context.settings.geometryDrawMode == renderer::GeometryDrawMode::GPU_INSTANCE_INDIRECT);
    // 同期時に不透明な ErrorSurface へ切り替わり得るため、同期前の材質から影の利用先を除外しない。
    view.directionalShadowPotential = context.settings.shadowEnabled;
    view.punctualShadowPotential = context.settings.shadowEnabled && context.punctualShadowViewCount > 0;
    view.rayPotential = rayTracingInputs;
    view.consumerScopeCertified = material_demand_detail::BindingScopeCertified(context, completing)
        && context.scene.GetUserRenderPasses().empty() && !context.objectMaskEnabled
        && context.settings.objectMaskRequests.empty() && context.settings.selectedObjects.empty();
    // ネイティブのセットアップフックはパス一覧の確認後にも利用先を追加できる。
    // ManagedScriptAdapter はネイティブの描画フックを上書きしない。
    if (view.consumerScopeCertified) {
        for (EntityID id : context.scene.GetEntities<ScriptComponent>()) {
            const auto* scripts = context.scene.GetComponent<ScriptComponent>(id);
            if (!scripts) {
                continue;
            }
            for (const auto& entry : scripts->scripts) {
                if (entry.script && !ManagedScriptRuntime::Instance().FindAdapter(entry.script.get())) {
                    view.consumerScopeCertified = false;
                    break;
                }
            }
            if (!view.consumerScopeCertified) {
                break;
            }
        }
    }
    for (EntityID id : context.scene.GetEntities<ReflectionProbeComponent>()) {
        const auto* object = context.scene.GetGameObject(id);
        const auto* probe = context.scene.GetComponent<ReflectionProbeComponent>(id);
        if (object && object->activeInHierarchy() && probe && probe->enabled
            && probe->captureMode == ReflectionProbeCaptureMode::DynamicScene) {
            view.reflectionProbePotential = true;
            break;
        }
    }
    for (EntityID id : context.scene.GetEntities<LightProbeVolumeComponent>()) {
        const auto* object = context.scene.GetGameObject(id);
        const auto* volume = context.scene.GetComponent<LightProbeVolumeComponent>(id);
        if (object && object->activeInHierarchy() && volume && volume->enabled) {
            view.lightProbePotential = true;
            break;
        }
    }
    return view;
}

bool CurrentRayMaterialInputs(const RenderPassContext& context)
{
    return context.experimentalRayTracingEnabled && (context.settings.modeRequest.mode != renderer::RenderMode::RASTER
        || renderer::IsRayDebugView(context.settings.viewMode));
}

void RepairViewMaterialBindings(RenderPassContext& context, renderer::RenderScene& snapshot,
    std::vector<uint32_t>& omittedItemIndices, uint64_t resourceFrame, uint64_t resourceResetVersion,
    MaterialDemandTraceScope& trace)
{
    FailOpenChangedLeafWindBounds(context.scene, snapshot);
    if (!omittedItemIndices.empty()) {
        const auto finalView = CurrentMaterialDemandView(context, CurrentRayMaterialInputs(context));
        const bool viewInputsChanged = resourceResetVersion != context.resources.GetResetVersion()
            || snapshot.sceneGeneration != context.scene.GetRenderSceneGeneration() || resourceFrame != context.resources.FrameStamp();
        if (viewInputsChanged || NeedsMaterialBindings(finalView, snapshot, omittedItemIndices, context.scene)) {
            FBZZ_PROFILE_SCOPE("RenderSceneExtractor::RestoreMaterialBindings");
            RestoreMaterialBindings(context, snapshot, omittedItemIndices, trace.Active() ? &trace : nullptr);
            omittedItemIndices.clear();
            FailOpenChangedLeafWindBounds(context.scene, snapshot);
        }
    }
}

}

namespace material_demand_detail {
void CompleteBindings(RenderPassContext& context)
{
    using namespace render_extraction_detail;
    if (s_bindingCompletionDepth != 0 || s_bindingDemandContext != &context
        || !s_bindingDemandScope || !s_bindingDemandScope->m_pending) {
        return;
    }
    auto& pending = *s_bindingDemandScope->m_pending;
    if (context.renderScene != pending.snapshot) {
        s_bindingDemandScope->m_pending.reset();
        return;
    }
    const auto view = CurrentMaterialDemandView(context, CurrentRayMaterialInputs(context), true);
    const bool changed = pending.resourceOwner != context.resources.GetInstanceId()
        || pending.resourceEpoch != context.resources.GetResetVersion() || pending.resourceFrame != context.resources.FrameStamp()
        || pending.snapshot->sceneGeneration != context.scene.GetRenderSceneGeneration()
        || pending.assetGeneration != asset::AssetManager::GetAssetGeneration()
        || std::any_of(pending.items.begin(), pending.items.end(), [&](uint32_t index) {
            return index < pending.snapshot->items.size()
                && pending.snapshot->items[index].objectIndex < pending.snapshot->Objects().size()
                && LeafWindBoundsMayHaveExpanded(context.scene, pending.snapshot->Objects()[pending.snapshot->items[index].objectIndex]);
        });
    if (!changed) {
        const auto witness = CaptureBindingDemandWitness(view);
        if (pending.noDemandWitness && *pending.noDemandWitness == witness) {
            return;
        }
        if (!NeedsMaterialBindings(view, *pending.snapshot, pending.items, context.scene)) {
            pending.noDemandWitness = witness;
            return;
        }
    }
    FBZZ_PROFILE_SCOPE("RenderSceneExtractor::CompleteMaterialBindings");
    // 資源コールバックの前に補完対象の所有権を移し、再入時の二重補完を防ぐ。
    auto owned = std::move(s_bindingDemandScope->m_pending);
    const auto preparedItems = FindPreparedSnapshotItems(owned->snapshot);
    auto completed = std::make_shared<renderer::RenderScene>(*owned->snapshot);
    completed->snapshotSerial = NextRenderSnapshotSerial();
    renderer::material_transfer_detail::MaterialTransferPayloadScope materialTransferPayloads(context.resources);
    CompletionDepthScope completionDepth;
    FailOpenChangedLeafWindBounds(context.scene, *completed);
    RestoreMaterialBindings(context, *completed, owned->items);
    FailOpenChangedLeafWindBounds(context.scene, *completed);
    if (preparedItems.known) {
        RememberPreparedSnapshotItems(completed, preparedItems.indices);
    }
    context.renderScene = std::move(completed);
}
}

}
