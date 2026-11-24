/// @file    PerformanceDescriptorTable.hpp
/// @brief   計測対象の名前とカテゴリを所有し、サンプルキーを管理する。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#pragma once
#include <Core/Profiler/Profiler.hpp>
#include <string_view>
#include <unordered_map>

namespace fbzz::profiler::detail {

class PerformanceDescriptorTable {
public:
    static constexpr size_t MAX_DESCRIPTORS = 8192;
    static constexpr uint32_t INVALID_INDEX = static_cast<uint32_t>(MAX_DESCRIPTORS);

    PerformanceDescriptorTable() = default;
    PerformanceDescriptorTable(const PerformanceDescriptorTable&) = delete;
    PerformanceDescriptorTable& operator=(const PerformanceDescriptorTable&) = delete;

    [[nodiscard]] uint64_t Resolve(const ProfilerMarker& marker);
    void RetireIfNeeded(const std::vector<PerformanceDescriptor>& publishedDescriptors);
    void Clear();

    [[nodiscard]] uint32_t FindIndex(uint64_t sampleKey) const;
    [[nodiscard]] const std::vector<PerformanceDescriptor>& Entries() const { return m_descriptors; }

private:
    struct DescriptorKey {
        std::string_view name;
        std::string_view category;
        uint32_t color;
        bool operator==(const DescriptorKey&) const = default;
    };
    struct DescriptorHash {
        size_t operator()(const DescriptorKey& key) const;
    };

    std::vector<PerformanceDescriptor> m_descriptors;
    /// @note キーは m_descriptors の文字列を借りる。表の移動・破棄前に検索索引を破棄する。
    std::unordered_map<DescriptorKey, uint64_t, DescriptorHash> m_sampleKeysByMarker;
    size_t m_ownedStringBytes = 0;
    uint64_t m_nextSampleKey = 0;
};

}
