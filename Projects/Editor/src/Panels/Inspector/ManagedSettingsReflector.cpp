/// @file    ManagedSettingsReflector.cpp
/// @brief   Inspector の編集 buffer を callback/直接 field 書込から分離する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include "ManagedSettingsReflector.hpp"
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/ManagedSettingsCodec.hpp>
#include <Editor/Util/ManagedPrimitiveWidgets.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/ComponentOps.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui_internal.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace fbzz::editor {
namespace {
std::unordered_map<std::string, std::shared_ptr<ManagedSettingsDraft>> s_drafts;
std::uint64_t s_draftSequence = 0;
std::string Text(const OpData& value, std::string_view name)
{
    const OpData* field = value.Find(name);
    return field && field->IsString() ? field->AsString() : std::string{};
}
float Number(const OpData& value) { return value.IsInt() ? static_cast<float>(value.AsInt64()) : static_cast<float>(value.AsDouble()); }
bool Boolean(const OpData& value, std::string_view name, bool fallback = false)
{
    const OpData* field = value.Find(name);
    return field && field->IsBool() ? field->AsBool() : fallback;
}
std::string FieldLabel(std::string label, const OpData& schema)
{
    const std::string unit = Text(schema, "unit");
    if (!unit.empty()) label += " [" + unit + ']';
    return label;
}

void CloseDraftSession(ManagedSettingsDraft& draft)
{
    if (!draft.editSession) return;
    scene::ManagedBridgeRequest request; request.op = scene::ManagedBridgeOp::CLOSE_SETTINGS_EDIT;
    request.generation = draft.target.generation; request.integer = static_cast<int64_t>(draft.editSession);
    scene::ManagedBridgeReply reply; (void)scene::ManagedScriptRuntime::Instance().Dispatch(request, reply);
    draft.editSession = 0;
}

void ReleaseDraftWidget(ManagedSettingsDraft& draft)
{
    if (draft.activeId && ImGui::GetCurrentContext() && ImGui::GetActiveID() == draft.activeId)
        ImGui::ClearActiveID();
    draft.activeId = 0;
}

bool HasPendingInput(const ManagedSettingsDraft& draft)
{
    return draft.editSession != 0 || draft.activeId != 0 || !draft.createPatches.AsArray().empty() ||
        !draft.values.empty() || !draft.rebindReferences.empty() || !draft.adoptionPaths.empty() ||
        !draft.targetRevisions.empty() || draft.revisionConflict || draft.commitAttempted ||
        !draft.diagnostic.empty() || !draft.staleDrafts.empty();
}

bool SameTargetIdentity(const ManagedSettingsTarget& left, const ManagedSettingsTarget& right)
{
    return left.sceneLifetime == right.sceneLifetime && left.objectId == right.objectId &&
        left.scriptId == right.scriptId && left.savedScriptId == right.savedScriptId && left.type == right.type;
}

bool SameSavedAddress(const ManagedSettingsTarget& left, const ManagedSettingsTarget& right)
{
    return left.objectId == right.objectId && left.scriptId == right.scriptId &&
        left.savedScriptId == right.savedScriptId && left.type == right.type;
}

bool MatchesTargets(const ManagedSettingsDraft& draft, const std::vector<ManagedSettingsTarget>& targets)
{
    if (draft.targets.size() != targets.size() || draft.target.generation != targets.front().generation) return false;
    for (size_t index = 0; index < targets.size(); ++index)
        if (draft.targets[index].generation != targets[index].generation ||
            !SameTargetIdentity(draft.targets[index], targets[index])) return false;
    return true;
}

bool IsCleanDraft(const ManagedSettingsDraft& draft)
{
    return !HasPendingInput(draft);
}

void PruneDraftCache(std::string_view protectedKey)
{
    while (s_drafts.size() > 64) {
        auto oldest = s_drafts.end();
        for (auto iterator = s_drafts.begin(); iterator != s_drafts.end(); ++iterator) {
            if (iterator->first == protectedKey || !iterator->second || !IsCleanDraft(*iterator->second)) continue;
            if (oldest == s_drafts.end() || iterator->second->lastUsed < oldest->second->lastUsed) oldest = iterator;
        }
        if (oldest == s_drafts.end()) break;
        s_drafts.erase(oldest);
    }
}

void CarryStaleDrafts(const std::shared_ptr<ManagedSettingsDraft>& previous,
    const std::shared_ptr<ManagedSettingsDraft>& current, std::string reason)
{
    current->staleDrafts = std::move(previous->staleDrafts);
    previous->staleDrafts.clear();
    current->staleReviewJson.clear();
    current->staleReviewAttempted = false;
    if (!HasPendingInput(*previous)) return;
    previous->staleReason = std::move(reason);
    CloseDraftSession(*previous);
    ReleaseDraftWidget(*previous);
    current->staleDrafts.push_back(previous);
}

std::shared_ptr<ManagedSettingsDraft> GetDraft(const std::string& key,
    const std::vector<ManagedSettingsTarget>& targets, const std::string& projectRoot,
    const ManagedSettingsDraft* schemaSource = nullptr)
{
    auto found = s_drafts.find(key);
    if (found != s_drafts.end() && found->second && MatchesTargets(*found->second, targets)) {
        found->second->targets = targets;
        found->second->lastUsed = ++s_draftSequence;
        return found->second;
    }

    auto current = std::make_shared<ManagedSettingsDraft>();
    current->target = targets.front(); current->targets = targets;
    current->registryKey = key; current->projectRoot = projectRoot;
    current->lastUsed = ++s_draftSequence;
    if (schemaSource) { current->schemas = schemaSource->schemas; current->catalog = schemaSource->catalog; }
    if (found != s_drafts.end() && found->second) {
        const bool sameTarget = SameTargetIdentity(found->second->target, targets.front());
        const std::string reason = sameTarget
            ? "C# 世代が変わりました。旧入力は未適用の下書きとして保持されています。"
            : "対象 Scene または Script 個体が変わりました。旧入力は未適用の下書きとして保持されています。";
        CarryStaleDrafts(found->second, current, reason);
        found->second = current;
    } else {
        for (auto iterator = s_drafts.begin(); iterator != s_drafts.end();) {
            const auto& previous = iterator->second;
            if (iterator->first != key && previous && previous->projectRoot == projectRoot &&
                previous->target.sceneLifetime != targets.front().sceneLifetime &&
                SameSavedAddress(previous->target, targets.front())) {
                CarryStaleDrafts(previous, current,
                    "Scene が再生成されました。旧入力は未適用の下書きとして保持されています。");
                iterator = s_drafts.erase(iterator);
            } else ++iterator;
        }
        s_drafts[key] = current;
    }
    PruneDraftCache(key);
    return current;
}

void CloseInactiveDraftSessions(std::string_view activeKey, std::string_view projectRoot)
{
    for (auto& [key, draft] : s_drafts) {
        if (!draft) continue;
        if (key != activeKey || draft->projectRoot != projectRoot) {
            CloseDraftSession(*draft);
            ReleaseDraftWidget(*draft);
        }
        for (const auto& stale : draft->staleDrafts) if (stale) CloseDraftSession(*stale);
    }
}

std::string StaleDraftsJson(const ManagedSettingsDraft& current)
{
    if (current.staleReviewAttempted) return current.staleReviewJson;
    current.staleReviewAttempted = true;
    OpData archived = OpData::MakeArray();
    for (const auto& draft : current.staleDrafts) {
        if (!draft) continue;
        OpData item = OpData::MakeObject();
        item.Set("reason", draft->staleReason); item.Set("projectRoot", draft->projectRoot);
        item.Set("sceneLifetime", std::to_string(draft->target.sceneLifetime));
        item.Set("generation", std::to_string(draft->target.generation)); item.Set("objectId", draft->target.objectId);
        item.Set("scriptId", draft->target.scriptId); item.Set("savedScriptId", draft->target.savedScriptId);
        item.Set("type", draft->target.type); item.Set("patches", draft->createPatches);
        OpData values = OpData::MakeObject();
        for (const auto& [path, value] : draft->values) values.Set(path, value);
        item.Set("values", std::move(values));
        OpData schemas = OpData::MakeObject();
        for (const auto& [name, schema] : draft->schemas) schemas.Set(name, schema);
        item.Set("schema", std::move(schemas));
        OpData references = OpData::MakeArray();
        for (const std::string& path : draft->rebindReferences) references.Push(path);
        item.Set("rebindReferences", std::move(references)); archived.Push(std::move(item));
    }
    std::string json;
    if (WriteOpDataJson(archived, json)) current.staleReviewJson = std::move(json);
    return current.staleReviewJson;
}

void DiscardArchivedDrafts(ManagedSettingsDraft& draft)
{
    for (const auto& stale : draft.staleDrafts) if (stale) CloseDraftSession(*stale);
    draft.staleDrafts.clear();
    draft.staleReviewJson.clear();
    draft.staleReviewAttempted = false;
}
} /// @note namespace

