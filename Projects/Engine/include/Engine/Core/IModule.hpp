/// @file    IModule.hpp
/// @brief   Application の共通メインループから呼び出される実行単位インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#pragma once

namespace fbzz::core {

/// Application::Run() が駆動するゲーム / エディタ固有処理の抽象インターフェース。
/// WHY: Window / Input / Time / Memory / Profiler の共通フレーム処理を Application に集約し、
///      Sandbox や Editor は「何を更新・描画するか」だけを実装することで main.cpp の肥大化を避ける。
class IModule {
public:
    virtual ~IModule() = default;

    /// ループ開始前に 1 回だけ呼ばれる初期化フェーズ。
    /// @return false の場合は OnShutdown() を呼んだうえでループに入らず終了する。
    [[nodiscard]] virtual bool OnInit() = 0;

    /// 毎フレーム呼ばれる主更新フェーズ。
    /// ホスト固有のフレーム処理を実行する。
    /// WHY: プロジェクトのScene/Script/Physics更新順はProjectRuntimeへ集約する。
    ///      ModuleがSceneManagerやWorldを直接駆動するとEditor/Standalone差異が再発するため、
    ///      ゲームSimulationを持つModuleはProjectRuntime::Update()へ委譲すること。
    virtual void OnUpdate(float dt) = 0;

    /// OnUpdate() の後に呼ばれる遅延更新フェーズ。
    /// アニメーション、IK、LateUpdate 相当の後処理を実行する。
    virtual void OnLateUpdate(float dt) = 0;

    /// 毎フレーム呼ばれる描画フェーズ。
    /// renderer.BeginFrame() から renderer.EndFrame() までの描画責務を持つ。
    virtual void OnRender() = 0;

    /// ループ終了後、Application::Shutdown() より前に 1 回だけ呼ばれる終了処理フェーズ。
    virtual void OnShutdown() = 0;
};

} // namespace fbzz::core
