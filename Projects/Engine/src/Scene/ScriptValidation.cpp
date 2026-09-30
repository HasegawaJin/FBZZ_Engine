/// @file    ScriptValidation.cpp
/// @brief   スクリプトの必須 / 任意コンポーネント充足検査の実装。
/// @author  Hasegawa Jin
/// @date    2026-08-19
#include <Engine/Scene/ScriptValidation.hpp>

#include <Engine/Scene/ComponentOps.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/DataAsset.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>

namespace fbzz::scene {

namespace {

std::size_t CountScriptMatches(GameObject& go, const char* type)
{
    std::size_t matches = 0;
    if (const auto* component = go.GetComponent<ScriptComponent>())
        for (const auto& entry : component->scripts)
            if (entry.script && entry.script->FbzzAsType(type)) ++matches;
    return matches;
}

ScriptRequirementIssue MakeIssue(GameObject& go, const Script& script,
                                  ScriptRequirementKind kind, std::string key, std::string reason)
{
    ScriptRequirementIssue issue;
    issue.entity = go.GetID();
    issue.objectName = go.name;
    issue.instanceId = go.instanceId;
    issue.scriptType = script.GetTypeName();
    issue.kind = kind;
    issue.fieldKey = std::move(key);
    issue.reason = std::move(reason);
    issue.blocksStart = true;
    return issue;
}

/// @note Inspector はファイル存在とキャッシュ済み型だけを調べ、Play 前・実行時だけアセットを解決する。
class RequirementReflector final : public IReflector {
public:
    RequirementReflector(GameObject& go, const Script& script,
                         std::vector<ScriptRequirementIssue>& issues, bool resolveAssets)
        : m_go(go), m_script(script), m_issues(issues), m_resolveAssets(resolveAssets) {}
    void Field(const char*, float&) override {}
    void Field(const char*, int&) override {}
    void Field(const char*, bool&) override {}
    void Field(const char*, math::Vector2&) override {}
    void Field(const char*, math::Vector3&) override {}
    void Field(const char*, math::Vector4&) override {}
    void Field(const char*, math::Quaternion&) override {}
    void Field(const char*, std::string&) override {}
    void BeginObject(const char* name) override { m_scope.emplace_back(PersistentKey(name)); }
    void EndObject() override { if (!m_scope.empty()) m_scope.pop_back(); }
    std::size_t BeginObjectList(const char* name, std::size_t count) override
    { BeginObject(name); return count; }
    void BeginObjectElement(std::size_t index) override { m_scope.push_back(std::to_string(index)); }
    void EndObjectElement() override { EndObject(); }
    std::size_t EndObjectList() override { EndObject(); return NO_REMOVE; }

    void RequireAsset(const char* name, const DataAssetRef& reference) override
    {
        std::string reason;
        if (reference.path.empty()) reason = "required asset is not assigned";
        else {
            const auto path = asset::AssetManager::ResolveAssetPath(reference.path);
            if (path.empty() || !util::FileSystem::Exists(util::FileSystem::PathFromUtf8(path)))
                reason = "required asset file cannot be found: " + reference.path;
            else {
                std::string actualType = asset::DataAssetRegistry::TypeOf(reference.path);
                if (m_resolveAssets) {
                    const auto* value = asset::DataAssetRegistry::Resolve(reference.path);
                    if (!value) reason = "required asset cannot be loaded: " + reference.path;
                    else actualType = value->GetTypeName();
                }
                if (reason.empty() && !actualType.empty() && actualType != reference.type)
                    reason = "required asset expects '" + reference.type + "', got '" + actualType + "'";
            }
        }
        if (!reason.empty()) Add(ScriptRequirementKind::Asset, name, std::move(reason));
    }

