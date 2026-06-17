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
