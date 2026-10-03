/// @file    ProfileRecorder.cpp
/// @brief   単調時計による入れ子収集、上限処理と checkpoint 復旧。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#include <Core/Profiler/ProfileRecorder.hpp>
#include <algorithm>
#include <chrono>

namespace fbzz::profiler {
namespace {
uint64_t s_nextRecorderId = 0;
}

ProfileRecorder::ProfileRecorder(bool recording, ClockFn clock)
    : m_clock(clock ? clock : NowMs), m_recorderId(++s_nextRecorderId),
      m_recording(recording), m_pendingRecording(recording)
{
    m_stack.reserve(MAX_DEPTH);
    m_suppressionAnchors.reserve(MAX_SAMPLES);
    m_current.samples.reserve(MAX_SAMPLES);
    m_snapshot.samples.reserve(MAX_SAMPLES);
}

/// @see https://learn.microsoft.com/en-us/cpp/standard-library/steady-clock-struct?view=msvc-170 steady_clock の単調時計契約。
double ProfileRecorder::NowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double ProfileRecorder::ReadClock() const { return m_clock(); }
void ProfileRecorder::RequestRecording(bool recording) { m_pendingRecording = recording; m_hasPendingRecording = true; }
void ProfileRecorder::RequestClear() { m_pendingClear = true; }

void ProfileRecorder::Reset()
{
    ++m_generation;
    m_stack.clear();
    m_suppressionAnchors.clear();
    m_suppressionDepth = 0;
    m_suppressionTopScopeId = 0;
    m_frameOpen = false;
    m_current.samples.clear();
    m_snapshot = {};
    m_current.samples.reserve(MAX_SAMPLES);
    m_snapshot.samples.reserve(MAX_SAMPLES);
    m_havePreviousStart = false;
    m_unframedInvocationCount = 0;
}

void ProfileRecorder::SetRecordingImmediate(bool recording)
{
    if (m_recording != recording) ++m_captureSessionId;
    m_recording = recording;
    m_hasPendingRecording = false;
    Reset();
    m_snapshot.recording = recording;
    m_snapshot.captureSessionId = m_captureSessionId;
}

void ProfileRecorder::BeginFrame(uint64_t applicationFrameSerial, double frameStartMs)
{
    if (m_frameOpen) EndFrame();
    if (m_pendingClear || (m_hasPendingRecording && m_pendingRecording != m_recording)) {
        ++m_captureSessionId;
        Reset();
    }
    if (m_hasPendingRecording) m_recording = m_pendingRecording;
    m_hasPendingRecording = false;
    m_pendingClear = false;
    m_snapshot.recording = m_recording;
    m_snapshot.captureSessionId = m_captureSessionId;
    if (!m_recording) return;
    m_frameOpen = true;
    ++m_frameIndex;
    m_frameStartMs = frameStartMs;
    m_current.samples.clear();
    m_current.recording = true;
    m_current.available = true;
    m_current.complete = true;
    m_current.captureSessionId = m_captureSessionId;
    m_current.applicationFrameSerial = applicationFrameSerial;
    m_current.frameIndex = m_frameIndex;
    m_current.recordedSampleCount = 0;
    m_current.droppedSampleCount = 0;
    m_current.scopeRootSumMs = 0.0;
    m_current.unframedInvocationCount = m_unframedInvocationCount;
    m_unframedInvocationCount = 0;
    m_current.wallFrameIntervalAvailable = m_havePreviousStart;
    m_current.wallFrameIntervalMs = m_havePreviousStart ? std::max(0.0, frameStartMs - m_previousStartMs) : 0.0;
    m_previousStartMs = frameStartMs;
    m_havePreviousStart = true;
    m_completionOrder = 0;
}

bool ProfileRecorder::Matches(uint64_t recorderId, uint64_t generation, uint64_t frame) const
{
    return m_frameOpen && recorderId == m_recorderId && generation == m_generation && frame == m_frameIndex;
}

void ProfileRecorder::MarkIncomplete()
{
    m_current.complete = false;
    for (auto& scope : m_stack) scope.selfAvailable = false;
}

ProfileToken ProfileRecorder::Begin(uint64_t sampleKey)
{
    if (!m_recording) return {};
    if (!m_frameOpen) { ++m_unframedInvocationCount; return {}; }
    ProfileToken token{m_recorderId, m_generation, m_frameIndex, ++m_nextScopeId};
    token.depth = static_cast<uint32_t>(m_stack.size()) + m_suppressionDepth;
    if (m_suppressionDepth || m_stack.size() == MAX_DEPTH || !sampleKey) {
        token.suppressed = true;
        token.parentScopeId = m_suppressionTopScopeId;
        m_suppressionTopScopeId = token.scopeId;
        ++m_suppressionDepth;
        ++m_current.droppedSampleCount;
        MarkIncomplete();
        return token;
    }
    ActiveScope active;
    active.token = token;
    active.startMs = ReadClock();
    if (m_current.samples.size() < MAX_SAMPLES) {
        ProfileSample sample;
        sample.sampleId = ++m_nextSampleId;
        sample.sampleKey = sampleKey;
        sample.depth = token.depth;
        sample.startOffsetMs = std::max(0.0, active.startMs - m_frameStartMs);
        if (!m_stack.empty() && m_stack.back().sampleIndex < m_current.samples.size())
            sample.parentSampleId = m_current.samples[m_stack.back().sampleIndex].sampleId;
        active.sampleIndex = m_current.samples.size();
        m_current.samples.push_back(sample);
    } else {
        ++m_current.droppedSampleCount;
        MarkIncomplete();
        active.selfAvailable = false;
    }
    m_stack.push_back(active);
    return token;
}

void ProfileRecorder::End(ProfileToken token, ProfileSampleStatus status)
{
    if (!Matches(token.recorderId, token.generation, token.frame)) return;
    if (token.suppressed) {
        if (m_suppressionDepth && m_suppressionTopScopeId == token.scopeId
            && token.depth + 1 == m_stack.size() + m_suppressionDepth) {
            --m_suppressionDepth;
            m_suppressionTopScopeId = token.parentScopeId;
            if (!m_suppressionAnchors.empty() && m_suppressionAnchors.back().scopeId == token.scopeId)
                m_suppressionAnchors.pop_back();
        }
        return;
    }
    if (m_suppressionDepth || m_stack.empty() || m_stack.back().token.scopeId != token.scopeId) return;
    const ActiveScope active = m_stack.back();
    m_stack.pop_back();
    const double elapsedMs = std::max(0.0, ReadClock() - active.startMs);
    if (active.sampleIndex < m_current.samples.size()) {
        auto& sample = m_current.samples[active.sampleIndex];
        sample.inclusiveMs = elapsedMs;
        sample.selfMs = std::max(0.0, elapsedMs - active.childMs);
        sample.selfAvailable = active.selfAvailable && status == ProfileSampleStatus::COMPLETE;
        sample.status = status;
        sample.completionOrder = ++m_completionOrder;
        if (!sample.parentSampleId) m_current.scopeRootSumMs += elapsedMs;
    }
    if (!m_stack.empty()) {
        m_stack.back().childMs += elapsedMs;
        if (!active.selfAvailable || status != ProfileSampleStatus::COMPLETE) MarkIncomplete();
    }
    if (status != ProfileSampleStatus::COMPLETE) MarkIncomplete();
}

void ProfileRecorder::Push(uint64_t sampleKey, double durationMs, ProfileSampleKind kind)
{
    if (!m_recording) return;
    if (!m_frameOpen) { ++m_unframedInvocationCount; return; }
    if (!sampleKey || m_suppressionDepth || m_current.samples.size() == MAX_SAMPLES) {
        ++m_current.droppedSampleCount;
        MarkIncomplete();
        return;
    }
    ProfileSample sample;
    sample.sampleId = ++m_nextSampleId;
    sample.sampleKey = sampleKey;
    sample.depth = static_cast<uint32_t>(m_stack.size());
    sample.sampleKind = kind;
    sample.inclusiveMs = kind == ProfileSampleKind::INSTANT ? 0.0 : std::max(0.0, durationMs);
    sample.selfMs = sample.inclusiveMs;
    sample.startAvailable = kind == ProfileSampleKind::INSTANT;
    sample.selfAvailable = kind == ProfileSampleKind::INSTANT;
    sample.startOffsetMs = sample.startAvailable ? std::max(0.0, ReadClock() - m_frameStartMs) : 0.0;
    sample.completionOrder = ++m_completionOrder;
    if (!m_stack.empty() && m_stack.back().sampleIndex < m_current.samples.size())
        sample.parentSampleId = m_current.samples[m_stack.back().sampleIndex].sampleId;
    if (kind == ProfileSampleKind::EXTERNAL_DURATION)
        for (auto& active : m_stack) active.selfAvailable = false;
    if (!sample.parentSampleId) m_current.scopeRootSumMs += sample.inclusiveMs;
    m_current.samples.push_back(sample);
}

ProfileCheckpoint ProfileRecorder::Checkpoint() const
{
    if (!m_frameOpen) return {};
    if (m_suppressionDepth && (m_suppressionAnchors.empty() || m_suppressionAnchors.back().scopeId != m_suppressionTopScopeId)) {
        if (m_suppressionAnchors.size() == MAX_SAMPLES) return {};
        m_suppressionAnchors.push_back({m_suppressionTopScopeId, m_suppressionDepth});
    }
    return {m_recorderId, m_generation, m_frameIndex, m_stack.empty() ? 0 : m_stack.back().token.scopeId,
            m_suppressionTopScopeId, static_cast<uint32_t>(m_stack.size()), m_suppressionDepth};
}

bool ProfileRecorder::Recover(ProfileCheckpoint checkpoint)
{
    if (!Matches(checkpoint.recorderId, checkpoint.generation, checkpoint.frame)
        || checkpoint.stackSize > m_stack.size()) return false;
    if (checkpoint.stackSize && m_stack[checkpoint.stackSize - 1].token.scopeId != checkpoint.topScopeId) return false;
    if (checkpoint.suppressionDepth > m_suppressionDepth
        || (checkpoint.suppressionDepth == m_suppressionDepth
            && checkpoint.suppressionTopScopeId != m_suppressionTopScopeId)) return false;
    /// @note 深さだけでは閉じた旧分岐と新しい分岐を区別できないため、保存した抑制 scope の生存を検証する。
    if (checkpoint.suppressionDepth && std::none_of(m_suppressionAnchors.begin(), m_suppressionAnchors.end(), [&](const auto& anchor) {
        return anchor.scopeId == checkpoint.suppressionTopScopeId && anchor.depth == checkpoint.suppressionDepth;
    })) return false;
    if (m_suppressionDepth != checkpoint.suppressionDepth || m_suppressionTopScopeId != checkpoint.suppressionTopScopeId)
        MarkIncomplete();
    m_suppressionDepth = 0;
    m_suppressionTopScopeId = 0;
    while (m_stack.size() > checkpoint.stackSize) End(m_stack.back().token, ProfileSampleStatus::ABORTED);
    m_suppressionDepth = checkpoint.suppressionDepth;
    m_suppressionTopScopeId = checkpoint.suppressionTopScopeId;
    while (!m_suppressionAnchors.empty() && m_suppressionAnchors.back().depth > checkpoint.suppressionDepth)
        m_suppressionAnchors.pop_back();
    return true;
}

void ProfileRecorder::EndFrame()
{
    if (!m_frameOpen) return;
    if (m_suppressionDepth) MarkIncomplete();
    m_suppressionDepth = 0;
    m_suppressionTopScopeId = 0;
    m_suppressionAnchors.clear();
    while (!m_stack.empty()) End(m_stack.back().token, ProfileSampleStatus::ABORTED);
    m_current.cpuFrameElapsedMs = std::max(0.0, ReadClock() - m_frameStartMs);
    m_current.recordedSampleCount = static_cast<uint32_t>(m_current.samples.size());
    std::swap(m_current, m_snapshot);
    m_frameOpen = false;
}

} /// @note namespace fbzz::profiler