ManagedSettingsReflector::ManagedSettingsReflector(EditorContext& context, scene::GameObject& owner, const scene::ScriptEntry& entry)
    : m_context(context)
{
    const ManagedSettingsTarget target = CaptureManagedSettingsTarget(context, owner, entry);
    const std::string key = context.projectRoot + "|" + std::to_string(target.sceneLifetime) + "/" + target.scriptId;
    m_draft = GetDraft(key, {target}, context.projectRoot);
    m_adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
    if (m_adapter) m_adapters.push_back(m_adapter);
    m_draft->targets = {target};
    if (m_adapter)
        for (const auto& field : m_adapter->Fields()) {
            if (field.flags & scene::kManagedFieldSetting) m_settingKeys.insert(field.name);
            OpData schema;
            if (!m_draft->schemas.contains(field.name) && ReadManagedSettingsJson(field.schemaJson, schema)) m_draft->schemas.emplace(field.name, std::move(schema));
        }
    if (m_adapter && m_draft->catalog.IsNull()) (void)ReadManagedSettingsJson(scene::ManagedScriptRuntime::Instance().GetTypeSchemaJson(target.type), m_draft->catalog);
}

ManagedSettingsReflector::ManagedSettingsReflector(EditorContext& context,
    const std::vector<std::pair<scene::GameObject*, const scene::ScriptEntry*>>& selection)
    : ManagedSettingsReflector(context, *selection.front().first, *selection.front().second)
{
    std::string key = "multi:" + context.projectRoot + ":";
    std::vector<ManagedSettingsTarget> targets;
    m_adapters.clear();
    for (const auto& [owner, entry] : selection) {
        auto target = CaptureManagedSettingsTarget(context, *owner, *entry);
        key += std::to_string(target.sceneLifetime) + '/' + target.scriptId + ';';
        targets.push_back(std::move(target));
        m_adapters.push_back(scene::ManagedScriptRuntime::Instance().FindAdapter(entry->script.get()));
    }
    m_draft = GetDraft(key, targets, context.projectRoot, m_draft.get());
}

