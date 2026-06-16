// FBZZ Engine
// NavMeshPatrolComponent.hpp | fbzz::scene
// NavMeshAgentComponent を巡回ウェイポイント間で自動移動させる定義とランタイム状態
//
// WHY: 「巡回して敵を見つけたら追跡」は最も基本的なゲーム AI パターンのため、
//      NavMeshAgentComponent の SetDestination を毎ウェイポイントで呼ぶだけの薄い
//      System として用意し、ユーザー Script から手動で呼ぶ必要がないようにする。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::scene {

struct NavMeshPatrolComponent {
    enum class Mode : uint8_t {
        LOOP,      // 最後のウェイポイントの次は先頭へ戻る
        PING_PONG, // 最後まで進んだら逆順に戻る
    };

    std::vector<math::Vector3> waypoints; // ワールド座標
    Mode  mode     = Mode::LOOP;
    float waitTime = 0.0f; // 各ウェイポイント到達後の待機時間 [s]

    bool enabled = true;

    // ── ランタイム状態 (NavMeshPatrolSystem が管理。非永続化) ────────────────
    size_t currentIndex = 0;
    int    direction    = 1; // PING_PONG 用: +1 または -1
    bool   waiting       = false;
    float  waitTimer     = 0.0f;
    bool   started       = false; // 最初のウェイポイントへまだ向かっていない

    const char* GetTypeName() const { return "NavMesh Patrol"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("waitTime", waitTime);
        // waypoints は vector<Vector3> のため SceneSerializer が専用コードで読み書きする。
    }
};

} // namespace fbzz::scene
