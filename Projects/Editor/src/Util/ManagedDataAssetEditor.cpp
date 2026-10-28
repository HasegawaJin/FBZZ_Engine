/// @file    ManagedDataAssetEditor.cpp
/// @brief   Portable .fzdata の準備・確認・原子的保存と exact file snapshot Undo。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include <Editor/Util/ManagedDataAssetEditor.hpp>
#include <Editor/Util/ManagedSettingsCodec.hpp>
#include <Editor/Util/ManagedPrimitiveWidgets.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/ImGuiReflector.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/Uuid.hpp>
#include <imgui.h>
#include <toml++/toml.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <bit>
#include <algorithm>
#include <limits>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace fbzz::editor {
namespace {
using Runtime = scene::ManagedScriptRuntime;
using Operation = scene::ManagedBridgeOp;
std::string Text(const OpData& data, std::string_view key)
{ const auto* value = data.Find(key); return value && value->IsString() ? value->AsString() : std::string{}; }
bool Fail(std::string& reason, std::string message) { reason = std::move(message); return false; }
bool Editable(const EditorContext& context)
{ return !context.projectRoot.empty() && (!context.playMode || context.playMode->IsInEditor()); }
/// @note 原本と履歴は改行・制御文字も含めて同じバイト列で競合判定する。
bool ReadFileBytes(const std::filesystem::path& path, std::string& bytes)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream output;
    output << file.rdbuf();
    if (file.bad() || !output) return false;
    bytes = output.str();
    return true;
}
std::string DiskGuid(const std::string& path)
{ std::string bytes; if (!util::FileSystem::ReadText(path + ".meta", bytes)) return {}; auto parsed = toml::parse(bytes); return parsed ? parsed.table()["meta"]["guid"].value_or(std::string{}) : std::string{}; }
/// @note Create refuses concurrent/preexisting target files. The owned sibling is deleted on failure; existing meta/data are never replaced.
bool WriteNewFile(const std::string& path, const std::string& bytes)
{
    const auto target = util::FileSystem::PathFromUtf8(path);
    if (!util::FileSystem::EnsureParentDirectory(target)) return false;
    const auto temporary = target.parent_path() / (".fbzz-asset-" + util::GenerateUUID() + ".tmp");
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool complete = true; size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD count = static_cast<DWORD>(std::min<size_t>(65536, bytes.size() - offset)); DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, count, &written, nullptr) || written != count) { complete = false; break; }
        offset += written;
    }
    complete = complete && FlushFileBuffers(file); CloseHandle(file);
    if (complete && MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
    std::error_code error; std::filesystem::remove(temporary, error); return false;
}
bool ResolvePath(const EditorContext& context, const std::string& input, std::string& path, std::string& reason)
{
    if (input.empty() || input.find('\0') != std::string::npos || context.projectRoot.empty()) return Fail(reason, "DataAsset requires a project Assets path.");
    const std::string resolved = input.starts_with("guid:") ? asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(input)) : input;
    if (resolved.empty()) return Fail(reason, "DataAsset GUID is missing.");
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(context.projectRoot) / "Assets", error);
    if (error) return Fail(reason, "Assets root cannot be resolved.");
    auto candidate = util::FileSystem::PathFromUtf8(resolved); if (!candidate.is_absolute()) candidate = util::FileSystem::PathFromUtf8(context.projectRoot) / candidate;
    candidate = std::filesystem::weakly_canonical(candidate, error); path = util::FileSystem::PathToUtf8(candidate);
    if (error || candidate.extension() != ".fzdata" || !util::FileSystem::IsChildPathText(path, util::FileSystem::PathToUtf8(root))) return Fail(reason, "DataAsset target must remain inside project Assets and have .fzdata extension.");
    return true;
}
/// @note Large history bytes are owned temporary files; Undo retains no CLR instance, descriptor or code generation.
struct FileSnapshot {
    std::string resident;
    std::filesystem::path spool;
    ~FileSnapshot() { if (!spool.empty()) { std::error_code error; std::filesystem::remove(spool, error); } }
    bool Capture(const std::string& bytes)
    {
        if (bytes.size() <= 1024U * 1024U) { resident = bytes; return true; }
        std::error_code error; const auto candidate = std::filesystem::temp_directory_path(error) / ("fbzz-asset-history-" + util::GenerateUUID() + ".tmp");
        if (error || !WriteNewFile(util::FileSystem::PathToUtf8(candidate), bytes)) return false;
        spool = candidate; return true;
    }
    bool Read(std::string& bytes) const { if (spool.empty()) { bytes = resident; return true; } return ReadFileBytes(spool, bytes); }
    bool Matches(const std::string& path) const
    {
        std::ifstream current(util::FileSystem::PathFromUtf8(path), std::ios::binary); if (!current) return false;
        std::ifstream saved; if (!spool.empty()) { saved.open(spool, std::ios::binary); if (!saved) return false; }
        std::array<char, 65536> actual{}, expected{}; size_t position = 0;
        for (;;) {
            current.read(actual.data(), actual.size()); const size_t count = static_cast<size_t>(current.gcount());
            if (!spool.empty()) { saved.read(expected.data(), expected.size()); if (saved.gcount() != static_cast<std::streamsize>(count) || !std::equal(actual.begin(), actual.begin() + count, expected.begin())) return false; }
            else if (position > resident.size() || count > resident.size() - position || resident.compare(position, count, actual.data(), count) != 0) return false;
            position += count; if (count < actual.size()) return spool.empty() ? position == resident.size() : saved.peek() == std::char_traits<char>::eof();
        }
    }
};
struct Document {
    uint64_t generation = 0, id = 0;
    void Close() { if (id && Runtime::Instance().IsLoaded()) { scene::ManagedBridgeRequest request; request.op = Operation::CLOSE_DATA_ASSET_DOCUMENT; request.generation = generation; request.instanceId = id; scene::ManagedBridgeReply reply; (void)Runtime::Instance().Dispatch(request, reply); } id = 0; }
    ~Document() { Close(); }
};
bool Call(Operation operation, const Document& document, uint32_t flags, std::string_view input, std::string& output,
          scene::ManagedBridgeReply& reply, uint64_t offset = 0, uint32_t count = 0)
{
    scene::ManagedBridgeRequest request; request.op = operation; request.generation = document.generation; request.instanceId = document.id;
    request.flags = flags; request.input = input.data(); request.inputBytes = static_cast<uint32_t>(input.size()); request.index = count;
    request.reserved0 = static_cast<uint32_t>(offset); request.reserved1 = static_cast<uint32_t>(offset >> 32);
    return Runtime::Instance().Dispatch(request, reply, &output);
}
bool Transfer(Operation operation, const Document& document, const std::string& bytes, OpData& summary, std::string& reason)
{
    scene::ManagedBridgeReply reply; std::string output;
    if (operation == Operation::PREPARE_DATA_ASSET_DOCUMENT && !Call(operation, document, 0, {}, output, reply)) return Fail(reason, Runtime::Instance().LastDiagnostic());
    for (size_t offset = 0; offset < bytes.size(); offset += 65536)
        if (!Call(operation, document, 1, std::string_view(bytes).substr(offset, std::min<size_t>(65536, bytes.size() - offset)), output, reply, offset)) return Fail(reason, Runtime::Instance().LastDiagnostic());
    if (!Call(operation, document, 2, {}, output, reply) || !ReadManagedSettingsJson(output, summary)) return Fail(reason, Runtime::Instance().LastDiagnostic());
    return true;
}
bool TypeSchema(const std::string& key, OpData& schema)
{
    for (uint32_t index = 0;; ++index) {
        scene::ManagedBridgeRequest request; request.op = Operation::GET_DATA_ASSET_SCHEMA; request.generation = Runtime::Instance().GetAppliedGeneration(); request.index = index;
        scene::ManagedBridgeReply reply; std::string type;
        if (!Runtime::Instance().Dispatch(request, reply, &type)) return false;
        if (type != key) continue;
        request.flags = 1; std::string json; return Runtime::Instance().Dispatch(request, reply, &json) && ReadManagedSettingsJson(json, schema);
    }
}
bool Open(Document& document, const std::string& type, const std::string& guid, bool create, const std::string& source, OpData& summary, std::string& reason)
{
    document.generation = Runtime::Instance().GetAppliedGeneration();
    if (!document.generation) return Fail(reason, "Build/apply the C# project before editing its DataAssets.");
    OpData input = OpData::MakeObject(); input.Set("type", type); input.Set("guid", guid); input.Set("create", create);
    std::string output, packet; scene::ManagedBridgeReply reply;
    if (!WriteOpDataJson(input, packet)) return Fail(reason, "DataAsset open arguments cannot be represented as finite UTF8 JSON.");
    if (!Call(Operation::OPEN_DATA_ASSET_DOCUMENT, document, 0, packet, output, reply)) return Fail(reason, Runtime::Instance().LastDiagnostic());
    document.id = std::bit_cast<uint64_t>(reply.integer);
    if (!create) return Transfer(Operation::OPEN_DATA_ASSET_DOCUMENT, document, source, summary, reason);
    if (!Call(Operation::READ_DATA_ASSET_PAGE, document, 0, "{\"path\":\"\",\"count\":0}", output, reply) || !ReadManagedSettingsJson(output, summary)) return Fail(reason, Runtime::Instance().LastDiagnostic());
    return true;
}
bool SourceJson(const std::string& bytes, std::string& type, std::string& json)
{
    auto parsed = toml::parse(bytes); if (!parsed) return false;
    type = parsed.table()["type"].value_or(std::string{});
    if (!type.starts_with("cs:" ) && !type.starts_with("cs-internal:")) return false;
    if (parsed.table()["formatVersion"].value_or(0) != 2) return false;
    const auto value = parsed.table()["json"].value<std::string>(); if (!value) return false; json = *value; return true;
}
struct Plan {
    std::string token = util::GenerateUUID(), project, path, guid, type, meta;
    bool create = false, applied = false;
    Document document;
    std::shared_ptr<FileSnapshot> before = std::make_shared<FileSnapshot>(), after = std::make_shared<FileSnapshot>();
    OpData summary;
    std::filesystem::path dataTomb, metaTomb;
    bool repairRequired = false;
    ~Plan() { if (!repairRequired) { std::error_code error; if (!dataTomb.empty()) std::filesystem::remove(dataTomb, error); if (!metaTomb.empty()) std::filesystem::remove(metaTomb, error); } }
};
struct State { std::unordered_map<std::string, std::shared_ptr<Plan>> plans; };
std::shared_ptr<Plan> Preview(EditorContext& context, const OpArgs& args, bool create, std::string& reason)
{
    if (!Editable(context)) { Fail(reason, "DataAsset file editing requires Edit mode."); return {}; }
    auto plan = std::make_shared<Plan>(); plan->project = context.projectRoot; plan->create = create;
    if (!ResolvePath(context, args.GetString("path"), plan->path, reason)) return {};
    std::string bytes, source;
    if (create) {
        if (util::FileSystem::Exists(plan->path) || util::FileSystem::Exists(plan->path + ".meta")) { Fail(reason, "DataAsset file/meta already exists; create cannot replace it."); return {}; }
        plan->guid = asset::AssetDatabase::GenerateGuid(); plan->type = args.GetString("type");
        toml::table meta; meta.insert("meta", toml::table{{"guid", plan->guid}}); std::ostringstream serialized; serialized << meta; plan->meta = serialized.str();
    } else {
        if (!ReadFileBytes(util::FileSystem::PathFromUtf8(plan->path), bytes) || !SourceJson(bytes, plan->type, source)) { Fail(reason, "Managed .fzdata format is invalid or unavailable."); return {}; }
        plan->guid = DiskGuid(plan->path);
        if (plan->guid.empty() || !util::FileSystem::ReadText(plan->path + ".meta", plan->meta) || !plan->before->Capture(bytes)) { Fail(reason, "DataAsset requires intact stable meta identity and owned history storage."); return {}; }
    }
    if (args.Has("expectedAssetGuid") && args.GetString("expectedAssetGuid") != plan->guid) { Fail(reason, "ASSET_IDENTITY_CHANGED: meta GUID changed; reopen the selected asset before editing."); return {}; }
    if (args.Has("expectedType") && args.GetString("expectedType") != plan->type) { Fail(reason, "ASSET_TYPE_CHANGED: reopen the selected asset with its current type."); return {}; }
    if (args.Has("expectedGeneration") && args.GetString("expectedGeneration") != std::to_string(Runtime::Instance().GetAppliedGeneration())) { Fail(reason, "ASSET_SCHEMA_CHANGED: C# reloaded; reopen the selected asset."); return {}; }
    OpData schema; if (!TypeSchema(plan->type, schema)) { Fail(reason, "DataAsset type is unavailable in the current C# schema; apply code or migrate the stored type."); return {}; }
    OpData opened; if (!Open(plan->document, plan->type, plan->guid, create, source, opened, reason)) return {};
    if (args.Has("expectedRevision") && args.GetString("expectedRevision") != Text(opened, "revision")) { Fail(reason, "SETTINGS_CONFLICT: DataAsset changed since its displayed values; refresh before saving."); return {}; }
    const OpData* fields = args.GetData("fields"), *patches = args.GetData("patches");
    if ((fields && !fields->IsObject()) || (patches && !patches->IsArray())) { Fail(reason, "DataAsset fields/patches require object/array values."); return {}; }
    OpData request = OpData::MakeObject(); request.Set("expectedRevision", Text(opened, "revision")); request.Set("fields", fields ? *fields : OpData::MakeObject()); if (patches) request.Set("patches", *patches);
    std::string packet; if (!WriteOpDataJson(request, packet)) { Fail(reason, "DataAsset candidate arguments contain unsupported or nonfinite JSON values."); return {}; }
    if (!Transfer(Operation::PREPARE_DATA_ASSET_DOCUMENT, plan->document, packet, plan->summary, reason)) return {};
    std::string prepared, output; scene::ManagedBridgeReply reply;
    for (uint64_t offset = 0;;) {
        if (!Call(Operation::READ_PREPARED_DATA_ASSET, plan->document, 0, {}, output, reply, offset, 65536)) { Fail(reason, Runtime::Instance().LastDiagnostic()); return {}; }
        if (reply.integer != static_cast<int64_t>(offset + output.size()) || (output.empty() && reply.flags == 0)) { Fail(reason, "Prepared DataAsset continuation is invalid."); return {}; }
        prepared.append(output); offset += output.size(); if (reply.flags == 1) break;
    }
    toml::table asset; asset.insert("formatVersion", 2); asset.insert("type", plan->type); asset.insert("json", prepared);
    std::ostringstream file; file << asset; if (!plan->after->Capture(file.str())) { Fail(reason, "DataAsset candidate history storage is unavailable."); return {}; }
    return plan;
}
bool MatchMeta(const Plan& plan)
{ std::string current; return util::FileSystem::ReadText(plan.path + ".meta", current) && current == plan.meta; }
bool Publish(EditorContext& context, Plan& plan, bool undo, std::string& reason)
{
    if (!Editable(context) || context.projectRoot != plan.project) return Fail(reason, "DataAsset command belongs to another project or requires Edit mode.");
    std::string canonical; if (!ResolvePath(context, plan.path, canonical, reason) || canonical != plan.path) return Fail(reason, "DataAsset path ownership changed.");
    if (undo) {
        if (!plan.applied || !MatchMeta(plan) || !plan.after->Matches(plan.path)) return Fail(reason, "DataAsset file/meta changed after save; Undo left the file and cursor unchanged.");
        for (auto* previous : {&plan.dataTomb, &plan.metaTomb}) if (!previous->empty()) {
            std::error_code error; std::filesystem::remove(*previous, error);
            if (error) return Fail(reason, "Previous owned DataAsset recovery storage is locked; original files are unchanged."); previous->clear();
        }
        if (plan.create) {
            const auto original = util::FileSystem::PathFromUtf8(plan.path), meta = util::FileSystem::PathFromUtf8(plan.path + ".meta");
            const auto nonce = util::GenerateUUID(); plan.metaTomb = original.parent_path() / (".fbzz-asset-undo-meta-" + nonce + ".tmp");
            plan.dataTomb = original.parent_path() / (".fbzz-asset-undo-data-" + nonce + ".tmp");
            /// @note Locked meta fails before touching data. Failure after moving meta uses rename rollback, never an unverified data reconstruction.
            if (!MoveFileExW(meta.c_str(), plan.metaTomb.c_str(), MOVEFILE_WRITE_THROUGH)) { plan.metaTomb.clear(); plan.dataTomb.clear(); return Fail(reason, "Created DataAsset meta is locked; original files and Undo cursor are unchanged."); }
            if (!MoveFileExW(original.c_str(), plan.dataTomb.c_str(), MOVEFILE_WRITE_THROUGH)) {
                plan.dataTomb.clear();
                if (!MoveFileExW(plan.metaTomb.c_str(), meta.c_str(), MOVEFILE_WRITE_THROUGH)) {
                    plan.repairRequired = true; return Fail(reason, "DataAsset Undo requires meta repair from owned recovery file: " + util::FileSystem::PathToUtf8(plan.metaTomb));
                }
                plan.metaTomb.clear(); return Fail(reason, "Created DataAsset data is locked; meta rename was rolled back and Undo cursor is unchanged.");
            }
            asset::AssetDatabase::OnAssetRemoved(plan.path);
        } else {
            std::string before; if (!plan.before->Read(before) || !util::FileSystem::WriteTextAtomic(plan.path, before)) return Fail(reason, "DataAsset Undo atomic write failed; old file remains.");
        }
        plan.applied = false;
    } else {
        if (plan.applied) return Fail(reason, "DataAsset candidate already applied.");
        if (plan.create) {
            if (util::FileSystem::Exists(plan.path) || util::FileSystem::Exists(plan.path + ".meta")) return Fail(reason, "DataAsset create file/meta conflict.");
            if (!WriteNewFile(plan.path + ".meta", plan.meta)) return Fail(reason, "DataAsset owned meta write failed or conflicted.");
        } else if (!MatchMeta(plan) || !plan.before->Matches(plan.path)) return Fail(reason, "DataAsset file/meta conflict; refresh before saving.");
        std::string after;
        if (!plan.after->Read(after) || !(plan.create ? WriteNewFile(plan.path, after) : util::FileSystem::WriteTextAtomic(plan.path, after))) {
            if (plan.create && MatchMeta(plan)) { std::error_code error; std::filesystem::remove(util::FileSystem::PathFromUtf8(plan.path + ".meta"), error); }
            return Fail(reason, "DataAsset atomic data write failed; no successful save was exposed.");
        }
        (void)asset::AssetDatabase::GuidFromPath(plan.path); plan.applied = true;
        for (auto* previous : {&plan.dataTomb, &plan.metaTomb}) if (!previous->empty()) { std::error_code error; std::filesystem::remove(*previous, error); if (!error) previous->clear(); }
    }
    context.requestAssetBrowserRefresh = true; return true;
}
OpData Describe(const Plan& plan)
{ OpData result = plan.summary; result.Set("plan", plan.token); result.Set("path", plan.path); result.Set("assetGuid", plan.guid); result.Set("type", plan.type); result.Set("applied", plan.applied); return result; }
OpParam Parameter(std::string name, OpParamType type, bool required)
{ OpParam parameter; parameter.name = std::move(name); parameter.type = type; parameter.required = required; return parameter; }
} /// @note namespace

