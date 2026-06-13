// FBZZ Engine
// RigidBodyComponent.hpp | fbzz::scene
// physics::RigidBody を Scene に紐付けるコンポーネント
// Scene の Transform と physics::World の剛体状態を同期するための橋渡し。
// RigidBodyComponent が RigidBody の唯一の所有者。World には RigidBody* を渡す。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Physics/BodyHandle.hpp>
#include <Physics/RigidBody.hpp>
#include <memory>

namespace fbzz::scene {

struct RigidBodyComponent {
    std::unique_ptr<physics::RigidBody> rigidBody;
    physics::BodyHandle bodyHandle;
    bool enabled = true;

    RigidBodyComponent() = default;
    ~RigidBodyComponent() = default;
    RigidBodyComponent(const RigidBodyComponent& o)
        : rigidBody(o.rigidBody ? std::make_unique<physics::RigidBody>(*o.rigidBody) : nullptr)
        , bodyHandle{}
        , enabled(o.enabled)
    {}
    RigidBodyComponent& operator=(const RigidBodyComponent& o)
    {
        if (this != &o) {
            rigidBody  = o.rigidBody ? std::make_unique<physics::RigidBody>(*o.rigidBody) : nullptr;
            bodyHandle = {};
            enabled    = o.enabled;
        }
        return *this;
    }
    RigidBodyComponent(RigidBodyComponent&&)            = default;
    RigidBodyComponent& operator=(RigidBodyComponent&&) = default;

    const char* GetTypeName() const { return "Rigid Body"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        if (!rigidBody)
            return;

        // WHY: RigidBody の内部状態は private を含むため、Reflect では一度ローカル値に写し、
        //      編集後に setter 経由で戻す。これにより質量変更時の invMass / inertia 再計算を保つ。
        auto& body = *rigidBody;
        bool isStatic = body.IsStatic();
        float mass = body.GetMass();
        math::Vector3 velocity = body.GetVelocity();
        math::Vector3 angularVelocity = body.GetAngularVelocity();
        physics::AxisLock freezePosition = body.GetFreezePosition();
        physics::AxisLock freezeRotation = body.GetFreezeRotation();

        r.Field("isStatic", isStatic);
        r.Field("mass", mass);
        r.Field("velocity", velocity);
        r.Field("angularVelocity", angularVelocity);
        r.Field("freezePositionX", freezePosition.x);
        r.Field("freezePositionY", freezePosition.y);
        r.Field("freezePositionZ", freezePosition.z);
        r.Field("freezeRotationX", freezeRotation.x);
        r.Field("freezeRotationY", freezeRotation.y);
        r.Field("freezeRotationZ", freezeRotation.z);
        r.Field("useGravity", body.m_useGravity);
        r.Field("gravityScale", body.m_gravityScale);
        r.Field("linearDrag", body.m_linearDrag);
        r.Field("angularDrag", body.m_angularDrag);
        r.Field("allowSleeping", body.m_allowSleeping);
        r.Field("useCCD", body.m_useCCD);
        r.Field("ccdRadius", body.m_ccdRadius);
        r.Field("charge", body.m_charge);
        r.Field("isGravitationalSource", body.m_isGravitationalSource);
        r.Field("gravitationalMass", body.m_gravitationalMass);

        body.m_isStatic = isStatic;
        body.SetMass(mass);
        body.SetVelocity(velocity);
        body.SetAngularVelocity(angularVelocity);
        body.SetFreezePosition(freezePosition);
        body.SetFreezeRotation(freezeRotation);
    }
};

} // namespace fbzz::scene
