/// @file    ComponentArray.hpp
/// @brief   EntityID から Component へ引くスパースセット。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// dense 配列で連続メモリを保ち、System の走査を高速化する。
/// Remove は末尾要素との swap で O(1) にする。
///
/// WHY Get() が assert で、«無ければ nullptr» を返す口を持たないか:
///   持っている前提で書ける場所と、持っているか分からない場所は別のコード。
///   ここは前者 ── System が Data() / Entities() で «実際に持っているものだけ» を
///   走査するための容器で、Has() を確かめずに Get() を呼ぶのは呼び出し側の誤りになる。
///   後者 (参照が生きているか分からない) の入口は Ref<T> と GameObject::GetComponent<T>()
///   で、どちらも nullptr を返して呼び出し側に判断させる。
///   ここへ nullable な取得口を足すと «同じことをする 2 つの道» ができ、
///   どちらを使うべきかがコードから読めなくなる。
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
        m_sparseToIndex[id.index] = di; // sparse[entity.index] → dense position
    }

    void Remove(EntityID id) {
        assert(Has(id));
        uint32_t di   = m_sparseToIndex[id.index];
        uint32_t last = m_count - 1;

        // 末尾要素を削除位置に swap して穴を埋める。O(1) だが順序は保たない。
        if (di != last) {
            m_dense[di]         = std::move(m_dense[last]);
            m_denseToEntity[di] = m_denseToEntity[last];
            m_sparseToIndex[m_denseToEntity[last].index] = di; // swap した要素の sparse を更新
        }

        m_sparseToIndex[id.index] = EMPTY;
        --m_count;
    }

    bool Has(EntityID id) const {
        if (m_sparseToIndex == nullptr) return false; // 未使用の型は確保すらしていない
        if (!id.IsValid() || id.index >= MAX) return false;
        uint32_t di = m_sparseToIndex[id.index];
        if (di == EMPTY || di >= m_count) return false;
        // generation を含む EntityID 全体で比較し、同じ index に再割り当てされた別 Entity を弾く
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

    // dense/sparse を初回 Add まで確保しない。
    // WHY: 以前は T m_dense[MAX] をインラインで持っていたため、ComponentArray 1 個だけで
    //      sizeof(T) * 4096 を占め、それを全コンポーネント型ぶん束ねる Scene が
    //      数十〜数百 MB の巨大オブジェクトになっていた。静的な Scene
    //      (エディタの GameObject クリップボード) がマップ範囲を越えて
    //      構築時にアクセス違反を起こす原因でもあった。
    //      遅延確保にすると、実際に使われる数種類ぶんしかメモリを取らない。
    //      確保後は決して再確保しないため、Get() が返す参照は従来どおり安定する。
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

    // 確保済みバッファごと引き取る。要素単位のコピーが不要になり move も安くなる。
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

} // namespace fbzz::scene
