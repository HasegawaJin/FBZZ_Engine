/// @file    NavMeshOffMeshLinkComponent.hpp
/// @brief   NavMesh 上の非連続エリアを接続するオフメッシュリンク（Unity の Off-Mesh Link 相当）。
/// @author  Hasegawa Jin
/// @date    2026-06-17
///
/// GO をリンクの起点側に置き、startPoint / endPoint をワールド座標で指定する。
/// NavMeshBakeSystem がベイク完了後に最近傍ポリゴンへ自動接続する。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

struct NavMeshOffMeshLinkComponent {
    math::Vector3 startPoint = math::Vector3::ZERO; // リンク起点（ワールド座標）
    math::Vector3 endPoint   = math::Vector3::ZERO; // リンク終点（ワールド座標）
    bool enabled        = true;
    bool bidirectional  = true;   // true: 双方向、false: startPoint → endPoint のみ
    bool activated      = true;   // false: A* がこのリンクを無視する
    float traversalTime = 0.3f;   // 通過アニメーション時間 [s]（0 = 即座に移動）
    // このリンクを使える agentTypeId のビットマスク。-1 = すべての Agent Type が通過可。
    // 例: agentTypeId=1 の Agent のみ → agentTypeMask = (1 << 1) = 2
    int agentTypeMask   = -1;

    const char* GetTypeName() const { return "Off-Mesh Link"; }
    void Reflect(IReflector& r)
    {
        r.Field("startPoint",    startPoint);
        r.Field("endPoint",      endPoint);
        r.Field("bidirectional", bidirectional);
        r.Field("activated",     activated);
        r.Field("traversalTime", traversalTime);
        r.Field("agentTypeMask", agentTypeMask);
    }
};

} // namespace fbzz::scene
