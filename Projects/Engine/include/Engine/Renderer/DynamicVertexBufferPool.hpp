/// @file    DynamicVertexBufferPool.hpp
/// @brief   1 フレーム内で何度も書き換える動的頂点バッファの貸出プール。
/// @author  Hasegawa Jin
/// @date    2026-08-22
#pragma once

#include "ResourceHandle.hpp"
#include "ResourceManager.hpp"
#include <Engine/Core/Time.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::renderer {

/// 「CPU で組み直した頂点列を 1 フレームに何度も Submit する」用途の頂点バッファ貸出プール。
///
/// WHY 1 本を使い回してはいけないか:
///   DX12 の Submit はコマンドリストへの「記録」でしかなく、GPU が頂点を読むのはフレーム終端の
///   実行時。DX12Buffer::Update は永続 Map した Upload ヒープの先頭へ memcpy するだけなので、
///   1 フレーム内で同じバッファを 2 回更新すると、先に記録した Draw まで後から書いた内容を読む。
///   「エミッターごとに Update → Submit」「パスごとに Flush」のような書き方は、
///   2 個目以降が 1 個目を巻き添えにして壊す (線や板が別の形に化ける・ちらつく)。
///   DX11 は MAP_WRITE_DISCARD がバッファをリネームするため同じコードでも無害で、
///   バックエンドを DX12 へ切り替えたときだけ症状が出る。
///
/// WHY «貸出順» ではなく «容量» で仕分けるか (2026-09-02 に変更):
///   以前は貸出順の添字でスロットを引き当て、要求が入らなければそのスロットを作り直していた。
///   貸出順は毎フレーム変わる — パーティクルは視点からの距離で並び替わるし、エミッターは
///   湧いては消える。その結果 «一度でも大きな要求を受けた添字» が大きいまま居座り、
///   最後にはすべてのスロットが «そのフレームの最大» まで太る。1 本 92KB の板が
///   10 スロットぶん張り付く、という形で «じわじわ増えて減らない» ように見えていた。
///   容量クラスごとに列を分ければ、小さい要求は小さいバッファを借り続ける。
class DynamicVertexBufferPool {
public:
    /// stride バイトの頂点を vertexCount 個ぶん収められるバッファを 1 つ貸し出す。
    /// 同じフレーム内で呼ぶたびに別のバッファを返す。確保に失敗した場合は無効ハンドル。
    /// @note where は既定のまま渡すこと。バッファの発生位置が «この Acquire» ではなく
    ///       «どのパスのプールか» として記録される (Where の WHY を参照)。
    [[nodiscard]] ResourceHandle<BufferTag> Acquire(ResourceManager& resources,
                                                    std::size_t vertexCount,
                                                    std::uint32_t stride,
                                                    Where where = Where::current())
    {
        if (vertexCount == 0 || stride == 0) return {};

        // ResourceManager::Reset() 後は旧ハンドルが無効なので、Release せずキャッシュだけ捨てる。
        if (m_resetVersion != resources.GetResetVersion()) {
            m_resetVersion = resources.GetResetVersion();
            m_buckets.clear();
        }
        // WHY パス単位ではなくフレーム単位で巻き戻すか: エディタは 1 フレームで Scene View と
        //     Game View を続けて描くため、パス単位で戻すと 2 つのビューが同じバッファを共有し、
        //     プールを分けた意味が無くなる。
        if (m_frame != Time::frameCount) {
            m_frame = Time::frameCount;
            for (Bucket& bucket : m_buckets) bucket.next = 0;
        }

        // 2 の冪へ切り上げて確保し、頂点数が少し増えるたびに再確保が走るのを避ける。
        std::size_t capacity = kMinCapacity;
        while (capacity < vertexCount) capacity *= 2;

        Bucket& bucket = BucketFor(capacity, stride);
        if (bucket.next >= bucket.handles.size()) {
            const ResourceHandle<BufferTag> handle =
                resources.CreateVertexBuffer(nullptr, capacity * stride, stride, where);
            if (!handle.IsValid()) return {};
            bucket.handles.push_back(handle);
        }
        return bucket.handles[bucket.next++];
    }

    /// 抱えているバッファをすべて返す。プールごと畳むとき (シャットダウン) に呼ぶ。
    void ReleaseAll(ResourceManager& resources)
    {
        for (Bucket& bucket : m_buckets)
            for (ResourceHandle<BufferTag> handle : bucket.handles)
                if (handle.IsValid()) resources.Release(handle);
        m_buckets.clear();
    }

    /// 抱えている本数 (診断用)。
    [[nodiscard]] std::size_t GetBufferCount() const
    {
        std::size_t count = 0;
        for (const Bucket& bucket : m_buckets) count += bucket.handles.size();
        return count;
    }

private:
    static constexpr std::size_t kMinCapacity = 1024; // 頂点数

    // 同じ «容量 × stride» のバッファを並べた列。next はこのフレームで何本貸したか。
    struct Bucket {
        std::size_t                            capacity = 0; // 頂点数
        std::uint32_t                          stride   = 0;
        std::size_t                            next     = 0;
        std::vector<ResourceHandle<BufferTag>> handles;
    };

    [[nodiscard]] Bucket& BucketFor(std::size_t capacity, std::uint32_t stride)
    {
        for (Bucket& bucket : m_buckets)
            if (bucket.capacity == capacity && bucket.stride == stride) return bucket;
        // 容量は 2 の冪なので種類は高々十数個。線形探索で足りる。
        m_buckets.push_back(Bucket{ capacity, stride, 0, {} });
        return m_buckets.back();
    }

    std::vector<Bucket> m_buckets;
    std::uint64_t       m_frame        = UINT64_MAX;
    std::uint64_t       m_resetVersion = 0;
};

} // namespace fbzz::renderer
