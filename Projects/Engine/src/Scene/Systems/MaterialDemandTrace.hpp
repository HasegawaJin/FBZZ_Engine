/// @file    MaterialDemandTrace.hpp
/// @brief   Conservative material-binding demand and bounded diagnostics for one extraction.
/// @author  Hasegawa Jin
/// @date    2026-10-13
#pragma once

#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/RenderVisibility.hpp>
#include <Math/Frustum.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#if defined(_WIN32) && defined(FBZZEngine_EXPORTS)
#define FBZZ_MATERIAL_DEMAND_API __declspec(dllexport)
#elif defined(_WIN32)
#define FBZZ_MATERIAL_DEMAND_API __declspec(dllimport)
#else
#define FBZZ_MATERIAL_DEMAND_API
#endif

namespace fbzz::scene { struct RenderPassContext; }

namespace fbzz::scene::material_demand_detail {

struct PendingBindings;
/// @note A changed consumer restores all omitted bindings once with fresh resource resolution; earlier published snapshots remain immutable.
FBZZ_MATERIAL_DEMAND_API void CompleteBindings(RenderPassContext& context);

/// @note Only the built-in view pipeline can certify its consumers; direct and nested extraction retain full bindings.
/// @note The scope owns only a snapshot and omitted indices between consumer boundaries; completion copies published snapshots.
class BindingDemandScope {
public:
    FBZZ_MATERIAL_DEMAND_API explicit BindingDemandScope(const RenderPassContext& context);
    FBZZ_MATERIAL_DEMAND_API ~BindingDemandScope();
    BindingDemandScope(const BindingDemandScope&) = delete;
    BindingDemandScope& operator=(const BindingDemandScope&) = delete;
private:
    const RenderPassContext* m_previous;
    BindingDemandScope* m_previousScope = nullptr;
    std::unique_ptr<PendingBindings> m_pending;
    friend bool BindingScopeCertified(const RenderPassContext&, bool);
    friend void RetainPendingBindings(const RenderPassContext&, std::shared_ptr<const renderer::RenderScene>, std::vector<uint32_t>);
    friend FBZZ_MATERIAL_DEMAND_API void CompleteBindings(RenderPassContext&);
    friend void RetargetBindings(const std::shared_ptr<const renderer::RenderScene>&,
        const std::shared_ptr<const renderer::RenderScene>&);
};

enum Reason : uint32_t {
    COLOR = 1u << 0,
    GPU_FRUSTUM_POTENTIAL = 1u << 1,
    VELOCITY = 1u << 2,
    DIRECTIONAL_SHADOW_POTENTIAL = 1u << 3,
    PUNCTUAL_SHADOW_POTENTIAL = 1u << 4,
    REFLECTION_PROBE_POTENTIAL = 1u << 5,
    LIGHT_PROBE_POTENTIAL = 1u << 6,
    RAY_POTENTIAL = 1u << 7,
    NUMERIC_FALLBACK = 1u << 8,
    UNKNOWN_CONSUMER = 1u << 9,
};

struct View {
    renderer::RenderCullingView culling;
    std::array<float, 32> layerDistances{};
    float maxDistance = 0.0f;
    float boundsPadding = 0.0f;
    uint32_t mask = UINT32_MAX;
    bool hasLayerDistances = false;
    bool staticGpuFrustumPossible = false;
    bool directionalShadowPotential = false;
    bool punctualShadowPotential = false;
    bool reflectionProbePotential = false;
    bool lightProbePotential = false;
    bool rayPotential = false;
    /// @note Certification also excludes native script hooks that can register consumers after extraction.
    bool consumerScopeCertified = false;
    /// @note Before Sync a failed/custom material may become opaque ErrorSurface; no pre-sync alpha exclusion is certified here.
    bool materialCoverageKnown = false;
};

struct Result {
    uint32_t reasons = 0;
    renderer::VisibilityResult cameraResult = renderer::VisibilityResult::VISIBLE;
    bool matchesView = false;
    bool cameraEvaluated = false;
    bool numericUnproven = false;
    bool KnownConsumerPotential() const { return (reasons & ~UNKNOWN_CONSUMER) != 0; }
};

/// @note The range guard proves finite intermediate float arithmetic before invoking Math visibility; out-of-range data stays conservative.
inline bool SafeScalar(float value)
{
    return std::isfinite(value) && std::abs(value) <= 1.0e10f;
}

inline bool SafeVector(const math::Vector3& value)
{
    return SafeScalar(value.x) && SafeScalar(value.y) && SafeScalar(value.z);
}

inline bool SafeCulling(const View& view, const renderer::RenderObject& object)
{
    if (!SafeVector(object.boundsCenter) || !SafeScalar(object.boundsRadius)
        || !SafeScalar(view.boundsPadding) || !SafeVector(view.culling.position)
        || !SafeVector(view.culling.forward) || !SafeScalar(view.culling.projectionScaleY)
        || !SafeScalar(view.culling.smallObjectScreenHeight) || !SafeScalar(view.maxDistance)) return false;
    const float layerDistance = view.layerDistances[object.layer & 31u];
    if (!SafeScalar(layerDistance)) return false;
    if (view.culling.frustum) {
        for (int index = 0; index < math::Frustum::PLANE_COUNT; ++index) {
            const auto plane = view.culling.frustum->GetPlane(index);
            if (!SafeVector(plane.normal) || !SafeScalar(plane.distance)) return false;
        }
    }
    const double x = static_cast<double>(object.boundsCenter.x) - view.culling.position.x;
    const double y = static_cast<double>(object.boundsCenter.y) - view.culling.position.y;
    const double z = static_cast<double>(object.boundsCenter.z) - view.culling.position.z;
    const double distance = view.culling.distanceSpherical ? std::sqrt(x*x + y*y + z*z)
        : x*view.culling.forward.x + y*view.culling.forward.y + z*view.culling.forward.z;
    /// @note Tiny positive depth can overflow the small-object projection division even when each input is finite.
    return distance <= 0.0 || distance >= 1.0e-10;
}

/// @note This pure predicate never increments renderer cull statistics or resolves resources; omission requires certified invocation provenance.
inline Result Evaluate(const View& view, const renderer::RenderObject& object, const renderer::RenderMeshItem& item)
{
    Result result;
    if (!object.colorEligible || !item.visible || !item.slotVisible) return result;
    if (!view.consumerScopeCertified) result.reasons |= UNKNOWN_CONSUMER;
    result.matchesView = object.lodVisible && object.layer < 32
        && (view.mask & (uint32_t{1} << object.layer)) != 0;
    const bool hasVertices = item.vertexBuffer.IsValid() || (object.skinned
        && (item.skinningVertexBuffer.IsValid() || item.deformedVertexBuffer.IsValid()));
    const bool hasGeometry = hasVertices && item.indexBuffer.IsValid();
    if (view.rayPotential && (object.rayVisible || object.rayLodSelectionRequired))
        result.reasons |= RAY_POTENTIAL;
    if (!result.matchesView || !hasGeometry) return result;
    if (!SafeCulling(view, object)) {
        result.numericUnproven = true;
        result.reasons |= NUMERIC_FALLBACK;
        return result;
    }
    const float radius = object.boundsRadius > 0.0f
        ? object.boundsRadius + (std::max)(view.boundsPadding, 0.0f) : 0.0f;
    const float layerDistance = view.layerDistances[object.layer & 31u];
    const float distance = view.hasLayerDistances && layerDistance > 0.0f ? layerDistance : view.maxDistance;
    const renderer::RenderCullingItem bounds{object.boundsCenter, radius, distance};
    result.cameraResult = renderer::EvaluateVisibility(view.culling, bounds);
    result.cameraEvaluated = true;
    if (result.cameraResult == renderer::VisibilityResult::VISIBLE) result.reasons |= COLOR;
    else if (!object.skinned && object.lodDither == 0.0f && view.staticGpuFrustumPossible) {
        auto gpuView = view.culling;
        gpuView.frustum = nullptr;
        if (renderer::EvaluateVisibility(gpuView, bounds) == renderer::VisibilityResult::VISIBLE)
            result.reasons |= GPU_FRUSTUM_POTENTIAL;
    }
    const bool opaque = item.material.capabilities.blend == renderer::BlendMode::OPAQUE_BLEND;
    const bool opaqueOrUnproven = opaque || !view.materialCoverageKnown;
    if ((result.reasons & (COLOR | GPU_FRUSTUM_POTENTIAL)) != 0 && opaqueOrUnproven
        && (!object.skinned || object.previousSkinningValid)) result.reasons |= VELOCITY;
    const bool withinShadowDistance = radius <= 0.0f || renderer::IsWithinDrawDistance(view.culling, bounds);
    if (object.castShadows && opaqueOrUnproven && withinShadowDistance) {
        if (view.directionalShadowPotential) result.reasons |= DIRECTIONAL_SHADOW_POTENTIAL;
        if (view.punctualShadowPotential) result.reasons |= PUNCTUAL_SHADOW_POTENTIAL;
    }
    if (!object.skinned) {
        if (view.reflectionProbePotential) result.reasons |= REFLECTION_PROBE_POTENTIAL;
        if (view.lightProbePotential && opaqueOrUnproven) result.reasons |= LIGHT_PROBE_POTENTIAL;
    }
    return result;
}

/// @note Non-owning invocation-local validation; the view and its frustum must remain unchanged until all object evaluators are destroyed.
struct PreparedView {
    explicit PreparedView(const View& source) : view(source)
    {
        safeCulling = SafeScalar(view.boundsPadding) && SafeVector(view.culling.position)
            && SafeVector(view.culling.forward) && SafeScalar(view.culling.projectionScaleY)
            && SafeScalar(view.culling.smallObjectScreenHeight) && SafeScalar(view.maxDistance);
        if (safeCulling && view.culling.frustum) {
            for (int index = 0; index < math::Frustum::PLANE_COUNT; ++index) {
                const auto plane = view.culling.frustum->GetPlane(index);
                if (!SafeVector(plane.normal) || !SafeScalar(plane.distance)) {
                    safeCulling = false;
                    break;
                }
            }
        }
        for (uint32_t layer = 0; layer < 32; ++layer)
            if (SafeScalar(view.layerDistances[layer])) safeLayerDistances |= uint32_t{1} << layer;
    }

