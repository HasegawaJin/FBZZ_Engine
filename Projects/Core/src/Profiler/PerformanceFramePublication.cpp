/// @file    PerformanceFramePublication.cpp
/// @brief   フレーム確定時の計測対象の公開と互換レコードの組み立て。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "PerformanceFramePublication.hpp"
#include "PerformanceDescriptorTable.hpp"
#include <utility>

namespace fbzz::profiler::detail {

void PerformanceFramePublication::Reset(bool recording, uint64_t captureSessionId)
{
    m_legacyRecords.clear();
    m_legacyRecordsDirty = false;
    m_snapshot = {};
    m_snapshot.recording = recording;
    m_snapshot.captureSessionId = captureSessionId;
}

void PerformanceFramePublication::UpdateSession(bool recording, uint64_t captureSessionId)
{
    if (m_snapshot.captureSessionId != captureSessionId) {
        Reset(recording, captureSessionId);
        return;
    }
    m_snapshot.recording = recording;
}

void PerformanceFramePublication::FinishFrame(const PerformanceCaptureFilter& filter,
                                              const PerformanceDescriptorTable& table)
{
    m_snapshot.captureFilter = filter;
    m_lastFrameIndex = m_snapshot.frameIndex;
    PublishDescriptors(table);
    m_legacyRecords.clear();
    m_legacyRecordsDirty = true;
}

void PerformanceFramePublication::PublishDescriptors(const PerformanceDescriptorTable& table)
{
    const auto& registeredDescriptors = table.Entries();
    if (m_snapshot.samples.empty()) {
        m_sampleDescriptorIndices.clear();
        m_snapshot.descriptors.clear();
        return;
    }
    if (m_sampleDescriptorIndices.capacity() < ProfileRecorder::MAX_SAMPLES)
        m_sampleDescriptorIndices.reserve(ProfileRecorder::MAX_SAMPLES);
    if (m_publishedIndexByDescriptor.capacity() < PerformanceDescriptorTable::MAX_DESCRIPTORS)
        m_publishedIndexByDescriptor.reserve(PerformanceDescriptorTable::MAX_DESCRIPTORS);
    m_sampleDescriptorIndices.resize(m_snapshot.samples.size());
    m_publishedIndexByDescriptor.assign(registeredDescriptors.size(), PerformanceDescriptorTable::INVALID_INDEX);
    /// @note 参照された登録項目を 0 で印付けし、次の走査で公開用の索引を割り当てる。
    for (size_t sampleIndex = 0; sampleIndex < m_snapshot.samples.size(); ++sampleIndex) {
        const uint32_t descriptorIndex = table.FindIndex(m_snapshot.samples[sampleIndex].sampleKey);
        m_sampleDescriptorIndices[sampleIndex] = descriptorIndex;
        if (descriptorIndex < registeredDescriptors.size()) {
            m_publishedIndexByDescriptor[descriptorIndex] = 0;
        }
    }

    UpdateDescriptorStorage(registeredDescriptors);

    /// @note 公開 snapshot の索引へ変換し、次の BeginFrame で実行表が退役しても旧記録を参照できる。
    for (auto& descriptorIndex : m_sampleDescriptorIndices) {
        if (descriptorIndex < registeredDescriptors.size()) {
            descriptorIndex = m_publishedIndexByDescriptor[descriptorIndex];
        }
    }
}

void PerformanceFramePublication::UpdateDescriptorStorage(
    const std::vector<PerformanceDescriptor>& registeredDescriptors)
{
    size_t publishedCount = 0;
    bool descriptorOrderUnchanged = true;
    for (size_t descriptorIndex = 0; descriptorIndex < registeredDescriptors.size(); ++descriptorIndex) {
        if (m_publishedIndexByDescriptor[descriptorIndex] == PerformanceDescriptorTable::INVALID_INDEX) continue;
        if (publishedCount >= m_snapshot.descriptors.size()
            || m_snapshot.descriptors[publishedCount].sampleKey != registeredDescriptors[descriptorIndex].sampleKey) {
            descriptorOrderUnchanged = false;
        }
        m_publishedIndexByDescriptor[descriptorIndex] = static_cast<uint32_t>(publishedCount++);
    }

    if (!descriptorOrderUnchanged || publishedCount != m_snapshot.descriptors.size()) {
        /// @note 同じ ID の所有文字列を移し、変更された descriptor だけを新しく複写する。
        m_snapshot.descriptors.swap(m_previousDescriptors);
        if (m_snapshot.descriptors.capacity() < PerformanceDescriptorTable::MAX_DESCRIPTORS)
            m_snapshot.descriptors.reserve(PerformanceDescriptorTable::MAX_DESCRIPTORS);
        size_t previousDescriptorIndex = 0;
        for (size_t descriptorIndex = 0; descriptorIndex < registeredDescriptors.size(); ++descriptorIndex) {
            if (m_publishedIndexByDescriptor[descriptorIndex] == PerformanceDescriptorTable::INVALID_INDEX) continue;
            const auto& descriptor = registeredDescriptors[descriptorIndex];
            while (previousDescriptorIndex < m_previousDescriptors.size()
                && m_previousDescriptors[previousDescriptorIndex].sampleKey < descriptor.sampleKey) {
                ++previousDescriptorIndex;
            }
            if (previousDescriptorIndex < m_previousDescriptors.size()
                && m_previousDescriptors[previousDescriptorIndex].sampleKey == descriptor.sampleKey) {
                m_snapshot.descriptors.push_back(std::move(m_previousDescriptors[previousDescriptorIndex++]));
            } else {
                m_snapshot.descriptors.push_back(descriptor);
            }
        }
        m_previousDescriptors.clear();
    }
}

const std::vector<ProfileRecord>& PerformanceFramePublication::LegacyRecords()
{
    if (!m_legacyRecordsDirty || m_snapshot.samples.empty()) {
        m_legacyRecordsDirty = false;
        return m_legacyRecords;
    }
    if (m_legacyRecords.capacity() < ProfileRecorder::MAX_SAMPLES) m_legacyRecords.reserve(ProfileRecorder::MAX_SAMPLES);
    if (m_sampleIndicesByCompletion.capacity() < ProfileRecorder::MAX_SAMPLES) m_sampleIndicesByCompletion.reserve(ProfileRecorder::MAX_SAMPLES);
    m_sampleIndicesByCompletion.resize(m_snapshot.samples.size());
    /// @note 全保存 sample は終了時に 1..N の一意な順番を持ち、異常終了もその順番を消費する。
    for (size_t sampleIndex = 0; sampleIndex < m_snapshot.samples.size(); ++sampleIndex)
        m_sampleIndicesByCompletion[m_snapshot.samples[sampleIndex].completionOrder - 1] = static_cast<uint32_t>(sampleIndex);
    for (const auto sampleIndex : m_sampleIndicesByCompletion) {
        const auto& sample = m_snapshot.samples[sampleIndex];
        const uint32_t descriptorIndex = m_sampleDescriptorIndices[sampleIndex];
        if (sample.status != ProfileSampleStatus::COMPLETE || descriptorIndex >= m_snapshot.descriptors.size()) continue;
        const auto& descriptor = m_snapshot.descriptors[descriptorIndex];
        m_legacyRecords.push_back({descriptor.name.c_str(), descriptor.category.c_str(), sample.inclusiveMs,
                             m_lastFrameIndex, sample.depth, descriptor.color});
    }
    m_legacyRecordsDirty = false;
    return m_legacyRecords;
}

}
