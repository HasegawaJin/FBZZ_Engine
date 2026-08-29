/// @file    ComponentOps.cpp
/// @brief   ComponentRegistry を畳んで名前引きテーブルを 1 度だけ構築する。
/// @author  Hasegawa Jin
/// @date    2026-08-19
#include <Engine/Scene/ComponentOps.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>

#include <algorithm>
#include <vector>

namespace fbzz::scene {

namespace {

// WHY 関数内 static か: ComponentRegistry の畳み込みは型ごとの関数ポインタ生成を伴う。
//     名前空間スコープの静的初期化に置くと、他 TU の静的初期化順序に依存しうる。
//     初回アクセス時に一度だけ組み立てれば順序問題が起きない。
const std::vector<ComponentOps>& BuildTable()
{
    static const std::vector<ComponentOps> table = [] {
        std::vector<ComponentOps> entries;
        ForEachRegisteredComponent([&]<typename T, typename Registration>() {
            entries.push_back(ComponentOps{
                Registration::serializedName,
                Registration::displayName,
                Registration::category,
                Registration::addable,
                +[](GameObject& go) -> bool { return go.GetComponent<T>() != nullptr; },
            });
        });
        return entries;
    }();
    return table;
}

} // namespace

std::span<const ComponentOps> ComponentOpsTable()
{
    const auto& table = BuildTable();
    return { table.data(), table.size() };
}

const ComponentOps* FindComponentOps(std::string_view typeName)
{
    if (typeName.empty()) return nullptr;

    const auto& table = BuildTable();
    const auto it = std::find_if(table.begin(), table.end(),
        [typeName](const ComponentOps& ops) { return typeName == ops.typeName; });
    return it == table.end() ? nullptr : &*it;
}

} // namespace fbzz::scene
