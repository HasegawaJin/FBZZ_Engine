// FBZZ Engine
// ISystem.hpp | fbzz
// 全 System が実装するインターフェース。SystemScheduler に登録して使う。
#pragma once
#include "Phase.hpp"
#include "ComponentAccess.hpp"
#include "OrderingHints.hpp"
#include <string_view>

namespace fbzz {

struct SystemContext;

class ISystem {
public:
    virtual ~ISystem() = default;

    virtual void Update(SystemContext& ctx) = 0;

    virtual std::string_view Name()       const = 0;
    virtual Phase            GetPhase()   const = 0;
    virtual RunMode          GetRunMode() const { return RunMode::Always; }
    virtual ComponentAccess  GetAccess()  const { return {}; }
    virtual OrderingHints    GetOrder()   const { return {}; }

    virtual void OnInit()     {}
    virtual void OnShutdown() {}

    // RunIf: false を返すとそのフレームはスキップ（デフォルト常に実行）
    virtual bool ShouldRun(const SystemContext&) const { return true; }
    // RunEvery(N): N フレームに 1 回実行（デフォルト毎フレーム）
    virtual int  RunInterval() const { return 1; }
};

} // namespace fbzz
