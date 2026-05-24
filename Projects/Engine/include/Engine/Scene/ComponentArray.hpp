// FBZZ Engine
// ComponentArray.hpp | fbzz::scene
// EntityID から Component へ引くスパースセット
// dense 配列で連続メモリを保ち、System の走査を高速化する。
// Remove は末尾要素との swap で O(1) にする。
#pragma once
#include "Entity.hpp"
#include <span>
#include <cassert>
#include <utility>

namespace fbzz::scene {

template<typename T>
class ComponentArray {
public:
    static constexpr uint32_t MAX = 4096;

    ComponentArray() { ResetSparse(); }
    ComponentArray(const ComponentArray&) = delete;
    ComponentArray& operator=(const ComponentArray&) = delete;
    ComponentArray(ComponentArray&& other) noexcept { MoveFrom(std::move(other)); }
    ComponentArray& operator=(ComponentArray&& other) noexcept {
        if (this != &other) MoveFrom(std::move(other));
        return *this;
    }

    void Add(EntityID id, T component) {
        assert(id.IsValid() && id.index < MAX);
        assert(!Has(id));
        assert(m_count < MAX);

        uint32_t di = m_count++;
        m_dense[di]         = std::move(component);
        m_denseToEntity[di] = id;
        m_sparseToIndex[id.index] = di;
    }

    void Remove(EntityID id) {
        assert(Has(id));
        uint32_t di   = m_sparseToIndex[id.index];
        uint32_t last = m_count - 1;

        if (di != last) {
            m_dense[di]         = std::move(m_dense[last]);
            m_denseToEntity[di] = m_denseToEntity[last];
            m_sparseToIndex[m_denseToEntity[last].index] = di;
        }

        m_sparseToIndex[id.index] = EMPTY;
        --m_count;
    }

    bool Has(EntityID id) const {
        if (!id.IsValid() || id.index >= MAX) return false;
        uint32_t di = m_sparseToIndex[id.index];
        if (di == EMPTY || di >= m_count) return false;
        return m_denseToEntity[di] == id;
    }

    T& Get(EntityID id) {
        assert(Has(id));
        return m_dense[m_sparseToIndex[id.index]];
    }

    const T& Get(EntityID id) const {
        assert(Has(id));
        return m_dense[m_sparseToIndex[id.index]];
    }

    std::span<T>              Data()     { return { m_dense, m_count }; }
    std::span<const T>        Data()     const { return { m_dense, m_count }; }
    std::span<const EntityID> Entities() const { return { m_denseToEntity, m_count }; }

    uint32_t Count() const { return m_count; }

    void Clear() {
        for (uint32_t i = 0; i < m_count; ++i) {
            m_dense[i] = {};
            m_denseToEntity[i] = {};
        }
        ResetSparse();
        m_count = 0;
    }

private:
    static constexpr uint32_t EMPTY = 0xFFFFFFFFu;

    void ResetSparse() {
        for (uint32_t i = 0; i < MAX; ++i) m_sparseToIndex[i] = EMPTY;
    }

    void MoveFrom(ComponentArray&& other) {
        Clear();
        m_count = other.m_count;
        for (uint32_t i = 0; i < m_count; ++i) {
            m_dense[i] = std::move(other.m_dense[i]);
            m_denseToEntity[i] = other.m_denseToEntity[i];
            m_sparseToIndex[m_denseToEntity[i].index] = i;
        }
        other.Clear();
    }

    T        m_dense[MAX]         = {};
    EntityID m_denseToEntity[MAX] = {};
    uint32_t m_sparseToIndex[MAX];
    uint32_t m_count = 0;
};

} // namespace fbzz::scene
