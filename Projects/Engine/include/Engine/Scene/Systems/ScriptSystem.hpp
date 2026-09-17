/// @file    ScriptSystem.hpp
/// @brief   GameObject に付いたユーザースクリプトの実行 System。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// Start / Update などのライフサイクルを Scene 全体に対して進める。
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

/// @note RunMode::Always: FBZZ_EXECUTE_ALWAYS の Script は編集中も回す。System 単位で SimOnly
///       にすると「編集中実行の Script が居るか」を ShouldRun で毎フレーム走査することになる
///       ため、選別は Script 単位で Update 内に置く。
class ScriptSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "ScriptSystem"; }
    Phase            GetPhase()   const override { return Phase::Script; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    ComponentAccess  GetAccess()  const override { return ComponentAccess{}.Unrestricted(); }
    void Update(SystemContext& ctx) override;
};

/// 固定ステップでスクリプトの OnFixedUpdate() を回す。
/// @note Phase::Physics に置く: SystemScheduler の固定ステップ (アキュムレータ) の対象は
///       ここだけで、1 フレームに 0〜N 回実行される。PhysicsSystem より前に走らせることで
///       スクリプトが加えた力が同じステップで積分される。
/// @note FBZZ_EXECUTE_ALWAYS でも編集中は回さない: PhysicsSystem が SimOnly のため、
///       積分する相手が居ず「効かない AddForce」が積み上がるだけになる。
class FixedScriptSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "FixedScriptSystem"; }
    Phase            GetPhase()   const override { return Phase::Physics; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override { return ComponentAccess{}.Unrestricted(); }
    OrderingHints    GetOrder()   const override;
    void Update(SystemContext& ctx) override;
};

/// Physics 後に実行するスクリプト更新。カメラ追従など物理適用後の位置を必要とする処理に使う。
class LateScriptSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "LateScriptSystem"; }
    Phase            GetPhase()   const override { return Phase::LateScript; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    ComponentAccess  GetAccess()  const override { return ComponentAccess{}.Unrestricted(); }
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