void RegisterManagedDataAssetOperators(OperatorRegistry& registry)
{
    auto state = std::make_shared<State>();
    EditorOperator catalog; catalog.id = "asset.data.catalog"; catalog.label = "Managed DataAsset Types"; catalog.category = "DataAsset"; catalog.kind = OpKind::Query;
    catalog.params = {Parameter("start", OpParamType::Int, false), Parameter("count", OpParamType::Int, false)};
    catalog.exec = [](OpContext&, const OpArgs& args) {
        const int start = args.GetInt("start", 0), count = args.GetInt("count", 32); if (start < 0 || count < 0 || count > 64 || start > std::numeric_limits<int>::max() - count) return OpResult::Err("BAD_RANGE", "Type catalog packet requires checked nonnegative bounds and at most64types.");
        OpData result = OpData::MakeObject(), types = OpData::MakeArray(); uint32_t total = 0;
        if (count == 0) { scene::ManagedBridgeRequest request; request.op = Operation::GET_DATA_ASSET_SCHEMA; request.generation = Runtime::Instance().GetAppliedGeneration(); scene::ManagedBridgeReply reply; std::string ignored;
            if (!Runtime::Instance().Dispatch(request, reply, &ignored) && reply.status != scene::ManagedBridgeStatus::NOT_FOUND) return OpResult::Err("ASSET_SCHEMA_UNAVAILABLE", Runtime::Instance().LastDiagnostic()); total = reply.index; }
        for (int index = start; index < start + count; ++index) {
            scene::ManagedBridgeRequest request; request.op = Operation::GET_DATA_ASSET_SCHEMA; request.generation = Runtime::Instance().GetAppliedGeneration(); request.index = static_cast<uint32_t>(index);
            scene::ManagedBridgeReply reply; std::string key;
            if (!Runtime::Instance().Dispatch(request, reply, &key)) { if (reply.status == scene::ManagedBridgeStatus::NOT_FOUND) { total = reply.index; break; } return OpResult::Err("ASSET_SCHEMA_UNAVAILABLE", Runtime::Instance().LastDiagnostic()); }
            total = reply.index; request.flags = 2; std::string name;
            if (!Runtime::Instance().Dispatch(request, reply, &name) || reply.index != total) return OpResult::Err("ASSET_SCHEMA_UNAVAILABLE", "Current DataAsset catalog changed during its bounded page read.");
            OpData row = OpData::MakeObject(); row.Set("type", key); row.Set("displayName", name); row.Set("schemaQuery", "asset.data.schema"); types.Push(std::move(row));
        }
        const size_t returned = types.AsArray().size(); result.Set("types", std::move(types)); result.Set("total", static_cast<int64_t>(total)); result.Set("start", start); result.Set("returned", static_cast<int64_t>(returned));
        result.Set("nextStart", start + returned < total ? OpData(static_cast<int64_t>(start + returned)) : OpData{}); return OpResult::Data(std::move(result));
    }; registry.Register(std::move(catalog));
    EditorOperator schema; schema.id = "asset.data.schema"; schema.label = "Managed DataAsset Schema"; schema.category = "DataAsset"; schema.kind = OpKind::Query; schema.params = {Parameter("type", OpParamType::String, true)};
    schema.exec = [](OpContext&, const OpArgs& args) { OpData result; return TypeSchema(args.GetString("type"), result) ? OpResult::Data(std::move(result)) : OpResult::Err("ASSET_SCHEMA_UNAVAILABLE", "Apply C# code and request a current persistent DataAsset type."); }; registry.Register(std::move(schema));
    EditorOperator inspect; inspect.id = "asset.data.inspect"; inspect.label = "Inspect Managed DataAsset Page"; inspect.category = "DataAsset"; inspect.kind = OpKind::Query; inspect.params = {Parameter("path", OpParamType::String, true), Parameter("page", OpParamType::Data, false)};
    inspect.exec = [](OpContext& context, const OpArgs& args) {
        std::string path, reason, bytes, type, json; if (!ResolvePath(context.ctx, args.GetString("path"), path, reason)) return OpResult::Err("BAD_ASSET_PATH", reason);
        if (!util::FileSystem::ReadText(path, bytes) || !SourceJson(bytes, type, json)) return OpResult::Err("BAD_ASSET_FORMAT", "Managed .fzdata requires its portable format2 value document.");
        const auto guid = DiskGuid(path); if (guid.empty()) return OpResult::Err("MISSING_ASSET_META", "Import the asset to establish its stable meta GUID.");
        Document document; OpData summary; if (!Open(document, type, guid, false, json, summary, reason)) return OpResult::Err("ASSET_DOCUMENT_FAILED", reason);
        const auto* page = args.GetData("page"); std::string output; scene::ManagedBridgeReply reply; OpData result;
        std::string packet = "{\"path\":\"\"}"; if (page && !WriteOpDataJson(*page, packet)) return OpResult::Err("ASSET_PAGE_INPUT", "Page arguments contain unsupported or nonfinite JSON values.");
        if (!Call(Operation::READ_DATA_ASSET_PAGE, document, 0, packet, output, reply) || !ReadManagedSettingsJson(output, result)) return OpResult::Err("ASSET_PAGE_FAILED", Runtime::Instance().LastDiagnostic());
        result.Set("type", type); result.Set("pathOnDisk", path); return OpResult::Data(std::move(result));
    }; registry.Register(std::move(inspect));
    const auto preview = [&](const char* id, bool create) {
        EditorOperator operation; operation.id = id; operation.label = create ? "Create Managed DataAsset Preview" : "Edit Managed DataAsset Preview";
        operation.category = "DataAsset"; operation.kind = OpKind::Action; operation.params = {Parameter("path", OpParamType::String, true), Parameter("type", OpParamType::String, create), Parameter("fields", OpParamType::Data, false), Parameter("patches", OpParamType::Data, false), Parameter("expectedRevision", OpParamType::String, false), Parameter("expectedAssetGuid", OpParamType::String, false), Parameter("expectedType", OpParamType::String, false), Parameter("expectedGeneration", OpParamType::String, false)};
        operation.exec = [state, create](OpContext& context, const OpArgs& args) {
            if (state->plans.size() >= 32) return OpResult::Err("ASSET_PLAN_LIMIT", "Discard unused DataAsset previews.");
            std::string reason; auto plan = Preview(context.ctx, args, create, reason); if (!plan) return OpResult::Err("ASSET_PREPARE_FAILED", reason);
            state->plans.emplace(plan->token, plan); return OpResult::Data(Describe(*plan));
        }; registry.Register(std::move(operation));
    };
    preview("asset.data.create.preview", true); preview("asset.data.configure.preview", false);
    EditorOperator commit; commit.id = "asset.data.commit"; commit.label = "Save Managed DataAsset"; commit.category = "DataAsset"; commit.kind = OpKind::Mutation; commit.params = {Parameter("plan", OpParamType::String, true)};
    commit.exec = [state](OpContext& context, const OpArgs& args) {
        const auto found = state->plans.find(args.GetString("plan")); if (found == state->plans.end()) return OpResult::Err("EXPIRED_ASSET_PLAN", "Prepare a fresh DataAsset candidate.");
        auto plan = found->second; if (plan->document.generation != Runtime::Instance().GetAppliedGeneration()) return OpResult::Err("STALE_GENERATION", "C# reloaded; reopen the DataAsset with the current schema.");
        std::string reason; if (!Publish(context.ctx, *plan, false, reason)) return OpResult::Err("ASSET_SAVE_FAILED", reason);
        plan->document.Close();
        state->plans.erase(found); auto* editor = &context.ctx;
        OpResult result = OpResult::Data(Describe(*plan)); result.command = std::make_unique<CheckedLambdaCommand>("Save DataAsset " + plan->path,
            [editor, plan](std::string& reason) { return Publish(*editor, *plan, false, reason); }, [editor, plan](std::string& reason) { return Publish(*editor, *plan, true, reason); }); return result;
    }; registry.Register(std::move(commit));
    EditorOperator review; review.id = "asset.data.review"; review.label = "Review Managed DataAsset"; review.category = "DataAsset"; review.kind = OpKind::Query; review.params = {Parameter("plan", OpParamType::String, true), Parameter("page", OpParamType::Data, false)};
    review.exec = [state](OpContext&, const OpArgs& args) {
        const auto found = state->plans.find(args.GetString("plan")); if (found == state->plans.end()) return OpResult::Err("EXPIRED_ASSET_PLAN", "Prepare a fresh DataAsset candidate.");
        const auto* page = args.GetData("page"); std::string output; scene::ManagedBridgeReply reply; OpData result;
        std::string packet = "{}"; if (page && !WriteOpDataJson(*page, packet)) return OpResult::Err("ASSET_REVIEW_INPUT", "Review arguments contain unsupported or nonfinite JSON values.");
        if (!Call(Operation::READ_PREPARED_DATA_ASSET, found->second->document, 1, packet, output, reply) || !ReadManagedSettingsJson(output, result)) return OpResult::Err("ASSET_REVIEW_FAILED", Runtime::Instance().LastDiagnostic());
        return OpResult::Data(std::move(result));
    }; registry.Register(std::move(review));
    EditorOperator discard; discard.id = "asset.data.discard"; discard.label = "Discard Managed DataAsset Preview"; discard.category = "DataAsset"; discard.kind = OpKind::Action; discard.params = {Parameter("plan", OpParamType::String, true)};
    discard.exec = [state](OpContext&, const OpArgs& args) { state->plans.erase(args.GetString("plan")); return OpResult::Ok(); }; registry.Register(std::move(discard));
}

