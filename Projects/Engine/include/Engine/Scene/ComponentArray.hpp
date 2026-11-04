/// @file    ComponentArray.hpp
/// @brief   EntityID から Component へ引くスパースセット。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note dense 要素は小さな連続 chunk に置き、追加で既存 Component のアドレスを変えない。Remove は末尾との swap で O(1)。
/// @note Entities の Span は追加で失効し得る。削除は swap される要素の参照先を変えるため EntityID から再取得する。
#pragma once
#include <Engine/Api.hpp>
#include "Entity.hpp"
#include <span>
#include <cassert>
#include <memory>
#include <utility>
#include <algorithm>
#include <vector>
#include <type_traits>
#include <cstddef>

namespace fbzz::scene {

/// @note Engine 本体で一元発行する。型・Scene・DLL を越えて再利用しない非ゼロの寿命・構造識別値。
[[nodiscard]] FBZZ_ENGINE_API uint64_t AllocateComponentLifetime();

/// @note chunk directory を全 Component 型で共有し、型ごとの STL export 増加による import library の枯渇を防ぐ。
/// @see https://learn.microsoft.com/en-us/cpp/error-messages/tool-errors/linker-tools-error-lnk1189 LNK1189 の library member 数制限。
class ComponentChunkStorage {
public:
    using Allocation = std::unique_ptr<void, void (*)(void*)>;
    FBZZ_ENGINE_API ComponentChunkStorage();
    FBZZ_ENGINE_API ~ComponentChunkStorage();
    ComponentChunkStorage(const ComponentChunkStorage&) = delete;
    ComponentChunkStorage& operator=(const ComponentChunkStorage&) = delete;
    FBZZ_ENGINE_API ComponentChunkStorage(ComponentChunkStorage&& other) noexcept;
    FBZZ_ENGINE_API ComponentChunkStorage& operator=(ComponentChunkStorage&& other) noexcept;
    /// @note 成功後だけ caller の typed owner を release する。directory 拡張の失敗時は caller が所有を維持する。
    FBZZ_ENGINE_API void Add(void* memory, void (*deleter)(void*));
    FBZZ_ENGINE_API void Clear();
    FBZZ_ENGINE_API size_t Count() const;
    FBZZ_ENGINE_API void* Get(size_t index) const;
private:
    std::vector<Allocation> m_chunks;
};

template<typename T>
class ComponentArray {
public:
    /// @note EntityID の invalid sentinel による数値表現限界。固定確保量や制作上の個体数上限ではない。
    static constexpr uint32_t MAX = EntityID::INVALID_INDEX;

    /// @note 固定長 Span と同じ対象数を保持し、chunk の境界を越えて Component の参照を返す。
    template<bool IS_CONST>
    class DataView {
        using Owner = std::conditional_t<IS_CONST, const ComponentArray, ComponentArray>;
        using Reference = std::conditional_t<IS_CONST, const T&, T&>;
        using Pointer = std::conditional_t<IS_CONST, const T*, T*>;
    public:
        struct Iterator {
            Owner* m_owner;
            uint32_t m_index;
            uint32_t m_count;
            Pointer m_chunk;
            Iterator(Owner* owner, uint32_t index, uint32_t count)
                : m_owner(owner), m_index(index), m_count(count),
                  m_chunk(index < count ? &owner->DenseAt(index - index % CHUNK_ELEMENTS) : nullptr) {}
            Reference operator*() const { return m_chunk[m_index % CHUNK_ELEMENTS]; }
            Iterator& operator++() {
                ++m_index;
                if (m_index < m_count && m_index % CHUNK_ELEMENTS == 0)
                    m_chunk = &m_owner->DenseAt(m_index);
                return *this;
            }
            bool operator==(const Iterator& other) const {
                return m_owner == other.m_owner && m_index == other.m_index;
            }
        };
        DataView(Owner* owner, uint32_t count) : m_owner(owner), m_count(count) {}
        Iterator begin() const { return {m_owner, 0, m_count}; }
        Iterator end() const { return {m_owner, m_count, m_count}; }
        size_t size() const { return m_count; }
        bool empty() const { return m_count == 0; }
        Reference operator[](size_t index) const { assert(index < m_count); return m_owner->DenseAt(index); }
    private:
        Owner* m_owner;
        uint32_t m_count;
    };

