/// @file    SequenceSystem.hpp
/// @brief   .sequence を時刻で評価し、複数の GameObject を 1 本の時間軸へ載せる
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

/// WHY Phase::LateScript か:
///   ここへ置くと Transform (LateUpdate の TransformLateUpdate) も
///   Animator の Slot も VFX も Audio も、すべて同じフレームのうちに拾われる。
///   Phase::LateUpdate へ入れると TransformLateUpdate より後ろになり、
///   ワールド行列が 1 フレーム古いまま描画される。
///
/// WHY RunMode::Always か: エディタで停止したままスクラブするため。
///   シミュレーション中でないときは editorScrubTime が指定された Player だけを評価する。
class SequenceSystem final : public ISystem {
public:
    std::string_view Name() const override { return "SequenceSystem"; }
    Phase   GetPhase()   const override { return Phase::LateScript; }
    RunMode GetRunMode() const override { return RunMode::Always; }
    ComponentAccess GetAccess() const override;
    OrderingHints   GetOrder()  const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
