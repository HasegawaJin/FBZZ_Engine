/// @file    SkinningOutputDemand.hpp
/// @brief   GPU 出力需要の診断と、元入力を所有する遅延計算の view 寿命。
/// @author  Hasegawa Jin
/// @date    2026-10-13
#pragma once
#include <array>
#include <cstdint>
#include <memory>

#if defined(_WIN32) && defined(FBZZEngine_EXPORTS)
#define FBZZ_SKINNING_DEMAND_API __declspec(dllexport)
#elif defined(_WIN32)
#define FBZZ_SKINNING_DEMAND_API __declspec(dllimport)
#else
#define FBZZ_SKINNING_DEMAND_API
#endif

namespace fbzz::scene {
struct RenderPassContext;
class GameObject;
struct SkinnedMeshRenderer;
namespace skinning_demand_detail {
enum class Reason : uint32_t {
    COLOR, SHADOW, RAY, OBJECT_MASK, PROBE_GI, NATIVE_SCRIPT_OR_PASS,
    FIBER_OR_PROCEDURAL, MORPH_OR_BAKE, UNPROVEN_BOUNDS,
    MATERIAL_PROOF_PENDING, INPUT_PROOF_PENDING, INITIAL_OR_FAILED,
    UNSCOPED_OR_REENTRY, COUNT
};
inline constexpr uint32_t REASON_COUNT = static_cast<uint32_t>(Reason::COUNT);
inline constexpr std::array<const char*, REASON_COUNT> REASON_NAMES{
    "color", "shadow", "ray", "objectMask", "probeGi", "nativeScriptOrPass",
    "fiberOrProcedural", "morphOrBake", "unprovenBounds",
    "materialProofPending", "inputProofPending", "initialOrFailed", "unscopedOrReentry"
};
[[nodiscard]] constexpr uint32_t Bit(Reason reason) { return uint32_t{1} << static_cast<uint32_t>(reason); }
/// @note Renderer-instance input observations overlap; typed-handle presence is not native liveness or input equality.
enum class InputFlag : uint32_t {
    WEIGHT_MAP_PRESENT, APPLIED_MAP_PRESENT, BUFFER_VECTOR_PRESENT, LOGICAL_BUFFER_HANDLE_PRESENT,
    SELECTED_MORPH_PATH, SELECTED_SOURCE_DIFFERS_FROM_BASE, SELECTED_TARGET_PRESENT,
    WEIGHT_FINITE_NONZERO, WEIGHT_NONFINITE, APPLIED_WEIGHT_FINITE_NONZERO, APPLIED_WEIGHT_NONFINITE,
    REQUESTED_APPLIED_DIFFER, REQUESTED_APPLIED_BITWISE_DIFFER, SELECTED_TARGET_REQUESTED_NONZERO, SELECTED_TARGET_REQUESTED_NONFINITE,
    SELECTED_TARGET_APPLIED_NONZERO, SELECTED_TARGET_APPLIED_NONFINITE, VECTOR_PRESENCE_ONLY,
    CONTAINERS_WITHOUT_SELECTED_MORPH_OR_BAKE, MAPS_ALL_FINITE_ZERO,
    BONE_BAKE_ACTIVE, BONE_BAKE_WITH_SOURCE, BONE_BAKE_GPU_MODE, COUNT
};
inline constexpr uint32_t INPUT_FLAG_COUNT=static_cast<uint32_t>(InputFlag::COUNT);
static_assert(INPUT_FLAG_COUNT<=32);
inline constexpr std::array<const char*,INPUT_FLAG_COUNT> INPUT_NAMES{
    "weightMapPresent","appliedMapPresent","bufferVectorPresent","logicalBufferHandlePresent",
    "selectedMorphPath","selectedSourceDiffersFromBase","selectedTargetPresent",
    "weightFiniteNonzero","weightNonfinite","appliedWeightFiniteNonzero","appliedWeightNonfinite",
    "requestedAppliedDiffer","requestedAppliedBitwiseDiffer","selectedTargetRequestedNonzero","selectedTargetRequestedNonfinite",
    "selectedTargetAppliedNonzero","selectedTargetAppliedNonfinite","vectorPresenceOnly",
    "containersWithoutSelectedMorphOrBake","mapsAllFiniteZero",
    "boneBakeActive","boneBakeWithSource","boneBakeGpuMode"
};
[[nodiscard]] constexpr uint32_t InputBit(InputFlag flag) {return uint32_t{1}<<static_cast<uint32_t>(flag);}
/// @note Stack value only. Counts refer to the current Scene; global flags conservatively retain candidates.
struct SceneDemandSnapshot {
    uint64_t sceneObjects=0,animatorComponents=0,skinnedRendererComponents=0;
    uint64_t activeNativeScriptEntries=0,activeManagedScriptEntries=0;
    uint64_t activeReflectionProbes=0,activeReflectionSceneProbes=0;
    uint64_t activeLightProbeVolumes=0,activeLightProbeBakeConsumers=0,activeInstancedComponents=0;
    uint32_t globalReasons=0;
    bool scopeActive=false,shadowRequested=false,rayRequested=false;
};
struct Observation {
    uint32_t reasons=0,inputFlags=0;
    bool provisionalColorExcluded=false;
    bool conservativeNonColorRequired=false;
    [[nodiscard]] bool Has(Reason reason) const { return (reasons & Bit(reason)) != 0; }
    [[nodiscard]] bool HasInput(InputFlag flag) const {return (inputFlags&InputBit(flag))!=0;}
};
struct GpuOutputDemandState;
/// @note OFF は state/observer/捕捉を作らない。未知 native consumer と再入は元の全 dispatch を使う。
/// @see Docs/design/gpu-skinning-demand.md
class GpuOutputDemandScope final {
public:
    FBZZ_SKINNING_DEMAND_API explicit GpuOutputDemandScope(RenderPassContext& context);
    FBZZ_SKINNING_DEMAND_API ~GpuOutputDemandScope();
    GpuOutputDemandScope(const GpuOutputDemandScope&) = delete;
    GpuOutputDemandScope& operator=(const GpuOutputDemandScope&) = delete;
private:
    std::unique_ptr<GpuOutputDemandState> m_state;
    GpuOutputDemandState* m_previous = nullptr;
};
/// @note Definitions and TLS live in the Engine skinning TU, including for callers in the test executable.
class OutputDemandScope {
public:
    explicit OutputDemandScope(const RenderPassContext& context);
    ~OutputDemandScope();
    OutputDemandScope(const OutputDemandScope&)=delete;
    OutputDemandScope& operator=(const OutputDemandScope&)=delete;
private:
    const RenderPassContext* m_previous=nullptr;
};
[[nodiscard]] bool IsOutputDemandScoped(const RenderPassContext& context);
/// @note Pure component/value reads; no native resource Get, user callback, asset load, GPU request or publication.
[[nodiscard]] SceneDemandSnapshot ObserveSceneDemand(const RenderPassContext& context);
/// @note Existing stored bounds provide only a provisional color exclusion. No result authorizes omission.
[[nodiscard]] Observation ObserveRendererDemand(const RenderPassContext& context,
    GameObject& object,const SkinnedMeshRenderer& skin,const SceneDemandSnapshot& scene);
}
}
#undef FBZZ_SKINNING_DEMAND_API
