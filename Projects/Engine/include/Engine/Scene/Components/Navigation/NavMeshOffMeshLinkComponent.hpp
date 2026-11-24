/// @file    NavMeshOffMeshLinkComponent.hpp
/// @brief   NavMesh オフメッシュリンクの端点。数値または TargetPoint で指定する。
/// @author  Hasegawa Jin
/// @date    2026-06-17
#pragma once

#include <Engine/Scene/ControlPoints/ControlPointModes.hpp>
#include <Engine/Scene/References/ObjectReference.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>

namespace fbzz::scene {

/// @brief ワールド座標の端点で二つの NavMesh ポリゴンを結ぶ。
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

    /// @brief Inspector 上のコンポーネント名を返す。
    const char* GetTypeName() const { return "Off-Mesh Link"; }

    /// @brief 従来の数値端点と、任意指定の TargetPoint 型参照を公開する。
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

}