namespace {
struct AssetView {
    std::string project, path, type, guid, revision, diagnostic;
    std::filesystem::file_time_type stamp;
    uintmax_t bytes = 0;
    Document document;
    OpData schema;
    std::unordered_map<std::string, OpData> pages, values, patches;
    std::unordered_map<std::string, ManagedSettingsPageCursor> pageCursors;
};
/// @note 選択が移っても未保存の値を保持し、SaveAll の callback と共有する。
std::unordered_map<std::string, std::shared_ptr<AssetView>> assetViews;
bool SaveAssetView(EditorContext& context, const std::shared_ptr<AssetView>& view)
{
    OpData patches = OpData::MakeArray();
    std::vector<std::string> order;
    for (const auto& [key, patch] : view->patches) { (void)patch; order.push_back(key); }
    std::sort(order.begin(), order.end());
    for (const auto& key : order) patches.Push(view->patches.at(key));
    OpArgs args;
    args.Set("path", view->path);
    args.Set("expectedRevision", view->revision);
    args.Set("expectedAssetGuid", view->guid);
    args.Set("expectedType", view->type);
    args.Set("expectedGeneration", std::to_string(view->document.generation));
    args.Set("patches", patches);
    const auto prepared = InvokeOperator(context, "asset.data.configure.preview", args);
    if (!prepared.ok) { view->diagnostic = prepared.message; return false; }
    OpArgs commit;
    commit.Set("plan", Text(prepared.data, "plan"));
    const auto applied = InvokeOperator(context, "asset.data.commit", commit);
    if (!applied.ok) {
        view->diagnostic = applied.message;
        (void)InvokeOperator(context, "asset.data.discard", commit);
        return false;
    }
    view->patches.clear();
    assetViews.erase(view->path);
    return true;
}
const OpData* Page(AssetView& view, const std::string& path, uint64_t start = 0, uint32_t count = 32)
{
    const std::string key = path + "#" + std::to_string(start) + ":" + std::to_string(count);
    if (auto found = view.pages.find(key); found != view.pages.end()) return &found->second;
    OpData request = OpData::MakeObject(); request.Set("path", path); request.Set("start", static_cast<int64_t>(start)); request.Set("count", static_cast<int64_t>(count));
    scene::ManagedBridgeReply reply; std::string json; OpData page;
    std::string packet; if (!WriteOpDataJson(request, packet)) { view.diagnostic = "DataAsset page request contains unsupported JSON values."; return nullptr; }
    if (!Call(Operation::READ_DATA_ASSET_PAGE, view.document, 0, packet, json, reply) || !ReadManagedSettingsJson(json, page)) { view.diagnostic = Runtime::Instance().LastDiagnostic(); return nullptr; }
    return &view.pages.emplace(key, std::move(page)).first->second;
}
bool ValidAssetPage(const OpData& page, uint64_t expectedStart)
{
    const OpData* start = page.Find("start"), *returned = page.Find("returned"), *total = page.Find("total"), *next = page.Find("nextStart");
    if (!start || !start->IsInt() || start->AsInt64() < 0 || static_cast<uint64_t>(start->AsInt64()) != expectedStart
        || !returned || !returned->IsInt() || returned->AsInt64() < 0
        || !total || !total->IsInt() || total->AsInt64() < 0 || !next) return false;
    const uint64_t count = static_cast<uint64_t>(returned->AsInt64()), size = static_cast<uint64_t>(total->AsInt64());
    if (expectedStart > size || count > size - expectedStart) return false;
    const uint64_t following = expectedStart + count;
    if (next->IsNull()) {
        if (following != size) return false;
    } else if (!next->IsInt() || next->AsInt64() < 0 || static_cast<uint64_t>(next->AsInt64()) != following || following >= size) return false;
    if (const OpData* items = page.Find("items")) {
        if (!items->IsArray() || items->AsArray().size() != count) return false;
    } else if (const OpData* text = page.Find("text")) {
        const OpData* unit = page.Find("unit");
        if (!text->IsString() || !unit || !unit->IsString() || unit->AsString() != "utf16_code_units") return false;
    } else if (count != 1 || !page.Find("value")) return false;
    return count != 0 || size == 0 || expectedStart == size;
}
void Queue(AssetView& view, const std::string& path, const OpData& value)
{ OpData patch = OpData::MakeObject(); patch.Set("path", path); patch.Set("value", value); view.patches[path] = std::move(patch); view.values[path] = value; }
void DrawValue(EditorContext& context, AssetView& view, const std::string& path, const std::string& label, const OpData& schema, const OpData& supplied)
{
    ImGui::PushID(path.c_str()); const std::string kind = Text(schema, "valueKind"); OpData value = supplied;
    if (const auto found = view.values.find(path); found != view.values.end()) value = found->second;
    if (value.Find("$deferred") || kind == "list" || kind == "object") {
        if (value.IsNull() && kind == "object") {
            ImGui::TextUnformatted(label.c_str()); ImGui::SameLine(); if (ImGui::SmallButton("作成")) { OpData patch = OpData::MakeObject(); patch.Set("path", path); patch.Set("action", "create"); view.patches[path] = std::move(patch); }
        } else if (ImGui::TreeNodeEx("##node", 0, "%s", label.c_str())) {
            ManagedSettingsPageCursor& cursor = view.pageCursors[path];
            const OpData* page = Page(view, path, cursor.Start(), 32);
            if (page && !ValidAssetPage(*page, cursor.Start())) {
                view.diagnostic = "DataAsset page response has an invalid range or payload shape.";
                page = nullptr;
            }
            ImGui::BeginDisabled(cursor.PageNumber() <= 1);
            if (ImGui::SmallButton("前へ") && cursor.Back()) {
                page = Page(view, path, cursor.Start(), 32);
                if (page && !ValidAssetPage(*page, cursor.Start())) { view.diagnostic = "DataAsset page response has an invalid range or payload shape."; page = nullptr; }
            }
            ImGui::EndDisabled(); ImGui::SameLine();
            ImGui::BeginDisabled(!page || !cursor.CanAdvance(*page));
            if (ImGui::SmallButton("次へ") && page && cursor.Advance(*page)) {
                page = Page(view, path, cursor.Start(), 32);
                if (page && !ValidAssetPage(*page, cursor.Start())) { view.diagnostic = "DataAsset page response has an invalid range or payload shape."; page = nullptr; }
            }
            ImGui::EndDisabled();
            if (page) {
                const auto* pageStart = page->Find("start"), *pageReturned = page->Find("returned"), *pageTotal = page->Find("total");
                ImGui::SameLine(); ImGui::Text("%zu · %lld +%lld / %lld", cursor.PageNumber(), pageStart->AsInt64(), pageReturned->AsInt64(), pageTotal->AsInt64());
                const auto* items = page->Find("items"), *text = page->Find("text");
                if (items && items->IsArray()) for (size_t index = 0; index < items->AsArray().size(); ++index) {
                    const auto& item = items->AsArray()[index]; const OpData* childSchema = schema.Find("element"), *child = &item;
                    std::string key = std::to_string(static_cast<size_t>(pageStart->AsInt64()) + index), childLabel = key;
                    if (kind == "object") {
                        key = Text(item, "name"); child = item.Find("value"); childSchema = nullptr;
                        if (const auto* fields = schema.Find("fields"); fields && fields->IsArray()) for (const auto& field : fields->AsArray()) if (Text(field, "name") == key) { childSchema = field.Find("value"); childLabel = Text(field, "label"); break; }
                    }
                    if (child && childSchema) DrawValue(context, view, ManagedSettingChildPath(path, key), childLabel, *childSchema, *child);
                }
                if (text && text->IsString()) {
                    const int64_t observedStart = pageStart->AsInt64(), observedCount = pageReturned->AsInt64();
                    const std::string patchKey = path + "@text:" + std::to_string(observedStart);
                    std::string edited = text->AsString(); if (auto previous = view.patches.find(patchKey); previous != view.patches.end()) edited = Text(previous->second, "value");
                    const std::string pageId = std::to_string(observedStart);
                    ImGui::PushID(pageId.c_str());
                    ImGuiReflector ui; ui.Field(label.c_str(), edited);
                    ImGui::PopID();
                    if (ui.m_changed) {
                        OpData patch = OpData::MakeObject(); patch.Set("path", path); patch.Set("action", "text_replace");
                        patch.Set("index", observedStart); patch.Set("count", observedCount); patch.Set("value", edited);
                        view.patches[patchKey] = std::move(patch); cursor.InvalidateFuture();
                    }
                }
            }
            if (kind == "list" && ImGui::SmallButton("追加")) { OpData patch = OpData::MakeObject(); patch.Set("path", path + "/-"); patch.Set("action", "create"); view.patches[path + "/-"] = std::move(patch); }
            ImGui::TreePop();
        }
        ImGui::PopID(); return;
    }
    ImGuiReflector ui;
    if (value.IsBool()) { bool next = value.AsBool(); ui.Field(label.c_str(), next); if (ui.m_changed) value = OpData(next); }
    else if (DrawManagedPrimitive(ui, label.c_str(), schema, value)) {}
    else if (value.IsArray()) {
        float next[4]{}; const auto count = value.AsArray().size(); if (count >= 2 && count <= 4 && ui.BeginRow(label.c_str())) {
            for (size_t index = 0; index < count; ++index) next[index] = value.AsArray()[index].IsInt() ? static_cast<float>(value.AsArray()[index].AsInt64()) : static_cast<float>(value.AsArray()[index].AsDouble());
            ui.m_changed = count == 2 ? ImGui::DragFloat2("##value", next, 0.01f) : count == 3 ? ImGui::DragFloat3("##value", next, 0.01f) : ImGui::DragFloat4("##value", next, 0.01f); ui.EndRow();
            if (ui.m_changed) { value = OpData::MakeArray(); for (size_t index = 0; index < count; ++index) value.Push(next[index]); }
        }
    } else if (kind == "materialAssetRef" || kind == "textureAssetRef" || kind == "modelAssetRef" || kind == "dataAssetRef") {
        const std::string before = value.IsString() ? value.AsString() : std::string{};
        std::string selected = before.empty() ? std::string{} : asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(before));
        const char* extensions = kind == "materialAssetRef" ? ".mat" : kind == "textureAssetRef" ? ".png,.jpg,.jpeg,.dds,.tga,.bmp,.hdr"
            : kind == "modelAssetRef" ? ".fbx,.fzasset,.fzmodel,.obj,.gltf,.glb" : ".fzdata";
        if (ui.BeginRow(label.c_str())) {
            if (widgets::AssetPathField("##asset", selected, extensions, context.projectRoot)) {
                bool accepted = selected.empty();
                if (!selected.empty()) {
                    auto absolute = util::FileSystem::PathFromUtf8(selected); if (!absolute.is_absolute()) absolute = util::FileSystem::PathFromUtf8(context.projectRoot) / absolute;
                    const std::string path = util::FileSystem::PathToUtf8(absolute);
                    if (kind == "materialAssetRef") accepted = asset::AssetManager::Load<asset::MaterialAsset>(path).IsValid();
                    else if (kind == "textureAssetRef") accepted = asset::AssetManager::Load<asset::TextureAsset>(path).IsValid();
                    else if (kind == "modelAssetRef") accepted = asset::AssetManager::Load<asset::ModelAsset>(path).IsValid();
                    else { std::string bytes, type, json; OpData selectedSchema; accepted = util::FileSystem::ReadText(path, bytes) && SourceJson(bytes, type, json)
                        && TypeSchema(type, selectedSchema) && Text(selectedSchema, "displayName") == Text(schema, "targetType"); }
                    if (accepted) { const auto guid = asset::AssetDatabase::GuidFromPath(path); accepted = !guid.empty(); if (accepted) value = OpData("guid:" + guid); }
                } else value = OpData("");
                ui.m_changed = accepted; if (!accepted) view.diagnostic = "参照先のアセット型が設定の型と一致しません。";
            }
            ui.EndRow();
        }
    } else if (value.IsString()) { std::string next = value.AsString(); ui.Field(label.c_str(), next); if (ui.m_changed) value = OpData(std::move(next)); }
    if (ui.m_changed) Queue(view, path, value); ImGui::PopID();
}
} /// @note namespace

