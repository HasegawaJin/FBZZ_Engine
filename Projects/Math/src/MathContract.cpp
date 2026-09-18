/// @file    MathContract.cpp
/// @brief   数学関数の契約違反の通報口。
/// @author  Hasegawa Jin
/// @date    2026-09-10
#include "Math/MathContract.hpp"

namespace fbzz::math {

namespace {
/// @note ハンドラー差し替えは起動時の 1 回だけだが、違反の報告は任意のスレッドから来る
///       (物理・アニメーション・描画は別スレッドで走りうる)。読み書きを atomic で揃える。
std::atomic<ContractHandler> s_handler{ nullptr };
} // namespace

void SetContractHandler(ContractHandler handler)
{
    s_handler.store(handler, std::memory_order_release);
}

void ReportContract(const ContractViolation& violation, std::atomic<bool>& alreadyReported)
{
    /// @note exchange で最初の 1 本だけを通す。複数スレッドから同時に踏んでも二重に出さない。
    if (alreadyReported.exchange(true, std::memory_order_relaxed)) return;

    if (const ContractHandler handler = s_handler.load(std::memory_order_acquire))
        handler(violation);
}

} // namespace fbzz::math
