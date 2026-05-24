// FBZZ Engine
// RigidBodyComponent.hpp | fbzz::scene
// physics::RigidBody を Scene に紐付けるコンポーネント
// Scene の Transform と physics::World の剛体状態を同期するための橋渡し。
// RigidBody の共有所有は World と Component の間で行う。
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
