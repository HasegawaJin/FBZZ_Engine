// FBZZ Engine
// ScriptSerializableFactory.cpp | fbzz::scene
// SerializeReference相当のネスト型factory
#include <Engine/Scene/Script.hpp>

#include <algorithm>
#include <unordered_map>

namespace fbzz::scene {
namespace {

std::unordered_map<std::string, ScriptSerializableFactory::Factory>& Registry()
{
    static std::unordered_map<std::string, ScriptSerializableFactory::Factory> registry;
    return registry;
}

} // namespace

bool ScriptSerializableFactory::Register(std::string_view typeName, Factory factory)
{
    if (typeName.empty() || !factory) return false;
    Registry()[std::string(typeName)] = std::move(factory);
    return true;
}

std::unique_ptr<IScriptSerializable> ScriptSerializableFactory::Create(std::string_view typeName)
{
    const auto it = Registry().find(std::string(typeName));
    return it != Registry().end() ? it->second() : nullptr;
}

std::vector<std::string> ScriptSerializableFactory::RegisteredTypeNames()
{
    std::vector<std::string> names;
    names.reserve(Registry().size());
    for (const auto& [name, factory] : Registry())
        if (!name.empty() && factory) names.push_back(name);
    std::sort(names.begin(), names.end());
    return names;
}

void ScriptSerializableFactory::UnregisterAll()
{
    Registry().clear();
}

bool ScriptSerializedReference::SetType(std::string_view typeName)
{
    if (type == typeName && value) return true;
    std::unique_ptr<IScriptSerializable> replacement =
        ScriptSerializableFactory::Create(typeName);
    if (!replacement) return false;
    type = typeName;
    value = std::move(replacement);
    preservedFieldsToml.clear();
    return true;
}

void ScriptSerializedReference::Clear()
{
    type.clear();
    value.reset();
    preservedFieldsToml.clear();
}

} // namespace fbzz::scene
