/// @file    ManagedAssetServices.hpp
/// @brief   実 ScriptAssetProxy が所有する要求を managed 個体と世代へ束縛する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <memory>
#include <span>
#include <string>
namespace fbzz::scene { class Script; class GameObject; class Scene; }
namespace fbzz::scene::managed_detail {
struct ManagedServiceResult;
class ManagedAssetServices final {
public:
    FBZZ_ENGINE_API ManagedAssetServices(); FBZZ_ENGINE_API ~ManagedAssetServices();
    FBZZ_ENGINE_API ManagedBridgeStatus Run(Scene&, Script& caller, GameObject& target, const ManagedBridgeRequest&, int64_t,
        std::span<const float>, std::span<const std::string>, ManagedServiceResult&);
    FBZZ_ENGINE_API void CancelOwner(uint64_t owner, Script* caller);
    FBZZ_ENGINE_API void RetireGeneration(uint64_t generation);
    FBZZ_ENGINE_API void EndCall(uint64_t call);
private:
    struct Impl; std::unique_ptr<Impl> m_impl;
};
} /// @note namespace fbzz::scene::managed_detail
