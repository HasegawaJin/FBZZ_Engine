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
    static constexpr std::size_t kMinCapacity = 1024; // 頂点数
    static ResourceHandle<BufferTag> Create(ResourceManager& resources, std::size_t capacity,
                                            std::uint32_t stride, Where where)
    {
        return resources.CreateVertexBuffer(nullptr, capacity * stride, stride, where);
    }
};

template <> struct DynamicBufferFactory<StructuredBufferTag> {
    // 力場・スポーン・ライト配列は数本〜数百本。頂点ほど大きく刻むと使わない領域が太る。
    static constexpr std::size_t kMinCapacity = 16; // 要素数
    static ResourceHandle<StructuredBufferTag> Create(ResourceManager& resources, std::size_t capacity,
                                                      std::uint32_t stride, Where where)
    {
        return resources.CreateStructuredBuffer(nullptr, static_cast<std::uint32_t>(capacity), stride, where);
    }
};

} // namespace detail

/// 「CPU で組み直した中身を 1 フレームに何度も Submit / Dispatch する」用途のバッファ貸出プール。
/// 頂点バッファ版 (DynamicVertexBufferPool) と読み取り専用 StructuredBuffer 版
/// (DynamicStructuredBufferPool) がある。書き換えるたびに Acquire で借り直すこと。
///
/// WHY 1 本を使い回してはいけないか:
///   DX12 の Submit はコマンドリストへの「記録」でしかなく、GPU が中身を読むのはフレーム終端の
///   実行時。DX12Buffer::Update と読み取り専用の DX12StructuredBuffer::Update は、永続 Map した
///   Upload ヒープの先頭へ memcpy するだけなので、1 フレーム内で同じバッファを 2 回更新すると、
///   先に記録した Draw まで後から書いた内容を読む。「エミッターごとに Update → Submit」
///   「パスごとに Flush」「Scene View と Game View がそれぞれ書く」のような書き方は、
///   2 個目以降が 1 個目を巻き添えにして壊す (線や板が別の形に化ける・ちらつく)。
///   撤去済みの DX11 では MAP_WRITE_DISCARD がバッファをリネームしていたため同じコードが
///   無害で、DX12 へ切り替えたときに初めて症状が出た経緯がある。
///
/// WHY フレームをまたいでも同じバッファを貸さないか (2026-09-11 に変更):
///   DX12 は GPU が CPU から最大 1 フレーム遅れて走る (DX12Context::FRAME_COUNT = 2。
///   BeginFrame が待つのは 2 フレーム前の完了だけ)。前のフレームで貸したバッファを
///   次のフレームで貸し直すと、GPU がまだ読んでいる中身を memcpy が上書きする。
///   貸出順が前フレームと同じなら中身もほぼ同じなので目立たないが、デバッグ表示の切り替えや
///   エミッターの増減で順番がずれた瞬間、前フレームの Draw が «別のパスの頂点» を読み、
///   Scene View / Game View に線や板のゴミが走る。VolumeFlipbookBaker の puff 輪番と同じく
///   フレームごとに貸出列を分け、書く列が必ず読み終わっているようにする。
///
/// WHY «貸出順» ではなく «容量» で仕分けるか (2026-09-02 に変更):
///   以前は貸出順の添字でスロットを引き当て、要求が入らなければそのスロットを作り直していた。
///   貸出順は毎フレーム変わる — パーティクルは視点からの距離で並び替わるし、エミッターは
///   湧いては消える。その結果 «一度でも大きな要求を受けた添字» が大きいまま居座り、
///   最後にはすべてのスロットが «そのフレームの最大» まで太る。1 本 92KB の板が
///   10 スロットぶん張り付く、という形で «じわじわ増えて減らない» ように見えていた。
///   容量クラスごとに列を分ければ、小さい要求は小さいバッファを借り続ける。
template <typename Tag>
class DynamicBufferPool {
    // WHY 先頭で宣言するか: コンストラクターの既定引数から引く。MSVC はクラステンプレートの
    //     既定引数で «後に宣言されるメンバー型» を解決できない (C2653)。
    using Factory = detail::DynamicBufferFactory<Tag>;

public:
    /// minCapacity: 容量クラスの下限 (要素数)。要求はこれ以上の 2 の冪へ切り上げる。
    /// UI の矩形のように 1 回が数頂点で本数が多い用途は小さく取る (本数 × 下限がそのまま常駐する)。
    explicit DynamicBufferPool(std::size_t minCapacity = Factory::kMinCapacity)
        : m_minCapacity(minCapacity == 0 ? 1 : minCapacity) {}

    /// stride バイトの要素を count 個ぶん収められるバッファを 1 つ貸し出す。
    /// 同じフレーム内で呼ぶたびに別のバッファを返し、直前 kFramesInFlight - 1 フレームに
    /// 貸したバッファも返さない。確保に失敗した場合は無効ハンドル。
    /// 返したバッファの持ち主はプール。借り手は Release しないこと。
    /// @note where は既定のまま渡すこと。バッファの発生位置が «この Acquire» ではなく
    ///       «どのパスのプールか» として記録される (Where の WHY を参照)。
    [[nodiscard]] ResourceHandle<Tag> Acquire(ResourceManager& resources,
                                              std::size_t count,
                                              std::uint32_t stride,
                                              Where where = Where::current())
    {
        if (count == 0 || stride == 0) return {};

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
            m_lane  = static_cast<std::size_t>(m_frame % kFramesInFlight);
            for (Bucket& bucket : m_buckets) bucket.next = 0;
        }

        // 2 の冪へ切り上げて確保し、要素数が少し増えるたびに再確保が走るのを避ける。
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

    /// 抱えているバッファをすべて返す。プールごと畳むとき (シャットダウン) に呼ぶ。
    void ReleaseAll(ResourceManager& resources)
    {
        for (Bucket& bucket : m_buckets)
            for (auto& lane : bucket.lanes)
                for (ResourceHandle<Tag> handle : lane)
                    if (handle.IsValid()) resources.Release(handle);
        m_buckets.clear();
    }

    /// 抱えている本数 (診断用)。
    [[nodiscard]] std::size_t GetBufferCount() const
    {
        std::size_t count = 0;
        for (const Bucket& bucket : m_buckets)
            for (const auto& lane : bucket.lanes) count += lane.size();
        return count;
    }

    /// 貸出列の本数。GPU が追いかけているフレーム数より 1 本多く持つ。
    /// WHY 2 本でなく 3 本か: Time::frameCount はレンダラーの GPU フレームと 1:1 ではない
    ///     (ウィンドウが隠れている間は BeginFrame が開かずに frameCount だけ進む)。
    ///     1 本余分に持てば、列の周期と GPU フレームがずれても書く列は読み終わっている。
    static constexpr std::size_t kFramesInFlight = 3;

private:
    // 同じ «容量 × stride» のバッファを並べた列。next はこのフレームで何本貸したか。
    struct Bucket {
        std::size_t   capacity = 0; // 要素数
        std::uint32_t stride   = 0;
        std::size_t   next     = 0;
        std::array<std::vector<ResourceHandle<Tag>>, kFramesInFlight> lanes;
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
    std::size_t         m_minCapacity  = Factory::kMinCapacity;
    std::uint64_t       m_frame        = UINT64_MAX;
    std::size_t         m_lane         = 0;
    std::uint64_t       m_resetVersion = 0;
};

using DynamicVertexBufferPool     = DynamicBufferPool<BufferTag>;
using DynamicStructuredBufferPool = DynamicBufferPool<StructuredBufferTag>;

} // namespace fbzz::renderer
