/// @file    SpatialDebugSnapshot.hpp
/// @brief   Value requests and immutable publications across the UI/Physics owner boundary.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <Physics/BroadPhaseDebugSnapshot.hpp>
#include <cstdint>
#include <memory>

namespace fbzz::scene {

/// @note sceneGeneration is Scene::GetRenderSceneGeneration(); zero disables publication.
/// @note Only PhysicsSystem resolves selectedEntity; physics.selected and selectedWorldEpoch are ignored.
struct PhysicsSpatialDebugRequest {
    uint64_t sceneGeneration = 0;
    EntityID selectedEntity;
    physics::BroadPhaseDebugRequest physics;
};

/// @note Retained values are safe on other threads; no Scene, World or Collider pointer is published.
struct PhysicsSpatialDebugSnapshot {
    uint64_t sceneGeneration = 0;
    uint64_t revision = 0;
    std::shared_ptr<const physics::BroadPhaseDebugSnapshot> physics;
};

} /// @note namespace fbzz::scene
