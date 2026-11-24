/// @file    SkinningPreparedOutput.hpp
/// @brief   需要経路が所有する変形チケットの公開状態。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Core/ExternalCallbackBoundary.hpp>
#include <Engine/Scene/Entity.hpp>
#include <memory>
#include <vector>

namespace fbzz::asset { struct Model; }
namespace fbzz::renderer {
class PreparedSkinningTicket;
struct RenderScene;
}

namespace fbzz::scene {
class GameObject;
struct RenderPassContext;
struct AnimatorComponent;
struct SkinnedMeshRenderer;

namespace skinning_demand_detail {

/// @note 元のディスパッチ境界で確定したチケットだけを外部コールバック前に完了する。
struct GpuOutputDemandState {
    struct Publication {
        EntityID owner = EntityID::INVALID;
        uint64_t lifetime = 0, generation = 0;
        const asset::Model* model = nullptr;
        std::shared_ptr<renderer::PreparedSkinningTicket> ticket;
        uint64_t vertices = 0;
        uint32_t dispatches = 0;
        bool submitted = false;
    };
    explicit GpuOutputDemandState(RenderPassContext& value);
    static bool BeforeCallback(void* value);
    bool SubmitTicket(const std::shared_ptr<renderer::PreparedSkinningTicket>& ticket);
    bool Complete();

    RenderPassContext& context;
    std::vector<Publication> publications;
    std::vector<std::shared_ptr<const renderer::RenderScene>> retiredSnapshots;
    bool cancelled = false, committed = false;
    core::ExternalCallbackScope observer;
};

[[nodiscard]] GpuOutputDemandState* FindGpuOutputDemand(RenderPassContext& context);
[[nodiscard]] bool NeedsCurrentOutput(RenderPassContext& context, GameObject& object,
    const SkinnedMeshRenderer& skin, const AnimatorComponent* animator);

}
}
