/// @file    ComponentAccess.hpp
/// @brief   System が読み書きする Component 型の宣言。Build() 時に競合検出と並列バッチ構築に使う。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <typeindex>
#include <vector>

namespace fbzz {

struct ComponentAccess {
    std::vector<std::type_index> reads;
    std::vector<std::type_index> writes;
    bool                         unrestricted = false;

    template<typename... Ts>
    ComponentAccess& Reads()        { (reads.push_back(typeid(Ts)),  ...); return *this; }

    template<typename... Ts>
    ComponentAccess& Writes()       { (writes.push_back(typeid(Ts)), ...); return *this; }

    ComponentAccess& Unrestricted() { unrestricted = true; return *this; }
};

} // namespace fbzz
