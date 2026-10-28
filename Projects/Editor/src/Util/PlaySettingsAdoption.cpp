/// @file    PlaySettingsAdoption.cpp
/// @brief   Explicit Play capture, bounded immutable review and shared checked Edit adoption operations.
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include <Editor/Util/PlaySettingsAdoption.hpp>
#include <Editor/Util/PlayChangesAdoption.hpp>
#include <Editor/Util/PlaySettingsBirthTable.hpp>
#include <Editor/Util/ManagedSettingsCodec.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <algorithm>
#include <Engine/Util/NumberParsing.hpp>
#include <imgui.h>
namespace fbzz::editor {
namespace {
using scene::ManagedScriptAdapter;
using scene::ManagedScriptRuntime;
std::string Text(const OpData& data, std::string_view key) { const auto* value = data.Find(key); return value && value->IsString() ? value->AsString() : std::string{}; }
bool Number(std::string_view text, uint64_t& value) {
    return util::ParseInteger(text, value);
}
ManagedScriptAdapter* Find(scene::Scene& source, const std::string& guid, const std::string& saved) {
    auto* owner = source.FindByGuid(guid); auto* scripts = owner ? owner->GetComponent<scene::ScriptComponent>() : nullptr;
    if (scripts) for (auto& entry : scripts->scripts) if (entry.savedScriptId == saved && !entry.m_pendingRemoval) return ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
    return nullptr;
}
bool Host(scene::ManagedBridgeOp op, uint64_t generation, uint64_t instance, uint64_t token, std::string_view input,
    OpData* output = nullptr, uint64_t* resultToken = nullptr, const std::vector<ManagedScriptAdapter*>* adapters = nullptr)
{
    scene::ManagedBridgeRequest request; request.op = op; request.generation = generation; request.instanceId = instance;
    request.integer = static_cast<int64_t>(token); request.input = input.data(); request.inputBytes = static_cast<uint32_t>(input.size());
    scene::ManagedBridgeReply reply; std::string encoded;
    const bool ok = adapters ? ManagedScriptRuntime::Instance().DispatchPreparedSettings(*adapters, request, reply) : ManagedScriptRuntime::Instance().Dispatch(request, reply, output ? &encoded : nullptr);
    if (!ok || reply.status != scene::ManagedBridgeStatus::OK) return false;
    if (resultToken) *resultToken = static_cast<uint64_t>(reply.integer);
    if (output && !ReadManagedSettingsJson(encoded, *output)) return false;
    return true;
}
OpData Describe(const PlaySettingsAdoptionDraft& draft) {
    OpData result = OpData::MakeObject(); result.Set("adoptionId", std::to_string(draft.id)); result.Set("consumed", draft.consumed);
    result.Set("totalTargets", static_cast<int64_t>(draft.targets.size())); result.Set("diagnostic", draft.diagnostic);
    result.Set("reviewOperation", "script.play_changes.review"); result.Set("prepareOperation", "script.play_changes.prepare");
    if (!draft.plan.empty()) result.Set("plan", draft.plan);
    return result;
}
void DrawValue(const OpData& value, int depth = 0) {
    if (depth > 4) { ImGui::TextDisabled("個別ページで確認してください"); return; }
    if (value.IsNull()) ImGui::TextDisabled("未設定");
    else if (value.IsBool()) ImGui::TextUnformatted(value.AsBool() ? "true" : "false");
    else if (value.IsInt()) ImGui::Text("%lld", static_cast<long long>(value.AsInt64()));
    else if (value.IsFloat()) ImGui::Text("%.9g", value.AsDouble());
    else if (value.IsString()) ImGui::TextWrapped("%s", value.AsString().c_str());
    else if (value.IsArray()) { for (const auto& item : value.AsArray()) DrawValue(item, depth + 1); }
    else if (value.IsObject()) {
        if (value.Find("$deferred")) ImGui::TextDisabled("大きな値 / ページで確認できます");
        else if (const auto* reference = value.Find("$go")) ImGui::TextWrapped("参照: %s", reference->AsString().c_str());
        else if (const auto* reference = value.Find("$component")) ImGui::TextWrapped("Component 参照: %s", Text(*reference, "ownerGuid").c_str());
        else for (const auto& [name, item] : value.AsObject()) { ImGui::TextUnformatted(name.c_str()); ImGui::SameLine(); DrawValue(item, depth + 1); }
    }
}
void DrawPage(const OpData& page) {
    if (const auto* value = page.Find("value")) DrawValue(*value);
    if (const auto* text = page.Find("text")) DrawValue(*text);
    if (const auto* items = page.Find("items")) DrawValue(*items);
    if (const auto* next = page.Find("nextStart"); next && !next->IsNull()) ImGui::TextDisabled("続きがあります / 次のページで確認できます");
}
int64_t ReviewOffset() {
    auto* storage = ImGui::GetStateStorage(); const uint64_t low = static_cast<uint32_t>(storage->GetInt(ImGui::GetID("offset.low")));
    const uint64_t high = static_cast<uint32_t>(storage->GetInt(ImGui::GetID("offset.high")));
    int64_t offset = static_cast<int64_t>(low | (high << 32));
    ImGui::SetNextItemWidth(180.0f); ImGui::InputScalar("開始位置", ImGuiDataType_S64, &offset); return std::max<int64_t>(0, offset);
}
void StoreReviewOffset(int64_t offset) {
    const uint64_t value = static_cast<uint64_t>(std::max<int64_t>(0, offset)); auto* storage = ImGui::GetStateStorage();
    storage->SetInt(ImGui::GetID("offset.low"), static_cast<int>(static_cast<uint32_t>(value)));
    storage->SetInt(ImGui::GetID("offset.high"), static_cast<int>(static_cast<uint32_t>(value >> 32)));
}
void ReviewNavigation(const OpData& page, int64_t offset, const char* nextKey = "nextStart") {
    int64_t next = 0; const OpData* marker = page.Find(nextKey); const bool more = marker && marker->IsInt(); if (more) next = marker->AsInt64();
    ImGui::BeginDisabled(offset == 0); if (ImGui::SmallButton("前の16件")) offset = std::max<int64_t>(0, offset - 16); ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(!more); if (ImGui::SmallButton("次のページ")) offset = next; ImGui::EndDisabled(); StoreReviewOffset(offset);
}
OpResult Capture(OpContext& c, const OpData& selection) {
    auto* play = c.ctx.playMode;
    if (!play || !play->CanEditTemporaryScene(c.ctx.activeScene) || !play->AdoptionBirths() || !play->ParkedEditScene())
        return OpResult::Err("NOT_IN_PLAY", "元の Edit を保持した Play 中だけ設定を捕捉できます");
    const OpData* targets = selection.Find("targets");
    if (!targets || !targets->IsArray() || targets->AsArray().empty()) return OpResult::Err("BAD_TARGET", "targets に保存 Script ID と Setting paths を指定してください");
    auto draft = std::make_shared<PlaySettingsAdoptionDraft>(); draft->births = play->AdoptionBirths(); draft->editLifetime = draft->births->EditLifetime();
    for (const OpData& specified : targets->AsArray()) {
        const std::string guid = Text(specified, "ownerGuid"), saved = Text(specified, "savedScriptId");
        ManagedScriptAdapter* original = nullptr; std::string diagnostic;
        if (!draft->births->ValidateSource(*play->ParkedEditScene(), *c.ctx.activeScene, guid, saved, original, diagnostic)) return OpResult::Err("ADOPTION_BIRTH_CHANGED", diagnostic);
        auto* source = Find(*c.ctx.activeScene, guid, saved); const OpData* paths = specified.Find("paths");
        if (!source || !paths || !paths->IsArray()) return OpResult::Err("BAD_TARGET", "Setting paths がありません");
        OpData request = OpData::MakeObject(); request.Set("paths", *paths); std::string input;
        if (!WriteOpDataJson(request, input)) return OpResult::Err("BAD_PATH", "Setting paths が不正です");
        PlaySettingsAdoptionTarget target; target.ownerGuid = guid; target.savedScriptId = saved; target.originalBirth = original->ReferenceBirth(); target.type = original->GetTypeName();
        target.originalEntity = play->ParkedEditScene()->FindByGuid(guid)->GetID();
        if (!Host(scene::ManagedBridgeOp::CAPTURE_SETTINGS_ADOPTION, source->Generation(), source->InstanceId(), 0, input, nullptr, &target.captureId))
            return OpResult::Err("CAPTURE_FAILED", ManagedScriptRuntime::Instance().LastDiagnostic());
        draft->targets.push_back(std::move(target)); auto& retained = draft->targets.back();
        if (!Host(scene::ManagedBridgeOp::MANAGE_SETTINGS_ADOPTION, 0, 0, retained.captureId, R"({"action":"summary"})", &retained.summary))
            return OpResult::Err("CAPTURE_FAILED", ManagedScriptRuntime::Instance().LastDiagnostic());
        uint64_t total = 0; const auto* count = retained.summary.Find("referenceCount"); if (count && count->IsInt()) total = static_cast<uint64_t>(count->AsInt64());
        for (uint64_t start = 0; start < total;) {
            OpData pageRequest = OpData::MakeObject(); pageRequest.Set("action", "references"); pageRequest.Set("start", static_cast<int64_t>(start)); pageRequest.Set("count", 64); pageRequest.Set("packetBytes", 16384);
            if (!WriteOpDataJson(pageRequest, input)) return OpResult::Err("BAD_CAPTURE_PAGE", "参照 page 指定が不正です"); OpData page;
            if (!Host(scene::ManagedBridgeOp::MANAGE_SETTINGS_ADOPTION, 0, 0, retained.captureId, input, &page)) return OpResult::Err("CAPTURE_FAILED", ManagedScriptRuntime::Instance().LastDiagnostic());
            const auto* items = page.Find("items"); if (!items || !items->IsArray() || items->AsArray().empty()) return OpResult::Err("BAD_CAPTURE_PAGE", "参照 page が進みませんでした");
            OpData references = OpData::MakeObject();
            for (const auto& item : items->AsArray()) { const auto* identity = item.Find("identity"); if (!identity) return OpResult::Err("BAD_CAPTURE_PAGE", "参照 identity がありません"); references.Set(Text(item, "path"), *identity); }
            OpData mappings;
            if (!draft->births->MapReferences(*play->ParkedEditScene(), *c.ctx.activeScene, references, mappings, diagnostic)) { draft->diagnostic = diagnostic; draft->referenceProofReady = false; break; }
            OpData proofs = OpData::MakeArray(); for (const auto& item : items->AsArray()) {
                const std::string path = Text(item, "path"); const auto* mapping = mappings.Find(path);
                if (!mapping) { draft->diagnostic = "失効・未接続の参照を採用できません: " + path; draft->referenceProofReady = false; break; }
                OpData proof = *mapping; proof.Set("path", path); proofs.Push(std::move(proof));
            }
            if (!draft->diagnostic.empty()) break;
            OpData append = OpData::MakeObject(); append.Set("action", "map"); append.Set("start", static_cast<int64_t>(start)); append.Set("items", std::move(proofs));
            append.Set("final", start + items->AsArray().size() == total); if (!WriteOpDataJson(append, input)) return OpResult::Err("BAD_CAPTURE_PAGE", "参照 proof を変換できません");
            if (!Host(scene::ManagedBridgeOp::MANAGE_SETTINGS_ADOPTION, 0, 0, retained.captureId, input)) return OpResult::Err("CAPTURE_FAILED", ManagedScriptRuntime::Instance().LastDiagnostic());
            start += items->AsArray().size();
        }
    }
    if (!play->AddAdoptionDraft(draft)) return OpResult::Err("ADOPTION_RESOURCE_FULL", "不要な採用候補を破棄して保持容量を空けてください");
    OpData result = Describe(*draft); result.Set("readyForAdoption", draft->diagnostic.empty()); return OpResult::Data(std::move(result));
}
OpResult Prepare(OpContext& c, PlaySettingsAdoptionDraft& draft) {
    if (draft.consumed) { OpResult result = OpResult::Data(Describe(draft)); result.noChange = true; return result; }
    if (!c.ctx.activeScene || c.ctx.activeScene->GetRenderSceneGeneration() != draft.editLifetime || !c.ctx.playMode || !c.ctx.playMode->IsInEditor())
        return OpResult::Err("NOT_ORIGINAL_EDIT", "Stop で元の Edit を復帰してから採用を準備してください");
    if (!draft.referenceProofReady) return OpResult::Err("ADOPTION_REFERENCE_EXPIRED", draft.diagnostic);
    if (!draft.plan.empty()) { OpArgs discard; discard.Set("plan", draft.plan); (void)InvokeOperator(c.ctx, "script.configure.discard", discard); draft.plan.clear(); }
    if (draft.preparedHostToken) { Host(scene::ManagedBridgeOp::DISCARD_SETTINGS, draft.preparedGeneration, 0, draft.preparedHostToken, {}); draft.preparedHostToken = 0; }
    std::vector<ManagedScriptAdapter*> adapters; OpData targets = OpData::MakeArray(), addresses = OpData::MakeArray();
    for (const auto& target : draft.targets) {
        ManagedScriptAdapter* adapter = nullptr; std::string diagnostic;
        if (!draft.births->ValidateDestination(*c.ctx.activeScene, target.ownerGuid, target.savedScriptId, adapter, diagnostic)) return OpResult::Err("ADOPTION_BIRTH_CHANGED", diagnostic);
        uint64_t session = 0; std::string revision;
        if (!adapter->OpenSettingsEdit(session, revision)) return OpResult::Err("ADOPTION_CONFLICT", ManagedScriptRuntime::Instance().LastDiagnostic());
        adapter->CloseSettingsEdit(session); adapters.push_back(adapter);
        OpData entry = OpData::MakeObject(); entry.Set("instanceId", std::to_string(adapter->InstanceId())); entry.Set("expectedRevision", revision);
        entry.Set("scriptBirth", adapter->ReferenceBirth()); entry.Set("captureId", std::to_string(target.captureId)); targets.Push(entry);
        entry = OpData::MakeObject(); entry.Set("ownerGuid", target.ownerGuid); entry.Set("savedScriptId", target.savedScriptId); entry.Set("scriptBirth", adapter->ReferenceBirth()); addresses.Push(entry);
    }
    OpData request = OpData::MakeObject(); request.Set("targets", std::move(targets)); std::string input; if (!WriteOpDataJson(request, input)) return OpResult::Err("BAD_ADOPTION", "採用対象を変換できません");
    const auto generation = ManagedScriptRuntime::Instance().GetAppliedGeneration(); uint64_t token = 0;
    if (!Host(scene::ManagedBridgeOp::PREPARE_SETTINGS_ADOPTION, generation, 0, 0, input, nullptr, &token, &adapters)) return OpResult::Err("ADOPTION_VALIDATION_FAILED", ManagedScriptRuntime::Instance().LastDiagnostic());
    draft.preparedHostToken = token; draft.preparedGeneration = generation;
    OpData registration = OpData::MakeObject(); registration.Set("preparedToken", std::to_string(token)); registration.Set("adoptionId", std::to_string(draft.id)); registration.Set("targets", addresses);
    OpArgs args; args.Set("settings", registration); return InvokeOperator(c.ctx, "script.configure.import_prepared", args);
}
} /// @note namespace
PlaySettingsAdoptionDraft::~PlaySettingsAdoptionDraft() { Consume(); }
void PlaySettingsAdoptionDraft::Consume() {
    if (preparedHostToken) { Host(scene::ManagedBridgeOp::DISCARD_SETTINGS, preparedGeneration, 0, preparedHostToken, {}); preparedHostToken = 0; }
    for (auto& target : targets) if (target.captureId) { Host(scene::ManagedBridgeOp::MANAGE_SETTINGS_ADOPTION, 0, 0, target.captureId, R"({"action":"discard"})"); target.captureId = 0; }
    consumed = true;
}
void RegisterPlaySettingsAdoptionOperators(OperatorRegistry& registry) {
    RegisterPlayChangesAdoptionOperators(registry);
    EditorOperator capture; capture.id = "script.play_changes.capture"; capture.label = "Capture Play Settings"; capture.category = "Script"; capture.kind = OpKind::Action;
    capture.params = {{"settings", OpParamType::Data, "exact targets と選択 Setting paths", true}};
    capture.exec = [](OpContext& c, const OpArgs& args) { const auto* data = args.GetData("settings"); return data ? Capture(c, *data) : OpResult::Err("BAD_TARGET", "settings がありません"); }; registry.Register(std::move(capture));
    EditorOperator review; review.id = "script.play_changes.review"; review.label = "Review Captured Play Settings"; review.category = "Script"; review.kind = OpKind::Query;
    review.params = {{"adoptionId", OpParamType::String, "保持した候補 ID", true}, {"options", OpParamType::Data, "targetIndex と bounded page 指定", false}};
    review.exec = [](OpContext& c, const OpArgs& args) {
        uint64_t id = 0; if (!Number(args.GetString("adoptionId"), id) || !c.ctx.playMode) return OpResult::Err("BAD_ADOPTION", "保持した採用候補 ID が不正です");
        auto draft = c.ctx.playMode->FindAdoptionDraft(id); if (!draft) return OpResult::Err("EXPIRED_ADOPTION", "採用候補は破棄されています");
        const auto* options = args.GetData("options"); if (!options || draft->consumed) return OpResult::Data(Describe(*draft));
        if (!options->IsObject()) return OpResult::Err("BAD_REVIEW", "options は page 指定の object です");
        const std::string action = Text(*options, "action");
        if (!action.empty() && action != "read" && action != "paths" && action != "summary" && action != "references")
            return OpResult::Err("BAD_REVIEW", "Query は保持した値と参照を読むだけです");
        int index = 0; if (const auto* number = options->Find("targetIndex"); number && !number->TryGetInt32(index)) return OpResult::Err("BAD_REVIEW", "targetIndex が不正です");
        if (index < 0 || static_cast<size_t>(index) >= draft->targets.size()) return OpResult::Err("BAD_REVIEW", "targetIndex が範囲外です");
        OpData request = OpData::MakeObject(); for (const auto& [key, value] : options->AsObject()) if (key != "targetIndex") request.Set(key, value);
        std::string input; if (!WriteOpDataJson(request, input)) return OpResult::Err("BAD_REVIEW", "Review 指定を変換できません"); OpData output;
        if (!Host(scene::ManagedBridgeOp::MANAGE_SETTINGS_ADOPTION, 0, 0, draft->targets[index].captureId, input, &output)) return OpResult::Err("REVIEW_FAILED", ManagedScriptRuntime::Instance().LastDiagnostic());
        return OpResult::Data(std::move(output));
    }; registry.Register(std::move(review));
    EditorOperator prepare; prepare.id = "script.play_changes.prepare"; prepare.label = "Prepare Captured Play Settings"; prepare.category = "Script"; prepare.kind = OpKind::Action;
    prepare.params = {{"adoptionId", OpParamType::String, "保持した候補 ID", true}};
    prepare.exec = [](OpContext& c, const OpArgs& args) { uint64_t id = 0; if (!Number(args.GetString("adoptionId"), id) || !c.ctx.playMode) return OpResult::Err("BAD_ADOPTION", "候補 ID が不正です"); auto draft = c.ctx.playMode->FindAdoptionDraft(id); return draft ? Prepare(c, *draft) : OpResult::Err("EXPIRED_ADOPTION", "候補が破棄されています"); }; registry.Register(std::move(prepare));
    EditorOperator discard; discard.id = "script.play_changes.discard"; discard.label = "Discard Captured Play Settings"; discard.category = "Script"; discard.kind = OpKind::Action;
    discard.params = {{"adoptionId", OpParamType::String, "保持した候補 ID", true}};
    discard.exec = [](OpContext& c, const OpArgs& args) { uint64_t id = 0; if (!Number(args.GetString("adoptionId"), id) || !c.ctx.playMode) return OpResult::Err("BAD_ADOPTION", "候補 ID が不正です"); c.ctx.playMode->DiscardAdoptionDraft(id); return OpResult::Ok(); }; registry.Register(std::move(discard));
}
void DrawPlaySettingsAdoptionReview(EditorContext& context) {
    DrawPlayChangesAdoptionReview(context);
    if (!context.playMode || context.playMode->AdoptionDrafts().empty()) return;
    ImGui::SeparatorText("保持した Play 設定");
    uint64_t discard = 0;
    for (const auto& draft : context.playMode->AdoptionDrafts()) {
        ImGui::PushID(std::to_string(draft->id).c_str()); ImGui::Text("候補 %llu / %zu Script%s", static_cast<unsigned long long>(draft->id), draft->targets.size(), draft->consumed ? " / 採用済み" : "");
        if (!draft->diagnostic.empty()) ImGui::TextWrapped("%s", draft->diagnostic.c_str());
        ImGui::BeginDisabled(!context.playMode->IsInEditor() || draft->consumed || !draft->referenceProofReady);
        if (ImGui::Button("確認・正規化")) { OpArgs args; args.Set("adoptionId", std::to_string(draft->id)); auto result = InvokeOperator(context, "script.play_changes.prepare", args); draft->diagnostic = result.ok ? "" : result.message; }
        ImGui::SameLine(); ImGui::BeginDisabled(draft->plan.empty());
        if (ImGui::Button("元の Edit へ採用")) { OpArgs args; args.Set("plan", draft->plan); auto result = InvokeOperator(context, "script.configure.apply", args); if (!result.ok) draft->diagnostic = result.message; }
        ImGui::EndDisabled(); ImGui::EndDisabled(); ImGui::SameLine(); if (ImGui::Button("破棄")) discard = draft->id;
        if (!draft->consumed && ImGui::TreeNode("保持値を確認")) {
            for (size_t target = 0; target < draft->targets.size(); ++target) {
                ImGui::PushID(static_cast<int>(target)); const int64_t pathOffset = ReviewOffset();
                OpData options = OpData::MakeObject(); options.Set("targetIndex", static_cast<int64_t>(target)); options.Set("action", "paths"); options.Set("start", pathOffset); options.Set("count", 16); options.Set("packetBytes", 4096);
                OpArgs args; args.Set("adoptionId", std::to_string(draft->id)); args.Set("options", options); const auto result = InvokeOperator(context, "script.play_changes.review", args);
                if (result.ok) if (const auto* items = result.data.Find("items")) for (const auto& path : items->AsArray()) {
                    const std::string name = Text(path, "path"); if (ImGui::TreeNode(name.c_str())) {
                        const int64_t valueOffset = ReviewOffset(); options.Set("action", "read"); options.Set("leafIndex", *path.Find("index")); options.Set("path", ""); options.Set("start", valueOffset); options.Set("count", 16); args.Set("options", options);
                        const auto value = InvokeOperator(context, "script.play_changes.review", args); if (value.ok) DrawPage(value.data); else ImGui::TextWrapped("%s", value.message.c_str());
                        if (value.ok) ReviewNavigation(value.data, valueOffset);
                        ImGui::TreePop();
                    }
                }
                if (result.ok) ReviewNavigation(result.data, pathOffset);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        if (!draft->plan.empty() && !draft->consumed && ImGui::TreeNode("補正後の変更を確認")) {
            for (size_t targetIndex = 0; targetIndex < draft->targets.size(); targetIndex++) {
                ImGui::PushID(static_cast<int>(targetIndex)); const int64_t offset = ReviewOffset();
                OpData options = OpData::MakeObject(); options.Set("mode", "changes"); options.Set("targetIndex", static_cast<int64_t>(targetIndex)); options.Set("changeOffset", offset); options.Set("changeCount", 16); options.Set("wireBytes", 8192);
                OpArgs query; query.Set("plan", draft->plan); query.Set("options", options); const auto changed = InvokeOperator(context, "script.configure.review", query);
                if (changed.ok) if (const auto* row = changed.data.Find("target")) {
                    if (const auto* changes = row->Find("changes")) for (const auto& change : changes->AsArray()) {
                        if (ImGui::TreeNode(Text(change, "path").c_str())) {
                            const int64_t valueOffset = ReviewOffset(); options = OpData::MakeObject(); options.Set("mode", "value"); options.Set("targetIndex", static_cast<int64_t>(targetIndex)); options.Set("changeIndex", *change.Find("changeIndex")); options.Set("side", "after"); options.Set("path", Text(change, "path")); options.Set("start", valueOffset); options.Set("count", 16); options.Set("wireBytes", 8192); query.Set("options", options);
                            const auto value = InvokeOperator(context, "script.configure.review", query); if (value.ok) if (const auto* page = value.data.Find("page")) { DrawPage(*page); ReviewNavigation(*page, valueOffset); }
                            if (!value.ok) ImGui::TextWrapped("%s", value.message.c_str()); ImGui::TreePop();
                        }
                    }
                    ReviewNavigation(*row, offset, "nextChangeOffset");
                }
                if (!changed.ok) ImGui::TextWrapped("%s", changed.message.c_str()); ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (discard) context.playMode->DiscardAdoptionDraft(discard);
}
} /// @note namespace fbzz::editor
