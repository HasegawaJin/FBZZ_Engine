/// @file    ManagedStoreTransfers.hpp
/// @brief   Save/Config の同期・未公開 UTF8 受信と一度の値 commit。
/// @author  Hasegawa Jin
/// @date    2026-10-05
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <memory>
#include <span>
#include <string>
namespace fbzz::scene {
class Scene;
class GameObject;
namespace managed_detail {
struct ManagedServiceResult;
class ManagedStoreTransfers final {
public:
    FBZZ_ENGINE_API ManagedStoreTransfers();
    FBZZ_ENGINE_API ~ManagedStoreTransfers();
    FBZZ_ENGINE_API ManagedBridgeStatus Begin(Scene& scene, GameObject& target, const ManagedBridgeRequest& request,
        int64_t expectedBytes, std::span<const float> values, std::span<const std::string> text, ManagedServiceResult& result);
    FBZZ_ENGINE_API ManagedBridgeStatus Run(Scene& scene, GameObject& target, const ManagedBridgeRequest& request, ManagedServiceResult& result);
    FBZZ_ENGINE_API void CancelOwner(uint64_t owner);
    FBZZ_ENGINE_API void RetireGeneration(uint64_t generation);
    FBZZ_ENGINE_API void EndCall(uint64_t call);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} /// @note namespace managed_detail
} /// @note namespace fbzz::scene
