/// @file    ManagedJointService.hpp
/// @brief   実 JointComponent 候補の preflight と FixedUpdate の設定変更。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <span>
#include <string>
namespace fbzz::scene { class Scene; class Script; class GameObject; struct JointComponent; }
namespace fbzz::scene::managed_detail {
struct ManagedServiceResult;
FBZZ_ENGINE_API ManagedBridgeStatus RunManagedJointService(Scene& scene, Script& proxy, GameObject& owner, GameObject* related,
    const ManagedBridgeRequest& request, std::span<const float> values, std::span<const std::string> text, ManagedServiceResult& result, bool fixed);
FBZZ_ENGINE_API bool PrepareJointChain(Scene& scene, GameObject& owner, std::span<GameObject* const> targets,
    float distance, uint32_t iterations, JointComponent& candidate);
} /// @note namespace fbzz::scene::managed_detail
