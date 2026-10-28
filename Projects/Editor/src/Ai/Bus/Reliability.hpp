/// @file    Reliability.hpp
/// @brief   Same-thread observation, guarded native edits and author-safe rollback over existing handlers.
/// @author  Hasegawa Jin
/// @date    2026-10-11
/// @see     Docs/design/ai-reliability.md
#pragma once
#include "BusInternal.hpp"
#include <Editor/PlayModeController.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::editor::ai::bus::reliability {

/// @note Guard equality is exact; requested float writes tolerate native float conversion only.
inline bool Equal(const JsonValue& a, const JsonValue& b, bool floatWrite = false)
{
    if (a.IsNumber() && b.IsNumber()) {
        const double left = a.AsNumber(), right = b.AsNumber();
        return floatWrite ? std::abs(left - right) <= 0.000001 * std::max({1.0, std::abs(left), std::abs(right)}) : left == right;
    }
    if (a.GetType() != b.GetType()) return false;
    if (a.IsObject()) {
        if (a.AsObject().size() != b.AsObject().size()) return false;
        for (const auto& [key, value] : a.AsObject()) {
            const auto* other = b.Find(key);
            if (!other || !Equal(value, *other, floatWrite)) return false;
        }
        return true;
    }
    if (a.IsArray()) {
        if (a.AsArray().size() != b.AsArray().size()) return false;
        for (std::size_t index = 0; index < a.AsArray().size(); ++index)
            if (!Equal(a.AsArray()[index], b.AsArray()[index], floatWrite)) return false;
        return true;
    }
    if (a.IsString()) return a.AsString() == b.AsString();
    if (a.IsBool()) return a.AsBool() == b.AsBool();
    return a.IsNull();
}

inline JsonValue Check(const char* status, const std::string& reason = {})
{
    auto value = JsonValue::MakeObject();
    value.Set("status", JsonValue(status));
    if (!reason.empty()) value.Set("reason", JsonValue(reason));
    return value;
}

inline Outcome Query(BusCall& call, const std::string& type, JsonValue payload = JsonValue::MakeObject())
{
    const auto* entry = call.table.Find(type);
    if (!entry || entry->kind != BusKind::QUERY) return Outcome::Err("UNSUPPORTED", "Query is unavailable: " + type);
    payload.Set("t", JsonValue(type));
    BusCall query{call.ctx, type, payload, true, call.state, call.table};
    return entry->handler(query);
}

inline Outcome Guard(BusCall& call)
{
    if (!call.ctx.activeScene) return Outcome::Err("NO_SCENE", "No active scene.");
    if (!call.ctx.undoStack || !call.ctx.undoStack->IsRecordingEnabled()) return Outcome::Err("NO_UNDOSTACK", "A recording UndoStack is required.");
    if (call.ctx.playMode && call.ctx.playMode->HasPendingRestore()) return Outcome::Err("RESTORE_PENDING", "Wait for Play mode scene restoration before reliable edits.");
    if (call.ctx.playMode && !call.ctx.playMode->IsInEditor()) return Outcome::Err("PLAY_MUTATION_UNSUPPORTED", "Reliable edits require Edit mode.");
    if (call.ctx.managedBuildBusy) return Outcome::Err("BUILD_BUSY", "Wait for the active managed build before reliable Editor execution.");
    auto snapshot = Query(call, "scene.snapshot");
    if (!snapshot.ok) return snapshot;
    auto guard = JsonValue::MakeObject();
    guard.Set("project", JsonValue(call.ctx.projectRoot));
    guard.Set("scene", JsonValue(call.ctx.currentScenePath));
    guard.Set("revision", JsonValue(std::to_string(call.ctx.undoStack->GetRevision())));
    guard.Set("cursor", JsonValue(std::to_string(call.ctx.undoStack->GetCursor())));
    guard.Set("snapshot", std::move(snapshot.result));
    return Outcome::Ok(std::move(guard));
}

