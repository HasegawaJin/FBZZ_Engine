/// @file    ProfilerMarker.hpp
/// @brief   プロファイラで計測区間を識別するための軽量メタデータ。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#pragma once

#include <cstdint>

namespace fbzz::profiler {

/// 計測区間の名前・カテゴリ・表示色をまとめる値オブジェクト。
/// @note 個別引数で渡すと呼び出し側の引数が増え、表示情報が散らばるため集約する。
struct ProfilerMarker {
    const char* name     = "Unnamed";
    const char* category = "General";
    uint32_t    color    = 0xFF4FA3FF;

    constexpr ProfilerMarker() = default;

    /// const char* を保持する。呼び出し側は文字列リテラルなど寿命が十分長い文字列を渡す前提。
    constexpr ProfilerMarker(const char* markerName,
                             const char* markerCategory = "General",
                             uint32_t markerColor = 0xFF4FA3FF)
        : name(markerName)
        , category(markerCategory)
        , color(markerColor)
    {
    }
};

} // namespace fbzz::profiler
