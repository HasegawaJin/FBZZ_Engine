// FBZZ Engine
// BehaviorTreeComponent.hpp | fbzz::scene
// エージェント 1 体に Behavior Tree を持たせるコンポーネント
//
// WHY 木そのものを持たないか:
//   木の構造は共有・不変で、100 体が同じ木を使っても実体は 1 つでよい。
//   ここが持つのは「実行状態」(どのノードが Running か、Blackboard の中身) だけ。
//   構造は shared_ptr<const BehaviorTreeRuntime> でパス単位に共有する。
#pragma once
#include <Engine/AI/BehaviorTreeEvaluator.hpp>
#include <Engine/AI/BehaviorTreeRuntime.hpp>
#include <Engine/AI/Blackboard.hpp>
#include <Engine/Scene/Script.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::scene {

struct BehaviorTreeComponent {
    // ── 永続化されるフィールド (Reflect に載せる) ────────────────────────────
    std::string treePath;              // "Assets/AI/Guard.behaviortree"
    bool  enabled     = true;
    // 評価間隔 [s]。0 で毎フレーム。
    //
    // WHY 既定を 0.1 秒にするか: BT の評価は毎フレームである必要がない。
    //     人間の反応速度は 200ms 程度で、100ms 間隔なら知覚上まったく違和感がない。
    //     敵 100 体を毎フレーム評価するのは純粋に無駄で、1/6 に間引けば
    //     そのぶんを描画に回せる。位相は個体ごとにずらすので集中もしない。
    float tickRate    = 0.1f;
    bool  startPaused = false;

    // ── ランタイム状態 (Reflect に載せない) ──────────────────────────────────
    // WHY 載せないか: Scene 保存にも Undo にも乗せる意味がなく、
    //     載せると木の実行状態がシーンファイルへ書き出されてしまう。
    //     VFXGraphComponent が runtimeNodes を Reflect していないのと同じ流儀。
    std::shared_ptr<const ai::BehaviorTreeRuntime> runtime;
    std::string          loadedTreePath;
    ai::Blackboard       blackboard;
    ai::BTInstanceState  state;
    float                tickTimer        = 0.0f;
    float                elapsedTime      = 0.0f;   // Blackboard の時間条件の基準
    bool                 initialized      = false;
    bool                 reloadRequested  = false;
    bool                 restartRequested = false;
    ai::BTStatus         lastRootStatus   = ai::BTStatus::Failure;
    std::uint32_t        tickCount        = 0;

    // エディタの実行中ハイライト用。nodes と同サイズ。
    // 0 = 未評価 / 1 = Success / 2 = Failure / 3 = Running
    std::vector<std::uint8_t> lastNodeStatus;

    const char* GetTypeName() const { return "Behavior Tree"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);

        r.BeginField("treePath", "Tree");
        r.SetFileExtensions(".behaviortree");
        r.Field("treePath", treePath);

        r.BeginField("tickRate", "Tick Rate");
        r.FloatRange("tickRate", tickRate, 0.0f, 1.0f);
        r.Tooltip("評価間隔 [s]。0 で毎フレーム。\n"
                  "個体ごとに位相をずらすため、同時スポーンでも評価が集中しません。");

        r.Field("startPaused", startPaused);
    }

    // 木を差し替える。次の評価で再ロードされ、実行状態はリセットされる。
    void SwitchTree(std::string_view path)
    {
        treePath = path;
        reloadRequested = true;
    }

    // 木を最初から評価し直す (Blackboard は保持する)。
    void Restart() { restartRequested = true; }

    [[nodiscard]] bool IsRunning() const { return lastRootStatus == ai::BTStatus::Running; }
};

} // namespace fbzz::scene
