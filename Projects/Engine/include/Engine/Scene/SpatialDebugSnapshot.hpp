/// @file    SpatialDebugSnapshot.hpp
/// @brief   UI と Physics の所有境界を越える値リクエストと不変の公開データ。
/// @author  Hasegawa Jin
/// @date    2026-10-24
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <Physics/BroadPhaseDebugSnapshot.hpp>
#include <cstdint>
#include <memory>

namespace fbzz::scene {

/// @note sceneGeneration は Scene::GetRenderSceneGeneration() の値。0 で公開を無効にする。
/// @note selectedEntity の解決は PhysicsSystem だけが行い、physics.selected と selectedWorldEpoch は使わない。
struct PhysicsSpatialDebugRequest {
    uint64_t sceneGeneration = 0;
    EntityID selectedEntity;
    physics::BroadPhaseDebugRequest physics;
};

/// @note 保持する値は別スレッドで利用できる。Scene、World、Collider のポインターは公開しない。
struct PhysicsSpatialDebugSnapshot {
    uint64_t sceneGeneration = 0;
    uint64_t revision = 0;
    std::shared_ptr<const physics::BroadPhaseDebugSnapshot> physics;
};

}