    const View& view;
    bool safeCulling = false;
    uint32_t safeLayerDistances = 0;
};

inline bool SafeObjectCulling(const PreparedView& prepared, const renderer::RenderObject& object)
{
    if (!prepared.safeCulling || !SafeVector(object.boundsCenter) || !SafeScalar(object.boundsRadius)
        || (prepared.safeLayerDistances & (uint32_t{1} << (object.layer & 31u))) == 0) return false;
    const auto& view = prepared.view;
    const double x = static_cast<double>(object.boundsCenter.x) - view.culling.position.x;
    const double y = static_cast<double>(object.boundsCenter.y) - view.culling.position.y;
    const double z = static_cast<double>(object.boundsCenter.z) - view.culling.position.z;
    const double distance = view.culling.distanceSpherical ? std::sqrt(x*x + y*y + z*z)
        : x*view.culling.forward.x + y*view.culling.forward.y + z*view.culling.forward.z;
    return distance <= 0.0 || distance >= 1.0e-10;
}

/// @note The object is immutable for this evaluator's lifetime; slot visibility, geometry and material coverage remain per-item decisions.
/// @note Camera work is lazy so hidden or geometry-less slots preserve the legacy unevaluated result without extra visibility queries.
class ObjectEvaluator {
public:
    ObjectEvaluator(const PreparedView& prepared, const renderer::RenderObject& object)
        : m_prepared(prepared), m_object(object)
    {
        const auto& view = prepared.view;
        if (!view.consumerScopeCertified) m_base.reasons |= UNKNOWN_CONSUMER;
        m_base.matchesView = object.lodVisible && object.layer < 32
            && (view.mask & (uint32_t{1} << object.layer)) != 0;
        if (view.rayPotential && (object.rayVisible || object.rayLodSelectionRequired))
            m_base.reasons |= RAY_POTENTIAL;
    }

