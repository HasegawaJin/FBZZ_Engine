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

    PerformanceSnapshot m_snapshot; ///< 現在公開しているフレームのサンプル・記述子・収集状態。
    std::vector<PerformanceDescriptor> m_previousDescriptors; ///< 再公開時に前フレームの文字列を移すための一時領域。
    std::vector<uint32_t> m_sampleDescriptorIndices; ///< サンプル配列の添字から引く公開記述子の添字。
    std::vector<uint32_t> m_publishedIndexByDescriptor; ///< 登録表の添字から引く公開記述子の添字。未公開は無効値。
    std::vector<ProfileRecord> m_legacyRecords; ///< 旧 API 向けに完了順で生成したレコード。文字列は m_snapshot を参照する。
    std::vector<uint32_t> m_sampleIndicesByCompletion; ///< 完了順から引く m_snapshot.samples の添字。
    uint64_t m_lastFrameIndex = 0; ///< 最後に確定した計測フレームの通し番号。
    bool m_legacyRecordsDirty = false; ///< 次の旧 API の参照時にレコードを組み立てる場合に true。
};

}
