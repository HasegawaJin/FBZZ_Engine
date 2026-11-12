/// @file    BehaviorTreeComponent.hpp
/// @brief   エージェント 1 体に Behavior Tree を持たせるコンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note 木の構造は共有・不変なので `shared_ptr<const BehaviorTreeRuntime>` でパス単位に共有し、
/// @note 本体は実行状態 (どのノードが Running か、Blackboard の中身) だけを持つ。
#pragma once
#include <Engine/AI/BehaviorTreeEvaluator.hpp>
#include <Engine/AI/BehaviorTreeRuntime.hpp>
#include <Engine/AI/Blackboard.hpp>
#include <Engine/Scene/Reflection.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::scene {

struct BehaviorTreeComponent {
    /// @name 永続化されるフィールド (Reflect に載せる)
    /// @{
    std::string treePath;              ///< @note "Assets/AI/Guard.behaviortree"
    bool  enabled     = true;
    /// @note 評価間隔 [s]。0 で毎フレーム。
    ///
    /// @note 既定 0.1 秒。人間の反応速度 (~200ms) に対し知覚上問題ない間引き。位相は個体ごとにずらす。
    float tickRate    = 0.1f;
    float farTickDistance = 0.0f; ///< @note すべての有効カメラからこの距離 [m] より遠い場合に適用する。0 で無効。
    float farTickRate = 0.5f; ///< @note 遠景での最小評価間隔 [s]。知覚・Blackboard の変更による wake は優先する。
    bool  startPaused = false;
    /// @}

    /// @name ランタイム状態 (Reflect に載せない)
    /// @{
    /// @note Reflect に載せると実行状態がシーンファイルへ書き出されてしまうため除外する
    /// @note (VFXGraphComponent が runtimeNodes を Reflect しないのと同じ流儀)。
    std::shared_ptr<const ai::BehaviorTreeRuntime> runtime;
    std::string          loadedTreePath;
    ai::Blackboard       blackboard;
    ai::BTInstanceState  state;
    float                tickTimer        = 0.0f;
    float                tickElapsed      = 0.0f;
    bool                 evaluationRequested = false;
    bool                 patrolRequested = false;
    std::uint64_t        evaluatedRevision = 0;
    float                elapsedTime      = 0.0f;   ///< @note Blackboard の時間条件の基準
    bool                 initialized      = false;
    bool                 reloadRequested  = false;
    bool                 restartRequested = false;
    ai::BTStatus         lastRootStatus   = ai::BTStatus::Failure;
    std::uint32_t        tickCount        = 0;

    /// @note エディタの実行中ハイライト用。nodes と同サイズ。
    /// @note 0 = 未評価 / 1 = Success / 2 = Failure / 3 = Running
    std::vector<std::uint8_t> lastNodeStatus;

    const char* GetTypeName() const { return "Behavior Tree"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);

        r.BeginField("treePath", "Tree");
        r.SetFileExtensions(".behaviortree");
        r.Field("treePath", treePath);
        r.EndField();

        r.BeginField("tickRate", "Tick Rate");
        r.FloatRange("tickRate", tickRate, 0.0f, 1.0f);
        r.Tooltip("評価間隔 [s]。0 で毎フレーム。\n"
                  "個体ごとに位相をずらすため、同時スポーンでも評価が集中しません。");
        r.EndField();

        r.FloatRange("farTickDistance", farTickDistance, 0.0f, 10000.0f);
        r.FloatRange("farTickRate", farTickRate, 0.0f, 10.0f);
        r.Field("startPaused", startPaused);
    }

    /// @note 木を差し替える。次の評価で再ロードされ、実行状態はリセットされる。
    void SwitchTree(std::string_view path)
    {
        treePath = path;
        reloadRequested = true;
    }

    /// @note 木を最初から評価し直す (Blackboard は保持する)。
    void Restart() { restartRequested = true; }

    /// @brief Wakes the next system update without discarding running action state.
    void RequestEvaluation() { evaluationRequested = true; }

    [[nodiscard]] bool IsRunning() const { return lastRootStatus == ai::BTStatus::Running; }
    /// @}
};

} /// @note namespace fbzz::scene
