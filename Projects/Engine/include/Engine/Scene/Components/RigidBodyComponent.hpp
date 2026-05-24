// FBZZ Engine
// RigidBodyComponent.hpp | fbzz::scene
// physics::RigidBody を Scene に紐付けるコンポーネント
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Physics/RigidBody.hpp>
#include <memory>

namespace fbzz::scene {

struct RigidBodyComponent {
    std::shared_ptr<physics::RigidBody> rigidBody;
    bool enabled = true;

    const char* GetTypeName() const { return "Rigid Body"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
    }
};

} // namespace fbzz::scene