bool DrawManagedDataAssetInspector(EditorContext& context, const std::string& input)
{
    std::string path, reason; if (!ResolvePath(context, input, path, reason)) return false;
    std::error_code error; const auto native = util::FileSystem::PathFromUtf8(path); const auto stamp = std::filesystem::last_write_time(native, error);
    if (error) return false; const auto size = std::filesystem::file_size(native, error); if (error) return false;
    const auto currentGuid = DiskGuid(path);
    for (auto it = assetViews.begin(); it != assetViews.end();) {
        if (it->first != path && !AssetDirtyRegistry::IsDirty(it->first)) it = assetViews.erase(it);
        else ++it;
    }
    auto selected = assetViews.find(path);
    if (selected != assetViews.end() && selected->second->project != context.projectRoot) {
        CloseManagedDataAssetEditorDocuments();
        selected = assetViews.end();
    }
    const bool pending = selected != assetViews.end() && !selected->second->patches.empty()
        && AssetDirtyRegistry::IsDirty(path);
    if (!pending && (selected == assetViews.end() || !selected->second->patches.empty()
        || selected->second->document.generation != Runtime::Instance().GetAppliedGeneration()
        || selected->second->stamp != stamp || selected->second->bytes != size || selected->second->guid != currentGuid)) {
        std::string bytes, type, json; if (!util::FileSystem::ReadText(path, bytes) || !SourceJson(bytes, type, json)) return false;
        auto openedView = std::make_shared<AssetView>(); auto& view = *openedView; view.project = context.projectRoot; view.path = path; view.type = type; view.stamp = stamp; view.bytes = size;
        OpData opened; const auto guid = currentGuid; view.guid = guid;
        if (guid.empty() || !TypeSchema(type, view.schema) || !Open(view.document, type, guid, false, json, opened, view.diagnostic)) { if (view.diagnostic.empty()) view.diagnostic = "型または参照が利用できません。C# を適用してから再取得してください。"; }
        else view.revision = Text(opened, "revision");
        selected = assetViews.insert_or_assign(path, std::move(openedView)).first;
    }
    const auto selectedView = selected->second;
    auto& view = *selectedView; ImGui::TextUnformatted(Text(view.schema, "displayName").c_str());
    if (ImGui::Button(view.patches.empty() ? "再取得" : "変更を破棄して再取得")) {
        AssetDirtyRegistry::MarkClean(path);
        assetViews.erase(path);
        return true;
    }
    if (!view.document.id || view.revision.empty()) { ImGui::TextWrapped("%s", view.diagnostic.c_str()); return true; }
    const bool currentGeneration = view.document.generation == Runtime::Instance().GetAppliedGeneration();
    if (!currentGeneration) ImGui::TextWrapped("C# が再読み込みされました。未保存の変更は保持しています。再取得する前に変更を確認してください。");
    ImGui::BeginDisabled(!Editable(context) || !currentGeneration);
    if (const auto* fields = view.schema.Find("fields"); fields && fields->IsArray()) for (const auto& field : fields->AsArray()) {
        const auto* saved = field.Find("setting"); if (!saved || !saved->IsBool() || !saved->AsBool()) continue;
        const auto path = ManagedSettingChildPath("", Text(field, "name")); const auto* page = Page(view, path, 0, 1);
        const auto* value = page ? page->Find("value") : nullptr; const auto* schema = field.Find("value");
        if (value && schema) DrawValue(context, view, path, Text(field, "label"), *schema, *value);
        else if (page && schema) { OpData deferred = OpData::MakeObject(); deferred.Set("$deferred", path); DrawValue(context, view, path, Text(field, "label"), *schema, deferred); }
    }
    if (!view.patches.empty()) {
        /// @note EditorContext はプロジェクトを閉じるまで生存し、その前に CloseManagedDataAssetEditorDocuments が callback を解除する。
        AssetDirtyRegistry::Register(path, path, "DATA", [&context, selectedView] {
            return SaveAssetView(context, selectedView);
        });
        ImGui::TextDisabled("未保存の変更があります。");
        if (ImGui::Button("設定を保存") && AssetDirtyRegistry::Save(path)) {
            ImGui::EndDisabled();
            return true;
        }
    }
    ImGui::EndDisabled(); if (!view.diagnostic.empty()) ImGui::TextWrapped("%s", view.diagnostic.c_str()); return true;
}

