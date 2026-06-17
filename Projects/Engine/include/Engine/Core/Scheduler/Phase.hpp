// FBZZ Engine
// Phase.hpp | fbzz
// フレーム内実行フェーズ定義と固定ステップ設定
#pragma once
#include <cstdint>

namespace fbzz {

enum class Phase : uint8_t {
    PreScript   = 0,   // [EditorOnly] TransformEditorPreview → FoliageBake || NavMeshBake → FoliageCull
    Script,            // ScriptSystem
    PrePhysics,        // TransformSystem（Physics 前同期）
    Physics,           // PhysicsSystem（固定ステップ）
    PostPhysics,       // TransformSystem（Physics 書き戻し）
    Navigation,        // NavMeshSensor → NavMeshPatrol → Navigation
    LateScript,        // LateScriptSystem
    Cleanup,           // LifetimeSystem → FlushDestroyQueueSystem
    LateUpdate,        // TransformSystem, AnimatorSystem, IKSystem
    Count
};

enum class RunMode : uint8_t {
    Always,      // エディタ停止中・シミュレーション中どちらでも実行
    SimOnly,     // シミュレーション中のみ
    EditorOnly,  // エディタ停止中のみ
};

struct PhaseConfig {
    bool  fixedStep  = false;
    int   hz         = 60;
    float maxCatchUp = 8.0f;   // fixedDt × maxCatchUp が accumulator 上限
};

} // namespace fbzz
