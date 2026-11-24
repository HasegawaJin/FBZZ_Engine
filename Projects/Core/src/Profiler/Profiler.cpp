/// @file    Profiler.cpp
/// @brief   Performance 計測の公開 API、収集フィルター、フレーム境界。
/// @author  Hasegawa Jin
/// @date    2026-06-02

#include <Core/Profiler/Profiler.hpp>
#include "ProfilerThreadState.hpp"
#include <atomic>
#include <string_view>
#include <utility>

namespace fbzz::profiler {

namespace {

using detail::CurrentThreadState;
using detail::ProfilerThreadState;

/// @note 収集フィルターの要求だけをスレッド間で共有し、適用は各 BeginFrame で行う。
std::atomic<std::shared_ptr<const PerformanceCaptureFilter>> s_requestedFilter{std::make_shared<const PerformanceCaptureFilter>()};

bool MatchesFrameFilter(const ProfilerThreadState& state, const ProfilerMarker& marker)
{
    if (!state.m_frameFilter) return true;
    const auto& filter = *state.m_frameFilter;
    return (filter.category.empty() || filter.category == (marker.category ? marker.category : "General"))
        && (filter.scope.empty()
            || std::string_view(marker.name ? marker.name : "Unnamed").find(filter.scope) != std::string_view::npos);
}

uint64_t ResolveSampleKey(ProfilerThreadState& state, const ProfilerMarker& marker)
{
    if (!state.m_recorder.IsRecording() || !state.m_recorder.IsFrameOpen()) return 0;
    return state.m_descriptorTable.Resolve(marker);
}

}

ProfilerThreadState& detail::CurrentThreadState()
{
    thread_local ProfilerThreadState state;
    return state;
}

void Profiler::SetEnabled(bool enabled)
{
    auto& state = CurrentThreadState();
    if (enabled && state.m_recorder.IsRecording()) return;
    state.m_recorder.SetRecordingImmediate(enabled);
    state.m_legacyScopes.Clear();
    state.m_publication.Reset(enabled, state.m_recorder.GetSnapshot().captureSessionId);
    if (!enabled) state.m_descriptorTable.Clear();
}
bool Profiler::IsEnabled() { return CurrentThreadState().m_recorder.IsRecording(); }
bool Profiler::IsRecordingRequested() { return CurrentThreadState().m_recorder.IsRecordingRequested(); }
void Profiler::RequestRecording(bool recording) { CurrentThreadState().m_recorder.RequestRecording(recording); }
void Profiler::RequestClear() { CurrentThreadState().m_recorder.RequestClear(); }
bool Profiler::RequestCaptureFilter(PerformanceCaptureFilter filter)
{
    if (filter.category.size() > 128 || filter.scope.size() > 256) return false;
    const auto currentFilter = s_requestedFilter.load();
    if (*currentFilter != filter) s_requestedFilter.store(std::make_shared<const PerformanceCaptureFilter>(std::move(filter)));
    return true;
}
PerformanceCaptureFilter Profiler::GetRequestedCaptureFilter() { return *s_requestedFilter.load(); }

void Profiler::BeginFrame()
{
    BeginFrame(++CurrentThreadState().m_nextHostFrameSerial, ProfileRecorder::NowMs());
}

void Profiler::BeginFrame(uint64_t applicationFrameSerial, double frameStartMs)
{
    auto& state = CurrentThreadState();
    state.m_legacyScopes.Clear();
    state.m_descriptorTable.RetireIfNeeded(state.m_publication.Snapshot().descriptors);
    const auto requestedFilter = s_requestedFilter.load();
    if (state.m_frameFilter && *state.m_frameFilter != *requestedFilter) state.m_recorder.RequestClear();
    state.m_frameFilter = requestedFilter;
    state.m_recorder.BeginFrame(applicationFrameSerial, frameStartMs);
    state.m_publication.UpdateSession(state.m_recorder.IsRecording(), state.m_recorder.GetSnapshot().captureSessionId);
}

void Profiler::EndFrame()
{
    auto& state = CurrentThreadState();
    if (!state.m_recorder.IsFrameOpen()) return;
    state.m_recorder.EndFrameTo(state.m_publication.SampleStorage());
    state.m_publication.FinishFrame(*state.m_frameFilter, state.m_descriptorTable);
    state.m_legacyScopes.Clear();
}

ProfileToken Profiler::BeginScope(const ProfilerMarker& marker)
{
    auto& state = CurrentThreadState();
    const bool matchesFilter = MatchesFrameFilter(state, marker);
    return state.m_recorder.Begin(matchesFilter ? ResolveSampleKey(state, marker) : 0, matchesFilter);
}

void Profiler::EndScope(ProfileToken token)
{
    CurrentThreadState().m_recorder.End(token);
}

void Profiler::PushMarker(const ProfilerMarker& marker)
{
    auto& state = CurrentThreadState();
    const bool matchesFilter = MatchesFrameFilter(state, marker);
    state.m_recorder.Push(matchesFilter ? ResolveSampleKey(state, marker) : 0,
                          0.0, ProfileSampleKind::INSTANT, matchesFilter);
}

void Profiler::PushSample(const ProfilerMarker& marker, double elapsedMs)
{
    auto& state = CurrentThreadState();
    const bool matchesFilter = MatchesFrameFilter(state, marker);
    state.m_recorder.Push(matchesFilter ? ResolveSampleKey(state, marker) : 0,
                          elapsedMs, ProfileSampleKind::EXTERNAL_DURATION, matchesFilter);
}

const std::vector<ProfileRecord>& Profiler::GetLastFrameRecords()
{
    return CurrentThreadState().m_publication.LegacyRecords();
}

uint64_t Profiler::GetLastFrameIndex()
{
    return CurrentThreadState().m_publication.LastFrameIndex();
}

const PerformanceSnapshot& Profiler::GetSnapshot()
{
    return CurrentThreadState().m_publication.Snapshot();
}

}