inline Outcome ValidateGuard(BusCall& call)
{
    auto current = Guard(call);
    if (!current.ok) return current;
    const auto* expected = call.payload.Find("guard");
    if (!expected || !expected->IsObject()) return Outcome::Err("BAD_ARG", "guard from editor.observation is required.");
    if (!Equal(*expected, current.result)) return Outcome::Err("STATE_CONFLICT", "Scene or history changed after observation; observe and plan again.");
    return current;
}

/// @note All sections are sampled in one dispatcher call; files and asynchronous build workers remain external.
inline Outcome Observe(BusCall& call)
{
    auto result = JsonValue::MakeObject();
    result.Set("schemaVersion", JsonValue(1));
    result.Set("consistency", JsonValue("main-thread"));
    auto checks = JsonValue::MakeObject();
    const std::pair<const char*, const char*> sections[] = {
        {"state", "editor.state"}, {"selection", "scene.selection"}, {"snapshot", "scene.snapshot"},
        {"validation", "scene.validate"}, {"build", "build.status"}, {"logs", "console.logs"},
        {"undoHistory", "editor.undoHistory"}
    };
    for (const auto& [key, type] : sections) {
        auto payload = JsonValue::MakeObject();
        if (std::string_view(key) == "logs") {
            payload.Set("minLevel", JsonValue("warning"));
            payload.Set("limit", JsonValue(512));
            if (const auto* after = call.payload.Find("afterSequence")) payload.Set("afterSequence", *after);
        }
        if (std::string_view(key) == "build") {
            for (const char* id : {"managedRequestId", "packageRequestId"})
                if (const auto* requested = call.payload.Find(id)) payload.Set(id, *requested);
        }
        auto outcome = Query(call, type, std::move(payload));
        if (outcome.ok) {
            result.Set(key, std::move(outcome.result));
            checks.Set(key, Check("success"));
        } else {
            checks.Set(key, Check("unverified", outcome.code + ": " + outcome.message));
        }
    }
    auto components = JsonValue::MakeArray();
    const auto* selection = result.Find("selection");
    const auto* ids = call.payload.Find("ids");
    const bool explicitIds = ids != nullptr;
    if (!ids && selection) ids = selection->Find("ids");
    if (ids && (!ids->IsArray() || (explicitIds && ids->AsArray().size() > 128))) return Outcome::Err("BAD_ARG", "Observation ids must contain at most 128 node IDs.");
    const auto requestedCount = ids ? ids->AsArray().size() : 0;
    bool inspectedAll = requestedCount <= 128;
    std::size_t inspectedCount = 0;
    if (ids) for (const auto& id : ids->AsArray()) {
        if (inspectedCount++ >= 128) break;
        if (!id.IsString() || id.AsString().empty()) return Outcome::Err("BAD_ARG", "Observation ids must be nonempty strings.");
        auto payload = JsonValue::MakeObject(); payload.Set("id", id);
        auto inspected = Query(call, "node.components", std::move(payload));
        auto item = JsonValue::MakeObject(); item.Set("id", id);
        if (!inspected.ok) { inspectedAll = false; item.Set("check", Check("unverified", inspected.code + ": " + inspected.message)); }
        else {
            auto scriptValues = JsonValue::MakeArray();
            if (const auto* scripts = inspected.result.Find("scripts"); scripts && scripts->IsArray()) {
                for (const auto& script : scripts->AsArray()) {
                    auto request = JsonValue::MakeObject(); request.Set("id", id);
                    request.Set("scriptId", JsonValue(StringField(script, "scriptId")));
                    auto values = Query(call, "script.inspect", std::move(request));
                    if (!values.ok) inspectedAll = false;
                    auto value = JsonValue::MakeObject(); value.Set("summary", script);
                    value.Set("check", Check(values.ok ? "success" : "unverified", values.ok ? "Existing script.inspect returned settings, observations and reference diagnostics." : values.code + ": " + values.message));
                    if (values.ok) value.Set("data", std::move(values.result));
                    scriptValues.Push(std::move(value));
                }
            }
            item.Set("check", Check("success")); item.Set("data", std::move(inspected.result)); item.Set("scriptInspections", std::move(scriptValues));
        }
        components.Push(std::move(item));
    }
    result.Set("components", std::move(components));
    result.Set("componentIdsRequested", JsonValue(static_cast<std::int64_t>(requestedCount)));
    checks.Set("components", Check(inspectedAll ? "success" : "unverified", inspectedAll ? "Requested inspections completed." : "At most 128 IDs inspected; individual inspection availability is recorded."));
    auto guard = Guard(call);
    if (guard.ok) result.Set("guard", std::move(guard.result));
    else checks.Set("guard", Check("unverified", guard.code + ": " + guard.message));
    result.Set("checks", std::move(checks));
    result.Set("coverage", JsonValue("Persisted nodes and public native fields; generated nodes excluded. Selected or explicit ids include existing node.components/script.inspect settings and reference diagnostics (max 128). scene.validate retains its existing coverage. External files/build workers, rendering and persistence are separately verified."));
    return Outcome::Ok(std::move(result));
}

