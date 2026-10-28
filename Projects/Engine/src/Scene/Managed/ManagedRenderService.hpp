/// @file    ManagedRenderService.hpp
/// @brief   Script の runtime RenderSettings 設定を型付き packet から処理する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <span>
#include <string>
namespace fbzz::scene { class Script; class Scene; }
namespace fbzz::scene::managed_detail {
struct ManagedServiceResult;
ManagedBridgeStatus RunManagedRenderService(Script& proxy, const ManagedBridgeRequest& request,
    int64_t integer, std::span<const float> values, std::span<const std::string> text, ManagedServiceResult& result);
ManagedBridgeStatus RunManagedDisplayService(Scene& scene, Script& proxy, const ManagedBridgeRequest& request,
    int64_t integer, std::span<const float> values, std::span<const std::string> text, ManagedServiceResult& result);
} /// @note namespace fbzz::scene::managed_detail
