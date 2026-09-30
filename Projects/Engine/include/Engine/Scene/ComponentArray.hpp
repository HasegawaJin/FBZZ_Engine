/// @file    ComponentArray.hpp
/// @brief   EntityID から Component へ引くスパースセット。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// @note dense 配列で連続メモリを保ち、System の走査を高速化する。Remove は末尾要素との swap で O(1)。
/// @note Get() は assert で失敗し nullable な取得口は持たない。ここは「持っている前提で書ける」System 走査専用の容器で、Has() を確かめず呼ぶのは呼び出し側の誤り。参照が生きているか分からない場面は `Ref<T>` / GameObject::GetComponent<T>() が別に nullptr を返す入口を持つ。
#pragma once
#include "Entity.hpp"
#include <span>
#include <cassert>
#include <memory>
#include <utility>

namespace fbzz::scene {

template<typename T>
class ComponentArray {
public:
    static constexpr uint32_t MAX = 4096;

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
        EnsureStorage();
        assert(!Has(id));

        uint32_t di = m_count++;
        m_dense[di]               = std::move(component);
        m_denseToEntity[di]       = id;
        /// @note sparse[entity.index] を dense position へ書く。
        m_sparseToIndex[id.index] = di;
    }

    void Remove(EntityID id) {
        assert(Has(id));
        uint32_t di   = m_sparseToIndex[id.index];
        uint32_t last = m_count - 1;

        /// @note 末尾要素を削除位置に swap して穴を埋める。O(1) だが順序は保たない。
        if (di != last) {
            m_dense[di]         = std::move(m_dense[last]);
            m_denseToEntity[di] = m_denseToEntity[last];
            /// @note swap した要素の sparse を更新する。
            m_sparseToIndex[m_denseToEntity[last].index] = di;
        }

        /// @note 論理削除だけでは末尾が所有する Script や音声の寿命が次の Add まで残る。
        m_dense[last] = {};
        m_denseToEntity[last] = {};
        m_sparseToIndex[id.index] = EMPTY;
        --m_count;
    }

    bool Has(EntityID id) const {
        /// @note 未使用の型は確保すらしていない。
        if (m_sparseToIndex == nullptr) return false;
        if (!id.IsValid() || id.index >= MAX) return false;
        uint32_t di = m_sparseToIndex[id.index];
        if (di == EMPTY || di >= m_count) return false;
        /// @note generation を含む EntityID 全体で比較し、同じ index に再割り当てされた別 Entity を弾く。
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

    std::span<T>              Data()     { return { m_dense.get(), m_count }; }
    std::span<const T>        Data()     const { return { m_dense.get(), m_count }; }
    std::span<const EntityID> Entities() const { return { m_denseToEntity.get(), m_count }; }

    uint32_t Count() const { return m_count; }

    void Clear() {
        if (m_dense == nullptr) { m_count = 0; return; }
        for (uint32_t i = 0; i < m_count; ++i) {
            m_dense[i] = {};
            m_denseToEntity[i] = {};
        }
        ResetSparse();
        m_count = 0;
    }

private:
    static constexpr uint32_t EMPTY = 0xFFFFFFFFu;

    /// @brief dense/sparse を初回 Add まで確保しない。
    /// @note 以前はインライン `T m_dense[MAX]` を持ち、型ごとに sizeof(T)*4096 を占め、全型分を束ねる Scene が数十〜数百 MB になり、エディタの GameObject クリップボードがマップ範囲超過で構築時にアクセス違反を起こしていた。
    /// @note 遅延確保で実際に使う型分しかメモリを取らず、確保後は再確保しないため Get() の参照は安定したまま。
    void EnsureStorage() {
        if (m_dense != nullptr) return;
        m_dense          = std::make_unique<T[]>(MAX);
        m_denseToEntity  = std::make_unique<EntityID[]>(MAX);
        m_sparseToIndex  = std::make_unique<uint32_t[]>(MAX);
        ResetSparse();
    }

    void ResetSparse() {
        for (uint32_t i = 0; i < MAX; ++i) m_sparseToIndex[i] = EMPTY;
    }

    /// @brief 確保済みバッファごと引き取る。要素単位のコピーが不要になり move も安くなる。
    void MoveFrom(ComponentArray&& other) {
        m_dense         = std::move(other.m_dense);
        m_denseToEntity = std::move(other.m_denseToEntity);
        m_sparseToIndex = std::move(other.m_sparseToIndex);
        m_count         = other.m_count;
        other.m_count   = 0;
    }

    std::unique_ptr<T[]>        m_dense;
    std::unique_ptr<EntityID[]> m_denseToEntity;
    std::unique_ptr<uint32_t[]> m_sparseToIndex;
    uint32_t m_count = 0;
};

} /// @note namespace fbzz::scene
