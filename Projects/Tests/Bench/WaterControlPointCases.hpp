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
    std::string name;
    std::string phase;
    std::string unit = "us/iteration";
    std::vector<double> samples;
    std::vector<std::uint64_t> regenerationSamples;
    std::optional<std::uint64_t> allocationsPerIteration;
    std::string note;
};

/// @brief GPU・asset を作らず Water/Spline/Zone の CPU authoring workset を計時する。
/// @param warmupIterations 各計測前に捨てる回数。
/// @param repeats 各結果の計測回数。
/// @param out 成功時に workset ごとの測定結果を置換する。
/// @return すべての数値 snapshot を生成できた場合は true。
[[nodiscard]] bool RunWaterControlPointMeasures(int warmupIterations, int repeats,
    std::vector<WaterControlPointMeasureResult>& out);

} /// @note namespace fbzz::bench
