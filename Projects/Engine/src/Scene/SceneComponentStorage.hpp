/// @file    SceneComponentStorage.hpp
/// @brief   Scene が所有する未公開のコンポーネント配列。
/// @author  Hasegawa Jin
/// @date    2026-11-22
#pragma once

#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/Scene.hpp>
#include <tuple>

namespace fbzz::scene {

struct Scene::ComponentStorage {
    ComponentStorage();
    ~ComponentStorage();

    void Clear();
    void MoveFrom(ComponentStorage& source) noexcept;

    template<typename Tuple> struct Arrays;
    template<typename... Ts> struct Arrays<std::tuple<Ts...>> { using Type = std::tuple<ComponentArray<Ts>...>; };
    typename Arrays<ComponentList>::Type m_arrays;
};

}
