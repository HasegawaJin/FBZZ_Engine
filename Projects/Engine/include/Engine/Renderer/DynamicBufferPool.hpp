/// @file    DynamicBufferPool.hpp
/// @brief   CPU が毎フレーム書き換える動的バッファ (頂点 / StructuredBuffer) の貸出プール。
/// @author  Hasegawa Jin
/// @date    2026-08-22
#pragma once

#include "ResourceHandle.hpp"
#include "ResourceManager.hpp"
#include <Engine/Core/Time.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::renderer {

namespace detail {

template <typename Tag> struct DynamicBufferFactory;

template <> struct DynamicBufferFactory<BufferTag> {
    static constexpr std::size_t kMinCapacity = 1024; ///< 頂点数
    static ResourceHandle<BufferTag> Create(ResourceManager& resources, std::size_t capacity,
                                            std::uint32_t stride, Where where)
    {
        return resources.CreateVertexBuffer(nullptr, capacity * stride, stride, where);
    }
};

template <> struct DynamicBufferFactory<StructuredBufferTag> {
    /// @note 力場・スポーン・ライト配列は数本〜数百本。頂点ほど大きく刻むと使わない領域が太る。
    static constexpr std::size_t kMinCapacity = 16; ///< 要素数
    static ResourceHandle<StructuredBufferTag> Create(ResourceManager& resources, std::size_t capacity,
                                                      std::uint32_t stride, Where where)
    {
        return resources.CreateStructuredBuffer(nullptr, static_cast<std::uint32_t>(capacity), stride, where);
    }
};

} // namespace detail

/// @brief 「CPU で組み直した中身を 1 フレームに何度も Submit / Dispatch する」用途のバッファ
///        貸出プール。書き換えるたびに Acquire で借り直すこと (使い回し禁止)。
/// @note DX12 の Submit はコマンドリストへの記録でしかないため、1 本を使い回して同じフレーム内で
///       2 回更新すると、先に記録した Draw まで後から書いた内容を読んでしまう。
/// @note GPU は CPU から最大 1 フレーム遅れて走るため、前フレームで貸したバッファを次フレームで
///       貸し直すと GPU がまだ読んでいる中身を上書きする。フレームごとに貸出列を分けて回避する。
/// @note 貸出順でなく容量クラスで列を分ける。貸出順は毎フレーム変わるため、順序で仕分けると
///       «一度でも大きな要求を受けた枠» が肥大したまま居座る。
template <typename Tag>
class DynamicBufferPool {
    /// @note 先頭で宣言するのは、コンストラクターの既定引数から引くため。MSVC はクラステンプレート
    ///       の既定引数で «後に宣言されるメンバー型» を解決できない (C2653)。
    using Factory = detail::DynamicBufferFactory<Tag>;

public:
    /// @param minCapacity 容量クラスの下限 (要素数)。要求はこれ以上の 2 の冪へ切り上げる。
    /// @note UI の矩形のように 1 回が数頂点で本数が多い用途は小さく取る (本数 × 下限が常駐する)。
    explicit DynamicBufferPool(std::size_t minCapacity = Factory::kMinCapacity)
        : m_minCapacity(minCapacity == 0 ? 1 : minCapacity) {}

