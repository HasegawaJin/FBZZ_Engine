// FBZZ Engine
// FoliageBakeSystem.hpp | fbzz::scene
// FoliageComponent の STAMP 配置に合わせて子 GO 階層を管理するシステム。
// needsBake が true のタイミングで旧子 GO を破棄して再生成する。
// 各 stamp に対して CapsuleCollider + (SubMesh 数の) MeshRenderer 孫 GO を生成する。
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

class FoliageBakeSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "FoliageBakeSystem"; }
    Phase            GetPhase()   const override { return Phase::PreScript; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    ComponentAccess  GetAccess()  const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