void ManagedSettingsReflector::DrawActions()
{
    if (!m_adapter || !m_draft) return;
    CloseInactiveDraftSessions(m_draft->registryKey, m_draft->projectRoot);
    if (m_adapters.size() != 1) return;
    const OpData* actions = m_draft->catalog.Find("actions");
    if (!actions || !actions->IsArray() || actions->AsArray().empty()) return;
    if (!ImGui::TreeNode("操作##managed_actions")) return;
    const bool editing = !m_context.playMode || m_context.playMode->IsInEditor();
    ImGui::BeginDisabled(!editing || m_draft->activeId != 0 || m_draft->revisionConflict);
    for (const OpData& action : actions->AsArray()) {
        const std::string key = Text(action, "id");
        const std::string label = Text(action, "label");
        ImGui::PushID(key.c_str());
        if (ImGui::Button(label.c_str())) {
            const OpResult applied = ApplyManagedInspectorAction(m_context, m_draft->target, key);
            if (!applied.ok) m_draft->diagnostic = applied.message;
            else { m_draft->pages.clear(); m_draft->diagnostic.clear(); }
        }
        Tooltip(Text(action, "help").c_str());
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::TreePop();
}

void ManagedSettingsReflector::DiscardStaleDrafts()
{
    if (m_draft) DiscardArchivedDrafts(*m_draft);
}

std::string ManagedSettingsReflector::CopyStaleDrafts() const
{
    return m_draft ? StaleDraftsJson(*m_draft) : std::string{};
}

ManagedSettingsDraft::~ManagedSettingsDraft()
{
    if (!editSession) return;
    scene::ManagedBridgeRequest request; request.op = scene::ManagedBridgeOp::CLOSE_SETTINGS_EDIT;
    request.generation = target.generation; request.integer = static_cast<int64_t>(editSession);
    scene::ManagedBridgeReply reply; (void)scene::ManagedScriptRuntime::Instance().Dispatch(request, reply);
}
const OpData* ManagedSettingsReflector::ReadPage(std::string_view path, uint64_t start, uint32_t count)
{
    const std::string key = std::string(path) + "#" + std::to_string(start) + ":" + std::to_string(count);
    if (const auto found = m_draft->pages.find(key); found != m_draft->pages.end()) return &found->second;
    std::string json; OpData page;
    if (!m_adapter->ReadSettingsPage(path, start, count, json) || !ReadManagedSettingsJson(json, page)) {
        m_draft->diagnostic = scene::ManagedScriptRuntime::Instance().LastDiagnostic(); return nullptr;
    }
    if (m_adapters.size() > 1) {
        for (size_t index = 1; index < m_adapters.size(); ++index) {
            OpData other; std::string otherJson;
            if (!m_adapters[index] || !m_adapters[index]->ReadSettingsPage(path, start, count, otherJson) || !ReadManagedSettingsJson(otherJson, other)) {
                m_draft->mixedPaths.insert(std::string(path)); continue;
            }
            if (!ManagedSettingsPagesEqual(page, other)) m_draft->mixedPaths.insert(std::string(path));
        }
    }
    return &m_draft->pages.emplace(key, std::move(page)).first->second;
}
bool ManagedSettingsReflector::BeginEdit()
{
    if (m_draft->editSession) return true;
    const std::vector<std::string> pinnedRevisions = m_draft->targetRevisions;
    if (!m_adapter->OpenSettingsEdit(m_draft->editSession, m_draft->revision)) {
        m_draft->diagnostic = scene::ManagedScriptRuntime::Instance().LastDiagnostic(); return false;
    }
    if (!pinnedRevisions.empty() && (pinnedRevisions.size() != m_draft->targets.size() || pinnedRevisions.front() != m_draft->revision)) {
        m_adapter->CloseSettingsEdit(m_draft->editSession); m_draft->editSession = 0;
        m_draft->revisionConflict = true; m_draft->diagnostic = "設定が下書きの作成後に変わりました。下書きを破棄して再取得してください。";
        return false;
    }
    if (pinnedRevisions.empty()) m_draft->targetRevisions = {m_draft->revision};
    for (size_t index = 1; index < m_adapters.size(); ++index) {
        uint64_t session = 0; std::string revision;
        if (!m_adapters[index] || !m_adapters[index]->OpenSettingsEdit(session, revision)) {
            m_adapter->CloseSettingsEdit(m_draft->editSession); m_draft->editSession = 0;
            if (pinnedRevisions.empty()) m_draft->targetRevisions.clear();
            m_draft->diagnostic = scene::ManagedScriptRuntime::Instance().LastDiagnostic(); return false;
        }
        m_adapters[index]->CloseSettingsEdit(session);
        if (!pinnedRevisions.empty()) {
            if (index >= pinnedRevisions.size() || pinnedRevisions[index] != revision) {
                m_adapter->CloseSettingsEdit(m_draft->editSession); m_draft->editSession = 0;
                m_draft->revisionConflict = true; m_draft->diagnostic = "選択した Script 設定が下書きの作成後に変わりました。下書きを破棄して再取得してください。";
                return false;
            }
        } else m_draft->targetRevisions.push_back(std::move(revision));
    }
    return true;
}
void ManagedSettingsReflector::QueuePatch(OpData patch)
{
    if (m_draft->revisionConflict) return;
    if (!BeginEdit()) return;
    OpData patches = OpData::MakeArray();
    const std::string path = Text(patch, "path");
    const std::string action = Text(patch, "action");
    const std::int64_t index = patch.Find("index") && patch.Find("index")->IsInt() ? patch.Find("index")->AsInt64() : -1;
    for (const OpData& previous : m_draft->createPatches.AsArray()) {
        const bool previousText = Text(previous, "action") == "text_replace";
        const bool sameTextRange = action == "text_replace" && previousText &&
            Text(previous, "path") == path && previous.Find("index") && previous.Find("index")->IsInt() &&
            previous.Find("index")->AsInt64() == index;
        if (Text(previous, "path") != path || (action == "text_replace" && previousText && !sameTextRange))
            patches.Push(previous);
    }
    patches.Push(std::move(patch)); m_draft->createPatches = std::move(patches);
    const std::string cursorPath = action == "create" && path.ends_with("/-") ? path.substr(0, path.size() - 2) : path;
    if (action == "text_replace" || action == "create" || action == "remove" || action == "move") {
        m_draft->pageCursors[cursorPath].InvalidateFuture();
        m_draft->invalidatedPagePaths.insert(cursorPath);
    }
    m_draft->activeId = ImGui::GetActiveID(); m_draft->diagnostic.clear(); m_draft->commitAttempted = false; m_changed = true;
}
bool ManagedSettingsReflector::BeginVisiblePath(const std::string& path)
{
    const auto previous = m_draft->fieldHeights.find(path);
    const float height = previous == m_draft->fieldHeights.end() ? ImGui::GetFrameHeightWithSpacing() : previous->second;
    const bool editing = m_draft->layoutActiveId != 0 && ImGui::GetActiveID() == m_draft->layoutActiveId && !m_draft->activeFieldPath.empty() &&
        (m_draft->activeFieldPath == path || m_draft->activeFieldPath.starts_with(path + "/"));
    const std::string focus = m_focusField.empty() || m_focusField.front() == '/' ? m_focusField : '/' + m_focusField;
    const bool focused = m_focusScrollRequested && (focus == path || focus.starts_with(path + "/") || focus.starts_with(path + "."));
    if (editing || focused || ImGui::IsRectVisible({ImGui::GetContentRegionAvail().x, height}) ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) return true;
    if (height > 0.0f) ImGui::Dummy({0.0f, std::max(0.0f, height - ImGui::GetStyle().ItemSpacing.y)});
    return false;
}

void ManagedSettingsReflector::EndVisiblePath(const std::string& path, float startY, unsigned int activeBefore)
{
    m_draft->fieldHeights[path] = std::max(0.0f, ImGui::GetCursorPosY() - startY);
    const unsigned int activeAfter = ImGui::GetActiveID();
    if (activeAfter != 0 && activeAfter != activeBefore) {
        m_draft->activeFieldPath = path;
        m_draft->layoutActiveId = activeAfter;
    }
}

void ManagedSettingsReflector::DrawPath(std::string_view path, const char* name, const OpData& schema,
    const OpData& summary, bool setting, size_t depth)
{
    if (depth > 32) { ImGui::TextUnformatted("設定の階層が深すぎます"); return; }
    const std::string currentPath(path), kind = Text(schema, "valueKind"), type = Text(schema, "type");
    if (m_adapters.size() > 1) (void)ReadPage(path, 0, 0);
    struct MixedValueScope {
        MixedValueScope(bool mixed) { ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, mixed); }
        ~MixedValueScope() { ImGui::PopItemFlag(); }
    } mixed(m_draft->mixedPaths.contains(currentPath) && !m_draft->values.contains(currentPath));
    const bool editable = setting && !m_draft->revisionConflict && (!m_context.playMode || m_context.playMode->IsInEditor() || m_context.playMode->CanEditTemporaryScene(m_context.activeScene));
    ImGui::PushID(currentPath.c_str());
    if (setting && m_draft->selectAdoptionPaths && m_context.playMode && !m_context.playMode->IsInEditor()) {
        bool selected = m_draft->adoptionPaths.contains(currentPath);
        const bool ancestor = std::any_of(m_draft->adoptionPaths.begin(), m_draft->adoptionPaths.end(), [&currentPath](const auto& path) { return currentPath.starts_with(path + "/"); });
        ImGui::BeginDisabled(ancestor || !m_draft->createPatches.AsArray().empty());
        if (ImGui::Checkbox("保持", &selected)) {
            if (selected) {
                std::erase_if(m_draft->adoptionPaths, [&currentPath](const auto& path) { return path.starts_with(currentPath + "/"); });
                m_draft->adoptionPaths.insert(currentPath);
            } else m_draft->adoptionPaths.erase(currentPath);
        }
        ImGui::EndDisabled();
        Tooltip("この設定を保持し、Stop 後に編集時の値へ採用するか確認します。");
    }
    if (const OpData* diagnostic = summary.Find("diagnostic"); diagnostic && diagnostic->IsString()) {
        ImGui::TextWrapped("%s: %s", name, diagnostic->AsString().c_str()); ImGui::PopID(); return;
    }
    const bool deferred = summary.Find("$deferred") != nullptr;
    if (summary.IsNull() && kind == "object") {
        ImGui::Text("%s: (null)", name);
        if (setting) {
            ImGui::SameLine(); ImGui::BeginDisabled(!editable);
            if (ImGui::SmallButton("作成")) { OpData patch = OpData::MakeObject(); patch.Set("path", currentPath); patch.Set("action", "create"); QueuePatch(std::move(patch)); }
            ImGui::EndDisabled();
        }
        ImGui::PopID(); return;
    }
    if (deferred && (kind == "list" || kind == "object" || Text(summary, "kind") == "text")) {
        const int64_t total = summary.Find("total") ? summary.Find("total")->AsInt64() : 0;
        const std::string title = std::string(name) + " (" + std::to_string(total) + ")";
        if (ImGuiReflector::SubHeader(title.c_str())) {
            ImGui::Indent(); constexpr uint32_t pageSize = 64;
            ManagedSettingsPageCursor& cursor = m_draft->pageCursors[currentPath];
            const OpData* packet = ReadPage(path, cursor.Start(), pageSize);
            ImGui::BeginDisabled(cursor.PageNumber() <= 1);
            if (ImGui::SmallButton("前へ") && cursor.Back()) packet = ReadPage(path, cursor.Start(), pageSize);
            ImGui::EndDisabled(); ImGui::SameLine();
            ImGui::BeginDisabled(!packet || !cursor.CanAdvance(*packet));
            if (ImGui::SmallButton("次へ") && packet && cursor.Advance(*packet))
                packet = ReadPage(path, cursor.Start(), pageSize);
            ImGui::EndDisabled();
            ImGui::SameLine();
            const uint64_t start = packet && packet->Find("start") && packet->Find("start")->IsInt()
                ? static_cast<uint64_t>(packet->Find("start")->AsInt64()) : cursor.Start();
            const uint64_t returned = packet && packet->Find("returned") && packet->Find("returned")->IsInt()
                ? static_cast<uint64_t>(packet->Find("returned")->AsInt64()) : 0;
            ImGui::Text("%zu · %llu +%llu / %lld", cursor.PageNumber(),
                static_cast<unsigned long long>(start), static_cast<unsigned long long>(returned),
                static_cast<long long>(total));
            if (packet) {
                if (const OpData* text = packet->Find("text"); text && text->IsString()) {
                    std::string value = text->AsString();
                    const std::string textDraftKey = currentPath + "#text@" + std::to_string(start);
                    if (const auto draft = m_draft->values.find(textDraftKey); draft != m_draft->values.end()) value = draft->second.AsString();
                    ImGui::BeginDisabled(!editable); const bool previous = m_changed; m_changed = false;
                    Field(name, value); const bool changed = m_changed; m_changed |= previous; ImGui::EndDisabled();
                    if (changed && editable) {
                        OpData patch = OpData::MakeObject(); patch.Set("path", currentPath); patch.Set("action", "text_replace");
                        patch.Set("index", static_cast<int64_t>(start)); patch.Set("count", packet->Find("returned")->AsInt64()); patch.Set("value", value);
                        QueuePatch(std::move(patch)); m_draft->values[textDraftKey] = OpData(std::move(value));
                    }
                } else if (const OpData* items = packet->Find("items"); items && items->IsArray()) {
                    const bool inheritedMinimum = HasFieldMin(), inheritedMaximum = HasFieldMax();
                    const float inheritedMin = FieldMin(), inheritedMax = FieldMax();
                    const bool parentGroupOpen = m_groupOpen; std::string memberGroup;
                    for (size_t index = 0; index < items->AsArray().size(); ++index) {
                        const OpData& item = items->AsArray()[index]; const OpData* childSchema = nullptr; const OpData* value = &item;
                        std::string key = std::to_string(start + index), label = key;
                        const OpData* description = nullptr;
                        if (kind == "object") {
                            key = Text(item, "name"); value = item.Find("value");
                            if (const OpData* fields = schema.Find("fields"); fields && fields->IsArray())
                                for (const OpData& member : fields->AsArray()) if (Text(member, "name") == key) { description = &member; break; }
                            if (description && !Boolean(*description, "visible", true)) continue;
                            const std::string group = description ? Text(*description, "group") : std::string{};
                            if (group != memberGroup) { if (group.empty()) m_groupOpen = true; else Group(group.c_str()); memberGroup = group; }
                            if (!m_groupOpen) continue;
                            childSchema = description ? description->Find("value") : nullptr;
                            label = description ? Text(*description, "label") : key;
                            if (label.empty()) label = key;
                            if (description && Boolean(*description, "requiredReference")) label += " *";
                            if (description) label = FieldLabel(label, *description);
                        } else childSchema = schema.Find("element");
                        if (!value || !childSchema) continue;
                        const std::string childPath = ManagedSettingChildPath(path, key);
                        if (!BeginVisiblePath(childPath)) continue;
                        const float childY = ImGui::GetCursorPosY();
                        const unsigned int childActive = ImGui::GetActiveID();
                        BeginField(key.c_str(), label.c_str()); SetFieldReadOnly(!setting);
                        const OpData* range = description ? description : &schema;
                        if (kind == "list") { if (inheritedMinimum) SetFieldMin(inheritedMin); if (inheritedMaximum) SetFieldMax(inheritedMax); }
                        if (const OpData* minimum = range->Find("minimum"); minimum && (minimum->IsInt() || minimum->IsFloat())) SetFieldMin(Number(*minimum));
                        if (const OpData* maximumValue = range->Find("maximum"); maximumValue && (maximumValue->IsInt() || maximumValue->IsFloat())) SetFieldMax(Number(*maximumValue));
                        DrawPath(childPath, label.c_str(), *childSchema, *value, setting, depth + 1);
                        if (description) Tooltip(Text(*description, "help").c_str()); EndField();
                        if (setting && kind == "list") {
                            ImGui::BeginDisabled(!editable); ImGui::PushID(static_cast<int>(index));
                            const auto queue = [&](const char* action, int64_t destination) {
                                OpData patch = OpData::MakeObject(); patch.Set("path", currentPath); patch.Set("action", action);
                                if (std::string_view(action) == "remove") patch.Set("index", static_cast<int64_t>(start + index));
                                else { patch.Set("from", static_cast<int64_t>(start + index)); patch.Set("to", destination); }
                                QueuePatch(std::move(patch));
                            };
                            if (ImGui::SmallButton("削除")) queue("remove", 0);
                            if (start + index > 0) { ImGui::SameLine(); if (ImGui::SmallButton("上へ")) queue("move", static_cast<int64_t>(start + index - 1)); }
                            if (start + index + 1 < static_cast<uint64_t>(total)) { ImGui::SameLine(); if (ImGui::SmallButton("下へ")) queue("move", static_cast<int64_t>(start + index + 1)); }
                            ImGui::PopID(); ImGui::EndDisabled();
                        }
                        EndVisiblePath(childPath, childY, childActive);
                    }
                    m_groupOpen = parentGroupOpen;
                }
            }
            if (setting && kind == "list") {
                ImGui::BeginDisabled(!editable);
                if (ImGui::SmallButton("追加")) { OpData patch = OpData::MakeObject(); patch.Set("path", currentPath + "/-"); patch.Set("action", "create"); QueuePatch(std::move(patch)); }
                ImGui::EndDisabled();
            } else if (setting && kind == "object" && Boolean(schema, "mayBeNull")) {
                ImGui::BeginDisabled(!editable);
                if (ImGui::SmallButton("null にする")) { OpData patch = OpData::MakeObject(); patch.Set("path", currentPath); patch.Set("value", OpData{}); QueuePatch(std::move(patch)); }
                ImGui::EndDisabled();
            }
            ImGui::Unindent();
        }
        ImGui::PopID(); return;
    }
    OpData value = summary;
    if (const auto draft = m_draft->values.find(currentPath); draft != m_draft->values.end()) value = draft->second;
    if (m_draft->mixedPaths.contains(currentPath) && !m_draft->values.contains(currentPath)) ImGui::TextDisabled("%s: 複数の値", name);
    ImGuiReflector ui = static_cast<const ImGuiReflector&>(*this); ui.m_changed = false;
    ui.SetFieldReadOnly(!setting); ImGui::BeginDisabled(!editable);
    if (type == "Fbzz.GameObject") {
        scene::ScriptObjectReference reference{value.Find("$go") ? Text(value, "$go") : std::string{}, scene::EntityID::INVALID};
        const OpData* packet = ReadPage(path, 0, 1);
        const OpData* identities = packet ? packet->Find("references") : nullptr;
        const OpData* identity = identities ? identities->Find(currentPath) : nullptr;
        if (identity) {
            uint64_t id = 0, lifetime = 0; const std::string idText = Text(*identity, "objectId"), lifetimeText = Text(*identity, "sceneLifetime");
            const auto objectParsed = std::from_chars(idText.data(), idText.data() + idText.size(), id);
            const auto lifetimeParsed = std::from_chars(lifetimeText.data(), lifetimeText.data() + lifetimeText.size(), lifetime);
            if (objectParsed.ec == std::errc{} && lifetimeParsed.ec == std::errc{} && lifetime == m_draft->target.sceneLifetime)
                scene::ManagedScriptRuntime::Instance().ResolveObjectId(*m_adapter, id, reference);
        }
        ui.ObjectReferenceField(name, reference);
        if (ui.m_changed) { value = {}; if (!reference.guid.empty()) { value = OpData::MakeObject(); value.Set("$go", reference.guid); m_draft->rebindReferences.insert(currentPath); } }
    } else if (kind == "componentRef") {
        if (DrawComponentReference(ui, name, schema, value)) {
            ui.m_changed = true; if (!value.IsNull()) m_draft->rebindReferences.insert(currentPath);
        }
    } else if (kind == "materialAssetRef" || kind == "textureAssetRef" || kind == "modelAssetRef") {
        if (DrawTypedAssetReference(ui, name, kind == "materialAssetRef" ? 0 : kind == "textureAssetRef" ? 1 : 2, value)) ui.m_changed = true;
    } else if (type.starts_with("Fbzz.DataAssetRef`1")) {
        scene::DataAssetRef reference{value.IsString() ? value.AsString() : ""}; ui.Field(name, reference); if (ui.m_changed) value = OpData(reference.path);
    } else if (DrawManagedPrimitive(ui, name, schema, value)) {
    } else if ((kind == "vector2" || kind == "vector3" || kind == "vector4" || kind == "quaternion") && value.IsArray()) {
        float numbers[4]{}; const size_t count = value.AsArray().size();
        if (count >= 2 && count <= 4) { for (size_t index = 0; index < count; ++index) numbers[index] = Number(value.AsArray()[index]);
            if (ui.BeginRow(name)) { const bool changed = count == 2 ? ImGui::DragFloat2("##v", numbers, 0.01f) : count == 3 ? ImGui::DragFloat3("##v", numbers, 0.01f) : ImGui::DragFloat4("##v", numbers, 0.01f);
                if (changed) { value = OpData::MakeArray(); for (size_t index = 0; index < count; ++index) value.Push(numbers[index]); ui.m_changed = true; } ui.EndRow(); } }
    } else if (value.IsBool()) { bool number = value.AsBool(); ui.Field(name, number); value = OpData(number); }
    else if (value.IsString()) { std::string text = value.AsString(); ui.Field(name, text); value = OpData(std::move(text)); }
    ImGui::EndDisabled();
    if (editable && ui.m_changed) {
        OpData patch = OpData::MakeObject(); patch.Set("path", currentPath); patch.Set("value", value); QueuePatch(std::move(patch)); m_draft->values[currentPath] = value;
    }
    m_changed |= ui.m_changed; m_focusFound |= ui.m_focusFound; ImGui::PopID();
}
std::string ManagedSettingsReflector::ComponentReferenceLabel(const OpData& schema, const OpData& address) const
{
    const bool script = Text(address, "kind") == "script";
    std::string type = Text(schema, "targetType"), nativeName;
    if (script) {
        type = Text(address, "typeKey");
        std::string resolved;
        if (scene::ScriptFactory::ResolveTypeName(type, resolved)) type = std::move(resolved);
    } else if (const OpData* native = schema.Find("nativeKinds"); native && native->IsArray())
        for (const OpData& candidate : native->AsArray()) {
            const OpData* kind = candidate.Find("kind"), *selected = address.Find("componentKind");
            if (kind && selected && kind->IsInt() && selected->IsInt() && kind->AsInt64() == selected->AsInt64()) {
                type = Text(candidate, "type"); nativeName = Text(candidate, "nativeName"); break;
            }
        }
    const auto* operations = script || nativeName.empty() ? nullptr : scene::FindComponentOps(nativeName);
    const std::string typeLabel = operations ? operations->displayName : type == "Fbzz.Transform" ? "Transform" : scene::ScriptFactory::DisplayName(type);
    auto* owner = m_context.activeScene ? m_context.activeScene->FindByGuid(Text(address, "ownerGuid")) : nullptr;
    if (!owner) return "(見つかりません) / " + typeLabel;
    if (!script) {
        const bool missing = operations && operations->has && !operations->has(*owner);
        return owner->name + (missing ? " / (見つかりません) / " : " / ") + typeLabel;
    }
    const auto* scripts = owner->GetComponent<scene::ScriptComponent>();
    size_t count = 0, selected = 0;
    if (scripts) for (const auto& entry : scripts->scripts) {
        if (entry.m_pendingRemoval || !entry.script || entry.script->GetTypeName() != type) continue;
        ++count;
        if (entry.savedScriptId == Text(address, "savedScriptId")) selected = count;
    }
    if (!selected) return owner->name + " / (見つかりません) / " + typeLabel;
    return owner->name + " / " + typeLabel + (count > 1 ? " (" + std::to_string(selected) + ")" : "");
}

