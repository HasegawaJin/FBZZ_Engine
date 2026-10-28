/// @file    ManagedScriptJobs.hpp
/// @brief   Script phase と既存 ScriptEventBus に接続する所有者付き managed jobs。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <string_view>

namespace fbzz::scene {
class Scene;
class ManagedScriptAdapter;
namespace managed_detail {
ManagedBridgeStatus RegisterManagedJob(ManagedScriptAdapter& owner, const ManagedBridgeRequest& request);
ManagedBridgeStatus SubscribeManagedEvent(ManagedScriptAdapter& owner, uint64_t token, std::string_view channel);
ManagedBridgeStatus PublishManagedEvent(std::string_view input);
void CancelManagedJob(ManagedScriptAdapter& owner, uint64_t token);
void CancelManagedOwner(ManagedScriptAdapter& owner);
/// @note Script phase 一回につき一回だけ進める。新規登録は次回 tick、長いフレームの repeat は一回まで。
void TickManagedJobs(Scene& scene, float scaledDelta, float unscaledDelta);
}
}
