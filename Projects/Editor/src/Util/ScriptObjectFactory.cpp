/// @file    ScriptObjectFactory.cpp
/// @brief   スクリプトから GameObject 一式を組み立てる実装。
/// @author  Hasegawa Jin
/// @date    2026-08-19
#include <Editor/Util/ScriptObjectFactory.hpp>

/// @note AddRegisteredComponentByName (コライダーの自動フィット・RigidBody の既定質量) はここにしか無い。
///       重いヘッダーなので依存はこの TU に閉じる。
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
    constexpr std::string_view kSuffix = "Component";
    if (scriptTypeName.size() > kSuffix.size() && scriptTypeName.ends_with(kSuffix))
        return scriptTypeName.substr(0, scriptTypeName.size() - kSuffix.size());
    return scriptTypeName;
}

std::vector<std::string> ScriptObjectTypeNames()
{
    std::vector<std::string> names = scene::ScriptFactory::RegisteredTypeNames();
    std::sort(names.begin(), names.end());
    return names;
}

scene::GameObject* CreateScriptObject(EditorContext& ctx, const std::string& scriptTypeName)
{
    if (!ctx.activeScene || scriptTypeName.empty()) return nullptr;

    auto script = scene::ScriptFactory::Create(scriptTypeName);
    if (!script) return nullptr;

    scene::GameObject& go = ctx.activeScene->CreateGameObject(ScriptObjectName(scriptTypeName));

    /// @note OnValidate は全部揃った後に回したいので、コンポーネント → スクリプトの順に固定する。
    for (const std::string& typeName : script->RequiredComponents())
        AddRegisteredComponentByName(go, typeName);

    auto* sc = go.GetComponent<scene::ScriptComponent>();
    if (!sc) sc = &go.AddComponent<scene::ScriptComponent>();

    script->SetContext(ctx.activeScene, &go);
    script->Reset();
    script->OnValidate();
    sc->scripts.emplace_back().script = std::move(script);
    return &go;
}

} // namespace fbzz::editor
