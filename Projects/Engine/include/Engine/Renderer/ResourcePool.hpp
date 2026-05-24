// FBZZ Engine
// ResourcePool.hpp | fbzz::renderer
// 世代番号付きリソーススロットプール
// 任意のリソース型を slot / generation で管理する内部コンテナ。
// 削除された slot を再利用しても古いハンドルが通らないようにする。
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace fbzz::renderer {

template<typename T, typename Tag>
class ResourcePool {
public:
    ResourceHandle<Tag> Insert(std::shared_ptr<T> resource)
    {
        if (!resource) return ResourceHandle<Tag>::Null();

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

    void Remove(ResourceHandle<Tag> handle)
    {
        if (!IsLive(handle)) return;

        Slot& slot = m_slots[handle.id];
        slot.resource.reset();
        slot.occupied = false;
        slot.gen = (slot.gen == (std::numeric_limits<uint32_t>::max)()) ? 1u : slot.gen + 1u;
        m_freeList.push_back(handle.id);
    }

private:
    struct Slot {
        std::shared_ptr<T> resource;
        uint32_t gen = 1;
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
};

} // namespace fbzz::renderer