bool ManagedSettingsReflector::DrawComponentReference(ImGuiReflector& ui, const char* name, const OpData& schema, OpData& value)
{
    if (!ui.BeginRow(name)) return false;
    const OpData* address = value.Find("$component");
    const std::string preview = address ? ComponentReferenceLabel(schema, *address) : "(未設定)";
    bool changed = false;
    const bool open = ImGui::BeginCombo("##component", preview.c_str());
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    if (open) {
        if (ImGui::Selectable("(未設定)", !address)) { value = {}; changed = true; }
        const auto select = [&](const std::string& label, scene::GameObject& object, uint32_t kind, const std::string& savedId, const std::string& typeKey) {
            OpData candidate = OpData::MakeObject(); candidate.Set("kind", kind == UINT32_MAX ? "script" : "native");
            candidate.Set("ownerGuid", object.instanceId); candidate.Set("componentKind", static_cast<int64_t>(kind));
            candidate.Set("savedScriptId", savedId); candidate.Set("typeKey", typeKey);
            const std::string option = ComponentReferenceLabel(schema, candidate) + "##" + label;
            if (!ImGui::Selectable(option.c_str(), false)) return;
            value = OpData::MakeObject(); value.Set("$component", std::move(candidate)); changed = true;
        };
        if (m_context.activeScene) for (auto& object : m_context.activeScene->GameObjects()) {
            if (const OpData* native = schema.Find("nativeKinds"); native && native->IsArray()) for (const auto& candidate : native->AsArray()) {
                const OpData* number = candidate.Find("kind"); if (!number || !number->IsInt()) continue;
                const uint32_t kind = static_cast<uint32_t>(number->AsInt64());
                const std::string nativeName = Text(candidate, "nativeName");
                const auto* operations = scene::FindComponentOps(nativeName);
                if (kind != 0 && (!operations || !operations->has || !operations->has(object))) continue;
                if (!scene::ManagedScriptRuntime::Instance().ReferenceComponentLifetime(*m_context.activeScene, object.GetID(), kind)) continue;
                select(object.name + " / " + Text(candidate, "type") + "##" + object.instanceId + std::to_string(kind), object, kind, "", "");
            }
            if (const auto* scripts = object.GetComponent<scene::ScriptComponent>()) for (const auto& entry : scripts->scripts) {
                if (entry.m_pendingRemoval || !entry.script) continue;
                auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); if (!adapter) continue;
                OpData catalog; if (!ReadManagedSettingsJson(scene::ManagedScriptRuntime::Instance().GetTypeSchemaJson(adapter->GetTypeName()), catalog)) continue;
                const OpData* types = catalog.Find("assignableTypes"); const std::string target = Text(schema, "targetType");
                if (!types || !types->IsArray() || std::none_of(types->AsArray().begin(), types->AsArray().end(), [&](const OpData& type) { return type.IsString() && type.AsString() == target; })) continue;
                select(object.name + " / " + Text(catalog, "displayName") + " / " + entry.savedScriptId + "##" + entry.inspectionId,
                    object, UINT32_MAX, entry.savedScriptId, adapter->GetTypeName());
            }
        }
        ImGui::EndCombo();
    }
    if (hovered && !changed && address) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(preview.c_str());
        ImGui::Text("Object GUID: %s", Text(*address, "ownerGuid").c_str());
        if (Text(*address, "kind") == "script") ImGui::Text("Script ID: %s", Text(*address, "savedScriptId").c_str());
        else if (const OpData* kind = address->Find("componentKind"); kind && kind->IsInt()) ImGui::Text("Component kind: %lld", static_cast<long long>(kind->AsInt64()));
        ImGui::EndTooltip();
    }
    ui.EndRow(); return changed;
}

