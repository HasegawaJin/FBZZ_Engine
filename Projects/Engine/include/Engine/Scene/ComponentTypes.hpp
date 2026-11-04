/// @file    ComponentTypes.hpp
/// @brief   登録 Component の前方宣言とストレージへの型付き接続。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#pragma once
#include <type_traits>

namespace fbzz::scene {
class Scene;
template<typename T> class ComponentArray;
#define FBZZ_COMPONENT_ENTRY(Kind, Type, ...) Kind Type;
#define FBZZ_COMPONENT_FIRST FBZZ_COMPONENT_ENTRY
#include "ComponentRegistry.inl"
#undef FBZZ_COMPONENT_FIRST
#undef FBZZ_COMPONENT_ENTRY

namespace detail {
template<typename T> inline constexpr bool IS_REGISTERED_COMPONENT = false;
template<typename T> struct ComponentStorageAccess;
#define FBZZ_COMPONENT_ENTRY(Kind, Type, ...) \
template<> inline constexpr bool IS_REGISTERED_COMPONENT<Type> = true; \
template<> struct ComponentStorageAccess<Type> { \
    static FBZZ_ENGINE_API ComponentArray<Type>& Get(Scene& scene); \
    static FBZZ_ENGINE_API const ComponentArray<Type>& Get(const Scene& scene); \
};
#define FBZZ_COMPONENT_FIRST FBZZ_COMPONENT_ENTRY
#include "ComponentRegistry.inl"
#undef FBZZ_COMPONENT_FIRST
#undef FBZZ_COMPONENT_ENTRY
} /// @note namespace detail
} /// @note namespace fbzz::scene
