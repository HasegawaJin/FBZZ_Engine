/// @file    SequenceSystem.hpp
/// @brief   .sequence を時刻で評価し、複数の GameObject を 1 本の時間軸へ載せる
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

/// @note Phase::LateScript: Transform (TransformLateUpdate) も Animator の Slot も VFX も Audio
/// @note も同じフレームで拾える。Phase::LateUpdate だと TransformLateUpdate より後ろになり、
/// @note ワールド行列が 1 フレーム古いまま描画される。
/// @note RunMode::Always: エディタ停止中もスクラブできるようにする。シミュレーション中でない
/// @note ときは editorScrubTime が指定された Player だけを評価する。
class SequenceSystem final : public ISystem {
public:
    std::string_view Name() const override { return "SequenceSystem"; }
    Phase   GetPhase()   const override { return Phase::LateScript; }
    RunMode GetRunMode() const override { return RunMode::Always; }
    FBZZ_ENGINE_API ComponentAccess GetAccess() const override;
    FBZZ_ENGINE_API OrderingHints   GetOrder()  const override;
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;
};

} /// @note namespace fbzz::scene
