/// @file    ManagedWorldFamilyServices.hpp
/// @brief   Terrain/Water/VFX/Sequence 固定 packet selectors の native 接続。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <span>
#include <string>
namespace fbzz::scene { class Scene; class Script; class GameObject; }
namespace fbzz::scene::managed_detail {
struct ManagedServiceResult;
/// @pre Runtime/bridge が exact caller scope と target/related tokens を検証し、view は target に同期 bind されている。
/// @return packet/権限/不在は result 未変更。正常時のみ実 Component を変更する。
FBZZ_ENGINE_API ManagedBridgeStatus RunWorldFamilyServices(Scene& scene, Script& view, GameObject& target, GameObject* related,
    const ManagedBridgeRequest& request, int64_t integer, std::span<const float> values,
    std::span<const std::string> text, ManagedServiceResult& result, bool fixedUpdate);
} /// @note namespace fbzz::scene::managed_detail