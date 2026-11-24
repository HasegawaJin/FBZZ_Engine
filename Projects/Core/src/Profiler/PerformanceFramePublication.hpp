/// @file    PerformanceFramePublication.hpp
/// @brief   確定フレームと旧 API 向けの遅延生成ビュー。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#pragma once
#include <Core/Profiler/Profiler.hpp>

namespace fbzz::profiler::detail {

class PerformanceDescriptorTable;

class PerformanceFramePublication {
public:
    void Reset(bool recording, uint64_t captureSessionId);
    void UpdateSession(bool recording, uint64_t captureSessionId);
    [[nodiscard]] ProfileSnapshot& SampleStorage() { return m_snapshot; }
    void FinishFrame(const PerformanceCaptureFilter& filter, const PerformanceDescriptorTable& table);

    [[nodiscard]] const PerformanceSnapshot& Snapshot() const { return m_snapshot; }
    [[nodiscard]] uint64_t LastFrameIndex() const { return m_lastFrameIndex; }
    [[nodiscard]] const std::vector<ProfileRecord>& LegacyRecords();

private:
    void PublishDescriptors(const PerformanceDescriptorTable& table);
    void UpdateDescriptorStorage(const std::vector<PerformanceDescriptor>& registeredDescriptors);

    PerformanceSnapshot m_snapshot;
    std::vector<PerformanceDescriptor> m_previousDescriptors;
    std::vector<uint32_t> m_sampleDescriptorIndices;
    std::vector<uint32_t> m_publishedIndexByDescriptor;
    std::vector<ProfileRecord> m_legacyRecords;
    std::vector<uint32_t> m_sampleIndicesByCompletion;
    uint64_t m_lastFrameIndex = 0;
    bool m_legacyRecordsDirty = false;
};

}