struct ExpectedWrite {
    std::string id;
    std::string group;
    std::string field;
    JsonValue before;
    JsonValue value;
};

inline const JsonValue* Node(const JsonValue& snapshot, const std::string& id)
{
    const auto* nodes = snapshot.Find("nodes");
    if (!nodes || !nodes->IsArray()) return nullptr;
    const JsonValue* found = nullptr;
    for (const auto& node : nodes->AsArray()) {
        if (LowerAscii(StringField(node, "id")) != LowerAscii(id)) continue;
        if (found) return nullptr; /// @note Ambiguous case aliases are not reliably addressable.
        found = &node;
    }
    return found;
}

inline const JsonValue* Field(const JsonValue& node, const std::string& group, const std::string& field)
{
    if (group.empty()) return node.Find(field);
    if (group == "transform") {
        const auto* transform = node.Find("transform");
        return transform ? transform->Find(field) : nullptr;
    }
    const auto* components = node.Find("components");
    if (components && components->IsArray()) {
        for (const auto& component : components->AsArray()) {
            if (StringField(component, "type") != group) continue;
            const auto* fields = component.Find("fields");
            return fields ? fields->Find(field) : nullptr;
        }
    }
    return nullptr;
}

inline bool AddWrite(std::vector<ExpectedWrite>& writes, const JsonValue& snapshot,
                     const std::string& id, const std::string& group, const std::string& field,
                     const JsonValue& value)
{
    const auto* node = Node(snapshot, id);
    const auto* before = node ? Field(*node, group, field) : nullptr;
    if (!before) return false;
    for (auto& write : writes) {
        if (write.id == id && write.group == group && write.field == field) { write.value = value; return true; }
    }
    writes.push_back({id, group, field, *before, value});
    return true;
}

inline bool DescribeWrites(const JsonValue& cmd, const JsonValue& snapshot, std::vector<ExpectedWrite>& writes)
{
    const auto type = StringField(cmd, "t"), id = LowerAscii(StringField(cmd, "id"));
    const char* field = nullptr;
    if (type == "node.rename") field = "name";
    else if (type == "node.setActive") field = "active";
    else if (type == "node.setTag") field = "tag";
    else if (type == "node.setLayer") field = "layer";
    if (field) {
        const auto* value = cmd.Find(field);
        return value && AddWrite(writes, snapshot, id, {}, field, *value);
    }
    if (type == "component.set") {
        const auto* value = cmd.Find("value");
        return value && AddWrite(writes, snapshot, id, StringField(cmd, "comp"), StringField(cmd, "field"), *value);
    }
    if (type != "transform.set") return false;
    bool any = false;
    for (const auto& [input, output] : {std::pair{"pos", "position"}, {"scale", "scale"}, {"rot", "rotation"}}) {
        if (!cmd.Find(input)) continue;
        math::Vector3 vector;
        if (!ReadVec3(cmd, input, vector)) return false;
        auto value = std::string_view(input) == "rot"
            ? QuaternionToJson(math::Quaternion::FromEuler({math::ToRad(vector.x), math::ToRad(vector.y), math::ToRad(vector.z)}))
            : VectorToJson(vector);
        if (!AddWrite(writes, snapshot, id, "transform", output, value)) return false;
        any = true;
    }
    return any;
}

