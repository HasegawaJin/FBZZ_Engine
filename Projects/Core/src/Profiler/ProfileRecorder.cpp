/// @file    ProfileRecorder.cpp
/// @brief   単調時計による入れ子区間の計測、記録上限の処理、チェックポイントからの復旧。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#include <Core/Profiler/ProfileRecorder.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>

namespace fbzz::profiler {
namespace {
std::atomic<uint64_t> s_nextRecorderId{0};
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
void ProfileRecorder::RequestRecording(bool recording)
{
    m_pendingRecording = recording;
    m_hasPendingRecording = true;
}
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
    // 前のフレームを閉じてから次の収集設定を適用する。
    if (m_frameOpen) {
        EndFrame();
    }

    const bool recordingChanged = m_hasPendingRecording && m_pendingRecording != m_recording;
    if (m_pendingClear || recordingChanged) {
        ++m_captureSessionId;
        Reset();
    }
    if (m_hasPendingRecording) {
        m_recording = m_pendingRecording;
    }
    m_hasPendingRecording = false;
    m_pendingClear = false;
    m_snapshot.recording = m_recording;
    m_snapshot.captureSessionId = m_captureSessionId;
    if (!m_recording) {
        return;
    }

    // 結果を公開して配列を引き渡した場合も、収集用の枠を確保してから開始する。
    m_frameOpen = true;
    ++m_frameIndex;
    m_frameStartMs = frameStartMs;
    m_current.samples.clear();
    if (m_current.samples.capacity() < MAX_SAMPLES) {
        m_current.samples.reserve(MAX_SAMPLES);
    }
    m_current.recording = true;
    m_current.available = true;
    m_current.complete = true;
    m_current.captureSessionId = m_captureSessionId;
    m_current.applicationFrameSerial = applicationFrameSerial;
    m_current.frameIndex = m_frameIndex;
    m_current.recordedSampleCount = 0;
    m_current.droppedSampleCount = 0;
    m_current.filteredSampleCount = 0;
    m_current.sampleCapacity = static_cast<uint32_t>(MAX_SAMPLES);
    m_current.capacityDroppedSampleCount = 0;
    m_current.depthDroppedSampleCount = 0;
    m_current.descriptorDroppedSampleCount = 0;
    m_current.faultedSampleCount = 0;
    m_current.abortedSampleCount = 0;
    m_current.scopeRootSumMs = 0.0;
    m_current.unframedInvocationCount = m_unframedInvocationCount;
    m_unframedInvocationCount = 0;

