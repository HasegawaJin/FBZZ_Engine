/// @file    ISystem.hpp
/// @brief   全 System が実装するインターフェース。SystemScheduler に登録して使う。
/// @author  Hasegawa Jin
/// @date    2026-06-18
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
    /// @note 未宣言のアクセスは全システムと競合させ、並列化には明示した読み書き契約を要求する。
    virtual ComponentAccess  GetAccess()  const { return ComponentAccess{}.Unrestricted(); }
    virtual OrderingHints    GetOrder()   const { return {}; }

    virtual void OnInit()     {}
    virtual void OnShutdown() {}

    /// @brief RunIf。false を返すとそのフレームはスキップ (デフォルト常に実行)。
    virtual bool ShouldRun(const SystemContext&) const { return true; }
    /// @brief RunEvery(N)。N フレームに 1 回実行 (デフォルト毎フレーム)。
    virtual int  RunInterval() const { return 1; }
};

} /// @note namespace fbzz