    /// @brief stride バイトの要素を count 個ぶん収められるバッファを 1 つ貸し出す。
    /// @return 確保に失敗した場合は無効ハンドル。返したバッファの持ち主はプールで、借り手は
    ///         Release しないこと。
    /// @note 同じフレーム内で呼ぶたびに別のバッファを返し、直前 kFramesInFlight - 1 フレームに
    ///       貸したバッファも返さない。
    /// @note where は既定のまま渡すこと。バッファの発生位置が «この Acquire» ではなく
    ///       «どのパスのプールか» として記録される。
    [[nodiscard]] ResourceHandle<Tag> Acquire(ResourceManager& resources,
                                              std::size_t count,
                                              std::uint32_t stride,
                                              Where where = Where::current())
    {
        if (count == 0 || stride == 0) return {};

        /// @note ResourceManager::Reset() 後は旧ハンドルが無効なので、Release せずキャッシュだけ捨てる。
        if (m_resetVersion != resources.GetResetVersion()) {
            m_resetVersion = resources.GetResetVersion();
            m_buckets.clear();
        }
        /// @note フレーム単位で巻き戻すのは、エディタが 1 フレームで Scene View と Game View を
        ///       続けて描くため。パス単位で戻すと 2 つのビューが同じバッファを共有してしまう。
        if (m_frame != Time::frameCount) {
            m_frame = Time::frameCount;
            m_lane  = static_cast<std::size_t>(m_frame % kFramesInFlight);
            for (Bucket& bucket : m_buckets) bucket.next = 0;
        }

        /// @note 2 の冪へ切り上げて確保し、要素数が少し増えるたびに再確保が走るのを避ける。
        std::size_t capacity = m_minCapacity;
        while (capacity < count) capacity *= 2;

        Bucket& bucket = BucketFor(capacity, stride);
        std::vector<ResourceHandle<Tag>>& lane = bucket.lanes[m_lane];
        if (bucket.next >= lane.size()) {
            const ResourceHandle<Tag> handle = Factory::Create(resources, capacity, stride, where);
            if (!handle.IsValid()) return {};
            lane.push_back(handle);
        }
        return lane[bucket.next++];
    }

    /// @brief 抱えているバッファをすべて返す。プールごと畳むとき (シャットダウン) に呼ぶ。
    void ReleaseAll(ResourceManager& resources)
    {
        for (Bucket& bucket : m_buckets)
            for (auto& lane : bucket.lanes)
                for (ResourceHandle<Tag> handle : lane)
                    if (handle.IsValid()) resources.Release(handle);
        m_buckets.clear();
    }

    /// @brief 抱えている本数 (診断用)。
    [[nodiscard]] std::size_t GetBufferCount() const
    {
        std::size_t count = 0;
        for (const Bucket& bucket : m_buckets)
            for (const auto& lane : bucket.lanes) count += lane.size();
        return count;
    }

    /// @brief 貸出列の本数。GPU が追いかけているフレーム数より 1 本多く持つ。
    /// @note 2 本でなく 3 本なのは、Time::frameCount がレンダラーの GPU フレームと 1:1 ではない
    ///       ため (ウィンドウが隠れている間は BeginFrame が開かず frameCount だけ進む)。1 本余分に
    ///       持てば、列の周期と GPU フレームがずれても書く列は読み終わっている。
    static constexpr std::size_t kFramesInFlight = 3;

private:
    /// @brief 同じ «容量 × stride» のバッファを並べた列。next はこのフレームで何本貸したか。
    struct Bucket {
        std::size_t   capacity = 0; ///< 要素数
        std::uint32_t stride   = 0;
        std::size_t   next     = 0;
        std::array<std::vector<ResourceHandle<Tag>>, kFramesInFlight> lanes;
    };

    [[nodiscard]] Bucket& BucketFor(std::size_t capacity, std::uint32_t stride)
    {
        for (Bucket& bucket : m_buckets)
            if (bucket.capacity == capacity && bucket.stride == stride) return bucket;
        /// @note 容量は 2 の冪なので種類は高々十数個。線形探索で足りる。
        m_buckets.push_back(Bucket{ capacity, stride, 0, {} });
        return m_buckets.back();
    }

    std::vector<Bucket> m_buckets;
    std::size_t         m_minCapacity  = Factory::kMinCapacity;
    std::uint64_t       m_frame        = UINT64_MAX;
    std::size_t         m_lane         = 0;
    std::uint64_t       m_resetVersion = 0;
};

using DynamicVertexBufferPool     = DynamicBufferPool<BufferTag>;
using DynamicStructuredBufferPool = DynamicBufferPool<StructuredBufferTag>;

} // namespace fbzz::renderer
