/// @file    ScriptObjectFactory.cpp
/// @brief   スクリプトから GameObject 一式を組み立てる実装。
/// @author  Hasegawa Jin
/// @date    2026-08-19
#include <Editor/Util/ScriptObjectFactory.hpp>

/// @note AddRegisteredComponentByName (コライダーの自動フィット・RigidBody の既定質量) はここにしか無い。 重いヘッダーなので依存はこの TU に閉じる。
#include <Panels/Inspector/InspectorCommon.hpp>

#include <Editor/EditorContext.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>

#include <algorithm>
#include <string_view>

namespace fbzz::editor {

std::string ScriptObjectName(const std::string& scriptTypeName)
{
    std::string displayName = scene::ScriptFactory::DisplayName(scriptTypeName);
    if (const std::size_t separator = displayName.find_last_of('.'); separator != std::string::npos)
        displayName.erase(0, separator + 1);
    constexpr std::string_view kSuffix = "Component";
    if (displayName.size() > kSuffix.size() && displayName.ends_with(kSuffix))
        return displayName.substr(0, displayName.size() - kSuffix.size());
    return displayName;
}

std::vector<std::string> ScriptObjectTypeNames()
{
    std::vector<std::string> names = scene::ScriptFactory::RegisteredTypeNames();
    std::sort(names.begin(), names.end(), [](const std::string& left, const std::string& right) {
        const std::string leftName = scene::ScriptFactory::DisplayName(left);
        const std::string rightName = scene::ScriptFactory::DisplayName(right);
        return leftName == rightName ? left < right : leftName < rightName;
    });
    return names;
}

scene::GameObject* CreateScriptObject(EditorContext& ctx, const std::string& scriptTypeName)
{
    if (!ctx.activeScene || scriptTypeName.empty()) return nullptr;

    std::string typeKey;
    const std::vector<std::string> registered = scene::ScriptFactory::RegisteredTypeNames();
    if (std::find(registered.begin(), registered.end(), scriptTypeName) != registered.end())
        typeKey = scriptTypeName;
    else if (!scene::ScriptFactory::ResolveTypeName(scriptTypeName, typeKey)) return nullptr;
    scene::ScriptEntry entry;
    auto script = scene::ScriptFactory::Create(typeKey, entry.savedScriptId);
    if (!script) return nullptr;

    auto* created = ctx.activeScene->TryCreateGameObject(ScriptObjectName(typeKey));
    if (!created) return nullptr;
    scene::GameObject& go = *created;

    /// @note OnValidate は全部揃った後に回したいので、コンポーネント → スクリプトの順に固定する。
    for (const std::string& typeName : script->RequiredComponents())
        AddRegisteredComponentByName(go, typeName);

    auto* sc = go.GetComponent<scene::ScriptComponent>();
    if (!sc) sc = &go.AddComponent<scene::ScriptComponent>();

    script->SetContext(ctx.activeScene, &go);
    script->Reset();
    script->ExecuteProfiledCallback(&scene::Script::OnValidate, scene::ScriptCallbackKind::VALIDATE, "OnValidate");
    entry.script = std::move(script);
    sc->scripts.push_back(std::move(entry));
    return &go;
}

} /// @note namespace fbzz::editor
