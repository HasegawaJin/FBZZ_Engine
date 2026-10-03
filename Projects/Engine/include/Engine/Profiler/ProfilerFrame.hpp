/// @file    ProfilerFrame.hpp
/// @brief   Performance と Script の共通フレーム境界。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#pragma once

namespace fbzz::profiler {

/// @note Graphics の FrameStamp またはホスト serial と同じ単調時刻を両収集器へ渡す。
void BeginApplicationProfileFrame();
void EndApplicationProfileFrame();

} /// @note namespace fbzz::profiler
