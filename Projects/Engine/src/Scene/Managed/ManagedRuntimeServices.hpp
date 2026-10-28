/// @file    ManagedRuntimeServices.hpp
/// @brief   C# の基本 runtime サービスと所有 Cursor 要求を既存 Engine へ接続する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
#pragma once
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <memory>
#include <span>
#include <string>

namespace fbzz::scene {
class Scene;
class Script;
class GameObject;
namespace managed_detail {
struct ManagedServiceResult;
class ManagedRuntimeServices final {
public:
    ManagedRuntimeServices();
    ~ManagedRuntimeServices();
    /// @note caller は Cursor 登録の実所有 Script。authority scope と別 receiver の lifetime は Runtime が先に検証する。
    ManagedBridgeStatus Run(Scene& scene, Script& caller, GameObject& target, const ManagedBridgeRequest& request,
        int64_t integer, std::span<const float> values, std::span<const std::string> text, ManagedServiceResult& result);
    void CancelOwner(uint64_t owner);
    void RetireGeneration(uint64_t generation);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} /// @note namespace managed_detail
} /// @note namespace fbzz::scene