    Result Evaluate(const renderer::RenderMeshItem& item)
    {
        if (!m_object.colorEligible || !item.visible || !item.slotVisible) return {};
        Result result = m_base;
        const bool hasVertices = item.vertexBuffer.IsValid() || (m_object.skinned
            && (item.skinningVertexBuffer.IsValid() || item.deformedVertexBuffer.IsValid()));
        if (!result.matchesView || !hasVertices || !item.indexBuffer.IsValid()) return result;
        if (!m_cameraPrepared) PrepareCamera();
        result.cameraResult = m_camera.cameraResult;
        result.cameraEvaluated = m_camera.cameraEvaluated;
        result.numericUnproven = m_camera.numericUnproven;
        result.reasons |= m_camera.reasons;
        if (result.numericUnproven) return result;
        const auto& view = m_prepared.view;
        const bool opaqueOrUnproven = item.material.capabilities.blend == renderer::BlendMode::OPAQUE_BLEND
            || !view.materialCoverageKnown;
        if ((result.reasons & (COLOR | GPU_FRUSTUM_POTENTIAL)) != 0 && opaqueOrUnproven
            && (!m_object.skinned || m_object.previousSkinningValid)) result.reasons |= VELOCITY;
        if (m_object.castShadows && opaqueOrUnproven && m_withinShadowDistance) {
            if (view.directionalShadowPotential) result.reasons |= DIRECTIONAL_SHADOW_POTENTIAL;
            if (view.punctualShadowPotential) result.reasons |= PUNCTUAL_SHADOW_POTENTIAL;
        }
        if (!m_object.skinned) {
            if (view.reflectionProbePotential) result.reasons |= REFLECTION_PROBE_POTENTIAL;
            if (view.lightProbePotential && opaqueOrUnproven) result.reasons |= LIGHT_PROBE_POTENTIAL;
        }
        return result;
    }

private:
    void PrepareCamera()
    {
        m_cameraPrepared = true;
        if (!SafeObjectCulling(m_prepared, m_object)) {
            m_camera.numericUnproven = true;
            m_camera.reasons = NUMERIC_FALLBACK;
            return;
        }
        const auto& view = m_prepared.view;
        const float radius = m_object.boundsRadius > 0.0f
            ? m_object.boundsRadius + (std::max)(view.boundsPadding, 0.0f) : 0.0f;
        const float layerDistance = view.layerDistances[m_object.layer & 31u];
        const float distance = view.hasLayerDistances && layerDistance > 0.0f ? layerDistance : view.maxDistance;
        const renderer::RenderCullingItem bounds{m_object.boundsCenter, radius, distance};
        m_camera.cameraResult = renderer::EvaluateVisibility(view.culling, bounds);
        m_camera.cameraEvaluated = true;
        if (m_camera.cameraResult == renderer::VisibilityResult::VISIBLE) m_camera.reasons |= COLOR;
        else if (!m_object.skinned && m_object.lodDither == 0.0f && view.staticGpuFrustumPossible) {
            auto gpuView = view.culling;
            gpuView.frustum = nullptr;
            if (renderer::EvaluateVisibility(gpuView, bounds) == renderer::VisibilityResult::VISIBLE)
                m_camera.reasons |= GPU_FRUSTUM_POTENTIAL;
        }
        m_withinShadowDistance = radius <= 0.0f || renderer::IsWithinDrawDistance(view.culling, bounds);
    }

