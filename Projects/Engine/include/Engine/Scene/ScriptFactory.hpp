// FBZZ Engine
// ScriptFactory.hpp | fbzz::scene
// スクリプト型名から生成関数を引くレジストリ
// SceneSerializer が保存名から Script を復元するために使う。
// 登録は明示的に行い、未登録型は生成失敗として扱う。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace fbzz::scene {

class ScriptFactory {
public:
    using Factory = std::function<std::unique_ptr<Script>()>;

    template<typename T>
    static bool Register()
    {
        static_assert(std::is_base_of_v<Script, T>);
        return Register(T::TYPE_NAME, []() { return std::make_unique<T>(); });
    }

    static bool Register(const std::string& typeName, Factory factory);
    static std::unique_ptr<Script> Create(const std::string& typeName);
    static std::vector<std::string> RegisteredTypeNames();
};

} // namespace fbzz::scene
