/// @file    WaterControlPointCases.hpp
/// @brief   --measure の Water TargetPoint workset 計測結果と入口。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fbzz::bench {

struct WaterControlPointMeasureResult {
    std::string name; ///< 測定結果で識別する作業セットの名前。
    std::string phase; ///< 作業セット内で測定した処理段階の名前。
    std::string unit = "us/iteration"; ///< samples の値が表す時間単位。
    std::vector<double> samples; ///< 計測した 1 反復あたりの経過時間。
    std::vector<std::uint64_t> regenerationSamples; ///< 計測区間ごとの再生成回数。
    std::optional<std::uint64_t> allocationsPerIteration; ///< 1 反復あたりの確保回数。計数対象外なら空。
    std::string note; ///< 集計や表示に添える計測条件の補足。
};

/// @brief GPU・asset を作らず Water/Spline/Zone の CPU authoring workset を計時する。
/// @param[in] warmupIterations 各計測前に捨てる回数。
/// @param[in] repeats 各結果の計測回数。
/// @param[out] out 成功時に workset ごとの測定結果を置換する。
/// @return すべての数値 snapshot を生成できた場合は true。
[[nodiscard]] bool RunWaterControlPointMeasures(int warmupIterations, int repeats,
    std::vector<WaterControlPointMeasureResult>& out);

} // namespace fbzz::bench
