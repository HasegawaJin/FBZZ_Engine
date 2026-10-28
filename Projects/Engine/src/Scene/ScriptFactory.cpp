/// @file    ScriptFactory.cpp
/// @brief   Script 型名から生成関数を引くレジストリ実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note SceneSerializer が保存名から Script を復元する。未登録型は生成失敗として扱う。
#include <Engine/Scene/ScriptFactory.hpp>
#include <algorithm>
#include <unordered_map>
#include <utility>

namespace fbzz::scene {

namespace {

struct Registration {
    ScriptFactory::IdentityFactory factory;
    std::string displayName;
};

std::unordered_map<std::string, Registration>& Registry()
{
    static std::unordered_map<std::string, Registration> registry;
    return registry;
}

struct Alias { std::string typeKey; };
std::unordered_map<std::string, Alias>& Aliases()
{
    static std::unordered_map<std::string, Alias> aliases;
    return aliases;
}

std::unordered_map<std::string, std::string>& DisplayNames()
{
    static std::unordered_map<std::string, std::string> names;
    return names;
}

bool ResolveRegisteredTypeName(std::string_view name, std::string& typeKey)
{
    std::string match;
    for (const auto& [key, registration] : Registry()) {
        const size_t separator = registration.displayName.find_last_of('.');
        const std::string_view shortName(registration.displayName.data() + (separator == std::string::npos ? 0 : separator + 1),
            registration.displayName.size() - (separator == std::string::npos ? 0 : separator + 1));
        if (name != key && name != registration.displayName && name != shortName) continue;
        if (!match.empty() && match != key) return false;
        match = key;
    }
    if (match.empty()) {
        const auto alias = Aliases().find(std::string(name));
        if (alias != Aliases().end()) {
            const auto target = Registry().find(alias->second.typeKey);
            if (target != Registry().end()) match = alias->second.typeKey;
        }
    }
    if (match.empty()) return false;
    typeKey = std::move(match);
    return true;
}

} /// @note namespace

bool ScriptFactory::RegisterManaged(const std::string& typeName, IdentityFactory factory)
{
    if (typeName.empty() || !factory) return false;
    auto& registry = Registry();
    registry.insert_or_assign(typeName, Registration{std::move(factory), typeName});
    DisplayNames().try_emplace(typeName, typeName);
    return true;
}

std::unique_ptr<Script> ScriptFactory::Create(const std::string& typeName, const std::string& savedScriptId)
{
    if (!IsValidSavedScriptId(savedScriptId)) return nullptr;
    auto& registry = Registry();
    auto it = registry.find(typeName);
    if (it == registry.end()) {
        const auto alias = Aliases().find(typeName);
        if (alias != Aliases().end()) it = registry.find(alias->second.typeKey);
    }
    if (it == registry.end()) return nullptr;
    return it->second.factory(savedScriptId);
}

bool ScriptFactory::CanCreate(std::string_view typeName)
{
    const std::string key(typeName);
    const auto& registry = Registry();
    if (registry.contains(key)) return true;
    const auto alias = Aliases().find(key);
    return alias != Aliases().end() && registry.contains(alias->second.typeKey);
}

bool ScriptFactory::IsValidSavedScriptId(std::string_view savedScriptId)
{
    if (savedScriptId.size() != 36) return false;
    bool nonzero = false;
    for (size_t index = 0; index < savedScriptId.size(); ++index) {
        const char value = savedScriptId[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value != '-') return false;
        } else {
            if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) return false;
            nonzero = nonzero || value != '0';
        }
    }
    return nonzero;
}

void ScriptFactory::UnregisterManaged()
{
    Registry().clear();
    Aliases().clear();
}

bool ScriptFactory::SetDisplayName(const std::string& typeName, std::string displayName)
{
    if (typeName.empty() || displayName.empty()) return false;
    DisplayNames().insert_or_assign(typeName, displayName);
    const auto found = Registry().find(typeName);
    if (found != Registry().end()) found->second.displayName = std::move(displayName);
    return true;
}

std::string ScriptFactory::DisplayName(const std::string& typeName)
{
    const auto found = DisplayNames().find(typeName);
    return found == DisplayNames().end() ? typeName : found->second;
}

bool ScriptFactory::ResolveTypeName(std::string_view name, std::string& typeKey)
{
    return ResolveRegisteredTypeName(name, typeKey);
}

bool ScriptFactory::RegisterAlias(const std::string& alias, const std::string& typeKey)
{
    const auto target = Registry().find(typeKey);
    if (alias.empty() || target == Registry().end()) return false;
    if (alias == typeKey) return true;
    if (Registry().contains(alias)) return false;
    const auto existing = Aliases().find(alias);
    if (existing != Aliases().end()) return false;
    Aliases().insert_or_assign(alias, Alias{typeKey});
    return true;
}

std::vector<std::string> ScriptFactory::RegisteredTypeNames()
{
    std::vector<std::string> names;
    auto& registry = Registry();
    names.reserve(registry.size());
    for (const auto& [typeName, registration] : registry) {
        if (!typeName.empty() && registration.factory)
            names.push_back(typeName);
    }
    std::sort(names.begin(), names.end());
    return names;
}

} /// @note namespace fbzz::scene
