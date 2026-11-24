/// @file    ManagedMaterialTransfers.hpp
/// @brief   Material 数値列の callback 所有 snapshot と全件検証後の atomic 書込み。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <Engine/Api.hpp>
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
    FBZZ_ENGINE_API ManagedMaterialTransfers(); FBZZ_ENGINE_API ~ManagedMaterialTransfers();
    FBZZ_ENGINE_API ManagedBridgeStatus Open(Scene& scene, GameObject& target, Script& proxy, const ManagedBridgeRequest& request,
        int64_t slot, std::span<const float> values, std::span<const std::string> text, ManagedServiceResult& result);
    FBZZ_ENGINE_API ManagedBridgeStatus Run(Scene& scene, GameObject& target, Script& proxy, const ManagedBridgeRequest& request,
        ManagedServiceResult& result, ManagedBridgeReply* rawReply = nullptr);
    FBZZ_ENGINE_API void CancelOwner(uint64_t owner); FBZZ_ENGINE_API void RetireGeneration(uint64_t generation); FBZZ_ENGINE_API void EndCall(uint64_t call);
private:
    struct Impl; std::unique_ptr<Impl> m_impl;
};
} /// @note namespace managed_detail
} /// @note namespace fbzz::scene
