// FBZZ Engine
// ScriptColliderProxy.hpp | fbzz::scene
// Script から ColliderComponent を安全に更新するショートハンド
#pragma once

#include <Math/Vector3.hpp>
#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptColliderProxy {
    Script* script = nullptr;

    void SetEnabled(bool enabled) const;
    void SetTrigger(bool trigger) const;
    void SetCenter(const math::Vector3& center) const;
    void SetBoxSize(const math::Vector3& size) const;
    void SetSphereRadius(float radius) const;
    void SetCapsule(float radius, float halfHeight) const;
    void SetMesh(std::string_view meshPath, int meshIndex = 0) const;
    void SetFriction(float staticFriction, float dynamicFriction) const;
    void SetRestitution(float restitution) const;
};

} // namespace fbzz::scene
