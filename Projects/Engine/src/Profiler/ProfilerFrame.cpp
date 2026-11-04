/// @file    ProfilerFrame.cpp
/// @brief   ホスト serial と単調時計を Performance/Script で共有する。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#include <Engine/Profiler/ProfilerFrame.hpp>
#include "ApplicationFrameElapsedObserver.hpp"
#include <Core/Profiler/Profiler.hpp>
#include <Engine/Profiler/ScriptProfiler.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Core/Time.hpp>
#include <Core/Profiler/FrameTimeline.hpp>

namespace fbzz::profiler {
namespace {
detail::ApplicationFrameElapsedObserver s_frameElapsedObserver;
detail::ApplicationFrameElapsedModeWitness FrameElapsedMode()
{
    return {Profiler::IsEnabled(), Profiler::IsRecordingRequested(), scene::ScriptProfiler::IsRecording(),
        Profiler::GetSnapshot().captureSessionId, scene::ScriptProfiler::GetSnapshot().captureSessionId,
        scene::ScriptProfiler::GetRuntimeEpoch(), scene::ScriptProfiler::GetDllGeneration()};
}
} /// @note namespace
namespace detail {
uint64_t ArmApplicationFrameElapsedObserver() { return s_frameElapsedObserver.Arm(); }
void RequestStopApplicationFrameElapsedObserver() { s_frameElapsedObserver.RequestStop(); }
ApplicationFrameElapsedSnapshot ReadApplicationFrameElapsedObserver() { return s_frameElapsedObserver.Read(); }
void MarkApplicationFrameElapsedAborted() { s_frameElapsedObserver.MarkAborted(); }
} /// @note namespace detail

void BeginApplicationProfileFrame(const renderer::ResourceManager* resources)
{
    static uint64_t hostSerial = 0;
    const uint64_t serial = resources ? resources->FrameStamp() : ++hostSerial;
    const double startMs = ProfileRecorder::NowMs();
    frame_timeline::BeginApplicationFrame(Time::frameCount, serial, startMs);
    Profiler::BeginFrame(serial, startMs);
    scene::ScriptProfiler::BeginFrame(serial, startMs);
    const auto mode = s_frameElapsedObserver.WillObserveNextBegin() ? FrameElapsedMode()
        : detail::ApplicationFrameElapsedModeWitness{};
    s_frameElapsedObserver.Begin(serial, startMs, mode);
}

void EndApplicationProfileFrame()
{
    scene::ScriptProfiler::EndFrame();
    const bool observed = s_frameElapsedObserver.NeedsEndClock();
    const double endMs = observed ? ProfileRecorder::NowMs() : 0.0;
    frame_timeline::EndApplicationFrame(endMs);
    Profiler::EndFrame();
    if (observed) s_frameElapsedObserver.End(endMs, FrameElapsedMode());
}

} /// @note namespace fbzz::profiler
