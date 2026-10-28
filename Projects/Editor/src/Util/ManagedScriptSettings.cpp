/// @file    ManagedScriptSettings.cpp
/// @brief   Inspector と AI の C# 設定を同じ個体照合・検証・Undo へ接続する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <Editor/Util/ManagedScriptSettings.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/Scene.hpp>

namespace fbzz::editor {
namespace {
bool EnabledArgs(EditorContext& context, const std::vector<ManagedSettingsTarget>& selection, OpArgs& args)
{
    if (selection.empty()) return false;
    OpData targets = OpData::MakeArray();
    for (const auto& target : selection) {
        if (!context.activeScene || context.activeScene->GetRenderSceneGeneration() != target.sceneLifetime ||
            scene::ManagedScriptRuntime::Instance().GetAppliedGeneration() != target.generation) return false;
        OpData item = OpData::MakeObject(); item.Set("node", target.objectId); item.Set("scriptId", target.scriptId);
        targets.Push(std::move(item));
    }
    args.Set("targets", std::move(targets));
    return true;
}
} /// @note namespace

ManagedScriptEnabledState ReadManagedInspectorEnabled(EditorContext& context, const std::vector<ManagedSettingsTarget>& targets)
{
    ManagedScriptEnabledState state;
    OpArgs args;
    if (!EnabledArgs(context, targets, args)) { state.diagnostic = "選択対象の Scene または C# 世代が変わりました。"; return state; }
    const auto result = InvokeOperator(context, "script.enabled_status", args);
    if (!result.ok) { state.diagnostic = result.message; return state; }
    if (const auto* value = result.data.Find("enabled"); value && value->IsBool()) state.enabled = value->AsBool();
    if (const auto* value = result.data.Find("mixed"); value && value->IsBool()) state.mixed = value->AsBool();
    if (const auto* value = result.data.Find("editable"); value && value->IsBool()) state.editable = value->AsBool();
    if (const auto* value = result.data.Find("diagnostic"); value && value->IsString()) state.diagnostic = value->AsString();
    return state;
}

OpResult ApplyManagedInspectorEnabled(EditorContext& context, const std::vector<ManagedSettingsTarget>& targets, bool enabled)
{
    OpArgs args;
    if (!EnabledArgs(context, targets, args)) return OpResult::Err("STALE_TARGET", "選択対象の Scene または C# 世代が変わりました。");
    args.Set("enabled", enabled);
    return InvokeOperator(context, "script.set_enabled", args);
}

OpResult ApplyManagedInspectorBatch(EditorContext& context, const std::vector<ManagedSettingsEdit>& edits)
{
    if (edits.empty()) return OpResult::Err("BAD_TARGET", "設定の選択対象が空です");
    OpData targets = OpData::MakeArray();
    for (const ManagedSettingsEdit& edit : edits) {
        const auto& target = edit.target;
        if (!context.activeScene || context.activeScene->GetRenderSceneGeneration() != target.sceneLifetime ||
            scene::ManagedScriptRuntime::Instance().GetAppliedGeneration() != target.generation || target.type != edits.front().target.type)
            return OpResult::Err("STALE_INSPECTOR_DRAFT", "選択対象の Scene、型、C# 世代が変わりました");
        OpData selected = OpData::MakeObject(); selected.Set("ownerGuid", target.objectId);
        selected.Set("savedScriptId", target.savedScriptId); selected.Set("scriptId", target.scriptId);
        selected.Set("fields", edit.fields); selected.Set("patches", edit.patches);
        selected.Set("referenceMoves", edit.referenceMoves);
        if (!edit.expectedRevision.empty()) selected.Set("expectedRevision", edit.expectedRevision);
        if (edit.editSession) selected.Set("editSession", std::to_string(edit.editSession));
        OpData rebind = OpData::MakeArray(); for (const auto& path : edit.rebindReferences) rebind.Push(path);
        selected.Set("rebindReferences", std::move(rebind)); targets.Push(std::move(selected));
    }
    OpData settings = OpData::MakeObject(); settings.Set("targets", std::move(targets));
    settings.Set("script", edits.front().target.type); settings.Set("fields", OpData::MakeObject());
    settings.Set("domain", context.playMode && !context.playMode->IsInEditor() ? "play" : "edit");
    OpArgs preview; preview.Set("settings", std::move(settings));
    OpResult prepared = InvokeOperator(context, "script.configure.preview", preview);
    if (!prepared.ok) return prepared;
    const OpData* plan = prepared.data.Find("plan");
    if (!plan || !plan->IsString()) return OpResult::Err("BAD_CONFIGURE_RESULT", "候補計画の ID がありません");
    OpArgs apply; apply.Set("plan", plan->AsString());
    return InvokeOperator(context, "script.configure.apply", apply);
}

ManagedSettingsTarget CaptureManagedSettingsTarget(EditorContext& context, scene::GameObject& owner, const scene::ScriptEntry& entry)
{
    ManagedSettingsTarget target;
    if (!context.activeScene || !entry.script) return target;
    target.sceneLifetime = context.activeScene->GetRenderSceneGeneration();
    target.generation = scene::ManagedScriptRuntime::Instance().GetAppliedGeneration();
    target.objectId = owner.instanceId;
    target.scriptId = entry.inspectionId + "/" + entry.script->InspectionId();
    target.savedScriptId = entry.savedScriptId;
    target.type = entry.script->GetTypeName();
    return target;
}

OpResult ApplyManagedInspectorFields(EditorContext& context, const ManagedSettingsTarget& target, const OpData& fields,
    const std::vector<std::string>& rebindReferences, const OpData& referenceMoves, std::string_view expectedRevision, const OpData& patches, uint64_t editSession)
{
    if (!context.activeScene || context.activeScene->GetRenderSceneGeneration() != target.sceneLifetime
        || scene::ManagedScriptRuntime::Instance().GetAppliedGeneration() != target.generation)
        return OpResult::Err("STALE_INSPECTOR_DRAFT", "Scene または C# 世代が変わりました。設定を取り直してください");
    scene::GameObject* owner = context.activeScene->FindByGuid(target.objectId);
    scene::ScriptComponent* scripts = owner ? owner->GetComponent<scene::ScriptComponent>() : nullptr;
    bool exact = false;
    if (scripts) for (const scene::ScriptEntry& entry : scripts->scripts)
        if (entry.script && target.type == entry.script->GetTypeName()
            && target.scriptId == entry.inspectionId + "/" + entry.script->InspectionId()) exact = true;
    if (!exact) return OpResult::Err("STALE_INSPECTOR_DRAFT", "編集中の Script 個体が削除・再生成されました");
    if (fields.IsObject() && fields.AsObject().empty() && rebindReferences.empty() && (patches.IsNull() || (patches.IsArray() && patches.AsArray().empty()))) {
        if (!referenceMoves.IsNull() && (!referenceMoves.IsArray() || !referenceMoves.AsArray().empty()))
            return OpResult::Err("REFERENCE_MOVES_REQUIRE_VALUES", "referenceMoves は対応する設定値または leaf patch と併用してください。配列の移動だけなら path/action=move/from/to を指定してください");
        OpResult result; result.noChange = true; return result;
    }
    OpData selector = OpData::MakeObject(); selector.Set("id", target.objectId);
    OpData settings = OpData::MakeObject();
    settings.Set("objects", std::move(selector)); settings.Set("script", target.type);
    settings.Set("scriptId", target.scriptId); settings.Set("fields", fields);
    if (!referenceMoves.IsNull()) settings.Set("referenceMoves", referenceMoves);
    if (!patches.IsNull()) settings.Set("patches", patches);
    if (editSession) settings.Set("editSession", std::to_string(editSession));
    if (!expectedRevision.empty()) settings.Set("expectedRevision", std::string(expectedRevision));
    if (!rebindReferences.empty()) {
        OpData paths = OpData::MakeArray();
        for (const std::string& path : rebindReferences) paths.Push(path);
        settings.Set("rebindReferences", std::move(paths));
    }
    OpArgs preview; preview.Set("settings", std::move(settings));
    OpResult prepared = InvokeOperator(context, "script.configure.preview", preview);
    if (!prepared.ok) return prepared;
    const OpData* token = prepared.data.Find("plan");
    if (!token || !token->IsString()) return OpResult::Err("BAD_CONFIGURE_RESULT", "Configure preview が plan を返しませんでした");
    OpArgs apply; apply.Set("plan", token->AsString());
    return InvokeOperator(context, "script.configure.apply", apply);
}
OpResult ApplyManagedInspectorAction(EditorContext& context, const ManagedSettingsTarget& target, std::string_view action)
{
    if (!context.activeScene || context.activeScene->GetRenderSceneGeneration() != target.sceneLifetime
        || scene::ManagedScriptRuntime::Instance().GetAppliedGeneration() != target.generation)
        return OpResult::Err("STALE_INSPECTOR_DRAFT", "Scene または C# 世代が変わりました。action を取り直してください");
    OpData selector = OpData::MakeObject(); selector.Set("id", target.objectId);
    OpData settings = OpData::MakeObject();
    settings.Set("objects", std::move(selector)); settings.Set("script", target.type); settings.Set("scriptId", target.scriptId);
    settings.Set("fields", OpData::MakeObject()); settings.Set("action", std::string(action));
    OpArgs preview; preview.Set("settings", std::move(settings));
    OpResult prepared = InvokeOperator(context, "script.configure.preview", preview);
    if (!prepared.ok) return prepared;
    const OpData* plan = prepared.data.Find("plan");
    if (!plan || !plan->IsString()) return OpResult::Err("BAD_CONFIGURE_RESULT", "Action preview が plan を返しませんでした");
    OpArgs apply; apply.Set("plan", plan->AsString());
    return InvokeOperator(context, "script.configure.apply", apply);
}
} /// @note namespace fbzz::editor
