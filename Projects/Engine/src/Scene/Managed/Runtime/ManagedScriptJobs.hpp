/// @file    ManagedScriptJobs.hpp
/// @brief   Script phase と既存 ScriptEventBus に接続する所有者付き managed jobs。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <string_view>

namespace fbzz::scene {
class Scene;
class ManagedScriptAdapter;
namespace managed_detail {
/// @brief 現在のスクリプト世代に属する時間ジョブを登録する。
/// @param[in] owner ジョブの所有者。登録後は生存と世代を照合してコールバックを配送する。
/// @param[in] request トークン、待機・繰り返し・ Tween の種別と引数。
/// @return 不正な要求や容量不足の場合は登録済みジョブを変更しない。
FBZZ_ENGINE_API ManagedBridgeStatus RegisterManagedJob(ManagedScriptAdapter& owner, const ManagedBridgeRequest& request);
FBZZ_ENGINE_API ManagedBridgeStatus SubscribeManagedEvent(ManagedScriptAdapter& owner, uint64_t token, std::string_view channel);
FBZZ_ENGINE_API ManagedBridgeStatus PublishManagedEvent(std::string_view input);
FBZZ_ENGINE_API void CancelManagedJob(ManagedScriptAdapter& owner, uint64_t token);
FBZZ_ENGINE_API void CancelManagedOwner(ManagedScriptAdapter& owner);
/// @note Script phase 一回につき一回だけ進める。新規登録は次回 tick、長いフレームの repeat は一回まで。
FBZZ_ENGINE_API void TickManagedJobs(Scene& scene, float scaledDelta, float unscaledDelta);
}
}
