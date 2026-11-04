/// @file    SkinningOutputDemand.hpp
/// @brief   GPU 出力需要の診断と、元入力を所有する遅延計算の view 寿命。
/// @author  Hasegawa Jin
/// @date    2026-10-13
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/Systems/SkinningDemandTypes.hpp>
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
    FBZZ_ENGINE_API explicit OutputDemandScope(const RenderPassContext& context);
    FBZZ_ENGINE_API ~OutputDemandScope();
    OutputDemandScope(const OutputDemandScope&)=delete;
    OutputDemandScope& operator=(const OutputDemandScope&)=delete;
private:
    const RenderPassContext* m_previous=nullptr;
};
[[nodiscard]] FBZZ_ENGINE_API bool IsOutputDemandScoped(const RenderPassContext& context);
/// @note Pure component/value reads; no native resource Get, user callback, asset load, GPU request or publication.
[[nodiscard]] FBZZ_ENGINE_API SceneDemandSnapshot ObserveSceneDemand(const RenderPassContext& context);
/// @note Existing stored bounds provide only a provisional color exclusion. No result authorizes omission.
[[nodiscard]] FBZZ_ENGINE_API Observation ObserveRendererDemand(const RenderPassContext& context,
    GameObject& object,const SkinnedMeshRenderer& skin,const SceneDemandSnapshot& scene);
}
}
#undef FBZZ_SKINNING_DEMAND_API