bool ManagedSettingsReflector::DrawTypedAssetReference(ImGuiReflector& ui, const char* name, int assetKind, OpData& value)
{
    if (!ui.BeginRow(name)) return false;
    const std::string original = value.IsString() ? value.AsString() : std::string{};
    std::string path = original.empty() ? std::string{} : asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(original));
    const char* extensions = assetKind == 0 ? ".mat" : assetKind == 1 ? ".png,.jpg,.jpeg,.dds,.tga,.bmp,.hdr" : ".fbx,.fzasset,.fzmodel,.obj,.gltf,.glb";
    if (!widgets::AssetPathField("##asset", path, extensions, asset::AssetDatabase::ProjectRoot())) { ui.EndRow(); return false; }
    bool accepted = path.empty();
    if (!path.empty()) {
        std::string absolute = path;
        if (!util::FileSystem::PathFromUtf8(path).is_absolute()) absolute = asset::AssetDatabase::ProjectRoot() + path;
        accepted = assetKind == 0 ? asset::AssetManager::Load<asset::MaterialAsset>(absolute).IsValid()
            : assetKind == 1 ? asset::AssetManager::Load<asset::TextureAsset>(absolute).IsValid()
            : asset::AssetManager::Load<asset::ModelAsset>(absolute).IsValid();
        if (accepted) {
            const std::string guid = asset::AssetDatabase::GuidFromPath(absolute); accepted = !guid.empty();
            if (accepted) value = OpData("guid:" + guid);
        }
    } else value = OpData("");
    if (!accepted) m_draft->diagnostic = assetKind == 0 ? "Material アセットを選択してください。"
        : assetKind == 1 ? "Texture アセットを選択してください。" : "Model アセットを選択してください。";
    ui.EndRow(); return accepted;
}

