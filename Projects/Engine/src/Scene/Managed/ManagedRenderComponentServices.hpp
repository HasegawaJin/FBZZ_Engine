/// @file    ManagedRenderComponentServices.hpp
/// @brief   型付き描画Componentサービスのnative検証境界。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <span>
#include <string>
namespace fbzz::scene {
class Scene; class Script; class GameObject;
namespace managed_detail {
struct ManagedServiceResult;
/// @return 入力・権限・rowの検証失敗は対象とresultを変更しない。
ManagedBridgeStatus RunRenderComponentServices(Scene& scene, Script& view, GameObject& target, GameObject* related,
    const ManagedBridgeRequest& request, int64_t integer, std::span<const float> values,
    std::span<const std::string> text, ManagedServiceResult& result);
}
}