    // 同じ収集セッションの開始時刻だけを使い、フレーム開始間隔を求める。
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
    for (auto& scope : m_stack) {
        scope.selfAvailable = false;
    }
}

ProfileToken ProfileRecorder::Begin(uint64_t sampleKey, bool collect)
{
    if (!m_recording) {
        return {};
    }
    if (!m_frameOpen) {
        ++m_unframedInvocationCount;
        return {};
    }

    ProfileToken token{m_recorderId, m_generation, m_frameIndex, ++m_nextScopeId};
    token.depth = static_cast<uint32_t>(m_stack.size()) + m_suppressionDepth;

    // 保存できない区間も終了を照合できるよう、通常スタックの外で入れ子を覚える。
    const bool nestedInSuppression = m_suppressionDepth != 0;
    const bool stackIsFull = m_stack.size() == MAX_DEPTH;
    const bool descriptorIsMissing = collect && sampleKey == 0;
    if (nestedInSuppression || stackIsFull || descriptorIsMissing) {
        token.suppressed = true;
        token.parentScopeId = m_suppressionTopScopeId;
        m_suppressionTopScopeId = token.scopeId;
        ++m_suppressionDepth;
        ++m_current.droppedSampleCount;
        if (m_suppressionDepth > 1 || stackIsFull) {
            ++m_current.depthDroppedSampleCount;
        } else {
            ++m_current.descriptorDroppedSampleCount;
        }
        MarkIncomplete();
        return token;
    }

    // 収集対象外の区間を通る場合も、子サンプルの参照先は直近の記録済みの祖先とする。
    ActiveScope active;
    active.token = token;
    active.filtered = !collect;
    if (!m_stack.empty()) {
        active.nearestSampleId = m_stack.back().nearestSampleId;
        active.retainedDepth = m_stack.back().retainedDepth;
    }
    if (!collect) {
        ++m_current.filteredSampleCount;
        for (auto& ancestor : m_stack) {
            ancestor.selfAvailable = false;
        }
        m_stack.push_back(active);
        return token;
    }

    // 開始行を予約し、終了時には確保した添字へ所要時間を書き込む。
    active.startMs = ReadClock();
    if (m_current.samples.size() < MAX_SAMPLES) {
        ProfileSample sample;
        sample.sampleId = ++m_nextSampleId;
        sample.sampleKey = sampleKey;
        sample.depth = active.retainedDepth;
        sample.parentSampleId = active.nearestSampleId;
        active.nearestSampleId = sample.sampleId;
        ++active.retainedDepth;
        sample.startOffsetMs = std::max(0.0, active.startMs - m_frameStartMs);
        active.sampleIndex = m_current.samples.size();
        m_current.samples.push_back(sample);
    } else {
        ++m_current.droppedSampleCount;
        ++m_current.capacityDroppedSampleCount;
        MarkIncomplete();
        active.selfAvailable = false;
    }
    m_stack.push_back(active);
    return token;
}

void ProfileRecorder::End(ProfileToken token, ProfileSampleStatus status)
{
    if (!Matches(token.recorderId, token.generation, token.frame)) {
        return;
    }

    // 抑制中の区間は通常スタックに無いため、専用の頂上 ID と深さを照合する。
    if (token.suppressed) {
        const bool isSuppressionTop = m_suppressionDepth != 0 && m_suppressionTopScopeId == token.scopeId;
        if (!isSuppressionTop || token.depth + 1 != m_stack.size() + m_suppressionDepth) {
            return;
        }

        --m_suppressionDepth;
        m_suppressionTopScopeId = token.parentScopeId;
        if (!m_suppressionAnchors.empty() && m_suppressionAnchors.back().scopeId == token.scopeId) {
            m_suppressionAnchors.pop_back();
        }
        return;
    }

    const bool canCloseRecordedScope = m_suppressionDepth == 0 && !m_stack.empty();
    if (!canCloseRecordedScope || m_stack.back().token.scopeId != token.scopeId) {
        return;
    }

    // 区間の終了種別は、フィルターでサンプルを保存していない場合も集計する。
    const ActiveScope active = m_stack.back();
    m_stack.pop_back();
    if (status == ProfileSampleStatus::FAULTED) {
        ++m_current.faultedSampleCount;
    }
    if (status == ProfileSampleStatus::ABORTED) {
        ++m_current.abortedSampleCount;
    }
    if (active.filtered) {
        if (status != ProfileSampleStatus::COMPLETE) {
            MarkIncomplete();
        }
        return;
    }

    // 自身の区間から直接の子区間の時間を引き、親には自身を含む経過時間を渡す。
    const double elapsedMs = std::max(0.0, ReadClock() - active.startMs);
    if (active.sampleIndex < m_current.samples.size()) {
        auto& sample = m_current.samples[active.sampleIndex];
        sample.inclusiveMs = elapsedMs;
        sample.selfMs = std::max(0.0, elapsedMs - active.childMs);
        sample.selfAvailable = active.selfAvailable && status == ProfileSampleStatus::COMPLETE;
        sample.status = status;
        sample.completionOrder = ++m_completionOrder;
        if (!sample.parentSampleId) {
            m_current.scopeRootSumMs += elapsedMs;
        }
    }
    if (!m_stack.empty()) {
        m_stack.back().childMs += elapsedMs;
        if (!active.selfAvailable) {
            m_stack.back().selfAvailable = false;
        }
    }
    if (status != ProfileSampleStatus::COMPLETE) {
        MarkIncomplete();
    }
}

void ProfileRecorder::Push(uint64_t sampleKey, double durationMs, ProfileSampleKind kind, bool collect)
{
    if (!m_recording) {
        return;
    }
    if (!m_frameOpen) {
        ++m_unframedInvocationCount;
        return;
    }

    // 時間を持つイベントが省略された場合、祖先の区間からその時間を差し引けない。
    if (!collect) {
        ++m_current.filteredSampleCount;
        if (kind != ProfileSampleKind::INSTANT) {
            for (auto& active : m_stack) {
                active.selfAvailable = false;
            }
        }
        return;
    }

    // 記録できなかった理由は、入れ子の抑制、記述子不足、保存枠不足の順に分類する。
    const bool nestedInSuppression = m_suppressionDepth != 0;
    const bool descriptorIsMissing = sampleKey == 0;
    const bool samplesAreFull = m_current.samples.size() == MAX_SAMPLES;
    if (descriptorIsMissing || nestedInSuppression || samplesAreFull) {
        ++m_current.droppedSampleCount;
        if (nestedInSuppression) {
            ++m_current.depthDroppedSampleCount;
        } else if (descriptorIsMissing) {
            ++m_current.descriptorDroppedSampleCount;
        } else {
            ++m_current.capacityDroppedSampleCount;
        }
        MarkIncomplete();
        return;
    }

    // 外部で計った時間に開始時刻は無い。瞬間イベントだけを現在時刻に配置する。
    ProfileSample sample;
    sample.sampleId = ++m_nextSampleId;
    sample.sampleKey = sampleKey;
    if (!m_stack.empty()) {
        sample.depth = m_stack.back().retainedDepth;
        sample.parentSampleId = m_stack.back().nearestSampleId;
    }
    sample.sampleKind = kind;
    sample.inclusiveMs = kind == ProfileSampleKind::INSTANT ? 0.0 : std::max(0.0, durationMs);
    sample.selfMs = sample.inclusiveMs;
    sample.startAvailable = kind == ProfileSampleKind::INSTANT;
    sample.selfAvailable = kind == ProfileSampleKind::INSTANT;
    sample.startOffsetMs = sample.startAvailable ? std::max(0.0, ReadClock() - m_frameStartMs) : 0.0;
    sample.completionOrder = ++m_completionOrder;
    if (kind == ProfileSampleKind::EXTERNAL_DURATION) {
        for (auto& active : m_stack) {
            active.selfAvailable = false;
        }
    }
    if (!sample.parentSampleId) {
        m_current.scopeRootSumMs += sample.inclusiveMs;
    }
    m_current.samples.push_back(sample);
}

ProfileCheckpoint ProfileRecorder::Checkpoint() const
{
    if (!m_frameOpen) {
        return {};
    }
    const bool needsSuppressionAnchor = m_suppressionDepth != 0
        && (m_suppressionAnchors.empty() || m_suppressionAnchors.back().scopeId != m_suppressionTopScopeId);
    if (needsSuppressionAnchor) {
        if (m_suppressionAnchors.size() == MAX_SAMPLES) {
            return {};
        }
        m_suppressionAnchors.push_back({m_suppressionTopScopeId, m_suppressionDepth});
    }
    const uint64_t topScopeId = m_stack.empty() ? 0 : m_stack.back().token.scopeId;
    return {m_recorderId, m_generation, m_frameIndex, topScopeId, m_suppressionTopScopeId,
            static_cast<uint32_t>(m_stack.size()), m_suppressionDepth};
}

bool ProfileRecorder::Recover(ProfileCheckpoint checkpoint)
{
    // 同じフレーム内で現在も開いている通常区間からだけ復元できる。
    if (!Matches(checkpoint.recorderId, checkpoint.generation, checkpoint.frame)) {
        return false;
    }
    if (checkpoint.stackSize > m_stack.size()) {
        return false;
    }
    if (checkpoint.stackSize != 0 && m_stack[checkpoint.stackSize - 1].token.scopeId != checkpoint.topScopeId) {
        return false;
    }

    // 同じ深さの別区間を復元先と誤認しないよう、抑制区間の ID も照合する。
    if (checkpoint.suppressionDepth > m_suppressionDepth) {
        return false;
    }
    const bool suppressionDepthMatches = checkpoint.suppressionDepth == m_suppressionDepth;
    const bool suppressionTopMatches = checkpoint.suppressionTopScopeId == m_suppressionTopScopeId;
    if (suppressionDepthMatches && !suppressionTopMatches) {
        return false;
    }
    if (checkpoint.suppressionDepth != 0) {
        const auto matchesCheckpoint = [&checkpoint](const auto& anchor) {
            return anchor.scopeId == checkpoint.suppressionTopScopeId && anchor.depth == checkpoint.suppressionDepth;
        };
        if (std::none_of(m_suppressionAnchors.begin(), m_suppressionAnchors.end(), matchesCheckpoint)) {
            return false;
        }
    }

    if (!suppressionDepthMatches || !suppressionTopMatches) {
        MarkIncomplete();
    }

    // End() は抑制中に通常スコープを閉じない。巻き戻し中だけ抑制を解除する。
    m_suppressionDepth = 0;
    m_suppressionTopScopeId = 0;
    while (m_stack.size() > checkpoint.stackSize) {
        End(m_stack.back().token, ProfileSampleStatus::ABORTED);
    }

    m_suppressionDepth = checkpoint.suppressionDepth;
    m_suppressionTopScopeId = checkpoint.suppressionTopScopeId;
    while (!m_suppressionAnchors.empty() && m_suppressionAnchors.back().depth > checkpoint.suppressionDepth) {
        m_suppressionAnchors.pop_back();
    }
    return true;
}

void ProfileRecorder::EndFrame()
{
    EndFrameTo(m_snapshot);
}

void ProfileRecorder::EndFrameTo(ProfileSnapshot& destination)
{
    if (!m_frameOpen) {
        return;
    }
    // 初回公開時やセッションのリセット後は、計測器側の未使用の予約領域を公開先で再利用する。
    if (&destination != &m_snapshot && destination.samples.capacity() < MAX_SAMPLES) {
        destination.samples.swap(m_snapshot.samples);
    }

    // 閉じていない区間を中断扱いで確定してから、フレームの結果を公開先へ渡す。
    if (m_suppressionDepth) {
        MarkIncomplete();
    }
    m_suppressionDepth = 0;
    m_suppressionTopScopeId = 0;
    m_suppressionAnchors.clear();
    while (!m_stack.empty()) {
        End(m_stack.back().token, ProfileSampleStatus::ABORTED);
    }
    m_current.cpuFrameElapsedMs = std::max(0.0, ReadClock() - m_frameStartMs);
    m_current.recordedSampleCount = static_cast<uint32_t>(m_current.samples.size());
    std::swap(m_current, destination);
    m_frameOpen = false;
}

} // namespace fbzz::profiler
