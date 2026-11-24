/// @file    PerformanceDescriptorTable.cpp
/// @brief   計測対象の登録、文字列予算、不要になった登録項目の退役。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "PerformanceDescriptorTable.hpp"
#include <algorithm>
#include <utility>

namespace fbzz::profiler::detail {

namespace {

/// @note 表を 1 MiB に制限し、最新 snapshot と初回登録の一時文字列を含めて 4 MiB 以内に保つ。
constexpr size_t MAX_STRING_BYTES = 1024 * 1024;
/// @note MSVC の新規 char 文字列の丸めと終端へ 32 byte の余裕を取り、確保前に過大な入力を拒否する。
/// @see https://github.com/microsoft/STL/blob/main/stl/inc/xstring basic_string の _Alloc_mask と _Calculate_growth。
size_t EstimateStringCapacityBytes(std::string_view value)
{
    return value.size() > MAX_STRING_BYTES - 32 ? MAX_STRING_BYTES + 1 : value.size() + 32;
}
size_t OwnedStringBytes(const PerformanceDescriptor& descriptor)
{
    return descriptor.name.capacity() + 1 + descriptor.category.capacity() + 1;
}

}

size_t PerformanceDescriptorTable::DescriptorHash::operator()(const DescriptorKey& key) const
{
    return std::hash<std::string_view>{}(key.name)
        ^ (std::hash<std::string_view>{}(key.category) << 1) ^ key.color;
}

uint64_t PerformanceDescriptorTable::Resolve(const ProfilerMarker& marker)
{
    const DescriptorKey key{marker.name ? marker.name : "Unnamed", marker.category ? marker.category : "General", marker.color};
    const auto knownMarker = m_sampleKeysByMarker.find(key);
    if (knownMarker != m_sampleKeysByMarker.end()) return knownMarker->second;
    const size_t estimatedStringBytes = EstimateStringCapacityBytes(key.name) + EstimateStringCapacityBytes(key.category);
    if (m_descriptors.size() == MAX_DESCRIPTORS || estimatedStringBytes > MAX_STRING_BYTES - m_ownedStringBytes) return 0;
    PerformanceDescriptor candidate;
    candidate.name = key.name;
    candidate.category = key.category;
    candidate.color = key.color;
    const size_t ownedBytes = OwnedStringBytes(candidate);
    if (ownedBytes > MAX_STRING_BYTES - m_ownedStringBytes) return 0;
    if (m_descriptors.capacity() < MAX_DESCRIPTORS) m_descriptors.reserve(MAX_DESCRIPTORS);
    candidate.sampleKey = ++m_nextSampleKey;
    m_descriptors.push_back(std::move(candidate));
    auto& descriptor = m_descriptors.back();
    m_ownedStringBytes += ownedBytes;
    m_sampleKeysByMarker.emplace(DescriptorKey{descriptor.name, descriptor.category, descriptor.color}, descriptor.sampleKey);
    return descriptor.sampleKey;
}

void PerformanceDescriptorTable::RetireIfNeeded(
    const std::vector<PerformanceDescriptor>& publishedDescriptors)
{
    if (m_descriptors.size() <= MAX_DESCRIPTORS / 2 && m_ownedStringBytes <= MAX_STRING_BYTES / 2) return;
    if (m_descriptors.size() == publishedDescriptors.size()) return;
    /// @note 両表は登録 ID 順。所有文字列を動かす前に非所有検索キーを破棄する。
    m_sampleKeysByMarker.clear();
    size_t retainedCount = 0;
    size_t publishedIndex = 0;
    for (size_t descriptorIndex = 0; descriptorIndex < m_descriptors.size(); ++descriptorIndex) {
        const auto sampleKey = m_descriptors[descriptorIndex].sampleKey;
        while (publishedIndex < publishedDescriptors.size() && publishedDescriptors[publishedIndex].sampleKey < sampleKey)
            ++publishedIndex;
        if (publishedIndex == publishedDescriptors.size() || publishedDescriptors[publishedIndex].sampleKey != sampleKey) continue;
        if (retainedCount != descriptorIndex) m_descriptors[retainedCount] = std::move(m_descriptors[descriptorIndex]);
        ++retainedCount;
    }
    m_descriptors.resize(retainedCount);
    m_ownedStringBytes = 0;
    for (const auto& descriptor : m_descriptors) {
        m_ownedStringBytes += OwnedStringBytes(descriptor);
        m_sampleKeysByMarker.emplace(DescriptorKey{descriptor.name, descriptor.category, descriptor.color}, descriptor.sampleKey);
    }
}

uint32_t PerformanceDescriptorTable::FindIndex(uint64_t sampleKey) const
{
    if (m_descriptors.empty() || sampleKey < m_descriptors.front().sampleKey) return INVALID_INDEX;
    /// @note 退役で ID に穴がない通常の表は定数時間で引き、疎な表でも探索を対数時間に制限する。
    const uint64_t offset = sampleKey - m_descriptors.front().sampleKey;
    if (offset < m_descriptors.size() && m_descriptors[static_cast<size_t>(offset)].sampleKey == sampleKey)
        return static_cast<uint32_t>(offset);
    const auto found = std::lower_bound(m_descriptors.begin(), m_descriptors.end(), sampleKey,
        [](const auto& descriptor, uint64_t searchKey) { return descriptor.sampleKey < searchKey; });
    return found != m_descriptors.end() && found->sampleKey == sampleKey
        ? static_cast<uint32_t>(found - m_descriptors.begin()) : INVALID_INDEX;
}

void PerformanceDescriptorTable::Clear()
{
    m_sampleKeysByMarker.clear();
    m_descriptors.clear();
    m_ownedStringBytes = 0;
}

}
