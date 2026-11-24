/// @file    GpuProfilerLedger.hpp
/// @brief   GPU 提出フェンスに結び付けた計測領域の純粋な台帳。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once

#include "GpuProfiler.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace fbzz::renderer {

/// @note フェンス完了の観測だけを入力とし、GPU の待機・同期読み戻しを行わない。
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/fence-based-resource-management Fence-Based Resource Management
class GpuProfilerLedger {
public:
    /// @note Scene/Game の約 60 区間に余裕を持たせ、超過は引き続き partial として公開する。
    static constexpr uint32_t MAX_PASSES = 128;

    explicit GpuProfilerLedger(uint32_t slotCount = 2) : m_frames(slotCount) {}

    void Reset(uint64_t deviceEpoch, bool supported)
    {
        for (auto& frame : m_frames) frame = {};
        m_activeSlot = INVALID_SLOT;
        m_lastBegunSerial = 0;
        m_deviceEpoch = deviceEpoch;
        m_supported = supported;
        m_snapshot = {};
        m_snapshot.supported = supported;
        m_snapshot.deviceEpoch = deviceEpoch;
    }

    /// @return 未回収の領域・重複 serial・別 device 世代では false。既存領域を変更しない。
    bool BeginFrame(uint32_t slot, uint64_t physicalFrameSerial, uint64_t deviceEpoch)
    {
        if (!m_supported || slot >= m_frames.size() || m_activeSlot != INVALID_SLOT
            || m_frames[slot].state != State::EMPTY || deviceEpoch != m_deviceEpoch
            || physicalFrameSerial == 0 || physicalFrameSerial <= m_lastBegunSerial) return false;
        auto& frame = m_frames[slot];
        frame = {};
        frame.state = State::RECORDING;
        frame.serial = physicalFrameSerial;
        frame.complete = true;
        m_activeSlot = slot;
        m_lastBegunSerial = physicalFrameSerial;
        return true;
    }

    bool BeginView(const GpuProfilerViewMetadata& metadata)
    {
        auto* frame = RecordingFrame();
        if (!frame) return false;
        if (frame->viewOpen || frame->passOpen || metadata.width == 0 || metadata.height == 0) {
            frame->complete = false;
            return false;
        }
        frame->metadata = metadata;
        frame->viewOpen = true;
        return true;
    }

    void EndView()
    {
        auto* frame = RecordingFrame();
        if (!frame) return;
        if (!frame->viewOpen) frame->complete = false;
        DiscardOpenPass(*frame);
        frame->viewOpen = false;
    }

    /// @return 記録可能なときだけ true。outIndex は全領域の timestamp 配列内の添字。
    bool BeginPass(std::string_view name, uint32_t& outIndex)
    {
        outIndex = INVALID_SLOT;
        auto* frame = RecordingFrame();
        if (!frame) return false;
        if (!frame->viewOpen || frame->passOpen || frame->count == MAX_PASSES) {
            frame->complete = false;
            IncrementDropped(*frame);
            return false;
        }
        frame->names[frame->count] = name;
        frame->views[frame->count] = frame->metadata;
        frame->passOpen = true;
        outIndex = (m_activeSlot * MAX_PASSES + frame->count) * 2;
        return true;
    }

    bool EndPass(std::string_view name, uint32_t& outIndex)
    {
        outIndex = INVALID_SLOT;
        auto* frame = RecordingFrame();
        if (!frame) return false;
        if (!frame->passOpen) {
            frame->complete = false;
            return false;
        }
        if (frame->names[frame->count] != name) {
            DiscardOpenPass(*frame);
            return false;
        }
        outIndex = (m_activeSlot * MAX_PASSES + frame->count) * 2 + 1;
        frame->passOpen = false;
        ++frame->count;
        return true;
    }

    /// @note command-list 境界を跨ぐ等、比較できない区間だけを破棄して完成済み区間は保持する。
    bool CancelOpenPass()
    {
        auto* frame = RecordingFrame();
        if (!frame || !frame->passOpen) return false;
        DiscardOpenPass(*frame);
        return true;
    }

    /// @note Begin/End が揃った範囲だけを Resolve する。未終了 query を読み戻さない。
    uint32_t FinishFrame()
    {
        auto* frame = RecordingFrame();
        if (!frame) return 0;
        if (frame->viewOpen) frame->complete = false;
        DiscardOpenPass(*frame);
        frame->viewOpen = false;
        frame->state = State::FINISHED;
        const uint32_t count = frame->count;
        m_activeSlot = INVALID_SLOT;
        return count;
    }

