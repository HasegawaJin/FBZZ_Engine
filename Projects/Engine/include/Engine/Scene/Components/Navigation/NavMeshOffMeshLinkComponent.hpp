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
    math::Vector3 startPoint = math::Vector3::ZERO; ///< インライン指定時のリンク起点のワールド座標 [m]。
    math::Vector3 endPoint = math::Vector3::ZERO; ///< インライン指定時のリンク終点のワールド座標 [m]。
    ControlPointSourceMode pointSourceMode = ControlPointSourceMode::INLINE; ///< 両端をワールド座標で指定するか、シーン上の参照で指定するか。
    ObjectReference startTarget; ///< 参照指定時にリンクの起点を与えるシーンオブジェクト。
    ObjectReference endTarget; ///< 参照指定時にリンクの終点を与えるシーンオブジェクト。
    bool enabled = true; ///< システムがこのリンクを扱うかどうか。
    bool bidirectional = true; ///< 終点から起点へ逆方向に渡ることを許すかどうか。
    bool activated = true; ///< 経路探索時にこのリンクを通過可能として扱うかどうか。
    float traversalTime = 0.3f; ///< リンク上の移動を補間する時間 [秒]。
    int agentTypeMask = -1; ///< リンクを使えるエージェント種別のビット集合。-1 は全種別。

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
