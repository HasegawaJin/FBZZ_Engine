// FBZZ Engine
// ScriptFactory.cpp | fbzz::scene
// Script 型名から生成関数を引くレジストリ実装
// SceneSerializer が文字列名からユーザースクリプトを復元するために使う。
// 登録されていない型は生成失敗として扱う。
#include <Engine/Scene/ScriptFactory.hpp>
#include <algorithm>
#include <unordered_map>
#include <utility>

namespace fbzz::scene {

namespace {

std::unordered_map<std::string, ScriptFactory::Factory>& Registry()
{
    static std::unordered_map<std::string, ScriptFactory::Factory> registry;
    return registry;
}

} // namespace

bool ScriptFactory::Register(const std::string& typeName, Factory factory)
{
    if (typeName.empty() || !factory) return false;
    Registry()[typeName] = std::move(factory);
    return true;
}

std::unique_ptr<Script> ScriptFactory::Create(const std::string& typeName)
{
    auto& registry = Registry();
    auto it = registry.find(typeName);
    if (it == registry.end()) return nullptr;
    return it->second();
}

void ScriptFactory::UnregisterAll()
{
    Registry().clear();
}

std::vector<std::string> ScriptFactory::RegisteredTypeNames()
{
    std::vector<std::string> names;
    auto& registry = Registry();
    names.reserve(registry.size());
    for (const auto& [typeName, factory] : registry) {
        if (!typeName.empty() && factory)
            names.push_back(typeName);
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace fbzz::scene