    ComponentArray() = default;
    ComponentArray(const ComponentArray&) = delete;
    ComponentArray& operator=(const ComponentArray&) = delete;
    ComponentArray(ComponentArray&& other) noexcept { MoveFrom(std::move(other)); }
    ComponentArray& operator=(ComponentArray&& other) noexcept {
        if (this != &other) MoveFrom(std::move(other));
        return *this;
    }

    void Add(EntityID id, T component) {
        assert(id.IsValid() && id.index < MAX);
        assert(m_count < MAX);
        EnsureStorage(id.index);
        assert(!Has(id));

        uint32_t di = m_count;
        DenseAt(di) = std::move(component);
        m_denseToEntity.push_back(id);
        m_lifetimes.push_back(AllocateComponentLifetime());
        m_sparseToIndex[id.index] = di;
        ++m_count;
        m_structureRevision = m_lifetimes.back();
    }

    void Remove(EntityID id) {
        assert(Has(id));
        uint32_t di   = m_sparseToIndex[id.index];
        uint32_t last = m_count - 1;

        /// @note 末尾要素を削除位置に swap して穴を埋める。O(1) だが順序は保たない。
        if (di != last) {
            DenseAt(di) = std::move(DenseAt(last));
            m_denseToEntity[di] = m_denseToEntity[last];
            m_lifetimes[di] = m_lifetimes[last];
            m_sparseToIndex[m_denseToEntity[last].index] = di;
        }

        /// @note 論理削除だけでは末尾が所有する Script や音声の寿命が次の Add まで残る。
        DenseAt(last) = {};
        m_denseToEntity.pop_back();
        m_lifetimes.pop_back();
        m_sparseToIndex[id.index] = EMPTY;
        --m_count;
        m_structureRevision = AllocateComponentLifetime();
    }

    bool Has(EntityID id) const {
        if (!id.IsValid() || id.index >= m_sparseToIndex.size()) return false;
        uint32_t di = m_sparseToIndex[id.index];
        if (di == EMPTY || di >= m_count) return false;
        /// @note generation を含む EntityID 全体で比較し、同じ index に再割り当てされた別 Entity を弾く。
        return m_denseToEntity[di] == id;
    }

    /// @return 未登録・削除済みなら 0。dense の拡張・swap と配列の move では個体の寿命を保持する。
    [[nodiscard]] uint64_t GetLifetime(EntityID id) const {
        return Has(id) ? m_lifetimes[m_sparseToIndex[id.index]] : 0;
    }

    /// @pre Has(id) が true であること。
    /// @note Remove の末尾 swap で添字は変わり得る。
    [[nodiscard]] uint32_t GetDenseIndex(EntityID id) const {
        assert(Has(id));
        return m_sparseToIndex[id.index];
    }

    T& Get(EntityID id) {
        assert(Has(id));
        return DenseAt(m_sparseToIndex[id.index]);
    }

    const T& Get(EntityID id) const {
        assert(Has(id));
        return DenseAt(m_sparseToIndex[id.index]);
    }

    DataView<false> Data() { return {this, m_count}; }
    DataView<true> Data() const { return {this, m_count}; }
    std::span<const EntityID> Entities() const { return {m_denseToEntity.data(), m_count}; }

    /// @note Entity と Component の span は同じ dense 順。末尾 chunk は実要素だけを含む。
    /// @pre callback 中に追加・削除・Clear・move を行わない。
    template<typename Fn>
    void ForEachChunk(Fn&& fn) {
        for (size_t offset = 0; offset < m_count; offset += CHUNK_ELEMENTS) {
            const size_t count = std::min(CHUNK_ELEMENTS, static_cast<size_t>(m_count) - offset);
            fn(std::span<const EntityID>{m_denseToEntity.data() + offset, count},
                std::span<T>{&DenseAt(offset), count});
        }
    }