inline JsonValue Changes(const std::vector<ExpectedWrite>& writes)
{
    auto changes = JsonValue::MakeArray();
    for (const auto& write : writes) {
        auto item = JsonValue::MakeObject();
        item.Set("id", JsonValue(write.id)); item.Set("group", JsonValue(write.group)); item.Set("field", JsonValue(write.field));
        item.Set("before", write.before); item.Set("proposed", write.value);
        changes.Push(std::move(item));
    }
    return changes;
}

inline bool VerifyWrites(const JsonValue& snapshot, const std::vector<ExpectedWrite>& writes)
{
    for (const auto& write : writes) {
        const auto* node = Node(snapshot, write.id);
        const auto* value = node ? Field(*node, write.group, write.field) : nullptr;
        if (!value || !Equal(*value, write.value, true)) return false;
    }
    return true;
}

/// @note Only in-memory existing native builders are admitted; no file writes, managed batches, creation or external jobs.
inline Outcome Checked(BusCall& call)
{
    auto before = ValidateGuard(call);
    if (!before.ok) return before;
    const auto owner = StringField(call.payload, "owner"), receipt = StringField(call.payload, "receipt");
    if (owner.empty() || owner.size() > 128 || receipt.empty() || receipt.size() > 128) return Outcome::Err("BAD_ARG", "owner and unique receipt are required (1..128 bytes).");
    for (const auto& prior : call.state.reliabilityReceipts)
        if (prior.id == receipt) return Outcome::Err("RECEIPT_REUSED", "Receipt already exists; do not replay mutations.");
    if (SerializeJson(before.result).size() > 4 * 1024 * 1024) return Outcome::Err("GUARD_TOO_LARGE", "Reliable editing guard exceeds 4 MiB; legacy editing remains available.");
    const auto* cmds = call.payload.Find("cmds");
    if (!cmds || !cmds->IsArray() || cmds->AsArray().empty() || cmds->AsArray().size() > 128) return Outcome::Err("BAD_ARG", "cmds must contain 1..128 commands.");
    const auto* snapshot = before.result.Find("snapshot");
    if (!snapshot) return Outcome::Err("NO_SNAPSHOT", "Native snapshot is unavailable.");
    const auto label = "AI checked " + receipt;
    auto composite = std::make_unique<CompositeCommand>(label);
    std::vector<ExpectedWrite> writes;
    for (const auto& sub : cmds->AsArray()) {
        const auto type = StringField(sub, "t");
        if (!DescribeWrites(sub, *snapshot, writes)) return Outcome::Err("UNSUPPORTED_RELIABLE_COMMAND", "No safely checked native write contract for " + type);
        const auto* entry = call.table.Find(type);
        if (!entry || !entry->builder) return Outcome::Err("UNSUPPORTED_RELIABLE_COMMAND", "No native builder for " + type);
        auto normalized = sub;
        const auto* target = Node(*snapshot, StringField(sub, "id"));
        if (!target) return Outcome::Err("UNSUPPORTED_RELIABLE_COMMAND", "Target is absent or has ambiguous case aliases.");
        normalized.Set("id", JsonValue(StringField(*target, "id")));
        Outcome error;
        auto command = entry->builder(call.ctx, type, normalized, error, nullptr, nullptr);
        if (!command) return error;
        if (!command->SupportsLegacyComposition()) return Outcome::Err("UNSUPPORTED_RELIABLE_COMMAND", "Use the existing checked settings API for managed batches.");
        composite->Add(std::move(command));
    }
    auto result = JsonValue::MakeObject();
    result.Set("atomic", JsonValue(false));
    result.Set("scope", JsonValue("checked-native-fields"));
    result.Set("changes", Changes(writes));
    result.Set("receipt", JsonValue(receipt));
    result.Set("dryRun", JsonValue(call.dryRun));
    result.Set("rollback", Check("unverified", "No rollback was required."));
    if (call.dryRun) {
        result.Set("status", JsonValue("unverified"));
        result.Set("preflight", Check("success"));
        result.Set("guard", before.result);
        return Outcome::Ok(std::move(result));
    }
    auto& stack = *call.ctx.undoStack;
    if (!stack.Execute(std::move(composite))) return Outcome::Err("TRANSACTION_FAILED", stack.LastDiagnostic());
    auto after = Guard(call);
    const auto* afterSnapshot = after.ok ? after.result.Find("snapshot") : nullptr;
    if (!afterSnapshot || !VerifyWrites(*afterSnapshot, writes)) {
        const bool undone = stack.GetUndoDescription() == label && stack.Undo();
        auto restored = Query(call, "scene.snapshot");
        const bool matches = undone && restored.ok && Equal(restored.result, *snapshot);
        result.Set("status", JsonValue("failure"));
        result.Set("verification", Check("failure", "Requested native field values were not observed."));
        result.Set("rollback", Check(matches ? "success" : "failure", matches ? "Own same-thread change was undone." : "Rollback did not restore the observed snapshot; inspect before retrying."));
        auto remaining = Guard(call);
        if (remaining.ok) result.Set("guard", std::move(remaining.result));
        return Outcome::Ok(std::move(result));
    }
    if (call.state.reliabilityReceipts.size() >= 8) call.state.reliabilityReceipts.erase(call.state.reliabilityReceipts.begin());
    call.state.reliabilityReceipts.push_back({owner, receipt, label, after.result, *snapshot, call.ctx.activeScene, false});
    result.Set("guard", after.result);
    result.Set("status", JsonValue("success"));
    result.Set("verification", Check("success", "Requested native fields read back in the same dispatcher call. Persistence and rendering require separate verification."));
    return Outcome::Ok(std::move(result));
}

