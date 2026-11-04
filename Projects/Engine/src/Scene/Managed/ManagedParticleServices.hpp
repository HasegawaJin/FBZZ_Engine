/// @file    ManagedParticleServices.hpp
/// @brief   ParticleEmitter の追加設定と GPU 縮退診断の native 接続。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <span>
#include <string>
namespace fbzz::scene { class Script; class GameObject; }
namespace fbzz::scene::managed_detail {
struct ManagedServiceResult;
/// @pre Runtime が exact caller/target/birth を照合し、view は target に bind 済み。
FBZZ_ENGINE_API ManagedBridgeStatus RunParticleServices(Script& view, GameObject& target, const ManagedBridgeRequest& request,
    int64_t integer, std::span<const float> values, std::span<const std::string> text, ManagedServiceResult& result);
FBZZ_ENGINE_API bool IsParticleServiceRead(uint32_t selector);
} /// @note namespace fbzz::scene::managed_detail
