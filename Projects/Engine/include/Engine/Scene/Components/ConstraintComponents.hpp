// FBZZ Engine
// ConstraintComponents.hpp | fbzz::scene
// Bone Socket追従とTransform制約をGameObject間で再利用するComponent
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <string>

namespace fbzz::scene {

struct SocketAttachmentComponent {
    bool enabled = true;
    EntityRef target;
    std::string socketName;
    math::Vector3 positionOffset = math::Vector3::ZERO;
    math::Vector3 rotationOffsetDegrees = math::Vector3::ZERO;
    math::Vector3 scaleMultiplier = math::Vector3::ONE;
    bool followPosition = true;
    bool followRotation = true;
    bool followScale = false;

    const char* GetTypeName() const { return "Socket Attachment"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("target", target);
        r.Field("socketName", socketName);
        r.Field("positionOffset", positionOffset);
        r.Field("rotationOffsetDegrees", rotationOffsetDegrees);
        r.Field("scaleMultiplier", scaleMultiplier);
        r.Field("followPosition", followPosition);
        r.Field("followRotation", followRotation);
        r.Field("followScale", followScale);
    }
};

enum class TransformConstraintMode : int {
    Parent = 0, Position = 1, Rotation = 2, Scale = 3, Aim = 4, LookAt = 5
};

struct TransformConstraintComponent {
    bool enabled = true;
    EntityRef target;
    TransformConstraintMode mode = TransformConstraintMode::Parent;
    float weight = 1.0f;
    math::Vector3 positionOffset = math::Vector3::ZERO;
    math::Vector3 rotationOffsetDegrees = math::Vector3::ZERO;
    math::Vector3 scaleMultiplier = math::Vector3::ONE;
    math::Vector3 aimAxis = math::Vector3::FORWARD;
    math::Vector3 upAxis = math::Vector3::UP;
    bool maintainOffset = true;

    const char* GetTypeName() const { return "Transform Constraint"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("target", target);
        int value = static_cast<int>(mode);
        static constexpr const char* MODES[] = {
            "Parent", "Position", "Rotation", "Scale", "Aim", "Look At"
        };
        r.Enum("mode", value, MODES);
        mode = static_cast<TransformConstraintMode>(value < 0 || value > 5 ? 0 : value);
        r.FloatRange("weight", weight, 0.0f, 1.0f);
        r.Field("positionOffset", positionOffset);
        r.Field("rotationOffsetDegrees", rotationOffsetDegrees);
        r.Field("scaleMultiplier", scaleMultiplier);
        r.Field("aimAxis", aimAxis);
        r.Field("upAxis", upAxis);
        r.Field("maintainOffset", maintainOffset);
    }
};

} // namespace fbzz::scene
