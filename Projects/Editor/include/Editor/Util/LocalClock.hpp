/// @file    LocalClock.hpp
/// @brief   エディターの保存状態に表示するローカル時刻。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once

#include <ctime>
#include <string>

namespace fbzz::editor {

/// @return 呼び出し時点のローカル時刻を HH:MM:SS 形式で返す。
[[nodiscard]] inline std::string LocalClockNow()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char buffer[16];
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
    return buffer;
}

} ///< @note namespace fbzz::editor
