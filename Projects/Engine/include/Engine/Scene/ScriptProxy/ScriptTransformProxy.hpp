// FBZZ Engine
// ScriptTransformProxy.hpp | fbzz::scene
// Script から Transform 操作へ転送するショートハンド
#pragma once

#include <Math/Vector3.hpp>

namespace fbzz::scene {

struct Transform;
class GameObject;
class Script;

struct ScriptTransformProxy {
    Script* script = nullptr;

    Transform* Get() const;
    Transform* operator->() const;
    explicit operator bool() const { return Get() != nullptr; }

    void SetPosition(const math::Vector3& v) const;
    void Translate(const math::Vector3& v) const;
    void Rotate(const math::Vector3& axis, float degrees) const;
    void LookAt(const math::Vector3& target) const;
    float DistanceTo(const GameObject& other) const;
    math::Vector3 DirectionTo(const GameObject& other) const;
};

} // namespace fbzz::scene
