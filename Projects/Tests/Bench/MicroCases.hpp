/// @file    MicroCases.hpp
/// @brief   --measure の micro 計測 (Math の小さな演算を 1 演算あたりで測る)。
/// @author  Hasegawa Jin
/// @date    2026-09-18
#pragma once

#include <vector>

namespace fbzz::bench {

/// @brief 1 つの micro 計測。run は ops 回の演算を行い、最適化で消されないよう結果を畳んだ値を返す。
struct MicroCase {
    const char* name;
    double (*run)(int ops);
};

/// @brief 全 micro 計測を並べ順どおりに返す。
/// @note 入力は初回呼び出し時に固定の線形合同法で 1 度だけ作る。どの実行も同じ値を処理する。
[[nodiscard]] const std::vector<MicroCase>& AllMicroCases();

} // namespace fbzz::bench
