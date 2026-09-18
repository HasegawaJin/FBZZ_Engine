/// @file    Measure.hpp
/// @brief   画面を出さずに各場面の Simulate を計時する (--measure)。
/// @author  Hasegawa Jin
/// @date    2026-09-18
#pragma once

#include <string>

namespace fbzz::bench {

struct MeasureOptions {
    int         warmupSteps = 120;  ///< 計時前に捨てる刻み数。遅延確保・キャッシュ・分岐予測を温める。
    int         steps       = 600;  ///< 1 回の計時で進める刻み数。
    int         repeats     = 7;    ///< 計時の繰り返し回数。代表値は中央値。
    std::string csvPath;            ///< 空なら CSV を書かない。既存ファイルには行を追記する。
    std::string label;              ///< CSV の label 列 (before / after など)。
};

/// @brief コマンドライン引数から計測設定を読む。
/// @return --measure が無ければ false。out は未変更。
[[nodiscard]] bool ParseMeasureOptions(int argc, char** argv, MeasureOptions& out);

/// @brief 全場面の Simulate と Present を別々に計時し、1 回あたりの時間 [us] を標準出力と CSV へ書く。
/// @note 各繰り返しの前に Reset するので、どの回も同じ軌道の同じ区間を計る。
/// @return プロセスの終了コード。引数が不正か CSV を開けなければ 1。
int RunMeasure(const MeasureOptions& options);

} // namespace fbzz::bench