inline Outcome Rollback(BusCall& call)
{
    const auto owner = StringField(call.payload, "owner"), id = StringField(call.payload, "receipt");
    auto found = std::find_if(call.state.reliabilityReceipts.begin(), call.state.reliabilityReceipts.end(), [&](const auto& entry) { return entry.id == id; });
    if (found == call.state.reliabilityReceipts.end()) return Outcome::Err("RECEIPT_NOT_FOUND", "Receipt is unavailable (last 8 checked edits only).");
    if (owner.empty() || found->owner != owner) return Outcome::Err("OWNER_MISMATCH", "Receipt belongs to another author.");
    if (found->rolledBack) return Outcome::Err("RECEIPT_CONSUMED", "This change was already rolled back.");
    auto current = Guard(call);
    if (!current.ok) return current;
    if (call.ctx.activeScene != found->scene || !Equal(current.result, found->afterGuard)
        || call.ctx.undoStack->GetUndoDescription() != found->label)
        return Outcome::Err("ROLLBACK_CONFLICT", "Scene or history changed; another author's work must not be undone.");
    auto result = JsonValue::MakeObject();
    result.Set("atomic", JsonValue(false));
    if (call.dryRun) { result.Set("status", JsonValue("unverified")); result.Set("dryRun", JsonValue(true)); return Outcome::Ok(std::move(result)); }
    if (!call.ctx.undoStack->Undo()) return Outcome::Err("ROLLBACK_FAILED", call.ctx.undoStack->LastDiagnostic());
    found->rolledBack = true;
    auto snapshot = Query(call, "scene.snapshot");
    const bool restored = snapshot.ok && Equal(snapshot.result, found->beforeSnapshot);
    result.Set("status", JsonValue(restored ? "success" : "failure"));
    result.Set("rollback", Check(restored ? "success" : "failure"));
    auto after = Guard(call);
    if (after.ok) result.Set("guard", std::move(after.result));
    return Outcome::Ok(std::move(result));
}

