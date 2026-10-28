/// @file    ScriptConfigureOperators.cpp
/// @brief   名前による一括 Script 設定、事前検証、適用と Undo。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <Editor/Op/ScriptConfigure.hpp>
#include <Editor/Ai/Json.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/PlaySettingsAdoption.hpp>
#include <Editor/Util/ManagedSettingsCodec.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/Uuid.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>

namespace fbzz::editor {
namespace {
using scene::GameObject;
using scene::ManagedScriptAdapter;
using scene::ManagedScriptRuntime;

bool Fail(std::string& diagnostic, std::string text)
{
    diagnostic = std::move(text);
    return false;
}

bool Keys(const OpData& object, std::initializer_list<std::string_view> allowed, std::string& diagnostic)
{
    if (!object.IsObject()) return Fail(diagnostic, "オブジェクト形式で指定してください");
    for (const auto& [key, value] : object.AsObject()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            return Fail(diagnostic, "不明な指定です: " + key);
    }
    return true;
}

std::string Text(const OpData& object, std::string_view key, std::string fallback = {})
{
    const OpData* value = object.Find(key);
    return value && value->IsString() ? value->AsString() : std::move(fallback);
}

/// @note 子の登録順の preorder。深さに比例する C++ call stack を使わず、欠損・重複・循環を拒否する。
bool Walk(scene::Scene& scene, const std::vector<GameObject*>& roots,
    std::vector<GameObject*>& output, std::string& diagnostic)
{
    std::vector<GameObject*> stack(roots.rbegin(), roots.rend());
    std::unordered_set<GameObject*> seen;
    while (!stack.empty()) {
        GameObject* object = stack.back();
        stack.pop_back();
        if (!object || !object->IsValid() || object->GetScene() != &scene || !seen.insert(object).second
            || seen.size() > scene::Scene::MAX_ENTITIES)
            return Fail(diagnostic, "階層に欠損、重複または循環があります");
        output.push_back(object);
        for (int i = object->GetChildCount(); i > 0; --i) {
            GameObject* child = object->GetChild(i - 1);
            if (!child || child->GetParent() != object)
                return Fail(diagnostic, "親子リンクが一致しません: " + object->name);
            stack.push_back(child);
        }
    }
    return true;
}

bool SceneObjects(scene::Scene& scene, std::vector<GameObject*>& output, std::string& diagnostic)
{
    if (!Walk(scene, scene.GetRootGameObjects(), output, diagnostic)) return false;
    std::size_t count = 0;
    for (auto& object : scene.GameObjects()) { (void)object; ++count; }
    return count == output.size() || Fail(diagnostic, "ルートから辿れないオブジェクトがあります");
}

bool Active(GameObject& object)
{
    for (GameObject* cursor = &object; cursor; cursor = cursor->GetParent())
        if (!cursor->activeSelf()) return false;
    return true;
}

OpData ObjectSummary(GameObject& object)
{
    OpData data = OpData::MakeObject();
    data.Set("name", object.name);
    data.Set("id", object.instanceId);
    OpData path = OpData::MakeArray();
    std::vector<std::string> segments;
    std::unordered_set<GameObject*> seen;
    for (GameObject* cursor = &object; cursor && seen.insert(cursor).second; cursor = cursor->GetParent())
        segments.push_back(cursor->name);
    for (auto i = segments.rbegin(); i != segments.rend(); ++i) path.Push(*i);
    data.Set("path", std::move(path));
    return data;
}

bool Unique(std::vector<GameObject*> matches, std::vector<GameObject*>& output, std::string& diagnostic)
{
    if (matches.size() == 1) { output.push_back(matches.front()); return true; }
    if (matches.empty()) return Fail(diagnostic, "対象が見つかりません");
    OpData candidates = OpData::MakeArray();
    for (GameObject* match : matches) candidates.Push(ObjectSummary(*match));
    std::string json;
    if (!WriteOpDataJson(candidates, json)) return Fail(diagnostic, "候補を表示できません");
    return Fail(diagnostic, "同名の対象があります。root または path で範囲を絞ってください: " + json);
}

bool PathSegments(const OpData& value, std::vector<std::string>& parts, std::string& diagnostic)
{
    if (value.IsArray()) {
        for (const OpData& part : value.AsArray()) {
            if (!part.IsString() || part.AsString().empty()) return Fail(diagnostic, "path の各要素は空でない名前です");
            parts.push_back(part.AsString());
        }
    } else if (value.IsString()) {
        const std::string& path = value.AsString();
        std::size_t begin = path.starts_with('/') ? 1 : 0;
        while (begin < path.size()) {
            std::size_t end = path.find('/', begin);
            if (end == std::string::npos) end = path.size();
            if (end == begin) return Fail(diagnostic, "空の階層名があります");
            parts.push_back(path.substr(begin, end - begin));
            begin = end + 1;
        }
    } else return Fail(diagnostic, "path は文字列または名前の配列です");
    return !parts.empty() || Fail(diagnostic, "path が空です");
}

bool Resolve(scene::Scene& scene, const OpData& selector, GameObject* owner,
    std::vector<GameObject*>& output, std::string& diagnostic, unsigned nesting)
{
    if (nesting > 32) return Fail(diagnostic, "selector の入れ子が深すぎます");
    if (selector.IsArray()) {
        for (const OpData& element : selector.AsArray())
            if (!Resolve(scene, element, owner, output, diagnostic, nesting + 1)) return false;
        return true;
    }
    OpData normalized = selector;
    if (selector.IsString()) {
        normalized = OpData::MakeObject();
        if (selector.AsString() == "$owner") {
            if (!owner) return Fail(diagnostic, "$owner は設定フィールド内で使用してください");
            output.push_back(owner);
            return true;
        }
        normalized.Set(selector.AsString().find('/') == std::string::npos ? "name" : "path", selector);
    }
    if (!Keys(normalized, {"name", "path", "root", "scope", "tag", "includeInactive", "id"}, diagnostic)) return false;
    for (std::string_view key : {"name", "scope", "tag", "id"}) {
        const OpData* value = normalized.Find(key);
        if (value && !value->IsString()) return Fail(diagnostic, std::string(key) + " は文字列です");
    }
    const OpData* includeInactive = normalized.Find("includeInactive");
    if (includeInactive && !includeInactive->IsBool()) return Fail(diagnostic, "includeInactive は bool です");
    const bool include = !includeInactive || includeInactive->AsBool();
    const OpData* rootSpec = normalized.Find("root");
    GameObject* root = nullptr;
    if (rootSpec) {
        std::vector<GameObject*> roots;
        if (!Resolve(scene, *rootSpec, owner, roots, diagnostic, nesting + 1)) return false;
        if (roots.size() != 1) return Fail(diagnostic, "root は一件だけ指定してください");
        root = roots.front();
    }
    if (const OpData* id = normalized.Find("id")) {
        if (normalized.AsObject().size() != 1) return Fail(diagnostic, "id と他の条件は併用できません");
        std::vector<GameObject*> candidates;
        if (!SceneObjects(scene, candidates, diagnostic)) return false;
        std::vector<GameObject*> matches;
        for (GameObject* object : candidates)
            if (object->instanceId == id->AsString()) matches.push_back(object);
        if (matches.size() > 1) return Fail(diagnostic, "同じ id を持つ対象が複数あります: " + id->AsString());
        return Unique(std::move(matches), output, diagnostic);
    }
    if (const OpData* path = normalized.Find("path")) {
        if (normalized.Find("name") || normalized.Find("tag") || normalized.Find("scope"))
            return Fail(diagnostic, "path と name/tag/scope は併用できません");
        std::vector<std::string> parts;
        if (!PathSegments(*path, parts, diagnostic)) return false;
        std::vector<GameObject*> level = root ? std::vector<GameObject*>{} : scene.GetRootGameObjects();
        if (root) for (int i = 0; i < root->GetChildCount(); ++i) level.push_back(root->GetChild(i));
        for (std::size_t index = 0; index < parts.size(); ++index) {
            std::vector<GameObject*> matches;
            for (GameObject* object : level)
                if (object->name == parts[index] && (include || Active(*object))) matches.push_back(object);
            std::vector<GameObject*> unique;
            if (!Unique(std::move(matches), unique, diagnostic)) return false;
            if (index + 1 == parts.size()) {
                output.push_back(unique.front());
                return true;
            }
            level.clear();
            for (int i = 0; i < unique.front()->GetChildCount(); ++i) level.push_back(unique.front()->GetChild(i));
        }
    }
    const std::string scope = Text(normalized, "scope", root ? "descendants" : "scene");
    std::vector<GameObject*> candidates;
    if (scope == "scene") {
        if (root) return Fail(diagnostic, "scene scope に root は指定できません");
        if (!SceneObjects(scene, candidates, diagnostic)) return false;
    } else {
        if (!root) return Fail(diagnostic, "children/descendants/self には root が必要です");
        if (scope == "self") candidates.push_back(root);
        else if (scope == "children") {
            for (int i = 0; i < root->GetChildCount(); ++i) candidates.push_back(root->GetChild(i));
        } else if (scope == "descendants") {
            std::vector<GameObject*> children;
            for (int i = 0; i < root->GetChildCount(); ++i) children.push_back(root->GetChild(i));
            if (!Walk(scene, children, candidates, diagnostic)) return false;
        } else return Fail(diagnostic, "scope は scene/self/children/descendants です");
    }
    std::vector<GameObject*> matches;
    const OpData* name = normalized.Find("name");
    const OpData* tag = normalized.Find("tag");
    if (!name && !tag && !normalized.Find("scope")) return Fail(diagnostic, "name/path/tag/scope を指定してください");
    for (GameObject* object : candidates)
        if ((!name || name->AsString() == object->name) && (!tag || tag->AsString() == object->tag) && (include || Active(*object)))
            matches.push_back(object);
    if (name) return Unique(std::move(matches), output, diagnostic);
    output.insert(output.end(), matches.begin(), matches.end());
    return true;
}

bool ReadSettings(ManagedScriptAdapter& script, OpData& values, std::string& json, OpData* snapshot = nullptr)
{
    std::string wire;
    OpData state;
    if (!script.ReadSettingsSnapshot(wire) || !ReadManagedSettingsJson(wire, state)) return false;
    const OpData* settings = state.Find("settings");
    if (!settings || !settings->IsObject()) return false;
    values = *settings;
    if (snapshot) *snapshot = std::move(state);
    return WriteOpDataJson(values, json);
}

bool SettingsOperation(scene::ManagedBridgeOp op, uint64_t generation, uint64_t token,
    std::string_view input = {}, std::string* output = nullptr, uint64_t* preparedToken = nullptr,
    const std::vector<ManagedScriptAdapter*>* adapters = nullptr)
{
    scene::ManagedBridgeRequest request;
    request.op = op; request.generation = generation; request.integer = static_cast<int64_t>(token);
    request.input = input.data(); request.inputBytes = static_cast<uint32_t>(input.size());
    scene::ManagedBridgeReply reply;
    const bool succeeded = adapters ? ManagedScriptRuntime::Instance().DispatchPreparedSettings(*adapters, request, reply)
        : ManagedScriptRuntime::Instance().Dispatch(request, reply, output);
    if (!succeeded) return false;
    if (preparedToken) *preparedToken = static_cast<uint64_t>(reply.integer);
    return true;
}
/// @note SHA-256 is computed per bounded upload packet; complete settings are never buffered by this hash helper.
/// @see https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptcreatehash CNG hash handle ownership.
bool SettingsChunkHash(std::string_view input, std::string& output)
{
    struct Handles {
        BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
        ~Handles() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    } handles;
    if (BCryptOpenAlgorithmProvider(&handles.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
        || BCryptCreateHash(handles.algorithm, &handles.hash, nullptr, 0, nullptr, 0, 0) < 0) return false;
    std::vector<uint8_t> bytes(input.begin(), input.end());
    if (BCryptHashData(handles.hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0) < 0) return false;
    std::array<uint8_t, 32> digest{};
    if (BCryptFinishHash(handles.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return false;
    constexpr char HEX[] = "0123456789abcdef"; output.clear();
    for (uint8_t value : digest) { output += HEX[value >> 4]; output += HEX[value & 15]; }
    return true;
}
bool StageSettingsInput(uint64_t generation, uint64_t session, std::string_view input)
{
    constexpr size_t packetBytes = 65536;
    for (size_t offset = 0; offset < input.size();) {
        const size_t count = std::min(packetBytes, input.size() - offset); const std::string_view chunk = input.substr(offset, count);
        std::string hash; if (!SettingsChunkHash(chunk, hash)) return false;
        std::vector<uint8_t> bytes(chunk.begin(), chunk.end());
        OpData packet = OpData::MakeObject(); packet.Set("offset", static_cast<int64_t>(offset));
        packet.Set("data", ai::Base64Encode(bytes)); packet.Set("sha256", hash); packet.Set("final", offset + count == input.size());
        std::string json;
        if (!WriteOpDataJson(packet, json) || !SettingsOperation(scene::ManagedBridgeOp::STAGE_SETTINGS_EDIT, generation, session, json)) return false;
        offset += count;
    }
    return !input.empty();
}
bool ReferenceValue(scene::Scene& scene, GameObject& owner, const OpData& request,
    bool array, OpData& value, std::string& diagnostic)
{
    if (!array && request.IsNull()) { value = {}; return true; }
    if (!array && request.IsObject() && request.Find("$go")) {
        if (!Keys(request, {"$go"}, diagnostic) || !request.Find("$go")->IsString()) return Fail(diagnostic, "$go は保存 GUID の文字列です");
        value = request;
        return true;
    }
    if (array && request.IsArray()) {
        value = OpData::MakeArray();
        for (const OpData& item : request.AsArray()) {
            OpData resolved;
            if (!ReferenceValue(scene, owner, item, false, resolved, diagnostic)) return false;
            value.Push(std::move(resolved));
        }
        return true;
    }
    /// @note 明示的な保存 GUID は欠損参照の維持にだけ使う。通常の記述では名前を解決する。
    if (!array && request.IsObject() && request.Find("guid")) {
        if (!Keys(request, {"guid"}, diagnostic) || !request.Find("guid")->IsString()) return Fail(diagnostic, "guid は文字列です");
        value = request.Find("guid")->AsString().empty() ? OpData{} : *request.Find("guid");
        return true;
    }
    std::vector<GameObject*> objects;
    if (!Resolve(scene, request, &owner, objects, diagnostic, 0)) return false;
    if (!array && objects.size() != 1) return Fail(diagnostic, "単一 GameObject 参照には一件だけ指定してください");
    value = array ? OpData::MakeArray() : OpData(objects.front()->instanceId);
    if (array) for (GameObject* object : objects) value.Push(object->instanceId);
    return true;
}

bool ArrayEdit(const OpData& request, const OpData& before, OpData& after, std::string& diagnostic)
{
    if (!before.IsArray() || !Keys(request, {"edit", "values", "indices"}, diagnostic)) return Fail(diagnostic, "配列編集の指定が不正です");
    const std::string edit = Text(request, "edit");
    const OpData* values = request.Find("values");
    const OpData* indices = request.Find("indices");
    after = OpData::MakeArray();
    if (edit == "replace" || edit == "append") {
        if (!values || !values->IsArray() || indices) return Fail(diagnostic, "replace/append は values 配列を指定してください");
        if (edit == "append") for (const OpData& item : before.AsArray()) after.Push(item);
        for (const OpData& item : values->AsArray()) after.Push(item);
        return true;
    }
    if ((edit != "remove" && edit != "reorder") || !indices || !indices->IsArray() || values)
        return Fail(diagnostic, "remove/reorder は indices 配列を指定してください");
    std::set<int> seen;
    for (const OpData& item : indices->AsArray()) {
        int index = 0;
        if (!item.TryGetInt32(index) || index < 0 || static_cast<std::size_t>(index) >= before.AsArray().size()
            || !seen.insert(index).second) return Fail(diagnostic, "indices に範囲外または重複があります");
        if (edit == "reorder") after.Push(before.AsArray()[static_cast<std::size_t>(index)]);
    }
    if (edit == "reorder" && seen.size() != before.AsArray().size()) return Fail(diagnostic, "reorder は全要素を一回ずつ指定してください");
    if (edit == "remove") for (std::size_t i = 0; i < before.AsArray().size(); ++i)
        if (!seen.contains(static_cast<int>(i))) after.Push(before.AsArray()[i]);
    return true;
}

bool VectorValue(OpData& value, std::string& diagnostic)
{
    if (!value.IsObject()) return true;
    if (!Keys(value, {"x", "y", "z"}, diagnostic)) return false;
    math::Vector3 vector;
    float* coordinates[] = {&vector.x, &vector.y, &vector.z};
    const char* keys[] = {"x", "y", "z"};
    for (int i = 0; i < 3; ++i) {
        const OpData* coordinate = value.Find(keys[i]);
        if (!coordinate || (!coordinate->IsInt() && !coordinate->IsFloat())) return Fail(diagnostic, "Vector3 は x/y/z の数値または三要素配列です");
        if (coordinate->IsInt()) {
            *coordinates[i] = static_cast<float>(coordinate->AsInt64());
        } else if (!coordinate->TryGetFloat(*coordinates[i])) {
            return Fail(diagnostic, "Vector3 の値は有限の float 範囲である必要があります");
        }
    }
    value = OpData(vector);
    return true;
}

bool NormalizeFields(scene::Scene& scene, GameObject& owner, ManagedScriptAdapter& script,
    const OpData& fields, OpData& settings, std::string& diagnostic)
{
    if (!fields.IsObject()) return Fail(diagnostic, "fields はフィールド名と値のオブジェクトです");
    for (const auto& [key, request] : fields.AsObject()) {
        const auto at = std::find_if(script.Fields().begin(), script.Fields().end(), [&](const auto& field) { return field.name == key; });
        if (at == script.Fields().end() || !(at->flags & scene::kManagedFieldSetting)) return Fail(diagnostic, "設定可能なフィールドがありません: " + key);
        OpData value = request;
        const bool object = at->kind == scene::ManagedValueKind::GAME_OBJECT;
        const bool objectArray = at->kind == scene::ManagedValueKind::GAME_OBJECT_ARRAY;
        if (request.IsObject() && request.Find("edit")) {
            const OpData* before = settings.Find(key);
            OpData editRequest = request;
            if (objectArray && request.Find("values")) {
                OpData refs;
                if (!ReferenceValue(scene, owner, *request.Find("values"), true, refs, diagnostic)) return false;
                editRequest.Set("values", std::move(refs));
            }
            if (!before || !ArrayEdit(editRequest, *before, value, diagnostic)) return false;
        } else if (object || objectArray) {
            if (!ReferenceValue(scene, owner, request, objectArray, value, diagnostic)) return false;
        }
        if (at->kind == scene::ManagedValueKind::VECTOR3 && !VectorValue(value, diagnostic)) return false;
        if (at->kind == scene::ManagedValueKind::VECTOR3_ARRAY && value.IsArray()) {
            OpData normalized = OpData::MakeArray();
            for (OpData item : value.AsArray()) {
                if (!VectorValue(item, diagnostic)) return false;
                normalized.Push(std::move(item));
            }
            value = std::move(normalized);
        }
        settings.Set(key, std::move(value));
    }
    return true;
}

bool WithinSettingPath(std::string_view path, std::string_view root)
{ return path == root || (path.starts_with(root) && path.size() > root.size() && path[root.size()] == '/'); }

/// @note Array edit indices identify original slots; move the snapshot's exact tokens with those slots, including duplicate GUIDs.
bool AppendArrayReferenceMoves(const OpData& fields, const OpData& snapshot, OpData& moves, std::string& diagnostic)
{
    const OpData* references = snapshot.Find("references"), *settings = snapshot.Find("settings");
    if (!references || !settings) return Fail(diagnostic, "参照 snapshot がありません");
    for (const auto& [key, request] : fields.AsObject()) {
        const std::string edit = Text(request, "edit");
        if (edit != "remove" && edit != "reorder") continue;
        const OpData* indices = request.Find("indices"), *before = settings->Find(key);
        if (!indices || !indices->IsArray() || !before || !before->IsArray()) return false;
        std::vector<int> origin;
        std::set<int> removed;
        for (const OpData& index : indices->AsArray()) {
            int value = 0;
            if (!index.TryGetInt32(value)) return false;
            if (edit == "reorder") origin.push_back(value); else removed.insert(value);
        }
        if (edit == "remove") for (std::size_t index = 0; index < before->AsArray().size(); ++index)
            if (!removed.contains(static_cast<int>(index))) origin.push_back(static_cast<int>(index));
        const std::string root = ManagedSettingChildPath("", key);
        for (std::size_t index = 0; index < origin.size(); ++index) {
            const std::string from = root + '/' + std::to_string(origin[index]), to = root + '/' + std::to_string(index);
            if (from == to) continue;
            for (const auto& [path, token] : references->AsObject()) if (WithinSettingPath(path, from)) {
                (void)token;
                OpData move = OpData::MakeObject(); move.Set("fromPath", path); move.Set("toPath", to + path.substr(from.size()));
                moves.Push(std::move(move));
            }
        }
    }
    return true;
}

std::string Handle(const scene::ScriptEntry& entry)
{
    return entry.inspectionId + "/" + (entry.script ? entry.script->InspectionId() : "missing");
}

scene::ScriptEntry* Entry(GameObject& object, const std::string& handle)
{
    auto* component = object.GetComponent<scene::ScriptComponent>();
    if (component) for (auto& entry : component->scripts) if (Handle(entry) == handle) return &entry;
    return nullptr;
}

bool Fingerprint(scene::Scene& scene, std::string& value, std::string& diagnostic)
{
    std::vector<GameObject*> objects;
    if (!SceneObjects(scene, objects, diagnostic)) return false;
    OpData data = OpData::MakeArray();
    for (GameObject* object : objects) {
        OpData node = OpData::MakeObject();
        node.Set("name", object->name);
        node.Set("id", object->instanceId);
        node.Set("parent", object->GetParent() ? object->GetParent()->instanceId : std::string{});
        node.Set("tag", object->tag);
        node.Set("active", object->activeSelf());
        node.Set("prefab", object->prefabAssetPath);
        node.Set("prefabSource", object->prefabSourceId);
        node.Set("entityIndex", std::to_string(object->GetID().index));
        node.Set("entityGeneration", std::to_string(object->GetID().generation));
        OpData scripts = OpData::MakeArray();
        if (auto* component = object->GetComponent<scene::ScriptComponent>())
            for (auto& entry : component->scripts) scripts.Push(Handle(entry));
        node.Set("scripts", std::move(scripts));
        data.Push(std::move(node));
    }
    return WriteOpDataJson(data, value);
}

struct Target {
    scene::EntityID id;
    std::string handle;
    std::string entryId;
    std::string savedScriptId = util::GenerateUUID();
    bool hadComponent = false;
    bool add = false;
    bool enabled = true;
    std::string ownerGuid;
    std::string before;
    std::string beforeRevision;
    std::string afterRevision;
    uint64_t instanceId = 0;
    uint64_t editSession = 0;
    std::size_t changedPathCount = 0;
    std::unique_ptr<scene::Script> candidate;
    OpData summary;
};

struct ReceiptState { bool undone = false; };
struct Plan {
    ~Plan() {
        if (hostToken) SettingsOperation(scene::ManagedBridgeOp::DISCARD_SETTINGS, generation, hostToken);
        for (const auto& target : targets) if (target.editSession) SettingsOperation(scene::ManagedBridgeOp::CLOSE_SETTINGS_EDIT, generation, target.editSession);
        for (auto& target : targets) if (target.candidate) target.candidate->SetContext(nullptr, nullptr);
    }
    uint64_t hostToken = 0;
    std::string token;
    std::string request;
    scene::Scene* scene = nullptr;
    uint64_t lifetime = 0;
    uint64_t generation = 0;
    bool temporary = false;
    std::string fingerprint;
    std::string type;
    std::vector<Target> targets;
    bool applied = false;
    bool undone = false;
    std::shared_ptr<ReceiptState> receiptState;
    std::weak_ptr<PlaySettingsAdoptionDraft> adoption;
    bool isAdoption = false;
};

struct Receipt {
    std::string token;
    std::weak_ptr<Plan> history;
    std::shared_ptr<ReceiptState> state;
    OpData description;
    bool temporary = false;
};

struct Plans {
    std::string session = util::GenerateUUID();
    uint64_t next = 0;
    std::deque<std::shared_ptr<Plan>> prepared;
    std::deque<Receipt> receipts;
};

bool Editable(const OpContext& c)
{
    return c.ctx.activeScene && !c.ctx.managedBuildBusy && c.undo.IsRecordingEnabled()
        && (!c.ctx.playMode || (c.ctx.playMode->IsInEditor() && !c.ctx.playMode->HasPendingRestore()) ||
            (c.ctx.playMode->CanEditTemporaryScene(c.ctx.activeScene) && &c.undo == c.ctx.playMode->TemporaryUndo()));
}

bool SameScene(EditorContext& ctx, const Plan& plan)
{
    const bool temporary = ctx.playMode && !ctx.playMode->IsInEditor();
    return temporary == plan.temporary && ctx.activeScene == plan.scene && ctx.activeScene
        && ctx.activeScene->GetRenderSceneGeneration() == plan.lifetime
        && ManagedScriptRuntime::Instance().GetAppliedGeneration() == plan.generation;
}

OpData Describe(const Plan& plan, bool replay = false)
{
    OpData data = OpData::MakeObject();
    data.Set("plan", plan.token);
    data.Set("script", plan.type);
    data.Set("applied", plan.applied);
    data.Set("replayed", replay);
    data.Set("undone", plan.undone);
    data.Set("totalTargets", static_cast<int64_t>(plan.targets.size()));
    data.Set("detailsDeferred", true);
    data.Set("reviewOperation", "script.configure.review");
    /// @note candidate と Undo の完全値は Host に保持し、通常応答は最初の bounded review page だけを返す。
    constexpr size_t wireBytes = 8192;
    std::string reviewJson;
    if (SettingsOperation(scene::ManagedBridgeOp::GET_PREPARED_REVIEW, plan.generation, plan.hostToken,
        R"({"mode":"targets","targetCount":16,"changeCount":16,"wireBytes":7168})", &reviewJson)) {
        OpData review;
        if (ReadManagedSettingsJson(reviewJson, review)) data.Set("review", std::move(review));
    } else data.Set("reviewError", "詳細は script.configure.review で wireBytes を指定して取得してください");
    std::string packet;
    if (!WriteOpDataJson(data, packet) || packet.size() > wireBytes) {
        /// @note 外側の plan metadata も含めて予算を守る。詳細を静かに切り捨てず別 query を案内する。
        data = OpData::MakeObject(); data.Set("plan", plan.token); data.Set("applied", plan.applied);
        data.Set("replayed", replay); data.Set("undone", plan.undone);
        data.Set("totalTargets", static_cast<int64_t>(plan.targets.size())); data.Set("detailsDeferred", true);
        data.Set("reviewOperation", "script.configure.review");
        data.Set("reviewError", "最初の page を含めた metadata が応答予算を超えました。詳細 query の wireBytes を増やしてください");
    }
    return data;
}

OpResult Review(OpContext& context, const std::string& token, const OpData& options, Plans& plans)
{
    std::shared_ptr<Plan> plan;
    for (const auto& prepared : plans.prepared) if (prepared->token == token) { plan = prepared; break; }
    if (!plan) for (const auto& receipt : plans.receipts)
        if (receipt.token == token) { plan = receipt.history.lock(); break; }
    if (!plan) return OpResult::Err("EXPIRED_REVIEW", "preview または Undo の保持値は解放済みです。新しい preview を作成してください");
    if (!SameScene(context.ctx, *plan)) return OpResult::Err("STALE_REVIEW", "Scene または managed 世代が変わりました");
    if (!options.IsObject()) return OpResult::Err("BAD_REVIEW", "options は対象・変更・値の page 指定です");
    std::string input, output;
    if (!WriteOpDataJson(options, input)) return OpResult::Err("BAD_REVIEW", "Review 指定を JSON へ変換できません");
    if (!SettingsOperation(scene::ManagedBridgeOp::GET_PREPARED_REVIEW, plan->generation, plan->hostToken, input, &output))
        return OpResult::Err("REVIEW_FAILED", ManagedScriptRuntime::Instance().LastDiagnostic());
    OpData result;
    if (!ReadManagedSettingsJson(output, result)) return OpResult::Err("BAD_REVIEW", "Review page の応答が不正です");
    return OpResult::Data(std::move(result));
}

bool LoadPreparedPlan(Plan& plan, std::string& diagnostic)
{
    std::size_t offset = 0;
    while (offset < plan.targets.size()) {
        const std::string options = "{\"mode\":\"plan\",\"targetOffset\":" + std::to_string(offset)
            + ",\"targetCount\":16,\"wireBytes\":7168}";
        std::string pageJson; OpData page;
        if (!SettingsOperation(scene::ManagedBridgeOp::GET_PREPARED_REVIEW, plan.generation, plan.hostToken, options, &pageJson)
            || !ReadManagedSettingsJson(pageJson, page))
            return Fail(diagnostic, "C# 候補 metadata の取得: " + ManagedScriptRuntime::Instance().LastDiagnostic());
        int total = -1, first = -1, returned = -1;
        const OpData* rows = page.Find("targets"), *totalValue = page.Find("totalTargets"), *offsetValue = page.Find("targetOffset"), *countValue = page.Find("returnedTargets");
        if (!rows || !rows->IsArray() || !totalValue || !totalValue->TryGetInt32(total) || total != static_cast<int>(plan.targets.size())
            || !offsetValue || !offsetValue->TryGetInt32(first) || first != static_cast<int>(offset)
            || !countValue || !countValue->TryGetInt32(returned) || returned <= 0 || returned != static_cast<int>(rows->AsArray().size())
            || offset + static_cast<std::size_t>(returned) > plan.targets.size())
            return Fail(diagnostic, "候補 metadata page の範囲または shape が一致しません");
        for (int rowIndex = 0; rowIndex < returned; ++rowIndex) {
            Target& target = plan.targets[offset + static_cast<std::size_t>(rowIndex)];
            const OpData& row = rows->AsArray()[static_cast<std::size_t>(rowIndex)];
            int index = -1, changedCount = -1;
            const OpData* indexValue = row.Find("targetIndex"), *changesValue = row.Find("changedPathCount");
            if (!indexValue || !indexValue->TryGetInt32(index) || index != static_cast<int>(offset) + rowIndex
                || Text(row, "instanceId") != std::to_string(target.instanceId)
                || !changesValue || !changesValue->TryGetInt32(changedCount) || changedCount < 0
                || Text(row, "ownerGuid") != target.ownerGuid
                || Text(row, "savedScriptId") != target.savedScriptId
                || Text(row, "action") != (target.add ? "add" : "update"))
                return Fail(diagnostic, "候補 metadata が exact target と一致しません");
            const std::string beforeRevision = Text(row, "beforeRevision");
            target.afterRevision = Text(row, "afterRevision");
            if (beforeRevision.empty() || target.afterRevision.empty()
                || (!target.beforeRevision.empty() && target.beforeRevision != beforeRevision))
                return Fail(diagnostic, "候補 revision が元の exact target と一致しません");
            target.beforeRevision = beforeRevision;
            target.changedPathCount = static_cast<std::size_t>(changedCount);
            target.summary.Set("changedPathCount", static_cast<int64_t>(target.changedPathCount));
        }
        offset += static_cast<std::size_t>(returned);
    }
    for (Target& target : plan.targets) { target.before.clear(); target.before.shrink_to_fit(); }
    return true;
}

OpResult Preview(OpContext& c, const OpData& request, Plans& plans)
{
    std::string diagnostic;
    if (!Keys(request, {"objects", "targets", "domain", "script", "fields", "patches", "mode", "scriptId", "scriptIndex", "rebindReferences", "referenceMoves", "expectedRevision", "editSession", "action"}, diagnostic))
        return OpResult::Err("BAD_PLAN", diagnostic);
    const OpData* objects = request.Find("objects");
    const OpData* script = request.Find("script");
    const OpData* fields = request.Find("fields");
    const OpData* exactTargets = request.Find("targets");
    if ((!objects && !exactTargets) || (objects && exactTargets) || !script || !script->IsString() || !fields || !fields->IsObject())
        return OpResult::Err("BAD_PLAN", "objects、script クラス名、fields を指定してください");
    if (exactTargets && (!exactTargets->IsArray() || exactTargets->AsArray().empty()))
        return OpResult::Err("BAD_PLAN", "targets は保存 Script ID を指定した対象の非空配列です");
    const bool temporary = c.ctx.playMode && !c.ctx.playMode->IsInEditor();
    const std::string domain = Text(request, "domain", temporary ? "play" : "edit");
    if ((request.Find("domain") && !request.Find("domain")->IsString()) || domain != (temporary ? "play" : "edit"))
        return OpResult::Err("SETTINGS_DOMAIN_CHANGED", "設定要求の Edit/Play domain が現在の対象と一致しません");
    if (const OpData* patches = request.Find("patches"); patches && !patches->IsArray())
        return OpResult::Err("BAD_PLAN", "patches は RFC 6901 path/action/value の配列です");
    if (const OpData* action = request.Find("action"); action && (!action->IsString() || action->AsString().empty()))
        return OpResult::Err("BAD_PLAN", "action は現在の catalog の安定 action ID です");
    std::vector<std::string> rebindReferences;
    if (const OpData* paths = request.Find("rebindReferences")) {
        if (!paths->IsArray()) return OpResult::Err("BAD_PLAN", "rebindReferences は RFC 6901 path の配列です");
        for (const OpData& path : paths->AsArray()) {
            if (!path.IsString()) return OpResult::Err("BAD_PLAN", "rebindReferences の各要素は RFC 6901 path の文字列です");
            rebindReferences.push_back(path.AsString());
        }
    }
    const std::string mode = Text(request, "mode", "configure");
    if (temporary && (mode == "add" || request.Find("action")))
        return OpResult::Err("PLAY_MUTATION_UNSUPPORTED", "Play では既存 managed Script の設定だけを一時編集できます");
    if (exactTargets && (mode != "configure" || request.Find("scriptId") || request.Find("scriptIndex") || request.Find("editSession")))
        return OpResult::Err("BAD_PLAN", "targets は configure の exact savedScriptId 個体だけを指定します");
    const OpData* explicitMoves = request.Find("referenceMoves");
    if (explicitMoves && !explicitMoves->IsArray())
        return OpResult::Err("BAD_PLAN", "referenceMoves は fromPath/toPath を持つ配列です");
    if (mode != "configure" && mode != "add") return OpResult::Err("BAD_PLAN", "mode は configure/add です");
    if (request.Find("mode") && !request.Find("mode")->IsString()) return OpResult::Err("BAD_PLAN", "mode は文字列です");
    if (request.Find("scriptId") && !request.Find("scriptId")->IsString()) return OpResult::Err("BAD_PLAN", "scriptId は文字列です");
    int scriptIndex = 0;
    if (const OpData* requestedIndex = request.Find("scriptIndex"); requestedIndex
        && (!requestedIndex->TryGetInt32(scriptIndex) || scriptIndex < 0))
        return OpResult::Err("BAD_PLAN", "scriptIndex は int 範囲の非負整数です");
    if ((mode == "add" && (request.Find("scriptId") || request.Find("scriptIndex"))) || (request.Find("scriptId") && request.Find("scriptIndex")))
        return OpResult::Err("BAD_PLAN", "Script インスタンス指定を併用できません");
    std::string type;
    if (!scene::ScriptFactory::ResolveTypeName(script->AsString(), type))
        return OpResult::Err("UNKNOWN_SCRIPT", "クラス名が未登録または曖昧です。script.catalog の完全名を使ってください: " + script->AsString());
    auto plan = std::make_shared<Plan>();
    plan->scene = c.ctx.activeScene;
    plan->lifetime = plan->scene->GetRenderSceneGeneration();
    plan->generation = ManagedScriptRuntime::Instance().GetAppliedGeneration();
    plan->temporary = c.ctx.playMode && !c.ctx.playMode->IsInEditor();
    plan->type = type;
    if (!WriteOpDataJson(request, plan->request) || !Fingerprint(*plan->scene, plan->fingerprint, diagnostic))
        return OpResult::Err("BAD_PLAN", diagnostic);
    std::vector<GameObject*> owners;
    std::vector<const OpData*> targetRequests;
    if (exactTargets) {
        for (const OpData& specified : exactTargets->AsArray()) {
            if (!Keys(specified, {"ownerGuid", "savedScriptId", "scriptId", "fields", "patches", "expectedRevision", "editSession", "rebindReferences", "referenceMoves"}, diagnostic))
                return OpResult::Err("BAD_TARGET", diagnostic);
            const OpData* guid = specified.Find("ownerGuid"), *saved = specified.Find("savedScriptId");
            if (!guid || !guid->IsString() || !saved || !saved->IsString() || !scene::ScriptFactory::IsValidSavedScriptId(saved->AsString()))
                return OpResult::Err("BAD_TARGET", "targets の ownerGuid/savedScriptId を明示してください");
            GameObject* owner = plan->scene->FindByGuid(guid->AsString());
            if (!owner) return OpResult::Err("BAD_TARGET", "設定対象が削除されています: " + guid->AsString());
            owners.push_back(owner); targetRequests.push_back(&specified);
        }
    } else {
        if (!ResolveScriptObjects(*plan->scene, *objects, nullptr, owners, diagnostic)) return OpResult::Err("BAD_TARGET", diagnostic);
        targetRequests.assign(owners.size(), &request);
    }
    if (owners.empty()) return OpResult::Err("BAD_TARGET", "設定先が空です");
    std::unordered_set<GameObject*> seen;
    std::unordered_set<uint64_t> seenInstances;
    OpData transactionTargets = OpData::MakeArray();
    std::vector<ManagedScriptAdapter*> preparingAdapters;
    for (size_t targetIndex = 0; targetIndex < owners.size(); ++targetIndex) {
        GameObject* owner = owners[targetIndex];
        const OpData& selected = *targetRequests[targetIndex];
        const OpData* selectedFields = selected.Find("fields"); if (!selectedFields) selectedFields = fields;
        if (!selectedFields->IsObject()) return OpResult::Err("BAD_FIELD", "対象ごとの fields は object です");
        if (!exactTargets && !seen.insert(owner).second) return OpResult::Err("BAD_TARGET", "設定先が重複しています: " + owner->name);
        Target target;
        struct PendingEditGuard {
            Target& target; uint64_t generation; bool transferred = false;
            ~PendingEditGuard() {
                if (!transferred && target.editSession) SettingsOperation(scene::ManagedBridgeOp::CLOSE_SETTINGS_EDIT, generation, target.editSession);
            }
        } pendingEdit{target, plan->generation};
        target.id = owner->GetID();
        target.ownerGuid = owner->instanceId;
        target.summary = ObjectSummary(*owner);
        auto* component = owner->GetComponent<scene::ScriptComponent>();
        target.hadComponent = component != nullptr;
        scene::ScriptEntry* existing = nullptr;
        std::vector<scene::ScriptEntry*> matches;
        if (component) for (auto& entry : component->scripts)
            if ((entry.script && type == entry.script->GetTypeName()) || (!entry.script && entry.serialized && type == entry.serialized->type)) matches.push_back(&entry);
        if (mode != "add") {
            if (exactTargets) {
                for (auto* match : matches) if (match->savedScriptId == Text(selected, "savedScriptId")) existing = match;
                if (!existing || !existing->script || (selected.Find("scriptId") && Text(selected, "scriptId") != Handle(*existing)))
                    return OpResult::Err("BAD_INSTANCE", "保存 ID の Script 個体が削除・再作成されています: " + owner->name);
            } else if (const OpData* exact = request.Find("scriptId")) {
                existing = Entry(*owner, exact->AsString());
                if (!existing || !existing->script || type != existing->script->GetTypeName()) return OpResult::Err("BAD_INSTANCE", "指定された Script 個体がありません: " + owner->name);
            } else if (const OpData* index = request.Find("scriptIndex")) {
                if (!component || static_cast<std::size_t>(scriptIndex) >= component->scripts.size()) return OpResult::Err("BAD_INSTANCE", "scriptIndex が範囲外です");
                existing = &component->scripts[static_cast<std::size_t>(scriptIndex)];
                if (!existing->script || type != existing->script->GetTypeName()) return OpResult::Err("BAD_INSTANCE", "scriptIndex のクラスが一致しません");
            } else {
                if (matches.size() > 1) return OpResult::Err("AMBIGUOUS_SCRIPT", "同じクラスが複数あります。scriptIndex を指定してください: " + owner->name);
                if (!matches.empty()) existing = matches.front();
            }
        }
        target.add = !existing;
        ManagedScriptAdapter* adapter = existing ? ManagedScriptRuntime::Instance().FindAdapter(existing->script.get()) : nullptr;
        if (target.add) {
            target.candidate = scene::ScriptFactory::Create(type, target.savedScriptId);
            adapter = ManagedScriptRuntime::Instance().FindAdapter(target.candidate.get());
            if (adapter) adapter->SetContext(plan->scene, owner);
        }
        OpData settings, snapshot;
        if (!adapter || adapter->Generation() != plan->generation || !adapter->RefreshOwnerBinding())
            return OpResult::Err("UNAVAILABLE_SCRIPT", "設定を読み出せません: " + owner->name);
        if (!seenInstances.insert(adapter->InstanceId()).second) return OpResult::Err("BAD_TARGET", "同じ Script 個体を複数回指定できません: " + owner->name);
        const bool leafOnly = !target.add && selectedFields->AsObject().empty();
        if (leafOnly) {
            if (const OpData* session = selected.Find("editSession")) {
                if (!session->IsString()) return OpResult::Err("BAD_PLAN", "editSession は opaque decimal string です");
                const auto parsed = std::from_chars(session->AsString().data(), session->AsString().data() + session->AsString().size(), target.editSession);
                if (parsed.ec != std::errc{} || parsed.ptr != session->AsString().data() + session->AsString().size() || !target.editSession)
                    return OpResult::Err("BAD_PLAN", "editSession が不正です");
                target.beforeRevision = Text(selected, "expectedRevision");
            } else if (exactTargets && !Text(selected, "expectedRevision").empty()) target.beforeRevision = Text(selected, "expectedRevision");
            else if (!adapter->OpenSettingsEdit(target.editSession, target.beforeRevision)) return OpResult::Err("UNAVAILABLE_SCRIPT", ManagedScriptRuntime::Instance().LastDiagnostic());
            settings = OpData::MakeObject();
        } else {
            if (!ReadSettings(*adapter, settings, target.before, &snapshot)) return OpResult::Err("UNAVAILABLE_SCRIPT", "設定を読み出せません: " + owner->name);
            target.beforeRevision = Text(snapshot, "revision");
        }
        if (selected.Find("expectedRevision") && Text(selected, "expectedRevision") != target.beforeRevision)
            return OpResult::Err("STALE_PLAN", "設定または参照実体が編集中に変わりました。入力を取り直してください");
        target.instanceId = adapter->InstanceId();
        target.savedScriptId = adapter->SavedScriptId();
        preparingAdapters.push_back(adapter);
        if (existing) { target.handle = Handle(*existing); target.enabled = adapter->enabled; }
        if (!NormalizeFields(*plan->scene, *owner, *adapter, *selectedFields, settings, diagnostic)) return OpResult::Err("BAD_FIELD", owner->name + ": " + diagnostic);
        OpData delta = OpData::MakeObject();
        for (const auto& [key, value] : selectedFields->AsObject()) {
            (void)value;
            delta.Set(key, *settings.Find(key));
        }
        OpData prepared = OpData::MakeObject();
        prepared.Set("instanceId", std::to_string(target.instanceId));
        prepared.Set("expectedRevision", target.beforeRevision);
        if (target.editSession) prepared.Set("editSession", std::to_string(target.editSession));
        prepared.Set("fields", std::move(delta));
        if (const OpData* action = request.Find("action")) prepared.Set("action", *action);
        if (const OpData* patches = selected.Find("patches")) prepared.Set("patches", *patches);
        else if (const OpData* commonPatches = request.Find("patches")) prepared.Set("patches", *commonPatches);
        prepared.Set("initial", target.add);
        OpData paths = OpData::MakeArray();
        for (const std::string& path : rebindReferences) paths.Push(path);
        if (exactTargets) if (const OpData* selectedRebind = selected.Find("rebindReferences")) {
            if (!selectedRebind->IsArray()) return OpResult::Err("BAD_PLAN", "対象ごとの rebindReferences は path 配列です");
            for (const OpData& path : selectedRebind->AsArray()) {
                if (!path.IsString()) return OpResult::Err("BAD_PLAN", "参照 path は文字列です");
                paths.Push(path);
            }
        }
        prepared.Set("rebind", std::move(paths));
        const OpData* selectedMoves = exactTargets ? selected.Find("referenceMoves") : nullptr;
        OpData moves = selectedMoves ? *selectedMoves : explicitMoves ? *explicitMoves : OpData::MakeArray();
        if (!moves.IsArray()) return OpResult::Err("BAD_PLAN", "対象ごとの referenceMoves は配列です");
        if (!selectedFields->AsObject().empty() && !AppendArrayReferenceMoves(*selectedFields, snapshot, moves, diagnostic)) return OpResult::Err("BAD_FIELD", diagnostic);
        prepared.Set("moves", std::move(moves));
        transactionTargets.Push(std::move(prepared));
        target.summary.Set("action", target.add ? "add" : "update");
        plan->targets.push_back(std::move(target));
        pendingEdit.transferred = true;
    }
    OpData transaction = OpData::MakeObject(); transaction.Set("targets", std::move(transactionTargets));
    std::string transactionJson;
    OpData prepared;
    if (!WriteOpDataJson(transaction, transactionJson)) return OpResult::Err("INVALID_SETTINGS", "候補設定を JSON へ変換できません");
    const uint64_t session = plan->targets.front().editSession;
    if (session && !StageSettingsInput(plan->generation, session, transactionJson))
        return OpResult::Err("INVALID_SETTINGS", "C# 設定の分割受信: " + ManagedScriptRuntime::Instance().LastDiagnostic());
    if (!SettingsOperation(session ? scene::ManagedBridgeOp::PREPARE_SETTINGS_EDIT : scene::ManagedBridgeOp::PREPARE_SETTINGS,
        plan->generation, session, session ? std::string_view{} : std::string_view(transactionJson), nullptr, &plan->hostToken, &preparingAdapters))
        return OpResult::Err("INVALID_SETTINGS", "C# 設定候補の検証: " + ManagedScriptRuntime::Instance().LastDiagnostic());
    for (Target& target : plan->targets) if (target.editSession) {
        if (!SettingsOperation(scene::ManagedBridgeOp::CLOSE_SETTINGS_EDIT, plan->generation, target.editSession,
            target.editSession == session ? "{\"retainPrepared\":true}" : std::string_view{}))
            return OpResult::Err("INVALID_SETTINGS", ManagedScriptRuntime::Instance().LastDiagnostic());
        target.editSession = 0;
    }
    if (!LoadPreparedPlan(*plan, diagnostic)) return OpResult::Err("INVALID_SETTINGS", diagnostic);
    plan->token = plans.session + ":" + std::to_string(++plans.next);
    plans.prepared.push_back(plan);
    while (plans.prepared.size() > 8) plans.prepared.pop_front();
    return OpResult::Data(Describe(*plan));
}

bool ValidateTargets(EditorContext& ctx, Plan& plan, bool after, std::string& diagnostic, bool deltaOnly = false)
{
    if (plan.isAdoption && !plan.applied) {
        auto draft = plan.adoption.lock();
        if (!draft || draft->consumed) return Fail(diagnostic, "保持した Play 採用候補は破棄または採用済みです");
    }
    if (!SameScene(ctx, plan)) return Fail(diagnostic, "シーンまたはコード世代が変わりました");
    std::string options = std::string("{\"mode\":\"validate\",\"side\":\"") + (after ? "after" : "before")
        + "\",\"deltaOnly\":" + (deltaOnly ? "true" : "false") + ",\"wireBytes\":8192}";
    std::string validationJson; OpData validation;
    if (!SettingsOperation(scene::ManagedBridgeOp::GET_PREPARED_REVIEW, plan.generation, plan.hostToken, options, &validationJson)
        || !ReadManagedSettingsJson(validationJson, validation))
        return Fail(diagnostic, "C# 設定 preflight: " + ManagedScriptRuntime::Instance().LastDiagnostic());
    const OpData* current = validation.Find("current");
    if (!current || !current->IsBool() || !current->AsBool())
        return Fail(diagnostic, Text(validation, "diagnostic", "Script の設定または参照実体が変わりました"));
    for (auto& target : plan.targets) {
        GameObject* owner = plan.scene->GetGameObject(target.id);
        if (!owner) return Fail(diagnostic, "設定先が削除または作り直されました");
        if (target.add && !after) continue;
        scene::ScriptEntry* entry = Entry(*owner, target.handle);
        ManagedScriptAdapter* script = entry ? ManagedScriptRuntime::Instance().FindAdapter(entry->script.get()) : nullptr;
        if (!script || script->Generation() != plan.generation)
            return Fail(diagnostic, "Script 個体または設定が変わりました: " + owner->name);
        if (script->enabled != target.enabled) return Fail(diagnostic, "Script の有効状態が変わりました: " + owner->name);
    }
    return true;
}

bool Publish(EditorContext& ctx, Plan& plan, std::string& diagnostic)
{
    OpData replacements = OpData::MakeArray();
    for (auto& target : plan.targets) {
        GameObject* owner = plan.scene->GetGameObject(target.id);
        if (!owner) return Fail(diagnostic, "設定先がありません");
        if (target.add && !target.candidate) {
            target.candidate = scene::ScriptFactory::Create(plan.type, target.savedScriptId);
            ManagedScriptAdapter* candidate = ManagedScriptRuntime::Instance().FindAdapter(target.candidate.get());
            if (!candidate) return Fail(diagnostic, "Script を作成できません");
            candidate->SetContext(plan.scene, owner);
            OpData settings, snapshot; std::string json;
            if (!ReadSettings(*candidate, settings, json, &snapshot)) return Fail(diagnostic, "再追加した設定を取得できません");
            OpData replacement = OpData::MakeObject(); replacement.Set("previousId", std::to_string(target.instanceId));
            replacement.Set("instanceId", std::to_string(candidate->InstanceId())); replacement.Set("expectedRevision", Text(snapshot, "revision"));
            replacements.Push(std::move(replacement));
        }
    }
    std::string remapJson;
    if (!replacements.AsArray().empty()) {
        OpData remap = OpData::MakeObject(); remap.Set("replacements", std::move(replacements));
        if (!WriteOpDataJson(remap, remapJson)) return false;
    }
    if (!SettingsOperation(scene::ManagedBridgeOp::COMMIT_SETTINGS, plan.generation, plan.hostToken, remapJson))
        return Fail(diagnostic, "C# 設定確定: " + ManagedScriptRuntime::Instance().LastDiagnostic());
    for (auto& target : plan.targets) if (target.add) {
        GameObject* owner = plan.scene->GetGameObject(target.id);
        auto* component = owner->GetComponent<scene::ScriptComponent>();
        if (!component) component = &owner->AddComponent<scene::ScriptComponent>();
        auto& entry = component->scripts.emplace_back();
        entry.savedScriptId = target.savedScriptId;
        if (target.entryId.empty()) target.entryId = entry.inspectionId;
        else entry.inspectionId = target.entryId;
        target.candidate->enabled = target.enabled;
        target.instanceId = ManagedScriptRuntime::Instance().FindAdapter(target.candidate.get())->InstanceId();
        entry.script = std::move(target.candidate);
        target.handle = Handle(entry);
    }
    if (plan.temporary) ctx.playMode->MarkTemporarySettingsChanged();
    else { if (ctx.markSceneDirty) ctx.markSceneDirty(); ctx.sceneDirty = true; }
    plan.applied = true;
    plan.undone = false;
    if (plan.receiptState) plan.receiptState->undone = false;
    return true;
}

bool UndoPlan(EditorContext& ctx, Plan& plan, std::string& diagnostic)
{
    if (!ValidateTargets(ctx, plan, true, diagnostic, true)) return false;
    if (!SettingsOperation(scene::ManagedBridgeOp::RESTORE_SETTINGS, plan.generation, plan.hostToken))
        return Fail(diagnostic, "C# 設定 Undo: " + ManagedScriptRuntime::Instance().LastDiagnostic());
    std::vector<std::unique_ptr<scene::Script>> destroyed;
    for (auto& target : plan.targets) {
        GameObject* owner = plan.scene->GetGameObject(target.id);
        scene::ScriptEntry* entry = Entry(*owner, target.handle);
        if (!target.add) {
            continue;
        } else {
            const bool awoken = entry->m_awoken;
            std::unique_ptr<scene::Script> removed = std::move(entry->script);
            auto* component = owner->GetComponent<scene::ScriptComponent>();
            const auto at = std::find_if(component->scripts.begin(), component->scripts.end(), [&](auto& value) { return &value == entry; });
            component->scripts.erase(at);
            if (!target.hadComponent && component->scripts.empty()) owner->RemoveComponent<scene::ScriptComponent>();
            if (awoken) destroyed.push_back(std::move(removed));
        }
    }
    plan.undone = true;
    if (plan.receiptState) plan.receiptState->undone = true;
    if (plan.temporary) ctx.playMode->MarkTemporarySettingsChanged();
    else { if (ctx.markSceneDirty) ctx.markSceneDirty(); ctx.sceneDirty = true; }
    /// @note 後始末 callback は全対象の取り消し後に呼び、vector iterator を user code の間に保持しない。
    for (auto& script : destroyed)
        script->ExecuteProfiledCallback(&scene::Script::OnDestroy, scene::ScriptCallbackKind::DESTROY, "OnDestroy");
    return true;
}

OpResult Apply(OpContext& c, const std::string& token, Plans& plans)
{
    for (const Receipt& receipt : plans.receipts) if (receipt.token == token) {
        OpResult result = OpResult::Data(receipt.description);
        const auto history = receipt.history.lock();
        std::string diagnostic;
        result.data.Set("replayed", true); result.data.Set("undone", receipt.state->undone);
        result.data.Set("current", history && !receipt.state->undone && ValidateTargets(c.ctx, *history, true, diagnostic, true));
        result.data.Set("historyRetained", history != nullptr);
        result.noChange = true;
        return result;
    }
    const auto found = std::find_if(plans.prepared.begin(), plans.prepared.end(), [&](const auto& plan) { return plan->token == token; });
    if (found == plans.prepared.end()) return OpResult::Err("EXPIRED_PLAN", "preview を再実行してください。計画が期限切れまたは別セッションです");
    std::shared_ptr<Plan> plan = *found;
    std::string diagnostic;
    std::string fingerprint;
    if (!ValidateTargets(c.ctx, *plan, false, diagnostic) || !Fingerprint(*plan->scene, fingerprint, diagnostic) || fingerprint != plan->fingerprint) {
        plans.prepared.erase(found);
        return OpResult::Err("STALE_PLAN", "preview を再実行してください: " + (diagnostic.empty() ? "対象選択または階層が変わりました" : diagnostic));
    }
    if (!Publish(c.ctx, *plan, diagnostic)) {
        for (auto& target : plan->targets) target.candidate.reset();
        return OpResult::Err("APPLY_FAILED", diagnostic);
    }
    if (plan->isAdoption) if (auto draft = plan->adoption.lock()) draft->Consume();
    plans.prepared.erase(found);
    for (auto& target : plan->targets) target.candidate.reset();
    plan->request.clear(); plan->fingerprint.clear();
    plan->receiptState = std::make_shared<ReceiptState>();
    EditorContext* ctx = &c.ctx;
    OpResult result = OpResult::Data(Describe(*plan));
    OpData compact = OpData::MakeObject(); compact.Set("plan", plan->token);
    compact.Set("script", scene::ScriptFactory::DisplayName(plan->type)); compact.Set("applied", true);
    OpData addresses = OpData::MakeArray();
    for (Target& target : plan->targets) {
        OpData address = OpData::MakeObject(); address.Set("savedScriptId", target.savedScriptId);
        address.Set("action", target.add ? "add" : "update"); address.Set("changedPathCount", static_cast<int64_t>(target.changedPathCount));
        if (const OpData* id = target.summary.Find("id")) address.Set("id", *id);
        addresses.Push(std::move(address));
        target.before.clear(); target.summary = {};
    }
    compact.Set("targets", std::move(addresses));
    plans.receipts.push_back(Receipt{plan->token, plan, plan->receiptState, std::move(compact), plan->temporary});
    while (plans.receipts.size() > 128) plans.receipts.pop_front();
    result.command = std::make_unique<CheckedLambdaCommand>("Configure Script " + scene::ScriptFactory::DisplayName(plan->type),
        [ctx, plan](std::string& diagnostic) {
            if (!plan->undone) return Fail(diagnostic, "この設定 transaction は取り消されていません");
            if (!ValidateTargets(*ctx, *plan, false, diagnostic, true)) return false;
            if (Publish(*ctx, *plan, diagnostic)) return true;
            for (auto& target : plan->targets) if (target.candidate) { target.candidate->SetContext(nullptr, nullptr); target.candidate.reset(); }
            return false;
        }, [ctx, plan](std::string& diagnostic) { return UndoPlan(*ctx, *plan, diagnostic); });
    return result;
}
} /// @note namespace

bool ResolveScriptObjects(scene::Scene& scene, const OpData& selector, GameObject* owner,
    std::vector<GameObject*>& output, std::string& diagnostic)
{
    std::vector<GameObject*> all;
    if (!SceneObjects(scene, all, diagnostic)) return false;
    std::vector<GameObject*> resolved;
    if (!Resolve(scene, selector, owner, resolved, diagnostic, 0)) return false;
    output = std::move(resolved);
    diagnostic.clear();
    return true;
}

void RegisterScriptConfigureOperators(OperatorRegistry& registry)
{
    auto plans = std::make_shared<Plans>();
    EditorOperator imported;
    imported.id = "script.configure.import_prepared"; imported.label = "Review Prepared Play Adoption";
    imported.category = "Script"; imported.kind = OpKind::Action;
    imported.desc = "保持した Play 値から準備した候補を、共通の review/apply/checked Undo へ登録する";
    imported.params = {{"settings", OpParamType::Data, "preparedToken、adoptionId、exact targets", true}};
    imported.poll = [](const OpContext& c, const OpArgs&) { return Editable(c) && (!c.ctx.playMode || c.ctx.playMode->IsInEditor()); };
    imported.exec = [plans](OpContext& c, const OpArgs& args) {
        const OpData* input = args.GetData("settings"); std::string diagnostic;
        if (!input || !Keys(*input, {"preparedToken", "adoptionId", "targets"}, diagnostic)) return OpResult::Err("BAD_PLAN", diagnostic);
        uint64_t token = 0, draftId = 0;
        const auto number = [&](const char* key, uint64_t& value) {
            const std::string text = Text(*input, key); const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            return value && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
        };
        if (!number("preparedToken", token) || !number("adoptionId", draftId) || !c.ctx.playMode) return OpResult::Err("BAD_PLAN", "保持した採用候補と準備 token を指定してください");
        auto draft = c.ctx.playMode->FindAdoptionDraft(draftId);
        if (!draft || draft->consumed) return OpResult::Err("EXPIRED_ADOPTION", "採用候補は破棄または採用済みです");
        if (draft->preparedHostToken != token || draft->preparedGeneration != ManagedScriptRuntime::Instance().GetAppliedGeneration())
            return OpResult::Err("BAD_PLAN", "この token は保持した採用候補から準備されていません");
        auto plan = std::make_shared<Plan>(); plan->scene = c.ctx.activeScene; plan->lifetime = plan->scene->GetRenderSceneGeneration();
        plan->generation = ManagedScriptRuntime::Instance().GetAppliedGeneration(); plan->hostToken = token; plan->isAdoption = true; plan->adoption = draft;
        draft->preparedHostToken = 0;
        const OpData* targets = input->Find("targets");
        if (!targets || !targets->IsArray() || targets->AsArray().empty()) return OpResult::Err("BAD_TARGET", "exact targets がありません");
        for (const OpData& specified : targets->AsArray()) {
            GameObject* owner = plan->scene->FindByGuid(Text(specified, "ownerGuid"));
            auto* component = owner ? owner->GetComponent<scene::ScriptComponent>() : nullptr; scene::ScriptEntry* selected = nullptr;
            if (component) for (auto& entry : component->scripts) if (entry.savedScriptId == Text(specified, "savedScriptId")) selected = &entry;
            auto* adapter = selected ? ManagedScriptRuntime::Instance().FindAdapter(selected->script.get()) : nullptr;
            if (!adapter || adapter->ReferenceBirth() != Text(specified, "scriptBirth") || adapter->Generation() != plan->generation)
                return OpResult::Err("ADOPTION_BIRTH_CHANGED", "元の Script 個体が削除・再追加されました");
            Target target; target.id = owner->GetID(); target.ownerGuid = owner->instanceId; target.handle = Handle(*selected); target.entryId = selected->inspectionId;
            target.savedScriptId = selected->savedScriptId; target.instanceId = adapter->InstanceId(); target.enabled = adapter->enabled;
            target.hadComponent = true; target.summary = ObjectSummary(*owner); plan->targets.push_back(std::move(target));
            if (plan->type.empty()) plan->type = adapter->GetTypeName();
        }
        if (!Fingerprint(*plan->scene, plan->fingerprint, diagnostic) || !LoadPreparedPlan(*plan, diagnostic)) return OpResult::Err("INVALID_SETTINGS", diagnostic);
        plan->token = plans->session + ":" + std::to_string(++plans->next);
        plans->prepared.push_back(plan); while (plans->prepared.size() > 8) plans->prepared.pop_front();
        draft->plan = plan->token; return OpResult::Data(Describe(*plan));
    };
    registry.Register(std::move(imported));
    EditorOperator preview;
    preview.id = "script.configure.preview";
    preview.label = "Preview Script Settings";
    preview.category = "Script";
    preview.desc = "名前で Script 設定を一括検証。未接続の必須参照は編集可能で、実行前に別途検証する。objects/script/fields、参照には root/path/tag/scope を指定。同じ GUID を明示的に選び直す場合は rebindReferences に RFC 6901 path を指定。シーンは変更しない";
    /// @note 候補の C# constructor が実行されるため read 権限の Query にはしない。
    preview.kind = OpKind::Action;
    preview.params = {{"settings", OpParamType::Data, "objects、script クラス名、fields を含む設定", true}};
    preview.poll = [](const OpContext& c, const OpArgs&) { return Editable(c); };
    preview.exec = [plans](OpContext& c, const OpArgs& args) {
        if (c.ctx.playMode && !c.ctx.playMode->IsInEditor()) {
            std::weak_ptr<Plans> retained = plans;
            c.ctx.playMode->SetTemporarySettingsCleanup([retained] {
                if (auto state = retained.lock()) {
                    std::erase_if(state->prepared, [](const auto& plan) { return plan->temporary; });
                    std::erase_if(state->receipts, [](const Receipt& receipt) { return receipt.temporary; });
                }
            });
        }
        const OpData* settings = args.GetData("settings");
        return settings ? Preview(c, *settings, *plans) : OpResult::Err("BAD_PLAN", "settings がありません");
    };
    registry.Register(std::move(preview));
    EditorOperator review;
    review.id = "script.configure.review"; review.label = "Read Script Settings Review"; review.category = "Script";
    review.desc = "候補の不変な対象・変更・before/after 値を page 取得する。mode=targets/changes/value、offset/count/wireBytes を指定。完全値と Undo は変更しない";
    review.kind = OpKind::Query;
    review.params = {{"plan", OpParamType::String, "保持中の preview または Undo の plan", true},
        {"options", OpParamType::Data, "mode、対象/変更 index、side/path/start/count、応答全体の wireBytes", false}};
    review.poll = [](const OpContext& c, const OpArgs&) { return c.ctx.activeScene && !c.ctx.managedBuildBusy; };
    review.exec = [plans](OpContext& c, const OpArgs& args) {
        const OpData* options = args.GetData("options");
        return Review(c, args.GetString("plan"), options ? *options : OpData::MakeObject(), *plans);
    };
    registry.Register(std::move(review));
    EditorOperator apply;
    apply.id = "script.configure.apply";
    apply.label = "Apply Script Settings";
    apply.category = "Script";
    apply.desc = "preview の plan を一回だけ適用し、一括 Undo を記録する。再送は適用済み結果を返す";
    apply.kind = OpKind::Mutation;
    apply.undoLabel = "Configure Script";
    apply.params = {{"plan", OpParamType::String, "preview が返した plan", true}};
    apply.poll = [](const OpContext& c, const OpArgs&) { return Editable(c); };
    apply.exec = [plans](OpContext& c, const OpArgs& args) { return Apply(c, args.GetString("plan"), *plans); };
    registry.Register(std::move(apply));
    EditorOperator discard;
    discard.id = "script.configure.discard";
    discard.label = "Discard Script Settings Preview";
    discard.category = "Script";
    discard.desc = "未確定の preview を破棄して設定容量を解放する。plan=* は未確定 preview 全件。適用済み Undo は履歴の消去で解放する";
    discard.kind = OpKind::Action;
    discard.params = {{"plan", OpParamType::String, "preview token、または *", true}};
    discard.exec = [plans](OpContext&, const OpArgs& args) {
        const std::string token = args.GetString("plan");
        if (token == "*") plans->prepared.clear();
        else {
            const auto found = std::find_if(plans->prepared.begin(), plans->prepared.end(), [&token](const auto& plan) { return plan->token == token; });
            if (found != plans->prepared.end()) plans->prepared.erase(found);
            else if (std::any_of(plans->receipts.begin(), plans->receipts.end(), [&token](const Receipt& receipt) { return receipt.token == token; }))
                return OpResult::Err("APPLIED_RECEIPT", "適用済み transaction は再適用されません。保持済み Undo 容量は履歴の消去で解放してください");
        }
        return OpResult::Ok();
    };
    registry.Register(std::move(discard));
    RegisterPlaySettingsAdoptionOperators(registry);
}
} /// @note namespace fbzz::editor
