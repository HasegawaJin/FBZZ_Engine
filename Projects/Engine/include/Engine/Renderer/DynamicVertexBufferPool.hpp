/// @file DynamicVertexBufferPool.hpp
/// @brief 1 フレーム内で何度も書き換える動的頂点バッファの貸出プール。
/// @author Hasegawa Jin
/// @date 2026-08-22
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
/// 使い方: Update → Submit の 1 セットごとに Acquire() を呼び、返ったハンドルへ書いて描く。
///   フレームが変わると貸出カーソルが先頭へ戻り、確保済みバッファをそのまま再利用する。
class DynamicVertexBufferPool {
public:
    /// stride バイトの頂点を vertexCount 個ぶん収められるバッファを 1 つ貸し出す。
    /// 同じフレーム内で呼ぶたびに別のバッファを返す。確保に失敗した場合は無効ハンドル。
    [[nodiscard]] ResourceHandle<BufferTag> Acquire(ResourceManager& resources,
                                                    std::size_t vertexCount,
                                                    std::uint32_t stride)
    {
        if (vertexCount == 0 || stride == 0) return {};

        // ResourceManager::Reset() 後は旧ハンドルが無効なので、Release せずキャッシュだけ捨てる。
        if (m_resetVersion != resources.GetResetVersion()) {
            m_resetVersion = resources.GetResetVersion();
            m_slots.clear();
            m_next = 0;
        }
        // WHY パス単位ではなくフレーム単位で巻き戻すか: エディタは 1 フレームで Scene View と
        //     Game View を続けて描くため、パス単位で戻すと 2 つのビューが同じバッファを共有し、
        //     プールを分けた意味が無くなる。
        if (m_frame != Time::frameCount) {
            m_frame = Time::frameCount;
            m_next  = 0;
        }

        if (m_next >= m_slots.size()) m_slots.emplace_back();
        Slot& slot = m_slots[m_next++];

        if (slot.capacity < vertexCount || slot.stride != stride) {
            if (slot.handle.IsValid()) resources.Release(slot.handle);
            // 2 の冪へ切り上げて確保し、頂点数が少し増えるたびに再確保が走るのを避ける。
            std::size_t capacity = kMinCapacity;
            while (capacity < vertexCount) capacity *= 2;
            slot.handle   = resources.CreateVertexBuffer(
                nullptr, capacity * stride, stride);
            slot.capacity = slot.handle.IsValid() ? capacity : 0;
            slot.stride   = slot.handle.IsValid() ? stride : 0;
        }
        return slot.handle;
    }

private:
    static constexpr std::size_t kMinCapacity = 1024; // 頂点数

    struct Slot {
        ResourceHandle<BufferTag> handle;
        std::size_t               capacity = 0; // 頂点数
        std::uint32_t             stride   = 0;
    };

    std::vector<Slot> m_slots;
    std::size_t       m_next         = 0;
    std::uint64_t     m_frame        = UINT64_MAX;
    std::uint64_t     m_resetVersion = 0;
};

} // namespace fbzz::renderer
