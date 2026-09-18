/// @file    Measure.hpp
/// @brief   画面を出さずに各場面と Math の演算を計時する (--measure)。
/// @author  Hasegawa Jin
/// @date    2026-09-18
#pragma once

#include <string>

namespace fbzz::bench {

struct MeasureOptions {
    int         warmupSteps = 120;     ///< 計時前に捨てる刻み数。遅延確保・キャッシュ・分岐予測を温める。
    int         steps       = 600;     ///< scene の 1 回の計時で進める刻み数。
    int         repeats     = 7;       ///< 計時の繰り返し回数。代表値は中央値。
    int         ops         = 200000;  ///< micro の 1 回の計時で行う演算回数。
    std::string jsonPath;              ///< 空なら JSON を書かない。既存ファイルは上書きする。
    std::string label;                 ///< 比較で基準と候補を見分ける名前 (baseline / candidate など)。
    std::string commit;                ///< 計測したコミット。exe は知らないので呼び出し側が渡す。
    bool        dirty       = false;   ///< コミット後の未コミット変更を含むビルドか。
};

/// @brief コマンドライン引数から計測設定を読む。
/// @return --measure が無ければ false。out は未変更。
[[nodiscard]] bool ParseMeasureOptions(int argc, char** argv, MeasureOptions& out);

/// @brief 全場面の Simulate / Present と全 micro 計測を計時し、標準出力と JSON へ書く。
/// @note 形式は schema "fbzz-bench/1"。
/// @see Docs/design/benchmark-report.md
/// @return プロセスの終了コード。引数が不正か JSON を書けなければ 1。
int RunMeasure(const MeasureOptions& options);

} // namespace fbzz::bench
