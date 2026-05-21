// FBZZ Engine
// ScriptFactory.cpp | fbzz::scene
// Script type name to factory registry implementation
#include <engine/Scene/ScriptFactory.hpp>
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

} // namespace fbzz::scene
