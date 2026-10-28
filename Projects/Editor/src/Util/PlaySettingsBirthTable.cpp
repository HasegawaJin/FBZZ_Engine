/// @file    PlaySettingsBirthTable.cpp
/// @brief   Adoption references cross scenes only through captured birth correspondence, never fresh GUID lookup.
/// @author  Hasegawa Jin
/// @date    2026-10-05
#include <Editor/Util/PlaySettingsBirthTable.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <algorithm>
#include <charconv>
#include <unordered_set>
namespace fbzz::editor {
namespace {
bool Failed(std::string& diagnostic, const std::string& message) { diagnostic = message; return false; }
std::string Text(const OpData& value, std::string_view key) { const auto* item = value.Find(key); return item && item->IsString() ? item->AsString() : std::string{}; }
bool Token(const OpData& value, std::string_view key, uint64_t expected)
{
    const std::string text = Text(value, key); uint64_t number = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && number == expected && text == std::to_string(number);
}
scene::ManagedScriptAdapter* Script(scene::Scene& sourceScene, scene::EntityID entity, const std::string& savedId)
{
    auto* owner = sourceScene.GetGameObject(entity); auto* component = owner ? owner->GetComponent<scene::ScriptComponent>() : nullptr;
    if (component) for (auto& entry : component->scripts) if (entry.savedScriptId == savedId && !entry.m_pendingRemoval)
        return scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
    return nullptr;
}
} /// @note namespace
const PlaySettingsBirthTable::Owner* PlaySettingsBirthTable::Find(const std::string& guid) const
{
    const auto found = std::find_if(m_owners.begin(), m_owners.end(), [&guid](const auto& owner) { return owner.guid == guid; });
    return found == m_owners.end() ? nullptr : &*found;
}
bool PlaySettingsBirthTable::Capture(scene::Scene& edit, scene::Scene& play, std::string& diagnostic)
{
    PlaySettingsBirthTable next; next.m_editLifetime = edit.GetRenderSceneGeneration(); next.m_playLifetime = play.GetRenderSceneGeneration();
    auto& runtime = scene::ManagedScriptRuntime::Instance(); std::vector<scene::GameObject*> pending = edit.GetRootGameObjects();
    std::unordered_set<scene::GameObject*> visited; std::unordered_set<std::string> guids;
    while (!pending.empty()) {
        auto* original = pending.back(); pending.pop_back();
        if (!original || !visited.insert(original).second || !guids.insert(original->instanceId).second)
            return Failed(diagnostic, "Invalid initial Edit hierarchy or duplicate owner GUID.");
        auto* clone = play.FindByGuid(original->instanceId); if (!clone) return Failed(diagnostic, "The initial Play clone omitted an Edit owner.");
        Owner owner; owner.guid = original->instanceId; owner.editEntity = original->GetID(); owner.playEntity = clone->GetID();
        owner.editObjectId = runtime.ReferenceObjectId(edit, owner.editEntity); owner.playObjectId = runtime.ReferenceObjectId(play, owner.playEntity);
        if (!owner.editObjectId || !owner.playObjectId) return Failed(diagnostic, "Initial owner reference tokens are unavailable.");
        for (uint32_t kind = 0; kind < owner.editComponents.size(); ++kind) {
            owner.editComponents[kind] = runtime.ReferenceComponentLifetime(edit, owner.editEntity, kind);
            owner.playComponents[kind] = runtime.ReferenceComponentLifetime(play, owner.playEntity, kind);
        }
        if (const auto* component = original->GetComponent<scene::ScriptComponent>())
            for (const auto& entry : component->scripts) {
                auto* originalScript = runtime.FindAdapter(entry.script.get()); if (!originalScript) continue;
                auto* cloneScript = Script(play, owner.playEntity, entry.savedScriptId);
                if (!cloneScript || std::string(originalScript->GetTypeName()) != cloneScript->GetTypeName()) return Failed(diagnostic, "The initial Play Script identity differs from Edit.");
                owner.scripts.push_back({entry.savedScriptId, originalScript->GetTypeName(), originalScript->ReferenceBirth(), cloneScript->ReferenceBirth()});
            }
        next.m_owners.push_back(std::move(owner));
        for (int index = 0; index < original->GetChildCount(); ++index) pending.push_back(original->GetChild(index));
    }
    *this = std::move(next); diagnostic.clear(); return true;
}
bool PlaySettingsBirthTable::LiveOwners(scene::Scene& edit, scene::Scene& play, const Owner& owner, std::string& diagnostic) const
{
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    if (edit.GetRenderSceneGeneration() != m_editLifetime || play.GetRenderSceneGeneration() != m_playLifetime ||
        runtime.ReferenceObjectId(edit, owner.editEntity) != owner.editObjectId || runtime.ReferenceObjectId(play, owner.playEntity) != owner.playObjectId)
        return Failed(diagnostic, "ADOPTION_BIRTH_CHANGED: an original or initial Play owner was removed or recreated.");
    return true;
}
bool PlaySettingsBirthTable::ValidateSource(scene::Scene& edit, scene::Scene& play, const std::string& ownerGuid,
    const std::string& savedScriptId, scene::ManagedScriptAdapter*& destination, std::string& diagnostic) const
{
    const auto* owner = Find(ownerGuid); if (!owner || !LiveOwners(edit, play, *owner, diagnostic)) return Failed(diagnostic, "ADOPTION_BIRTH_CHANGED: the selected Script did not survive from the initial clone.");
    const auto found = std::find_if(owner->scripts.begin(), owner->scripts.end(), [&savedScriptId](const auto& script) { return script.savedId == savedScriptId; });
    auto* before = Script(edit, owner->editEntity, savedScriptId); auto* current = Script(play, owner->playEntity, savedScriptId);
    if (found == owner->scripts.end() || !before || !current || before->ReferenceBirth() != found->editBirth || current->ReferenceBirth() != found->playBirth ||
        std::string(before->GetTypeName()) != found->type || std::string(current->GetTypeName()) != found->type)
        return Failed(diagnostic, "ADOPTION_BIRTH_CHANGED: the Script was added/readded or its saved type changed.");
    destination = before; return true;
}
bool PlaySettingsBirthTable::MapReferences(scene::Scene& edit, scene::Scene& play, const OpData& references, OpData& mappings, std::string& diagnostic) const
{
    if (!references.IsObject()) return Failed(diagnostic, "Adoption reference identities require a path table.");
    OpData result = OpData::MakeObject(); auto& runtime = scene::ManagedScriptRuntime::Instance();
    for (const auto& [path, source] : references.AsObject()) {
        const std::string guid = Text(source, "guid"), kind = Text(source, "kind");
        if (guid.empty() && kind.empty() && Token(source, "objectId", 0) && Token(source, "sceneLifetime", 0)) continue;
        const auto* owner = Find(guid); if (!owner || !LiveOwners(edit, play, *owner, diagnostic) || !Token(source, "objectId", owner->playObjectId) || !Token(source, "sceneLifetime", m_playLifetime))
            return Failed(diagnostic, "ADOPTION_REFERENCE_EXPIRED: a selected reference is not the initial surviving Play object at " + path);
        OpData target = source; target.Set("objectId", std::to_string(owner->editObjectId)); target.Set("sceneLifetime", std::to_string(m_editLifetime));
        if (kind == "native") {
            const OpData* specified = source.Find("componentKind"); int componentKind = -1;
            if (!specified || !specified->TryGetInt32(componentKind) || componentKind < 0 || componentKind >= static_cast<int>(owner->editComponents.size())) return Failed(diagnostic, "Unsupported native reference kind at " + path);
            const uint64_t before = owner->editComponents[componentKind], current = owner->playComponents[componentKind];
            if (!before || !current || runtime.ReferenceComponentLifetime(edit, owner->editEntity, static_cast<uint32_t>(componentKind)) != before || runtime.ReferenceComponentLifetime(play, owner->playEntity, static_cast<uint32_t>(componentKind)) != current || !Token(source, "componentLifetime", current))
                return Failed(diagnostic, "ADOPTION_REFERENCE_EXPIRED: a Component was removed/readded at " + path);
            target.Set("componentLifetime", std::to_string(before));
        } else if (kind == "script") {
            const std::string saved = Text(source, "savedScriptId"); scene::ManagedScriptAdapter* destination = nullptr;
            if (!ValidateSource(edit, play, guid, saved, destination, diagnostic)) return false;
            auto* current = Script(play, owner->playEntity, saved);
            if (!current || Text(source, "scriptBirth") != current->ReferenceBirth() || !Token(source, "generation", current->Generation()) || !Token(source, "scriptInstanceId", current->InstanceId()))
                return Failed(diagnostic, "ADOPTION_REFERENCE_EXPIRED: the held Script reference is not its current exact instance at " + path);
            target.Set("scriptBirth", destination->ReferenceBirth()); target.Set("generation", std::to_string(destination->Generation())); target.Set("scriptInstanceId", std::to_string(destination->InstanceId()));
        } else if (!kind.empty()) return Failed(diagnostic, "Unsupported exact reference kind at " + path);
        OpData mapping = OpData::MakeObject(); mapping.Set("source", source); mapping.Set("target", std::move(target)); result.Set(path, std::move(mapping));
    }
    mappings = std::move(result); diagnostic.clear(); return true;
}
bool PlaySettingsBirthTable::ValidateDestination(scene::Scene& edit, const std::string& ownerGuid, const std::string& savedScriptId,
    scene::ManagedScriptAdapter*& destination, std::string& diagnostic) const
{
    const auto* owner = Find(ownerGuid); auto& runtime = scene::ManagedScriptRuntime::Instance();
    if (!owner || edit.GetRenderSceneGeneration() != m_editLifetime || runtime.ReferenceObjectId(edit, owner->editEntity) != owner->editObjectId)
        return Failed(diagnostic, "ADOPTION_BIRTH_CHANGED: the original owner was removed or recreated.");
    const auto expected = std::find_if(owner->scripts.begin(), owner->scripts.end(), [&savedScriptId](const auto& script) { return script.savedId == savedScriptId; });
    auto* current = Script(edit, owner->editEntity, savedScriptId);
    if (expected == owner->scripts.end() || !current || current->ReferenceBirth() != expected->editBirth || std::string(current->GetTypeName()) != expected->type)
        return Failed(diagnostic, "ADOPTION_BIRTH_CHANGED: the original Script was removed or recreated.");
    destination = current; return true;
}
} /// @note namespace fbzz::editor