void DrawManagedDiagnostic(const char* title, std::string_view message, std::string_view secondary)
{
    ImGui::PushID(title);
    ImGui::TextColored({1.0f, 0.65f, 0.35f, 1.0f}, "%s", title);
    ImGui::SameLine();
    if (ImGui::TreeNode("詳細##managed_diagnostic")) {
        if (!message.empty()) ImGui::TextWrapped("%.*s", static_cast<int>(message.size()), message.data());
        if (!secondary.empty()) ImGui::TextWrapped("%.*s", static_cast<int>(secondary.size()), secondary.data());
        if (ImGui::SmallButton("コピー")) {
            std::string text(message);
            if (!secondary.empty()) { text += '\n'; text += secondary; }
            ImGui::SetClipboardText(text.c_str());
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void ManagedSettingsReflector::Draw()
{
    if (!m_adapter) return;
    CloseInactiveDraftSessions(m_draft->registryKey, m_draft->projectRoot);
    if (!m_draft->staleDrafts.empty()) {
        const std::string title = "未適用の下書き (" + std::to_string(m_draft->staleDrafts.size()) + ")##managed_stale_drafts";
        if (ImGui::TreeNode(title.c_str())) {
        for (const auto& stale : m_draft->staleDrafts) {
            if (!stale) continue;
            const std::string title = stale->target.type + " / " + stale->target.savedScriptId + " / generation " +
                std::to_string(stale->target.generation);
            if (ImGui::TreeNode(title.c_str())) {
                ImGui::TextWrapped("%s", stale->staleReason.c_str());
                for (const OpData& patch : stale->createPatches.AsArray()) {
                    std::string detail;
                    if (WriteOpDataJson(patch, detail)) ImGui::TextWrapped("%s", detail.c_str());
                }
                for (const auto& [path, value] : stale->values) {
                    std::string detail;
                    if (WriteOpDataJson(value, detail)) ImGui::TextWrapped("%s: %s", path.c_str(), detail.c_str());
                }
                ImGui::TreePop();
            }
        }
        std::string staleReview = CopyStaleDrafts();
        if (!staleReview.empty()) {
            ImGui::InputTextMultiline("##stale_draft_review", staleReview.data(), staleReview.size() + 1,
                ImVec2(-FLT_MIN, 120.0f), ImGuiInputTextFlags_ReadOnly);
        }
        if (ImGui::SmallButton("旧下書きをコピー")) {
            const std::string json = CopyStaleDrafts();
            if (json.empty()) m_draft->diagnostic = "旧下書きを JSON に変換できませんでした。入力は保持されています。";
            else ImGui::SetClipboardText(json.c_str());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("旧下書きを破棄")) DiscardStaleDrafts();
        ImGui::Separator();
        ImGui::TreePop();
        }
    }
    if (m_draft->revisionConflict) {
        DrawManagedDiagnostic("編集中に設定が変更されました", "下書きは保持しています。破棄して再取得するまで適用できません。");
        if (ImGui::Button("下書きを破棄して設定を再取得")) {
            if (m_draft->editSession) m_adapter->CloseSettingsEdit(m_draft->editSession);
            m_draft->editSession = 0; m_draft->activeId = 0; m_draft->revisionConflict = false;
            m_draft->values.clear(); m_draft->rebindReferences.clear(); m_draft->createPatches = OpData::MakeArray();
            m_draft->targetRevisions.clear(); m_draft->pages.clear(); m_draft->mixedPaths.clear();
            m_draft->pageCursors.clear(); m_draft->diagnostic.clear();
        }
    }
    uint64_t counter = 0;
    if (!m_adapter->ReadSettingsRevision(counter)) { ImGui::TextUnformatted("設定の世代を取得できません"); return; }
    std::vector<uint64_t> counters{counter};
    for (size_t index = 1; index < m_adapters.size(); ++index) {
        uint64_t selectedCounter = 0;
        if (!m_adapters[index] || !m_adapters[index]->ReadSettingsRevision(selectedCounter)) { ImGui::TextUnformatted("選択した Script 個体が削除されました"); return; }
        counters.push_back(selectedCounter);
    }
    m_draft->invalidatedPagePaths.clear();
    /// @note callback の直接 field 代入は revision counter を進めないため、表示ページは毎フレーム生値を読む。
    m_draft->pages.clear();
    m_draft->mixedPaths.clear();
    m_draft->targetCounters = std::move(counters);
    m_draft->settingsRevision = counter;
    const float width = ImGui::GetContentRegionAvail().x, fontSize = ImGui::GetFontSize();
    if (std::abs(width - m_draft->layoutWidth) > 0.5f || fontSize != m_draft->layoutFontSize ||
        m_draft->selectAdoptionPaths != m_draft->layoutAdoption) {
        m_draft->fieldHeights.clear();
        m_draft->layoutWidth = width; m_draft->layoutFontSize = fontSize; m_draft->layoutAdoption = m_draft->selectAdoptionPaths;
    }
    DrawFields(true);
    const bool hasRuntimeState = std::any_of(m_adapter->Fields().begin(), m_adapter->Fields().end(), [&](const auto& field) {
        const auto description = m_draft->schemas.find(field.name);
        return !(field.flags & scene::kManagedFieldSetting) && (field.flags & scene::kManagedFieldObservation) &&
            !(field.flags & scene::kManagedFieldHidden) && description != m_draft->schemas.end() && Boolean(description->second, "visible", true);
    });
    /// @note Observation is opt-in so closed runtime details never read or copy private caches.
    if (hasRuntimeState && ImGui::TreeNode("実行時の状態（読み取り専用）##managed_runtime_state")) {
        DrawFields(false);
        ImGui::TreePop();
    }
}

void ManagedSettingsReflector::DrawFields(bool settings)
{
    m_group.clear(); m_groupOpen = true;
    for (const auto& field : m_adapter->Fields()) {
        if (((field.flags & scene::kManagedFieldSetting) != 0) != settings ||
            !(field.flags & scene::kManagedFieldObservation) || (field.flags & scene::kManagedFieldHidden)) continue;
        const auto description = m_draft->schemas.find(field.name);
        if (description == m_draft->schemas.end() || !Boolean(description->second, "visible", true)) continue;
        const std::string group = Text(description->second, "group");
        if (group != m_group) { if (group.empty()) m_groupOpen = true; else Group(group.c_str()); m_group = group; }
        if (!m_groupOpen) continue;
        const std::string root = ManagedSettingChildPath("", field.name);
        if (!BeginVisiblePath(root)) continue;
        const float fieldY = ImGui::GetCursorPosY();
        const unsigned int activeBefore = ImGui::GetActiveID();
        const OpData* packet = ReadPage(root, 0, 0);
        if (!packet) { EndVisiblePath(root, fieldY, activeBefore); continue; }
        const OpData* value = packet->Find("value"); OpData summary;
        if (value) summary = *value;
        else { summary = OpData::MakeObject(); summary.Set("$deferred", root); summary.Set("kind", Text(*packet, "kind")); if (const OpData* total = packet->Find("total")) summary.Set("total", *total); }
        const OpData* schema = description->second.Find("value"); if (!schema) continue;
        const bool setting = m_settingKeys.contains(field.name);
        const std::string label = FieldLabel(field.label + (Boolean(description->second, "requiredReference") ? " *" : ""), description->second);
        BeginField(field.name.c_str(), label.c_str()); SetFieldReadOnly(!setting);
        if (const OpData* minimum = description->second.Find("minimum"); minimum && (minimum->IsInt() || minimum->IsFloat())) SetFieldMin(Number(*minimum));
        if (const OpData* maximum = description->second.Find("maximum"); maximum && (maximum->IsInt() || maximum->IsFloat())) SetFieldMax(Number(*maximum));
        DrawPath(root, label.c_str(), *schema, summary, setting); Tooltip(Text(description->second, "help").c_str()); EndField();
        EndVisiblePath(root, fieldY, activeBefore);
    }
}
void ManagedSettingsReflector::Finish()
{
    if (!m_draft->revisionConflict && !m_draft->commitAttempted &&
        m_draft->targetRevisions.size() == m_draft->targets.size() && !m_draft->createPatches.AsArray().empty() &&
        (m_draft->activeId == 0 || ImGui::GetActiveID() != m_draft->activeId) &&
        (m_draft->editSession != 0 || BeginEdit())) {
        std::vector<std::string> rebind(m_draft->rebindReferences.begin(), m_draft->rebindReferences.end()); std::sort(rebind.begin(), rebind.end());
        std::vector<ManagedSettingsEdit> edits;
        std::vector<const OpData*> textPatches;
        for (const OpData& patch : m_draft->createPatches.AsArray())
            if (Text(patch, "action") == "text_replace") textPatches.push_back(&patch);
        std::stable_sort(textPatches.begin(), textPatches.end(), [](const OpData* left, const OpData* right) {
            if (Text(*left, "path") != Text(*right, "path")) return Text(*left, "path") < Text(*right, "path");
            const auto* leftIndex = left->Find("index"); const auto* rightIndex = right->Find("index");
            return leftIndex && rightIndex && leftIndex->IsInt() && rightIndex->IsInt()
                ? leftIndex->AsInt64() > rightIndex->AsInt64() : false;
        });
        OpData orderedPatches = OpData::MakeArray();
        std::size_t nextTextPatch = 0;
        for (const OpData& patch : m_draft->createPatches.AsArray()) {
            if (Text(patch, "action") == "text_replace") orderedPatches.Push(*textPatches[nextTextPatch++]);
            else orderedPatches.Push(patch);
        }
        for (size_t index = 0; index < m_draft->targets.size(); ++index) {
            ManagedSettingsEdit edit; edit.target = m_draft->targets[index]; edit.patches = orderedPatches; edit.rebindReferences = rebind;
            edit.expectedRevision = m_draft->targetRevisions[index]; edit.editSession = index == 0 ? m_draft->editSession : 0;
            edits.push_back(std::move(edit));
        }
        const uint64_t submittedSession = m_draft->editSession;
        const OpResult result = ApplyManagedInspectorBatch(m_context, edits);
        m_draft->activeId = 0;
        m_draft->editSession = 0;
        if (result.ok) {
            m_draft->values.clear(); m_draft->rebindReferences.clear(); m_draft->createPatches = OpData::MakeArray();
            m_draft->pages.clear(); m_draft->mixedPaths.clear(); m_draft->targetRevisions.clear(); m_draft->diagnostic.clear(); m_draft->commitAttempted = false;
        } else {
            if (submittedSession) m_adapter->CloseSettingsEdit(submittedSession);
            m_draft->revisionConflict = result.errorCode == "STALE_INSPECTOR_DRAFT" || result.errorCode == "STALE_PLAN" ||
                result.errorCode == "SETTINGS_CONFLICT" || result.message.find("SETTINGS_CONFLICT") != std::string::npos;
            m_draft->commitAttempted = true;
            m_draft->diagnostic = result.message;
            FBZZ_LOG_WARN("C# Inspector: %s", result.message.c_str());
        }
    }
    if (!m_draft->diagnostic.empty()) DrawManagedDiagnostic("設定の状態", m_draft->diagnostic);
    if (m_context.playMode && !m_context.playMode->IsInEditor()) {
        m_draft->selectAdoptionPaths = ImGui::TreeNode("Play 設定の保持##managed_adoption");
        if (!m_draft->selectAdoptionPaths) return;
        ImGui::TextDisabled("保持する設定を選択 (%zu 項目)", m_draft->adoptionPaths.size());
        ImGui::BeginDisabled(m_draft->editSession != 0 || m_draft->adoptionPaths.empty());
        if (ImGui::Button("選択した Play 設定を保持")) {
            std::vector<std::string> selected(m_draft->adoptionPaths.begin(), m_draft->adoptionPaths.end()); std::sort(selected.begin(), selected.end());
            OpData paths = OpData::MakeArray(); for (const auto& path : selected) paths.Push(path);
            OpData targets = OpData::MakeArray();
            for (const auto& target : m_draft->targets) { OpData entry = OpData::MakeObject(); entry.Set("ownerGuid", target.objectId); entry.Set("savedScriptId", target.savedScriptId); entry.Set("paths", paths); targets.Push(entry); }
            OpData settings = OpData::MakeObject(); settings.Set("targets", std::move(targets)); OpArgs args; args.Set("settings", settings);
            const auto result = InvokeOperator(m_context, "script.play_changes.capture", args);
            m_draft->diagnostic = result.ok ? "保持しました。Stop で元の Edit に戻してから確認・採用できます。" : result.message;
        }
        ImGui::EndDisabled();
        ImGui::TreePop();
    }
}
} /// @note namespace fbzz::editor
