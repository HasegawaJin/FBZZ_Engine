// FBZZ Engine
// ScriptSystem.hpp | fbzz::scene
// GameObject に付いたユーザースクリプトの実行 System
// Start / Update などのライフサイクルを Scene 全体に対して進める。
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

class ScriptSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "ScriptSystem"; }
    Phase            GetPhase()   const override { return Phase::Script; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override { return ComponentAccess{}.Unrestricted(); }
    void Update(SystemContext& ctx) override;
};

// 固定ステップでスクリプトの OnFixedUpdate() を回す。
//
// WHY Phase::Physics に置くか: このフェーズだけが SystemScheduler の固定ステップ
//     ループ (アキュムレータ) の対象になっており、1 フレームに 0〜N 回実行される。
//     Phase::Script 側に置くと描画フレームと同じ可変回数になり、固定ステップの
//     意味が消える。PhysicsSystem より前に走るよう OrderingHints で明示することで、
//     スクリプトが加えた力が「同じステップ」で積分される。
class FixedScriptSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "FixedScriptSystem"; }
    Phase            GetPhase()   const override { return Phase::Physics; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override { return ComponentAccess{}.Unrestricted(); }
    OrderingHints    GetOrder()   const override;
    void Update(SystemContext& ctx) override;
};

// Physics 後に実行するスクリプト更新。カメラ追従など物理適用後の位置を必要とする処理に使う。
class LateScriptSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "LateScriptSystem"; }
    Phase            GetPhase()   const override { return Phase::LateScript; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override { return ComponentAccess{}.Unrestricted(); }
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
