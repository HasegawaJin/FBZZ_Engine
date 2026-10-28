/// @file    ScriptResumeOperators.cpp
/// @brief   C# 個体の有効状態・停止理由・明示復帰を Inspector・AI 共通操作として提供する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <algorithm>
#include <unordered_set>

namespace fbzz::editor {
namespace {
scene::ManagedScriptAdapter* Resolve(const EditorContext& ctx, const OpArgs& args, bool requireIdle = true)
{
    if (!ctx.activeScene || (requireIdle && (ctx.managedBuildBusy || !scene::ManagedScriptRuntime::Instance().IsDispatchIdle()))) return nullptr;
    auto* owner = ctx.activeScene->FindByGuid(args.GetString("node"));
    auto* component = owner ? owner->GetComponent<scene::ScriptComponent>() : nullptr;
    if (!component) return nullptr;
    const std::string scriptId = args.GetString("scriptId");
    for (const auto& entry : component->scripts)
        if (entry.script && !entry.m_pendingRemoval && entry.inspectionId + "/" + entry.script->InspectionId() == scriptId)
            return scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
    return nullptr;
}

OpData Status(const scene::ManagedScriptAdapter& adapter, const std::string& scriptId)
{
    OpData data = OpData::MakeObject();
    data.Set("scriptId", scriptId);
    data.Set("type", std::string(adapter.GetTypeName()));
    data.Set("faulted", adapter.IsRuntimeFaulted());
    data.Set("requirementsBlocked", adapter.RequirementsBlocked());
    data.Set("enabled", adapter.enabled);
    data.Set("diagnostic", adapter.FaultDiagnostic());
    data.Set("resumeDiagnostic", adapter.ResumeDiagnostic());
    return data;
}

struct EnabledTarget {
    OpArgs address;
    uint64_t generation = 0;
    uint64_t instance = 0;
    bool before = true;
};

std::string EnabledContextReason(const EditorContext& context)
{
    if (!context.activeScene) return "Scene が開かれていません。";
    if (context.managedBuildBusy) return "C# スクリプトをビルド中です。完了後に切り替えられます。";
    if (!scene::ManagedScriptRuntime::Instance().IsDispatchIdle()) return "C# の処理中です。処理が終わるまでお待ちください。";
    if (!context.undoStack || !context.undoStack->IsRecordingEnabled()) return "履歴を更新できないため、切り替えできません。";
    if (context.playMode) {
        if (context.playMode->HasPendingRestore()) return "Play の停止処理中です。";
        if (!context.playMode->IsInEditor() &&
            (!context.playMode->CanEditTemporaryScene(context.activeScene) || context.undoStack != context.playMode->TemporaryUndo()))
            return "Play 中の編集先が利用できません。";
    }
    return {};
}

bool ReadEnabledTargets(const EditorContext& context, const OpArgs& args,
    std::vector<EnabledTarget>& targets, std::string& reason)
{
    std::vector<OpArgs> addresses;
    if (const OpData* batch = args.GetData("targets")) {
        if (args.Has("node") || args.Has("scriptId") || !batch->IsArray() || batch->AsArray().empty()) {
            reason = "対象は単体または空でない一覧のいずれかを指定してください。";
            return false;
        }
        for (const OpData& item : batch->AsArray()) {
            const OpData* node = item.Find("node"), *script = item.Find("scriptId");
            if (!node || !node->IsString() || !script || !script->IsString()) {
                reason = "対象の所有者と Script 個体を指定してください。";
                return false;
            }
            OpArgs address; address.Set("node", node->AsString()); address.Set("scriptId", script->AsString());
            addresses.push_back(std::move(address));
        }
    } else addresses.push_back(args);
    std::unordered_set<std::string> unique;
    for (const auto& address : addresses) {
        auto* adapter = Resolve(context, address, false);
        if (!adapter) { reason = "対象の C# Script が削除または再生成されました。"; return false; }
        const std::string identity = address.GetString("node") + '/' + address.GetString("scriptId");
        if (!unique.insert(identity).second) { reason = "同じ Script 個体が重複しています。"; return false; }
        targets.push_back({address, adapter->Generation(), adapter->InstanceId(), adapter->enabled});
    }
    return true;
}

std::string EnabledTargetReason(const EditorContext& context, const std::vector<EnabledTarget>& targets, bool enabled)
{
    if (auto reason = EnabledContextReason(context); !reason.empty()) return reason;
    for (const auto& target : targets) {
        auto* adapter = Resolve(context, target.address);
        if (!adapter) return "対象の C# Script が削除または再生成されました。";
        if (enabled && (adapter->IsRuntimeFaulted() || adapter->RequirementsBlocked()))
            return "停止原因を修復し、「再検証して再開」から再開してください。";
    }
    return {};
}

/// @note 全対象の Scene・コード世代・個体・変更前値を照合してから一括公開し、一回の Undo に記録する。
bool ApplyEnabled(EditorContext& context, const std::vector<EnabledTarget>& targets, uint64_t lifetime,
    bool temporary, bool enabled, bool undo, std::string& reason)
{
    const bool currentTemporary = context.playMode && !context.playMode->IsInEditor();
    if (!context.activeScene || context.activeScene->GetRenderSceneGeneration() != lifetime ||
        temporary != currentTemporary) {
        reason = "有効状態の編集先 Scene または履歴が変わりました。";
        return false;
    }
    reason = EnabledContextReason(context);
    if (!reason.empty()) return false;
    std::vector<scene::ManagedScriptAdapter*> adapters;
    for (const auto& target : targets) {
        auto* adapter = Resolve(context, target.address);
        if (!adapter || adapter->Generation() != target.generation || adapter->InstanceId() != target.instance) {
            reason = "有効状態の編集先 C# Script 個体が変わりました。";
            return false;
        }
        const bool value = undo ? target.before : enabled;
        if (value && (adapter->IsRuntimeFaulted() || adapter->RequirementsBlocked())) {
            reason = "停止原因を修復し、再検証してから再開してください。";
            return false;
        }
        if (adapter->enabled != (undo ? enabled : target.before)) {
            reason = "C# Script の有効状態が別の操作で変わりました。";
            return false;
        }
        adapters.push_back(adapter);
    }
    /// @note 通知は次 Tick の共通 SynchronizeEnabledState に任せ、設定操作中に user callback を実行しない。
    for (size_t index = 0; index < targets.size(); ++index)
        adapters[index]->enabled = undo ? targets[index].before : enabled;
    if (temporary) context.playMode->MarkTemporarySettingsChanged();
    else {
        context.sceneDirty = true;
        if (context.markSceneDirty) context.markSceneDirty();
    }
    reason.clear();
    return true;
}
} /// @note namespace

void RegisterScriptResumeOperators(OperatorRegistry& registry)
{
    const std::vector<OpParam> target = {{"node", OpParamType::NodeId, "所有者の GUID"},
        {"scriptId", OpParamType::String, "対象 Script の inspection ID"}};
    EditorOperator status;
    status.id = "script.status";
    status.label = "C# Script の実行状態";
    status.category = "Inspector";
    status.desc = "停止状態・最初のエラー理由・enabled 状態を取得する。";
    status.kind = OpKind::Query;
    status.params = target;
    status.poll = [](const OpContext& c, const OpArgs& args) { return Resolve(c.ctx, args) != nullptr; };
    status.exec = [](OpContext& c, const OpArgs& args) -> OpResult {
        auto* adapter = Resolve(c.ctx, args);
        return adapter ? OpResult::Data(Status(*adapter, args.GetString("scriptId"))) : OpResult::Err("STALE_TARGET", "対象の C# Script が見つかりません。");
    };
    registry.Register(std::move(status));

    std::vector<OpParam> enabledTargets = target;
    for (auto& parameter : enabledTargets) parameter.required = false;
    enabledTargets.push_back({"targets", OpParamType::Data, "所有者と Script 個体の一覧", false});
    EditorOperator enabledStatus;
    enabledStatus.id = "script.enabled_status";
    enabledStatus.label = "C# Script の有効状態と切り替え可否";
    enabledStatus.category = "Inspector";
    enabledStatus.kind = OpKind::Query;
    enabledStatus.params = enabledTargets;
    enabledStatus.exec = [](OpContext& context, const OpArgs& args) -> OpResult {
        std::vector<EnabledTarget> targets; std::string reason;
        if (!ReadEnabledTargets(context.ctx, args, targets, reason)) return OpResult::Err("STALE_TARGET", reason);
        const bool value = targets.front().before;
        const bool mixed = std::any_of(targets.begin(), targets.end(), [value](const auto& item) { return item.before != value; });
        reason = EnabledTargetReason(context.ctx, targets, mixed || !value);
        OpData data = OpData::MakeObject(); data.Set("enabled", value); data.Set("mixed", mixed);
        data.Set("editable", reason.empty()); data.Set("diagnostic", reason);
        return OpResult::Data(std::move(data));
    };
    registry.Register(std::move(enabledStatus));

    EditorOperator enabled;
    enabled.id = "script.set_enabled";
    enabled.label = "C# Script の有効状態を変更";
    enabled.category = "Inspector";
    enabled.desc = "共通の有効・無効チェックを変更し、Edit または Play の履歴へ記録する。";
    enabled.kind = OpKind::Mutation;
    enabled.undoLabel = "Change Script Enabled";
    enabled.params = enabledTargets;
    enabled.params.push_back({"enabled", OpParamType::Bool, "Script の有効状態"});
    enabled.poll = [](const OpContext& context, const OpArgs& args) {
        std::vector<EnabledTarget> targets; std::string reason;
        return ReadEnabledTargets(context.ctx, args, targets, reason) && EnabledTargetReason(context.ctx, targets, args.GetBool("enabled")).empty();
    };
    enabled.checked = [](const OpContext& context, const OpArgs& args) {
        std::vector<EnabledTarget> targets; std::string reason;
        return ReadEnabledTargets(context.ctx, args, targets, reason) &&
            std::all_of(targets.begin(), targets.end(), [](const auto& target) { return target.before; });
    };
    enabled.exec = [](OpContext& context, const OpArgs& args) -> OpResult {
        std::vector<EnabledTarget> targets; std::string reason;
        if (!ReadEnabledTargets(context.ctx, args, targets, reason)) return OpResult::Err("STALE_TARGET", reason);
        const bool after = args.GetBool("enabled");
        std::erase_if(targets, [after](const auto& item) { return item.before == after; });
        OpResult result = OpResult::Data(OpData::MakeObject()); result.data.Set("enabled", after);
        if (targets.empty()) { result.noChange = true; return result; }
        const uint64_t lifetime = context.ctx.activeScene->GetRenderSceneGeneration();
        const bool temporary = context.ctx.playMode && !context.ctx.playMode->IsInEditor();
        const auto apply = [&ctx = context.ctx, targets = std::move(targets), lifetime, temporary, after](
            bool undo, std::string& reason) {
            return ApplyEnabled(ctx, targets, lifetime, temporary, after, undo, reason);
        };
        if (!apply(false, reason)) return OpResult::Err("ENABLED_CONFLICT", reason);
        result.command = std::make_unique<CheckedLambdaCommand>("Change Script Enabled",
            [apply](std::string& reason) { return apply(false, reason); },
            [apply](std::string& reason) { return apply(true, reason); });
        return result;
    };
    registry.Register(std::move(enabled));

    EditorOperator resume;
    resume.id = "script.resume";
    resume.label = "C# Script を再開";
    resume.category = "Inspector";
    resume.desc = "必須設定を再検証して停止を解除し、成功済みの Awake・Start を保って次 Tick から再試行する。";
    resume.caution = "解除した jobs は復元せず、有効な個体の Connect で購読を再構築する。再開後に同じ原因が残れば再停止する。";
    resume.kind = OpKind::Action;
    resume.params = target;
    resume.params.push_back({"enabled", OpParamType::Bool, "再開後の enabled 状態", false, true});
    resume.poll = [](const OpContext& c, const OpArgs& args) {
        auto* adapter = Resolve(c.ctx, args);
        return adapter && (adapter->IsRuntimeFaulted() || adapter->RequirementsBlocked());
    };
    resume.exec = [](OpContext& c, const OpArgs& args) -> OpResult {
        auto* adapter = Resolve(c.ctx, args);
        if (!adapter) return OpResult::Err("STALE_TARGET", "対象の C# Script が見つかりません。");
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        if (!runtime.Resume(*adapter, args.GetBool("enabled", true)))
            return OpResult::Err("RESUME_REJECTED", runtime.LastDiagnostic());
        return OpResult::Data(Status(*adapter, args.GetString("scriptId")));
    };
    registry.Register(std::move(resume));
}
} /// @note namespace fbzz::editor