    /// @note actualFence は query Resolve を含む最後の command list を提出した後の Signal 値。
    /// @return fence 0 / UINT64_MAX は提出失敗として領域を破棄し false。以前の公開結果は維持する。
    bool SubmitFrame(uint32_t slot, uint64_t actualFence)
    {
        if (slot >= m_frames.size() || m_frames[slot].state != State::FINISHED) return false;
        auto& frame = m_frames[slot];
        if (actualFence == 0 || actualFence == std::numeric_limits<uint64_t>::max()) {
            frame = {};
            return false;
        }
        frame.fence = actualFence;
        frame.state = State::SUBMITTED;
        return true;
    }

    /// @note 完了した領域を一度だけ回収し、serial が最大の一件だけを公開する。
    /// @note timestamp 0 自体は合法。逆順、欠落、frequency 0 は unavailable とする。
    /// @note completedFence の UINT64_MAX は device removal。Reset まで公開・記録を無効化する。
    /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing Timing
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue ID3D12Fence::GetCompletedValue
    void Collect(uint64_t completedFence, std::span<const uint64_t> timestamps, uint64_t frequency)
    {
        if (completedFence == std::numeric_limits<uint64_t>::max()) {
            Reset(m_deviceEpoch, false);
            return;
        }
        for (uint32_t slot = 0; slot < m_frames.size(); ++slot) {
            auto& frame = m_frames[slot];
            if (frame.state != State::SUBMITTED || frame.fence > completedFence) continue;
            if (frame.serial > m_snapshot.physicalFrameSerial) {
                GpuProfilerSnapshot candidate;
                candidate.supported = m_supported;
                candidate.complete = frame.complete && frame.count != 0;
                candidate.physicalFrameSerial = frame.serial;
                candidate.deviceEpoch = m_deviceEpoch;
                candidate.recordedPassCount = frame.count;
                candidate.droppedPassCount = frame.dropped;
                candidate.passes.reserve(frame.count);
                for (uint32_t pass = 0; pass < frame.count; ++pass) {
                    GpuPassProfile profile;
                    profile.name = frame.names[pass];
                    profile.metadata = frame.views[pass];
                    profile.physicalFrameSerial = frame.serial;
                    profile.deviceEpoch = m_deviceEpoch;
                    const size_t beginIndex = (static_cast<size_t>(slot) * MAX_PASSES + pass) * 2;
                    if (frequency != 0 && beginIndex + 1 < timestamps.size()
                        && timestamps[beginIndex + 1] >= timestamps[beginIndex]) {
                        const double ms = static_cast<double>(timestamps[beginIndex + 1] - timestamps[beginIndex])
                            * 1000.0 / static_cast<double>(frequency);
                        if (std::isfinite(ms) && ms >= 0.0) {
                            profile.gpuMs = ms;
                            profile.available = true;
                            candidate.available = true;
                        }
                    }
                    candidate.complete = candidate.complete && profile.available;
                    candidate.passes.push_back(std::move(profile));
                }
                m_snapshot = std::move(candidate);
            }
            frame = {};
        }
    }

    const GpuProfilerSnapshot& GetSnapshot() const { return m_snapshot; }

private:
    static constexpr uint32_t INVALID_SLOT = std::numeric_limits<uint32_t>::max();
    enum class State { EMPTY, RECORDING, FINISHED, SUBMITTED };
    struct Frame {
        std::array<std::string, MAX_PASSES> names;
        std::array<GpuProfilerViewMetadata, MAX_PASSES> views;
        GpuProfilerViewMetadata metadata;
        uint64_t serial = 0;
        uint64_t fence = 0;
        uint32_t count = 0;
        uint32_t dropped = 0;
        State state = State::EMPTY;
        bool complete = false;
        bool viewOpen = false;
        bool passOpen = false;
    };

    Frame* RecordingFrame()
    {
        return m_activeSlot < m_frames.size() && m_frames[m_activeSlot].state == State::RECORDING
            ? &m_frames[m_activeSlot] : nullptr;
    }

    static void IncrementDropped(Frame& frame)
    {
        if (frame.dropped != std::numeric_limits<uint32_t>::max()) ++frame.dropped;
    }

    static void DiscardOpenPass(Frame& frame)
    {
        if (!frame.passOpen) return;
        frame.complete = false;
        IncrementDropped(frame);
        frame.passOpen = false;
    }

    std::vector<Frame> m_frames;
    GpuProfilerSnapshot m_snapshot;
    uint64_t m_lastBegunSerial = 0;
    uint64_t m_deviceEpoch = 0;
    uint32_t m_activeSlot = INVALID_SLOT;
    bool m_supported = false;
};

} /// @note namespace fbzz::renderer