    /// @note Entity と Component の span は同じ dense 順。末尾 chunk は実要素だけを含む。
    /// @pre callback 中に追加・削除・Clear・move を行わない。
    template<typename Fn>
    void ForEachChunk(Fn&& fn) const {
        for (size_t offset = 0; offset < m_count; offset += CHUNK_ELEMENTS) {
            const size_t count = std::min(CHUNK_ELEMENTS, static_cast<size_t>(m_count) - offset);
            fn(std::span<const EntityID>{m_denseToEntity.data() + offset, count},
                std::span<const T>{&DenseAt(offset), count});
        }
    }

    uint32_t Count() const { return m_count; }

    /// @note dense ID 列の構造識別値。値の直接編集では変わらない。0 は未変更または move 後の空配列だけに使う。
    [[nodiscard]] uint64_t GetStructureRevision() const { return m_structureRevision; }

    void Clear() {
        const bool changed = m_count != 0;
        for (uint32_t i = 0; i < m_count; ++i) {
            DenseAt(i) = {};
        }
        m_denseToEntity.clear();
        m_lifetimes.clear();
        ResetSparse();
        m_count = 0;
        if (changed) m_structureRevision = AllocateComponentLifetime();
    }

private:
    static constexpr uint32_t EMPTY = 0xFFFFFFFFu;
    /// @note 確保の粒度だけを定める。必要な chunk を追加でき、Component 件数を制限しない。
    static constexpr size_t CHUNK_ELEMENTS = 64;

    T& DenseAt(size_t index) { return static_cast<T*>(m_chunks.Get(index / CHUNK_ELEMENTS))[index % CHUNK_ELEMENTS]; }
    const T& DenseAt(size_t index) const { return static_cast<const T*>(m_chunks.Get(index / CHUNK_ELEMENTS))[index % CHUNK_ELEMENTS]; }

    /// @note 型を消した所有権を元の配列 owner に戻し、全要素の destructor と配列解放を実行する。
    static void ReleaseChunk(void* memory) { std::unique_ptr<T[]> owner(static_cast<T*>(memory)); }

    /// @note dense は実要素数、sparse は到達した Entity index に応じて確保し、未使用型は確保しない。
    void EnsureStorage(uint32_t entityIndex) {
        const size_t sparseSize = static_cast<size_t>(entityIndex) + 1;
        if (m_sparseToIndex.size() < sparseSize) m_sparseToIndex.resize(sparseSize, EMPTY);
        if (m_count / CHUNK_ELEMENTS >= m_chunks.Count()) {
            auto chunk = std::make_unique<T[]>(CHUNK_ELEMENTS);
            m_chunks.Add(chunk.get(), &ReleaseChunk);
            chunk.release();
        }
    }

    void ResetSparse() {
        std::fill(m_sparseToIndex.begin(), m_sparseToIndex.end(), EMPTY);
    }

    /// @brief 確保済みバッファごと引き取る。要素単位のコピーが不要になり move も安くなる。
    void MoveFrom(ComponentArray&& other) {
        m_chunks = std::move(other.m_chunks);
        m_denseToEntity = std::move(other.m_denseToEntity);
        m_lifetimes = std::move(other.m_lifetimes);
        m_sparseToIndex = std::move(other.m_sparseToIndex);
        m_count         = other.m_count;
        m_structureRevision = other.m_structureRevision;
        other.m_count   = 0;
        other.m_structureRevision = 0;
        other.m_chunks.Clear();
        other.m_denseToEntity.clear();
        other.m_lifetimes.clear();
        other.m_sparseToIndex.clear();
    }

    ComponentChunkStorage m_chunks;
    std::vector<EntityID> m_denseToEntity;
    std::vector<uint64_t> m_lifetimes;
    std::vector<uint32_t> m_sparseToIndex;
    uint32_t m_count = 0;
    uint64_t m_structureRevision = 0;
};

} /// @note namespace fbzz::scene