inline Outcome Command(BusCall& call, const std::string& type, JsonValue payload)
{
    const auto* entry = call.table.Find(type);
    if (!entry || entry->kind != BusKind::COMMAND) return Outcome::Err("UNSUPPORTED", "Command is unavailable: " + type);
    payload.Set("t", JsonValue(type));
    BusCall command{call.ctx, type, payload, call.dryRun, call.state, call.table};
    return entry->handler(command);
}

/// @note Disk persistence is not reversible; reload explicitly requires a successful save of the current scene.
inline Outcome Finalize(BusCall& call)
{
    auto before = ValidateGuard(call);
    if (!before.ok) return before;
    const auto* saveValue = call.payload.Find("save"), *reloadValue = call.payload.Find("reload");
    if ((saveValue && !saveValue->IsBool()) || (reloadValue && !reloadValue->IsBool())) return Outcome::Err("BAD_ARG", "save and reload must be booleans.");
    const bool save = saveValue && saveValue->AsBool(), reload = reloadValue && reloadValue->AsBool();
    if (reload && !save) return Outcome::Err("BAD_ARG", "reload requires save=true; never discard unsaved work.");
    auto result = JsonValue::MakeObject();
    result.Set("atomic", JsonValue(false));
    result.Set("save", Check("unverified", "Not requested."));
    result.Set("reload", Check("unverified", "Not requested."));
    result.Set("status", JsonValue("unverified"));
    if (!save) return Outcome::Ok(std::move(result));
    auto saved = Command(call, "scene.save", JsonValue::MakeObject());
    if (!saved.ok) {
        result.Set("status", JsonValue("failure"));
        result.Set("save", Check("failure", saved.code + ": " + saved.message));
        return Outcome::Ok(std::move(result));
    }
    if (call.dryRun) {
        result.Set("save", Check("unverified", "Dry-run only; no disk write occurred."));
        return Outcome::Ok(std::move(result));
    }
    if (call.ctx.sceneDirty) {
        result.Set("status", JsonValue("failure")); result.Set("save", Check("failure", "Host reported save but scene remains dirty."));
        return Outcome::Ok(std::move(result));
    }
    result.Set("save", Check("success", "Existing scene.save host completed and scene is clean; no disk roundtrip yet."));
    result.Set("status", JsonValue("success"));
    if (reload) {
        auto request = JsonValue::MakeObject(); request.Set("path", JsonValue(call.ctx.currentScenePath));
        auto opened = Command(call, "scene.open", std::move(request));
        if (!opened.ok) {
            result.Set("reload", Check("failure", opened.code + ": " + opened.message)); result.Set("status", JsonValue("failure"));
        } else {
            call.state.reliabilityReceipts.clear();
            auto snapshot = Query(call, "scene.snapshot");
            const auto* original = before.result.Find("snapshot");
            const bool matched = snapshot.ok && original && Equal(snapshot.result, *original);
            result.Set("reload", Check(matched ? "success" : "failure", matched ? "Current scene reloaded and native reflected snapshot matched." : "Reloaded snapshot differs or is unavailable."));
            if (!matched) result.Set("status", JsonValue("failure"));
        }
    }
    auto observation = Observe(call);
    if (observation.ok) result.Set("observation", std::move(observation.result));
    return Outcome::Ok(std::move(result));
}

} /// @note namespace fbzz::editor::ai::bus::reliability