void DrawManagedDataAssetCreateMenu(EditorContext& context, const std::string& directory)
{
    static uint64_t generation = 0; static int start = 0; static std::unordered_map<int, OpData> pages;
    const uint64_t current = Runtime::Instance().GetAppliedGeneration(); if (current != generation) { generation = current; start = 0; pages.clear(); }
    if (!pages.contains(start)) { OpArgs request; request.Set("start", start); const auto catalog = InvokeOperator(context, "asset.data.catalog", request); if (!catalog.ok) return; pages.emplace(start, catalog.data); }
    const auto& catalog = pages.at(start); const auto* types = catalog.Find("types"); if (!types || !types->IsArray()) return;
    for (const auto& type : types->AsArray()) {
        const auto label = Text(type, "displayName"), key = Text(type, "type");
        if (!ImGui::MenuItem(label.c_str(), nullptr, false, Editable(context))) continue;
        std::string name = label.substr(label.find_last_of('.') == std::string::npos ? 0 : label.find_last_of('.') + 1);
        std::string path = directory + "/New " + name + ".fzdata"; int suffix = 1;
        while (util::FileSystem::Exists(path) || util::FileSystem::Exists(path + ".meta")) path = directory + "/New " + name + " " + std::to_string(suffix++) + ".fzdata";
        OpArgs create; create.Set("path", path); create.Set("type", key); const auto prepared = InvokeOperator(context, "asset.data.create.preview", create);
        if (!prepared.ok) continue; OpArgs commit; commit.Set("plan", Text(prepared.data, "plan")); const auto applied = InvokeOperator(context, "asset.data.commit", commit);
        if (applied.ok) { context.requestRevealAssetPath = path; context.requestAssetBrowserRefresh = true; }
        else (void)InvokeOperator(context, "asset.data.discard", commit);
    }
    ImGui::Separator(); if (start > 0 && ImGui::MenuItem("前の型一覧")) start = std::max(0, start - 32);
    const auto* next = catalog.Find("nextStart"); int following = 0;
    if (next && next->TryGetInt32(following) && following > start && ImGui::MenuItem("次の型一覧")) start = following;
}
void CloseManagedDataAssetEditorDocuments()
{
    for (const auto& [path, view] : assetViews) {
        if (!view->patches.empty()) AssetDirtyRegistry::MarkClean(path);
    }
    assetViews.clear();
}
} /// @note namespace fbzz::editor
