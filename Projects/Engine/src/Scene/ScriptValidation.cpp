// FBZZ Engine
// ScriptValidation.cpp | fbzz::scene
#include <Engine/Scene/ScriptValidation.hpp>

#include <Engine/Scene/ComponentOps.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptComponent.hpp>

namespace fbzz::scene {

namespace {

// 1 グループ (必須 or 任意) ぶんの走査。要求名を名前引きして、持っていなければ 1 件積む。
void CollectGroup(GameObject& go,
                  const Script& script,
                  std::span<const std::string> requirements,
                  bool optional,
                  std::vector<ScriptRequirementIssue>& out)
{
    for (const std::string& typeName : requirements) {
        if (typeName.empty()) continue;

        const ComponentOps* ops = FindComponentOps(typeName);
        // 登録済みで、かつ実際に付いているなら何も言うことはない。
        if (ops && ops->has && ops->has(go)) continue;

        ScriptRequirementIssue issue;
        issue.entity           = go.GetID();
        issue.objectName       = go.name;
        issue.instanceId       = go.instanceId;
        issue.scriptType       = script.GetTypeName();
        issue.componentType    = typeName;
        issue.componentDisplay = ops ? ops->displayName : typeName;
        issue.optional         = optional;
        issue.unknown          = (ops == nullptr);
        issue.addable          = ops && ops->addable;
        out.push_back(std::move(issue));
    }
}

} // namespace

void CollectScriptRequirementIssues(GameObject& go,
                                    const Script& script,
                                    std::vector<ScriptRequirementIssue>& out,
                                    bool includeOptional)
{
    CollectGroup(go, script, script.RequiredComponents(), false, out);
    if (includeOptional)
        CollectGroup(go, script, script.OptionalComponents(), true, out);
}

std::vector<ScriptRequirementIssue> ValidateSceneScriptRequirements(Scene& scene,
                                                                    bool includeOptional)
{
    std::vector<ScriptRequirementIssue> issues;

    for (EntityID id : scene.GetEntities<ScriptComponent>()) {
        auto* sc = scene.GetComponent<ScriptComponent>(id);
        auto* go = scene.GetGameObject(id);
        if (!sc || !go) continue;

        for (const ScriptEntry& entry : sc->scripts) {
            // 実体が無いもの (Missing Script) は要求を宣言しようがないので飛ばす。
            // その状態自体は Inspector が別途「Missing Script」として表示している。
            if (!entry.script) continue;
            CollectScriptRequirementIssues(*go, *entry.script, issues, includeOptional);
        }
    }

    return issues;
}

std::string FormatScriptRequirementIssue(const ScriptRequirementIssue& issue)
{
    std::string text = issue.objectName + " / " + issue.scriptType + ": ";
    if (issue.unknown) {
        // 宣言側の誤り。足しても直らないので、文面もそう読めるようにする。
        text += "unknown component type '" + issue.componentType +
                "' in FBZZ_REQUIRE_COMPONENT";
        return text;
    }
    text += (issue.optional ? "optional component '" : "requires '")
          + issue.componentDisplay + "'";
    if (!issue.optional) text += " but it is not attached";
    return text;
}

} // namespace fbzz::scene
