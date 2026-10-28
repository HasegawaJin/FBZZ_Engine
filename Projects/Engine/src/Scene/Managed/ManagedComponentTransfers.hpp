/// @file    ManagedComponentTransfers.hpp
/// @brief   Ragdoll の骨名 recipe 全件を同期 call が未公開候補として所有する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <memory>
#include <span>
#include <string>
namespace fbzz::scene { class Scene; class GameObject; }
namespace fbzz::scene::managed_detail {
struct ManagedServiceResult;
class ManagedComponentTransfers final {
public:
    ManagedComponentTransfers();
    ~ManagedComponentTransfers();
    /// @note selector40 は depth と count と対象 recipe、41 は順序付き chunk、42 は commit、43 は abort。
    ManagedBridgeStatus Run(Scene& scene, GameObject& target, const ManagedBridgeRequest& request,
        int64_t integer, std::span<const float> values, std::span<const std::string> text,
        ManagedServiceResult& result, bool fixedUpdate);
    void EndCall(uint64_t call);
    void CancelOwner(uint64_t owner);
    void RetireGeneration(uint64_t generation);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} /// @note namespace fbzz::scene::managed_detail
