/// @file    PerformanceDescriptorTable.cpp
/// @brief   計測対象の登録、文字列予算、不要になった登録項目の退役。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "PerformanceDescriptorTable.hpp"
#include <algorithm>
#include <utility>

namespace fbzz::profiler::detail {

namespace {

/// @note 登録表は 1 MiB に制限し、最新のスナップショットと初回登録時の一時文字列を含めて 4 MiB 以内に収める。
constexpr size_t MAX_STRING_BYTES = 1024 * 1024;
constexpr size_t STRING_CAPACITY_MARGIN = 32;

/// @note MSVC の文字列容量の切り上げと終端文字を見込んで 32 バイトの余裕を取り、大きすぎる要求を確保前に退ける。
/// @see https://github.com/microsoft/STL/blob/main/stl/inc/xstring basic_string の _Alloc_mask と _Calculate_growth。
size_t EstimateStringCapacityBytes(std::string_view value)
{
    // 加算が上限を超える入力は、長い文字列でもオーバーフローしない予算超過値で返す。
    if (value.size() > MAX_STRING_BYTES - STRING_CAPACITY_MARGIN) {
        return MAX_STRING_BYTES + 1;
    }

    return value.size() + STRING_CAPACITY_MARGIN;
}

size_t OwnedStringBytes(const PerformanceDescriptor& descriptor)
{
    // 予約済みの空きと終端文字も含め、登録後に表が保持する容量を数える。
    const size_t nameBytes = descriptor.name.capacity() + 1;
    const size_t categoryBytes = descriptor.category.capacity() + 1;
    return nameBytes + categoryBytes;
}

}

size_t PerformanceDescriptorTable::DescriptorHash::operator()(const DescriptorKey& key) const
{
    // 所有元が違っても同じ内容を引けるように、文字列の内容と表示色を組み合わせる。
    const auto nameHash = std::hash<std::string_view>{}(key.name);
    const auto categoryHash = std::hash<std::string_view>{}(key.category);
    return nameHash ^ (categoryHash << 1) ^ key.color;
}

uint64_t PerformanceDescriptorTable::Resolve(const ProfilerMarker& marker)
{
    // 同じ名前・分類・色を繰り返し計測する通常経路では、文字列をコピーせず既存 ID を返す。
    constexpr ProfilerMarker DEFAULT_MARKER{};
    const DescriptorKey key{
        marker.name ? marker.name : DEFAULT_MARKER.name,
        marker.category ? marker.category : DEFAULT_MARKER.category,
        marker.color
    };
    const auto knownMarker = m_sampleKeysByMarker.find(key);
    if (knownMarker != m_sampleKeysByMarker.end()) {
        return knownMarker->second;
    }

    // 新規登録では、件数と文字列容量の見積もりを先に調べ、大きすぎる入力のコピーを避ける。
    if (m_descriptors.size() == MAX_DESCRIPTORS) {
        return 0;
    }
    const size_t remainingStringBytes = MAX_STRING_BYTES - m_ownedStringBytes;
    const size_t estimatedStringBytes =
        EstimateStringCapacityBytes(key.name) + EstimateStringCapacityBytes(key.category);
    if (estimatedStringBytes > remainingStringBytes) {
        return 0;
    }

    // 文字列の予約容量はコピー後に確定する。実際に保持する容量が収まるまで ID は発行しない。
    PerformanceDescriptor candidate;
    candidate.name = key.name;
    candidate.category = key.category;
    candidate.color = key.color;
    const size_t ownedBytes = OwnedStringBytes(candidate);
    if (ownedBytes > remainingStringBytes) {
        return 0;
    }

    // 索引は表内の文字列を借りる。追加で要素が移動しないよう、初回に登録上限まで予約する。
    if (m_descriptors.capacity() < MAX_DESCRIPTORS) {
        m_descriptors.reserve(MAX_DESCRIPTORS);
    }
    candidate.sampleKey = ++m_nextSampleKey;
    m_descriptors.push_back(std::move(candidate));

    // 呼び出し元や一時候補の文字列は参照せず、表が所有する文字列で検索キーを作る。
    const auto& descriptor = m_descriptors.back();
    const DescriptorKey storedKey{descriptor.name, descriptor.category, descriptor.color};
    m_ownedStringBytes += ownedBytes;
    m_sampleKeysByMarker.emplace(storedKey, descriptor.sampleKey);
    return descriptor.sampleKey;
}

void PerformanceDescriptorTable::RetireIfNeeded(
    const std::vector<PerformanceDescriptor>& publishedDescriptors)
{
    // 件数か文字列容量が上限の半分を超え、かつ未使用の登録があるときだけ表を詰め直す。
    const bool hasSpareEntries = m_descriptors.size() <= MAX_DESCRIPTORS / 2;
    const bool hasSpareStringBytes = m_ownedStringBytes <= MAX_STRING_BYTES / 2;
    if (hasSpareEntries && hasSpareStringBytes) {
        return;
    }
    if (m_descriptors.size() == publishedDescriptors.size()) {
        return;
    }

    // 索引が借りている文字列は移動で無効になる。索引を外してから、公開中の登録だけを前に詰める。
    m_sampleKeysByMarker.clear();
    size_t retainedCount = 0;
    size_t publishedIndex = 0;
    for (size_t descriptorIndex = 0; descriptorIndex < m_descriptors.size(); ++descriptorIndex) {
        const auto sampleKey = m_descriptors[descriptorIndex].sampleKey;
        // 両表が ID 順なので、公開側の読み位置を巻き戻さずに照合できる。
        while (publishedIndex < publishedDescriptors.size() && publishedDescriptors[publishedIndex].sampleKey < sampleKey) {
            ++publishedIndex;
        }
        if (publishedIndex == publishedDescriptors.size()) {
            break;
        }
        if (publishedDescriptors[publishedIndex].sampleKey != sampleKey) {
            continue;
        }

        if (retainedCount != descriptorIndex) {
            m_descriptors[retainedCount] = std::move(m_descriptors[descriptorIndex]);
        }
        ++retainedCount;
    }
    m_descriptors.resize(retainedCount);

    // 移動後の文字列を使って検索索引と容量の合計を作り直す。
    m_ownedStringBytes = 0;
    for (const auto& descriptor : m_descriptors) {
        m_ownedStringBytes += OwnedStringBytes(descriptor);
        m_sampleKeysByMarker.emplace(DescriptorKey{descriptor.name, descriptor.category, descriptor.color}, descriptor.sampleKey);
    }
}

uint32_t PerformanceDescriptorTable::FindIndex(uint64_t sampleKey) const
{
    // 先頭 ID より古いキーは表に存在しない。差分を取る前に除いて符号なし整数の折り返しを防ぐ。
    if (m_descriptors.empty() || sampleKey < m_descriptors.front().sampleKey) {
        return INVALID_INDEX;
    }

    // 登録 ID が連続していれば、先頭との差がそのまま配列の添字になる。
    const uint64_t offset = sampleKey - m_descriptors.front().sampleKey;
    if (offset < m_descriptors.size() && m_descriptors[static_cast<size_t>(offset)].sampleKey == sampleKey) {
        return static_cast<uint32_t>(offset);
    }

    // 退役で ID に穴ができても順序は保たれるため、残った表を二分探索する。
    const auto found = std::lower_bound(m_descriptors.begin(), m_descriptors.end(), sampleKey,
        [](const auto& descriptor, uint64_t searchKey) { return descriptor.sampleKey < searchKey; });
    if (found == m_descriptors.end() || found->sampleKey != sampleKey) {
        return INVALID_INDEX;
    }
    return static_cast<uint32_t>(found - m_descriptors.begin());
}

void PerformanceDescriptorTable::Clear()
{
    // 文字列を借りる索引を先に空にする。発行済み ID が再利用されないよう採番値は引き継ぐ。
    m_sampleKeysByMarker.clear();
    m_descriptors.clear();
    m_ownedStringBytes = 0;
}

}
