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
    // unique_ptr overload — converts to shared_ptr internally so MemoryDebug can track via weak_ptr.
    // bytes は「この実体が占める GPU メモリ」。0 は «計上しない» を意味する
    // (PipelineState のような状態オブジェクトや、サイズを取り出せないバックエンド)。
    ResourceHandle<Tag> Insert(std::unique_ptr<T> resource,
                               std::size_t bytes,
                               const char* debugName = "ResourcePool",
                               const char* file = "Unknown",
                               int line = 0)
    {
        return Insert(std::shared_ptr<T>(std::move(resource)), bytes, debugName, file, line);
    }

    ResourceHandle<Tag> Insert(std::shared_ptr<T> resource,
                               std::size_t bytes,
                               const char* debugName = "ResourcePool",
                               const char* file = "Unknown",
                               int line = 0)
    {
        if (!resource) return ResourceHandle<Tag>::Null();

        // WHY: ResourcePool は GPU リソースの実所有者なので、ここで追跡すれば各呼び出し元へ侵襲せず解放漏れを見つけられる。
        // WHY: 追跡枠が尽きても (MAX_DEBUG_ALLOCATIONS) プールの機能自体は成立するため戻り値は捨てる。
        // WHY sizeof(T) を使わないか: T は IBuffer などのインターフェース型で、
        //     その大きさは vptr 数バイト。実体の GPU メモリとは何の関係もない。
        static_cast<void>(m_debug.TrackShared(resource,
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

    // HLSL ホットリロード用: 既存スロットのリソースを新しいものに差し替える。
    // WHY: Remove → Insert すると world generation が上がり既存ハンドルが無効になる。
    //      Replace は generation を維持したまま中身だけ入れ替えるため、
    //      シェーダーを参照する Material / PipelineState を更新せずにホットスワップできる。
    void Replace(ResourceHandle<Tag> handle, std::unique_ptr<T> resource, std::size_t bytes = 0)
    {
        Replace(handle, std::shared_ptr<T>(std::move(resource)), bytes);
    }

    void Replace(ResourceHandle<Tag> handle, std::shared_ptr<T> resource, std::size_t bytes = 0)
    {
        if (!IsLive(handle) || !resource) return;
        Slot& slot = m_slots[handle.id];
        static_cast<void>(m_debug.Untrack(slot.resource.get()));
        static_cast<void>(m_debug.TrackShared(
            resource, MakeAllocationInfo(resource.get(), bytes, "ResourceReload", __FILE__, __LINE__)));
        slot.resource = std::move(resource);
    }

    void Remove(ResourceHandle<Tag> handle)
    {
        if (!IsLive(handle)) return;

        Slot& slot = m_slots[handle.id];
        static_cast<void>(m_debug.Untrack(slot.resource.get()));
        slot.resource.reset();
        slot.occupied = false;
        // generation を進めて、同じ id を再利用しても古いハンドルが IsLive を通過しないようにする。
        // 0 に戻すと ResourceHandle のデフォルト値 (gen=0) と衝突するため 1 に巻き戻す。
        slot.gen = (slot.gen == (std::numeric_limits<uint32_t>::max)()) ? 1u : slot.gen + 1u;
        m_freeList.push_back(handle.id);
    }

    [[nodiscard]] std::size_t GetLiveDebugCount() const
    {
        return m_debug.GetLiveCount();
    }

    [[nodiscard]] const core::AllocationInfo* GetLiveDebugInfo(std::size_t index) const
    {
        return m_debug.GetLive(index);
    }

    void ReleaseOwnedForShutdown()
    {
        // WHY: Remove() と同様に Untrack → reset の順で処理する。
        //      reset() で shared_ptr が解放されても weak_ptr が期限切れになるのは
        //      use_count が 0 になった瞬間であり、それは reset() の呼び出し完了後。
        //      Untrack を先に呼ぶことで「プールが意図的に解放したスロット」は
        //      LogLiveDebugResources に現れなくなり、true な外部保持のみを検出できる。
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
        m_debug.SweepExpired();
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
        std::shared_ptr<T> resource;
        uint32_t gen = 1; // 初期値を 1 にし、ResourceHandle デフォルトの gen=0 とは絶対に一致しない
        bool occupied = false;
    };

    [[nodiscard]] bool IsLive(ResourceHandle<Tag> handle) const
    {
        if (!handle.IsValid()) return false;
        if (handle.id >= m_slots.size()) return false;
        const Slot& slot = m_slots[handle.id];
        return slot.occupied && slot.gen == handle.gen;
    }

    std::vector<Slot> m_slots = { Slot{} };
    std::vector<uint32_t> m_freeList;
    core::MemoryDebug m_debug;
};

} // namespace fbzz::renderer
