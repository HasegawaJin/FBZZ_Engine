/// @file    OrderingHints.hpp
/// @brief   同 Phase 内の実行順序制約を型安全に宣言する。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <typeindex>
#include <vector>

namespace fbzz {

struct OrderingHints {
    std::vector<std::type_index> after;
    std::vector<std::type_index> before;

    template<typename T> OrderingHints& After()  { after.push_back(typeid(T));  return *this; }
    template<typename T> OrderingHints& Before() { before.push_back(typeid(T)); return *this; }
};

} // namespace fbzz
