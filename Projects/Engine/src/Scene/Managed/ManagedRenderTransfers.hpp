/// @file    ManagedRenderTransfers.hpp
/// @brief   Line と手続き Mesh の未公開 CPU buffer を同期 scope 内で一度だけ commit する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <memory>
#include <span>
#include <string>

namespace fbzz::scene {
class Scene;
class GameObject;
class Script;
namespace managed_detail {
struct ManagedServiceResult;
class ManagedRenderTransfers final {
public:
    ManagedRenderTransfers();
    ~ManagedRenderTransfers();
    /// @note 全 stage を owner/generation/instance/call/Scene/Entity/object token/Component birth に結ぶ。
    /// @return 失敗で live geometry と result は未変更。正常 commit だけが候補 buffer の所有を移す。
    ManagedBridgeStatus Run(Scene& scene, GameObject& target, Script& view, const ManagedBridgeRequest& request,
        int64_t integer, std::span<const float> values, std::span<const std::string> text, ManagedServiceResult& result);
    void CancelOwner(uint64_t owner);
    void RetireGeneration(uint64_t generation);
    void EndCall(uint64_t call);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} /// @note namespace managed_detail
} /// @note namespace fbzz::scene
