/// @file    ManagedMaterialTransfers.hpp
/// @brief   Material 数値列の callback 所有 snapshot と全件検証後の atomic 書込み。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <memory>
#include <span>
#include <string>
namespace fbzz::scene {
class Scene; class GameObject; class Script;
namespace managed_detail {
struct ManagedServiceResult;
class ManagedMaterialTransfers final {
public:
    ManagedMaterialTransfers(); ~ManagedMaterialTransfers();
    ManagedBridgeStatus Open(Scene& scene, GameObject& target, Script& proxy, const ManagedBridgeRequest& request,
        int64_t slot, std::span<const float> values, std::span<const std::string> text, ManagedServiceResult& result);
    ManagedBridgeStatus Run(Scene& scene, GameObject& target, Script& proxy, const ManagedBridgeRequest& request,
        ManagedServiceResult& result, ManagedBridgeReply* rawReply = nullptr);
    void CancelOwner(uint64_t owner); void RetireGeneration(uint64_t generation); void EndCall(uint64_t call);
private:
    struct Impl; std::unique_ptr<Impl> m_impl;
};
} /// @note namespace managed_detail
} /// @note namespace fbzz::scene
