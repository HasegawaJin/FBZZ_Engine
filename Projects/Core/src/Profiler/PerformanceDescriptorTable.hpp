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
    static constexpr size_t MAX_DESCRIPTORS = 8192; ///< 同時に保持できる計測対象の上限。
    static constexpr uint32_t INVALID_INDEX = static_cast<uint32_t>(MAX_DESCRIPTORS); ///< 登録表に該当する ID がないことを表す添字。

    PerformanceDescriptorTable() = default;
    PerformanceDescriptorTable(const PerformanceDescriptorTable&) = delete;
    PerformanceDescriptorTable& operator=(const PerformanceDescriptorTable&) = delete;

    /// @param[in] marker 名前と分類は呼び出し中に参照する。新規登録時は表が文字列のコピーを所有する。
    /// @return 名前・分類・色が一致する計測対象の ID。新規登録が件数または文字列容量の上限を超えると 0。
    [[nodiscard]] uint64_t Resolve(const ProfilerMarker& marker);
    /// @param[in] publishedDescriptors 直近のフレームが参照している登録項目。登録 ID の昇順。
    void RetireIfNeeded(const std::vector<PerformanceDescriptor>& publishedDescriptors);
    void Clear();

    [[nodiscard]] uint32_t FindIndex(uint64_t sampleKey) const;
    [[nodiscard]] const std::vector<PerformanceDescriptor>& Entries() const { return m_descriptors; }

private:
    /// @brief 名前、分類、表示色の組み合わせで計測対象を検索するキー。
    struct DescriptorKey {
        std::string_view name;     ///< 名前の非所有参照。登録済みのキーは記述子の文字列を参照する。
        std::string_view category; ///< 分類名の非所有参照。登録済みのキーは記述子の文字列を参照する。
        uint32_t color;            ///< 同じ名前・分類でも別の表示色を区別するための値（0xAARRGGBB）。
        bool operator==(const DescriptorKey&) const = default;
    };
    struct DescriptorHash {
        size_t operator()(const DescriptorKey& key) const;
    };

    std::vector<PerformanceDescriptor> m_descriptors; ///< 表示名と分類名を所有する登録表。ID の昇順で保持する。
    std::unordered_map<DescriptorKey, uint64_t, DescriptorHash> m_sampleKeysByMarker; ///< 登録表の文字列を借り、名前・分類・色から ID を引く索引。
    size_t m_ownedStringBytes = 0; ///< 登録済みの名前と分類が占める合計バイト数。予約容量と終端を含む。
    uint64_t m_nextSampleKey = 0; ///< 最後に発行した ID。登録を消去した後も採番を続ける。
};

}
