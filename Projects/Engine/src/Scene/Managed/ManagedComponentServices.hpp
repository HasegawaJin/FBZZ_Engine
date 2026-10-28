/// @file    ManagedComponentServices.hpp
/// @brief   アニメーション・UI・経路・カメラ・音声・流速場の型付き実行境界。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <span>
#include <string>
namespace fbzz::scene { class Scene; class Script; class GameObject; }
namespace fbzz::scene::managed_detail {
struct ManagedServiceResult;
/// @pre 呼出元と exact row birth は Runtime が検証し、view は target に同期 bind する。
/// @return 不正 packet・scope・不在では result と Component を変更しない。
ManagedBridgeStatus RunComponentServices(Scene& scene, Script& view, GameObject& target, GameObject* related,
    const ManagedBridgeRequest& request, int64_t integer, std::span<const float> values,
    std::span<const std::string> text, ManagedServiceResult& result, bool fixedUpdate);
/// @return 対応する読み取り selector だけ true。未対応 selector は実行境界で拒否する。
bool IsComponentServiceRead(ManagedBridgeOp operation, uint32_t selector);
} /// @note namespace fbzz::scene::managed_detail
