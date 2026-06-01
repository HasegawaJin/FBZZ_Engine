// FBZZ Engine
// ScriptPhysicsProxy.hpp | fbzz::scene
// Script から物理コンポーネントへ転送するショートハンド
#pragma once

#include <Math/Vector3.hpp>
#include <vector>

namespace fbzz::physics { class Collider; }

namespace fbzz::scene {

class GameObject;
class Script;

// RaycastHit — Script から physics::World のヒット情報を扱うための軽量 DTO。
// WHY: Script では GameObject を優先して扱えるようにしつつ、必要なら低レベル Collider も参照できる。
struct RaycastHit {
    GameObject* gameObject = nullptr;
    const physics::Collider* collider = nullptr;
    math::Vector3 point = math::Vector3::ZERO;
    math::Vector3 normal = math::Vector3::UP;
    float distance = 0.0f;
};

struct ScriptPhysicsProxy {
    Script* script = nullptr;

    void AddForce(const math::Vector3& v) const;
    void AddImpulse(const math::Vector3& v) const;
    void SetVelocity(const math::Vector3& v) const;
    math::Vector3 GetVelocity() const;
    void AddTorque(const math::Vector3& v) const;
    bool Raycast(const math::Vector3& origin, const math::Vector3& dir, float dist, RaycastHit& hit) const;
    std::vector<RaycastHit> RaycastAll(const math::Vector3& origin, const math::Vector3& dir, float dist) const;
    bool SphereCast(const math::Vector3& center, float radius, const math::Vector3& dir, float dist, RaycastHit& hit) const;
    std::vector<GameObject*> OverlapSphere(const math::Vector3& center, float radius) const;
};

} // namespace fbzz::scene
