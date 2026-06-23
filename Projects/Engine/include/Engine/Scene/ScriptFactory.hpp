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

    // DLL ホットリロード用: レジストリを全クリアする。
    // WHY: スクリプト DLL をアンロードする前に呼び、古い型の factory (DLL へのポインタを含む)
    //      が残らないようにする。DLL ロード時の静的初期化子が再登録する。
    static void UnregisterAll();
};

} // namespace fbzz::scene

// スクリプト型を ScriptFactory に静的登録するマクロ。
// WHY: main.cpp に RegisterXxxScripts() を置くと、スクリプト追加のたびに起動コードを触る必要がある。
//      各ゲーム側 Translation Unit に登録を置くことで、Script の所有者が登録責務も持てる。
#define FBZZ_SCRIPT_FACTORY_CONCAT_INNER(a, b) a##b
#define FBZZ_SCRIPT_FACTORY_CONCAT(a, b) FBZZ_SCRIPT_FACTORY_CONCAT_INNER(a, b)
#define FBZZ_REGISTER_SCRIPT(T) \
    namespace { \
        [[maybe_unused]] const bool FBZZ_SCRIPT_FACTORY_CONCAT(s_fbzzScriptRegistered_, __COUNTER__) = []() { \
            ::fbzz::scene::ScriptFactory::Register<T>(); \
            return true; \
        }(); \
    }
