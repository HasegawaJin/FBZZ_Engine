/// @file    ProfilerMarker.hpp
/// @brief   プロファイラで計測区間を識別するための軽量メタデータ。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#pragma once

#include <cstdint>

namespace fbzz::profiler {

/// @brief 計測区間の名前・分類・表示色を束ねる。
struct ProfilerMarker {
    const char* name     = "Unnamed";  ///< 区間の非所有の表示名。nullptr なら Unnamed を使う。
    const char* category = "General";  ///< 絞り込みと表示に使う非所有の分類名。nullptr なら General を使う。
    uint32_t    color    = 0xFF4FA3FF; ///< 区間の表示色（0xAARRGGBB）。

    constexpr ProfilerMarker() = default;

    /// @param[in] markerName 計測 API がこのマーカーを参照するまで有効な表示名。
    /// @param[in] markerCategory 計測 API がこのマーカーを参照するまで有効な分類名。
    /// @param[in] markerColor 区間の表示色（0xAARRGGBB）。
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
