/// @file    BoneBakeSkinning.hpp
/// @brief   Resolve scene-owned bone bakes into existing independent skinning requests.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <cstdint>
#include <span>
namespace fbzz::asset { struct Model; }
namespace fbzz::scene {
struct RenderPassContext;
struct BoneBakeRuntime;
struct BoneBakeSkinRequest {
    uint32_t requestIndex = 0;
    BoneBakeRuntime* runtime = nullptr;
    const asset::Model* model = nullptr;
    EntityID animatorEntity = EntityID::INVALID;
    uint64_t animatorLifetime = 0;
};
/// @note CPU packed palettes are retained until replay succeeds, then unused matrix transfers are removed.
void PrepareBakedSkinningRequests(RenderPassContext& context, std::span<const BoneBakeSkinRequest> requests);
void ReleaseBoneBakeSkinningCaches();
}
