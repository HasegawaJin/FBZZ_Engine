/// @file    NavMeshOffMeshLinkComponent.hpp
/// @brief   Authored numeric or TargetPoint-backed endpoints for a NavMesh off-mesh link.
/// @author  Hasegawa Jin
/// @date    2026-06-17
#pragma once

#include <Engine/Scene/ControlPointModes.hpp>
#include <Engine/Scene/ObjectReference.hpp>
#include <Engine/Scene/Reflection.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

/// @brief Connect two NavMesh polygons with numeric world-space endpoints.
struct NavMeshOffMeshLinkComponent {
    math::Vector3 startPoint = math::Vector3::ZERO;
    math::Vector3 endPoint = math::Vector3::ZERO;
    ControlPointSourceMode pointSourceMode = ControlPointSourceMode::INLINE;
    ObjectReference startTarget;
    ObjectReference endTarget;
    bool enabled = true;
    bool bidirectional = true;
    bool activated = true;
    float traversalTime = 0.3f;
    int agentTypeMask = -1;

    /// @brief Return the Inspector component label.
    const char* GetTypeName() const { return "Off-Mesh Link"; }

    /// @brief Reflect legacy endpoints and optional typed TargetPoint endpoints.
    void Reflect(IReflector& reflector)
    {
        reflector.Field("enabled", enabled);
        reflector.Field("startPoint", startPoint);
        reflector.Field("endPoint", endPoint);
        ReflectControlPointSourceMode(reflector, "pointSourceMode", pointSourceMode);
        reflector.TypedObjectReferenceField("startTarget", startTarget, "TargetPointComponent");
        reflector.TypedObjectReferenceField("endTarget", endTarget, "TargetPointComponent");
        reflector.Field("bidirectional", bidirectional);
        reflector.Field("activated", activated);
        reflector.Field("traversalTime", traversalTime);
        reflector.Field("agentTypeMask", agentTypeMask);
    }
};

} /// @note namespace fbzz::scene
