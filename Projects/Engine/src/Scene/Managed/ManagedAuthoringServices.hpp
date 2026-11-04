/// @file    ManagedAuthoringServices.hpp
/// @brief   型付き Material override と実 Save/Config ストアの同期操作。
/// @author  Hasegawa Jin
/// @date    2026-10-05
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <span>
#include <string>
namespace fbzz::scene {
class Scene;
class Script;
class GameObject;
namespace managed_detail {
struct ManagedServiceResult;
/// @note proxy は現在の target へ同期 bind 済み。Material slot の範囲を確認してから既存 proxy を呼ぶ。
FBZZ_ENGINE_API ManagedBridgeStatus RunManagedAuthoringService(Scene& scene, Script& proxy, GameObject& target,
    const ManagedBridgeRequest& request, int64_t integer, std::span<const float> values,
    std::span<const std::string> text, ManagedServiceResult& result);
} /// @note namespace managed_detail
} /// @note namespace fbzz::scene
