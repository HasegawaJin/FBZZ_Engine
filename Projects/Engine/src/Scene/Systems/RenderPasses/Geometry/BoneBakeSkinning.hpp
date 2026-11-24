/// @file    BoneBakeSkinning.hpp
/// @brief   シーン所有のボーンベイクを既存の独立したスキニング要求に解決する。
/// @author  Hasegawa Jin
/// @date    2026-10-12
#pragma once
#include <Engine/Api.hpp>
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
/// @note 再生が成功するまで CPU パレットを保持し、成功後に不要となった行列転送を除く。
FBZZ_ENGINE_API void PrepareBakedSkinningRequests(RenderPassContext& context, std::span<const BoneBakeSkinRequest> requests);
FBZZ_ENGINE_API void ReleaseBoneBakeSkinningCaches();
}
