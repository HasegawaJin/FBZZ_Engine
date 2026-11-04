/// @file    ProfilerFrame.hpp
/// @brief   Performance と Script の共通フレーム境界。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#pragma once
#include <Engine/Api.hpp>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::profiler {

/// @note Graphics の FrameStamp またはホスト serial と同じ単調時刻を両収集器へ渡す。
FBZZ_ENGINE_API void BeginApplicationProfileFrame(const renderer::ResourceManager* resources = nullptr);
FBZZ_ENGINE_API void EndApplicationProfileFrame();

} /// @note namespace fbzz::profiler
