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

constexpr size_t MAX_FILTER_CATEGORY_BYTES = 128;
constexpr size_t MAX_FILTER_SCOPE_BYTES = 256;

/// @note 収集フィルターの要求だけをスレッド間で共有し、適用は各 BeginFrame で行う。
std::atomic<std::shared_ptr<const PerformanceCaptureFilter>> s_requestedFilter{std::make_shared<const PerformanceCaptureFilter>()};

bool MatchesFrameFilter(const ProfilerThreadState& state, const ProfilerMarker& marker)
{
    if (!state.m_frameFilter) {
        return true;
    }
    const auto& filter = *state.m_frameFilter;
    const char* category = marker.category ? marker.category : "General";
    if (!filter.category.empty() && filter.category != category) {
        return false;
    }

    if (filter.scope.empty()) {
        return true;
    }
    const std::string_view scopeName = marker.name ? marker.name : "Unnamed";
    return scopeName.find(filter.scope) != std::string_view::npos;
}

uint64_t ResolveSampleKey(ProfilerThreadState& state, const ProfilerMarker& marker)
{
    const bool canRecordSample = state.m_recorder.IsRecording() && state.m_recorder.IsFrameOpen();
    if (!canRecordSample) {
        return 0;
    }
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
    if (enabled && state.m_recorder.IsRecording()) {
        return;
    }

    // 即時切り替えで失効した区間と公開済みの結果を、同じ境界で破棄する。
    state.m_recorder.SetRecordingImmediate(enabled);
    state.m_legacyScopes.Clear();
    state.m_publication.Reset(enabled, state.m_recorder.GetSnapshot().captureSessionId);
    if (!enabled) {
        state.m_descriptorTable.Clear();
    }
}

bool Profiler::IsEnabled() { return CurrentThreadState().m_recorder.IsRecording(); }
bool Profiler::IsRecordingRequested() { return CurrentThreadState().m_recorder.IsRecordingRequested(); }
void Profiler::RequestRecording(bool recording) { CurrentThreadState().m_recorder.RequestRecording(recording); }
void Profiler::RequestClear() { CurrentThreadState().m_recorder.RequestClear(); }

bool Profiler::RequestCaptureFilter(PerformanceCaptureFilter filter)
{
    if (filter.category.size() > MAX_FILTER_CATEGORY_BYTES) {
        return false;
    }
    if (filter.scope.size() > MAX_FILTER_SCOPE_BYTES) {
        return false;
    }

    const auto currentFilter = s_requestedFilter.load();
    if (*currentFilter != filter) {
        s_requestedFilter.store(std::make_shared<const PerformanceCaptureFilter>(std::move(filter)));
    }
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

    // 前のフレームから区間を引き継がず、公開中の記述子を残して登録表を整理する。
    state.m_legacyScopes.Clear();
    state.m_descriptorTable.RetireIfNeeded(state.m_publication.Snapshot().descriptors);

    // フィルターが切り替わった境界で収集をクリアし、フレーム内では同じ条件を使う。
    const auto requestedFilter = s_requestedFilter.load();
    const bool filterChanged = state.m_frameFilter && *state.m_frameFilter != *requestedFilter;
    if (filterChanged) {
        state.m_recorder.RequestClear();
    }
    state.m_frameFilter = requestedFilter;

    state.m_recorder.BeginFrame(applicationFrameSerial, frameStartMs);
    state.m_publication.UpdateSession(state.m_recorder.IsRecording(), state.m_recorder.GetSnapshot().captureSessionId);
}

void Profiler::EndFrame()
{
    auto& state = CurrentThreadState();
    if (!state.m_recorder.IsFrameOpen()) {
        return;
    }

    // サンプルの終了時刻を確定してから、それらが参照する記述子を公開する。
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
