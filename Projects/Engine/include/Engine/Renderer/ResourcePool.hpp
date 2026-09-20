/// @file    ResourcePool.hpp
/// @brief   世代番号付きリソーススロットプール。
/// @author  Hasegawa Jin
/// @date    2026-05-22
///
/// 任意のリソース型を slot / generation で管理する内部コンテナ。
/// 削除された slot を再利用しても古いハンドルが通らないようにする。
#pragma once
#include <Engine/Core/Memory/MemoryDebug.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace fbzz::renderer {

template<typename T, typename Tag>
class ResourcePool {
public:
    /// bytes は「この実体が占める GPU メモリ」。0 は «計上しない» を意味する
    /// (PipelineState のような状態オブジェクトや、サイズを取り出せないバックエンド)。
    ResourceHandle<Tag> Insert(std::unique_ptr<T> resource,
                               std::size_t bytes,
                               const char* debugName = "ResourcePool",
                               const char* file = "Unknown",
                               int line = 0)
    {
        if (!resource) return ResourceHandle<Tag>::Null();

        /// @note ResourcePool は GPU リソースの唯一の所有者なので、ここで記録すれば各呼び出し元へ
        ///       侵襲せず «返し忘れ» を見つけられる。追跡枠が尽きても (MAX_DEBUG_ALLOCATIONS)
        ///       プールの機能自体は成立するため戻り値は捨てる。T は IBuffer などの
        ///       インターフェース型で sizeof(T) は vptr 数バイトしかなく、
        ///       実体の GPU メモリとは無関係なので bytes を別引数で受け取る。
        static_cast<void>(m_debug.Track(
            MakeAllocationInfo(resource.get(), bytes, debugName, file, line)));

        uint32_t id = 0;
        if (!m_freeList.empty()) {
            id = m_freeList.back();
            m_freeList.pop_back();
        } else {
            id = static_cast<uint32_t>(m_slots.size());
            m_slots.push_back({});
        }

        Slot& slot = m_slots[id];
        slot.resource = std::move(resource);
        slot.occupied = true;
        return { id, slot.gen };
    }

    T* Get(ResourceHandle<Tag> handle)
    {
        if (!IsLive(handle)) return nullptr;
        return m_slots[handle.id].resource.get();
    }

    const T* Get(ResourceHandle<Tag> handle) const
    {
        if (!IsLive(handle)) return nullptr;
        return m_slots[handle.id].resource.get();
    }

    /// HLSL ホットリロード用: 既存スロットのリソースを新しいものに差し替える。
    /// @note Remove → Insert すると generation が上がり既存ハンドルが無効になる。Replace は
    ///       generation を維持したまま中身だけ入れ替えるため、シェーダーを参照する
    ///       Material / PipelineState を更新せずにホットスワップできる。
    void Replace(ResourceHandle<Tag> handle, std::unique_ptr<T> resource, std::size_t bytes = 0)
    {
        if (!IsLive(handle) || !resource) return;
        Slot& slot = m_slots[handle.id];
        static_cast<void>(m_debug.Untrack(slot.resource.get()));
        static_cast<void>(m_debug.Track(
            MakeAllocationInfo(resource.get(), bytes, "ResourceReload", __FILE__, __LINE__)));
        slot.resource = std::move(resource);
    }

    /// @brief スロットから実体を取り出して返す。スロットは Remove と同じく世代を進めて空ける。
    /// @note 別スロットで作った実体を既存ハンドルへ移す (Replace へ渡す) ときに使う。
    std::unique_ptr<T> Take(ResourceHandle<Tag> handle)
    {
        if (!IsLive(handle)) return nullptr;
        Slot& slot = m_slots[handle.id];
        static_cast<void>(m_debug.Untrack(slot.resource.get()));
        std::unique_ptr<T> taken = std::move(slot.resource);
        slot.occupied = false;
        slot.gen = (slot.gen == (std::numeric_limits<uint32_t>::max)()) ? 1u : slot.gen + 1u;
        m_freeList.push_back(handle.id);
        return taken;
    }

    void Remove(ResourceHandle<Tag> handle)
    {
        if (!IsLive(handle)) return;

        Slot& slot = m_slots[handle.id];
        static_cast<void>(m_debug.Untrack(slot.resource.get()));
        slot.resource.reset();
        slot.occupied = false;
        /// @note generation を進めて、同じ id を再利用しても古いハンドルが IsLive を通過しないようにする。
        ///       0 に戻すと ResourceHandle のデフォルト値 (gen=0) と衝突するため 1 に巻き戻す。
        slot.gen = (slot.gen == (std::numeric_limits<uint32_t>::max)()) ? 1u : slot.gen + 1u;
        m_freeList.push_back(handle.id);
    }

    [[nodiscard]] std::size_t GetLiveDebugCount() const
    {
        return m_debug.GetLiveCount();
    }

    void CollectLiveDebugInfo(std::vector<core::AllocationInfo>& out) const
    {
        m_debug.CollectLive(out);
    }

    [[nodiscard]] std::size_t GetLiveDebugBytes() const
    {
        return m_debug.GetLiveBytes();
    }

    void ReleaseOwnedForShutdown()
    {
        /// @note Remove() と同じく Untrack → reset の順。ここを通ったスロットは «意図して
        ///       返した» ものなので終了時のログには残らない。残っているものだけが
        ///       «誰も返さなかった» リソース。
        m_freeList.clear();
        for (uint32_t id = 0; id < static_cast<uint32_t>(m_slots.size()); ++id) {
            Slot& slot = m_slots[id];
            if (slot.occupied) {
                static_cast<void>(m_debug.Untrack(slot.resource.get()));
                slot.resource.reset();
                slot.occupied = false;
                slot.gen = (slot.gen == (std::numeric_limits<uint32_t>::max)()) ? 1u : slot.gen + 1u;
                if (id != 0) {
                    m_freeList.push_back(id);
                }
            }
        }
    }

private:
    static core::AllocationInfo MakeAllocationInfo(const T* pointer,
                                                   std::size_t bytes,
                                                   const char* debugName,
                                                   const char* file,
                                                   int line)
    {
        core::AllocationInfo info;
        info.pointer       = const_cast<T*>(pointer);
        info.size          = bytes;
        info.alignment     = alignof(T);
        info.tag           = core::MemoryTag::RENDERER;
        info.allocatorName = debugName;
        info.file          = file;
        info.line          = line;
        return info;
    }

    struct Slot {
        std::unique_ptr<T> resource;
        uint32_t gen = 1; ///< 初期値を 1 にし、ResourceHandle デフォルトの gen=0 とは絶対に一致しない
        bool occupied = false;
    };

    [[nodiscard]] bool IsLive(ResourceHandle<Tag> handle) const
    {
        if (!handle.IsValid()) return false;
        if (handle.id >= m_slots.size()) return false;
        const Slot& slot = m_slots[handle.id];
        return slot.occupied && slot.gen == handle.gen;
    }

    /// id = 0 は «無効ハンドル» 用の番兵。中身は常に空のまま。
    /// @note Slot は unique_ptr を持つのでコピーできない (初期化子リストは要素をコピーする)。
    ///       そのため初期化子リストではなく個数指定で 1 つ値初期化する。
    std::vector<Slot> m_slots = std::vector<Slot>(1);
    std::vector<uint32_t> m_freeList;
    core::MemoryDebug m_debug;
};

} // namespace fbzz::renderer
