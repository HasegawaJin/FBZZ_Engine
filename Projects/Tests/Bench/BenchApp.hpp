/// @file    BenchApp.hpp
/// @brief   ビジュアル検証ベンチのホスト。場面の切り替えと再生制御だけを持つ。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// ここでテストは «実行しない»。自動テストの正は CTest / Visual Studio のテストエクスプローラー
/// であり、両方から実行できる仕組みを二重に持つと «どちらが正しいのか» が分からなくなる。
#pragma once

#include "BenchScene.hpp"
#include "Viewport2D.hpp"

#include <Engine/Core/IModule.hpp>

#include <memory>
#include <vector>

namespace fbzz::bench {

class BenchApp final : public core::IModule {
public:
    [[nodiscard]] bool OnInit() override;
    void OnUpdate(float dt) override;
    void OnLateUpdate(float dt) override;
    void OnRender() override;
    void OnShutdown() override;

private:
    void SelectScene(int index);
    void DrawUI();
    void DrawSidebar();
    void DrawAnomalies();
    void DrawPerformance();
    void DrawViewportPanel();

    std::vector<std::unique_ptr<BenchScene>> m_scenes;
    int                                      m_selected = 0;
    Viewport2D                               m_view;
    AnomalyLog                               m_anomalies;

    bool  m_paused        = false;
    bool  m_stepRequested = false;
    float m_speed         = 1.0f;
    /// 実時間の揺れを持ち込まないよう、固定刻みを溜めてから進める。
    float m_accumulator = 0.0f;

    /// 溜まった刻みを消化しきれなかったフレーム数。0 でなければ絵は実時間より遅れている。
    int   m_droppedFrames = 0;
    int   m_stepsThisFrame = 0;
    /// 表示用にならした値 [ms]。生の値は毎フレーム跳ねて読めない。
    float m_simMsAvg      = 0.0f;
    float m_frameMsAvg    = 0.0f;

    void* m_imguiContext = nullptr;
};

} // namespace fbzz::bench
