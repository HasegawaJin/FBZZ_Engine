/// @file    RenderCandidateLease.hpp
/// @brief   描画抽出の候補 Entity を保存するスレッド内作業領域。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <cstddef>
#include <memory>
#include <vector>

namespace fbzz::scene::detail {

/// @note 同一スレッドで取得と返却をスタック順に行う。再入時も外側の候補配列を移動せず、返却時は容量を保持する。
class RenderCandidateLease {
public:
    RenderCandidateLease() : m_pool(PoolForThread())
    {
        if (m_pool.depth == m_pool.lists.size())
            m_pool.lists.push_back(std::make_unique<std::vector<EntityID>>());
        m_entities = m_pool.lists[m_pool.depth++].get();
    }

    ~RenderCandidateLease()
    {
        m_entities->clear();
        --m_pool.depth;
    }

    RenderCandidateLease(const RenderCandidateLease&) = delete;
    RenderCandidateLease& operator=(const RenderCandidateLease&) = delete;

    /// @return リース終了まで有効な配列。候補の列挙順は呼び出し側が決める。
    std::vector<EntityID>& Entities() { return *m_entities; }

private:
    struct Pool {
        std::vector<std::unique_ptr<std::vector<EntityID>>> lists;
        std::size_t depth = 0;
    };

    static Pool& PoolForThread()
    {
        static thread_local Pool pool;
        return pool;
    }

    Pool& m_pool;
    std::vector<EntityID>* m_entities = nullptr;
};

}