    const PreparedView& m_prepared;
    const renderer::RenderObject& m_object;
    Result m_base;
    Result m_camera;
    bool m_cameraPrepared = false;
    bool m_withinShadowDistance = false;
};

/// @note Every field is an integer count or identity; bytes are logical observed source/output bytes, not native upload or physical allocation.
#define FBZZ_MATERIAL_DEMAND_COLUMNS(X) \
    X(resourceInstanceId) X(resourceEpoch) X(resourceFrame) X(sceneGeneration) X(snapshotSerial) \
    X(passOrdinalWithinResourceFrame) X(ordinalKnown) X(reentryDepth) X(sceneObjects) X(sceneMeshRenderers) \
    X(sceneSkinnedRenderers) X(sceneMaterialComponents) X(renderObjects) X(renderItems) X(consideredItems) \
    X(syncEligibleItems) X(synchronizedReuseItems) X(syncAttempts) X(syncNonNullItems) X(errorFallbackItems) \
    X(eligibleDeclaredVertices) X(eligibleDeclaredIndices) X(knownPotentialDeclaredVertices) \
    X(projectedTextureRequestItems) X(positiveScreenPixelRequests) \
    X(syncReturnedParameterBytes) X(canonicalCopiedLogicalBytes) X(knownConsumerPotentialItems) \
    X(knownConsumerNoDemandItems) X(conservativeUnionItems) X(provenNoDemandItems) X(unknownConsumerItems) \
    X(colorPotentialItems) X(gpuFrustumPotentialItems) X(velocityPotentialItems) X(directionalShadowPotentialItems) \
    X(punctualShadowPotentialItems) X(reflectionProbePotentialItems) X(lightProbePotentialItems) X(rayPotentialItems) \
    X(numericFallbackItems) X(cameraVisibleItems) X(cameraMaskOrLodRejectedItems) X(cameraDistanceRejectedItems) \
    X(cameraSmallRejectedItems) X(cameraFrustumRejectedItems) X(probeCensusObjects) X(activeReflectionProbes) \
    X(activeLightProbeVolumes) X(userPassesAtExtraction) X(consumerScopeCertified) X(identityChangedDuringInvocation) \
    X(omittedBindingItems) X(restoredBindingItems) X(bindingFallbacks) X(complete)

struct Row {
#define FBZZ_MATERIAL_DEMAND_FIELD(name) uint64_t name = 0;
    FBZZ_MATERIAL_DEMAND_COLUMNS(FBZZ_MATERIAL_DEMAND_FIELD)
#undef FBZZ_MATERIAL_DEMAND_FIELD
};

inline void CountPotential(Row& row, const Result& result)
{
    if (result.reasons == 0) ++row.provenNoDemandItems;
    if (result.KnownConsumerPotential()) ++row.knownConsumerPotentialItems;
    else ++row.knownConsumerNoDemandItems;
    if (result.reasons != 0) ++row.conservativeUnionItems;
    if ((result.reasons & UNKNOWN_CONSUMER) != 0) ++row.unknownConsumerItems;
    if ((result.reasons & COLOR) != 0) ++row.colorPotentialItems;
    if ((result.reasons & GPU_FRUSTUM_POTENTIAL) != 0) ++row.gpuFrustumPotentialItems;
    if ((result.reasons & VELOCITY) != 0) ++row.velocityPotentialItems;
    if ((result.reasons & DIRECTIONAL_SHADOW_POTENTIAL) != 0) ++row.directionalShadowPotentialItems;
    if ((result.reasons & PUNCTUAL_SHADOW_POTENTIAL) != 0) ++row.punctualShadowPotentialItems;
    if ((result.reasons & REFLECTION_PROBE_POTENTIAL) != 0) ++row.reflectionProbePotentialItems;
    if ((result.reasons & LIGHT_PROBE_POTENTIAL) != 0) ++row.lightProbePotentialItems;
    if ((result.reasons & RAY_POTENTIAL) != 0) ++row.rayPotentialItems;
    if ((result.reasons & NUMERIC_FALLBACK) != 0) ++row.numericFallbackItems;
    if (!result.matchesView) ++row.cameraMaskOrLodRejectedItems;
    else if (result.cameraEvaluated) {
        switch (result.cameraResult) {
        case renderer::VisibilityResult::VISIBLE: ++row.cameraVisibleItems; break;
        case renderer::VisibilityResult::DISTANCE_CULLED: ++row.cameraDistanceRejectedItems; break;
        case renderer::VisibilityResult::SMALL_OBJECT_CULLED: ++row.cameraSmallRejectedItems; break;
        case renderer::VisibilityResult::FRUSTUM_CULLED: ++row.cameraFrustumRejectedItems; break;
        }
    }
}

} /// @note namespace fbzz::scene::material_demand_detail
#undef FBZZ_MATERIAL_DEMAND_API