    void RequireReference(const char* name, const EntityRef& reference, const char* type) override
    {
        auto* scene = m_go.GetScene();
        GameObject* target = scene ? reference.Resolve(*scene) : nullptr;
        if (!target) {
            Add(ScriptRequirementKind::Reference, name,
                reference.IsValid() ? "required reference target no longer exists" : "required reference is not assigned");
            return;
        }
        if (!type || !*type || std::string_view(type) == "GameObject") return;
        if (const auto* component = FindComponentOps(type)) {
            if (!component->has || !component->has(*target))
                Add(ScriptRequirementKind::Reference, name, "required reference lacks component '" + std::string(type) + "'");
            return;
        }
        const auto matches = CountScriptMatches(*target, type);
        if (matches != 1)
            Add(ScriptRequirementKind::Reference, name,
                "required reference expects one '" + std::string(type) + "', found " + std::to_string(matches));
    }

private:
    void Add(ScriptRequirementKind kind, const char* name, std::string reason)
    {
        std::string key;
        for (const auto& part : m_scope) key += part + ".";
        key += PersistentKey(name);
        m_issues.push_back(MakeIssue(m_go, m_script, kind, std::move(key), std::move(reason)));
    }
    GameObject& m_go;
    const Script& m_script;
    std::vector<ScriptRequirementIssue>& m_issues;
    bool m_resolveAssets;
    std::vector<std::string> m_scope;
};

/// @note  1 グループ (必須 or 任意) ぶんの走査。要求名を名前引きして、持っていなければ 1 件積む。
void CollectGroup(GameObject& go,
                  const Script& script,
                  std::span<const std::string> requirements,
                  bool optional,
                  std::vector<ScriptRequirementIssue>& out)
{
    for (const std::string& typeName : requirements) {
        if (typeName.empty()) continue;

        const ComponentOps* ops = FindComponentOps(typeName);
        /// @note 登録済みで、かつ実際に付いているなら何も言うことはない。
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

} /// @note namespace

void CollectScriptRequirementIssues(GameObject& go,
                                    const Script& script,
                                    std::vector<ScriptRequirementIssue>& out,
                                    bool includeOptional, bool resolveAssets)
{
    CollectGroup(go, script, script.RequiredComponents(), false, out);
    if (includeOptional)
        CollectGroup(go, script, script.OptionalComponents(), true, out);
    for (const auto& type : script.RequiredScripts()) {
        const auto matches = CountScriptMatches(go, type.c_str());
        if (matches != 1)
            out.push_back(MakeIssue(go, script, ScriptRequirementKind::Script, type,
                "requires one attached script '" + type + "', found " + std::to_string(matches)));
    }
    RequirementReflector reflector(go, script, out, resolveAssets);
    /// @note Reflect の既存 ABI は非 const。検証用 reflector は値・参照を書き換えず観測 getter も呼ばない。
    const_cast<Script&>(script).Reflect(reflector);
}

bool ValidateScriptRequirementsForStart(GameObject& go, Script& script)
{
    if (script.RequirementsBlocked()) return false;
    std::vector<ScriptRequirementIssue> issues;
    CollectScriptRequirementIssues(go, script, issues, false, true);
    bool blocked = false;
    for (const auto& issue : issues) {
        FBZZ_LOG_ERROR("Script requirement: %s", FormatScriptRequirementIssue(issue).c_str());
        blocked = blocked || issue.blocksStart;
    }
    if (blocked) script.BlockForMissingRequirements();
    return !blocked;
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
            /// @note 実体が無いもの (Missing Script) は要求を宣言しようがないので飛ばす。
            /// @note        その状態自体は Inspector が別途「Missing Script」として表示している。
            if (!entry.script) continue;
            CollectScriptRequirementIssues(*go, *entry.script, issues, includeOptional, true);
        }
    }

    return issues;
}

std::string FormatScriptRequirementIssue(const ScriptRequirementIssue& issue)
{
    std::string text = issue.objectName + " / " + issue.scriptType + ": ";
    if (issue.kind != ScriptRequirementKind::Component)
        return text + issue.fieldKey + ": " + issue.reason;
    if (issue.unknown) {
        /// @note 宣言側の誤り。足しても直らないので、文面もそう読めるようにする。
        text += "unknown component type '" + issue.componentType +
                "' in FBZZ_REQUIRE_COMPONENT";
        return text;
    }
    text += (issue.optional ? "optional component '" : "requires '")
          + issue.componentDisplay + "'";
    if (!issue.optional) text += " but it is not attached";
    return text;
}

} /// @note namespace fbzz::scene
