/// @file    ManagedJointTransfers.hpp
/// @brief   Joint chain の全件候補と row birth を同期 call が所有する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <memory>
#include <span>
#include <string>
namespace fbzz::scene { class Scene; class GameObject; }
namespace fbzz::scene::managed_detail {
struct ManagedServiceResult;
class ManagedJointTransfers final {
public:
    using Resolver = ManagedBridgeStatus (*)(void*, uint64_t, GameObject*&);
    FBZZ_ENGINE_API ManagedJointTransfers(); FBZZ_ENGINE_API ~ManagedJointTransfers();
    FBZZ_ENGINE_API ManagedBridgeStatus Begin(Scene&, GameObject&, const ManagedBridgeRequest&, int64_t count,
        std::span<const float>, std::span<const std::string>, ManagedServiceResult&, bool fixed);
    FBZZ_ENGINE_API ManagedBridgeStatus Run(Scene&, GameObject&, const ManagedBridgeRequest&, ManagedServiceResult&, Resolver, void*, bool fixed);
    FBZZ_ENGINE_API void EndCall(uint64_t call); FBZZ_ENGINE_API void CancelOwner(uint64_t owner); FBZZ_ENGINE_API void RetireGeneration(uint64_t generation);
private:
    struct Impl; std::unique_ptr<Impl> m_impl;
};
} /// @note namespace fbzz::scene::managed_detail
