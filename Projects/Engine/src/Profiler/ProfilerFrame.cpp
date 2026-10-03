/// @file    ProfilerFrame.cpp
/// @brief   ホスト serial と単調時計を Performance/Script で共有する。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#include <Engine/Profiler/ProfilerFrame.hpp>
#include <Core/Profiler/Profiler.hpp>
#include <Engine/Profiler/ScriptProfiler.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

namespace fbzz::profiler {

void BeginApplicationProfileFrame()
{
    static uint64_t hostSerial = 0;
    const auto* resources = renderer::ResourceManager::Active();
    const uint64_t serial = resources ? resources->FrameStamp() : ++hostSerial;
    const double startMs = ProfileRecorder::NowMs();
    Profiler::BeginFrame(serial, startMs);
    scene::ScriptProfiler::BeginFrame(serial, startMs);
}

void EndApplicationProfileFrame()
{
    scene::ScriptProfiler::EndFrame();
    Profiler::EndFrame();
}

} /// @note namespace fbzz::profiler
