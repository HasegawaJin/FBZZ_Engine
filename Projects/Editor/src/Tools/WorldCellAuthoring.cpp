/// @file    WorldCellAuthoring.cpp
/// @brief   Checked world-grid authoring and observable streaming controls shared by UI and AI.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include "WorldAuthoring.hpp"
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/WorldPartition.hpp>
#include <Engine/Scene/Components/WorldPartitionComponent.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace fbzz::editor {
namespace {
using Partition = scene::WorldPartitionComponent;

bool Authoring(const EditorContext& ctx)
{
    return ctx.activeScene && !ctx.projectRoot.empty() && !ctx.managedBuildBusy
        && (!ctx.playMode || (ctx.playMode->IsInEditor() && !ctx.playMode->HasPendingRestore()));
}

scene::GameObject* PartitionNode(EditorContext& ctx, const std::string& requested, std::string& error)
{
    if (!ctx.activeScene) { error = "No active scene"; return nullptr; }
    if (!requested.empty()) {
        auto* node = ctx.activeScene->FindByGuid(requested);
        if (!node || !node->GetComponent<Partition>()) error = "Partition target does not have WorldPartitionComponent";
        return error.empty() ? node : nullptr;
    }
    scene::GameObject* found = nullptr;
    for (const auto id : ctx.activeScene->GetEntities<Partition>()) {
        auto* candidate = ctx.activeScene->GetGameObject(id);
        if (!candidate || candidate->runtimeGenerated) continue;
        if (found) { error = "Multiple partitions exist; specify partition explicitly"; return nullptr; }
        found = candidate;
    }
    if (!found) error = "Configure a world partition first";
    return found;
}

bool ProjectAssetPath(const EditorContext& ctx, std::string reference, std::string& absolute)
{
    if (reference.empty() || reference.find('\0') != std::string::npos) return false;
    if (reference.starts_with("guid:")) reference = asset::AssetManager::ResolveAssetPath(reference);
    auto path = util::FileSystem::PathFromUtf8(reference);
    if (!path.is_absolute()) path = util::FileSystem::PathFromUtf8(ctx.projectRoot) / path;
    path = path.lexically_normal();
    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(ctx.projectRoot) / "Assets", ec);
    if (ec) return false;
    const auto resolved = std::filesystem::weakly_canonical(path, ec);
    if (ec || !util::FileSystem::IsChildPathText(util::FileSystem::PathToUtf8(resolved), util::FileSystem::PathToUtf8(root))) return false;
    absolute = util::FileSystem::PathToUtf8(resolved);
    return true;
}

uint64_t Fingerprint(std::string_view value)
{
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char c : value) { hash ^= c; hash *= 1099511628211ull; }
    return hash;
}

std::string ConfigKey(const Partition& c)
{
    std::ostringstream s; s << std::setprecision(9) << c.manifestPath << '\n'
        << c.gridOrigin.x << ',' << c.gridOrigin.y << ',' << c.gridOrigin.z << ',' << c.cellSize << ','
        << c.nearDistance << ',' << c.hlodDistance << ',' << c.hysteresis << ',' << c.maxConcurrentLoads << ','
        << c.maxPublishesPerFrame << ',' << c.streamInEditor << ',' << c.enabled << ',' << c.useMainCamera << ','
        << c.sourceCamera.guid << ',' << c.sourcePosition.x << ',' << c.sourcePosition.y << ',' << c.sourcePosition.z << ','
        << c.maxResidentCells << ',' << c.maxCellMegabytes << ',' << c.maxResidentMegabytes;
    return s.str();
}

void ApplyConfig(Partition& to, const Partition& from)
{
    to.manifestPath = from.manifestPath; to.gridOrigin = from.gridOrigin; to.cellSize = from.cellSize;
    to.nearDistance = from.nearDistance; to.hlodDistance = from.hlodDistance; to.hysteresis = from.hysteresis;
    to.maxConcurrentLoads = from.maxConcurrentLoads; to.maxPublishesPerFrame = from.maxPublishesPerFrame;
    to.streamInEditor = from.streamInEditor;
}

OpData PartitionData(EditorContext& ctx, const scene::GameObject& node, const Partition& c)
{
    OpData data = OpData::MakeObject();
    data.Set("partition", node.instanceId); data.Set("manifest", c.manifestPath);
    data.Set("origin", c.gridOrigin); data.Set("cellSize", c.cellSize);
    data.Set("nearDistance", c.nearDistance); data.Set("hlodDistance", c.hlodDistance); data.Set("hysteresis", c.hysteresis);
    data.Set("maxConcurrentReads", c.maxConcurrentLoads); data.Set("maxCommitsPerUpdate", c.maxPublishesPerFrame);
    data.Set("streamInEditor", c.streamInEditor);
    std::string path, text, error; scene::WorldPartitionManifest manifest;
    const bool saved = ProjectAssetPath(ctx, c.manifestPath, path) && util::FileSystem::ReadText(path, text)
        && scene::DeserializeWorldPartitionManifest(text, manifest, error);
    data.Set("persisted", saved); data.Set("cells", static_cast<int>(saved ? manifest.cells.size() : 0));
    data.Set("revision", std::to_string(saved ? manifest.revision : 0));
    const bool matches=!saved||(manifest.grid.origin==c.gridOrigin&&manifest.grid.cellSize==c.cellSize);
    data.Set("gridMatchesManifest",matches);data.Set("needsBake",!saved||!matches);
    data.Set("expected", saved ? std::to_string(Fingerprint(text)) : std::string{});
    if (!error.empty()) data.Set("diagnostic", error);
    return data;
}

scene::WorldPartitionSettings RuntimeSettings(const Partition& c, bool playing)
{
    scene::WorldPartitionSettings settings;
    settings.nearDistance = c.nearDistance; settings.hlodDistance = c.hlodDistance; settings.hysteresis = c.hysteresis;
    settings.maxConcurrentLoads = c.maxConcurrentLoads > 0 ? static_cast<uint32_t>(c.maxConcurrentLoads) : 0;
    settings.maxPublishesPerFrame = c.maxPublishesPerFrame > 0 ? static_cast<uint32_t>(c.maxPublishesPerFrame) : 0;
    settings.maxResidentCells = c.maxResidentCells > 0 ? static_cast<uint32_t>(c.maxResidentCells) : 0;
    settings.maxCellBytes = c.maxCellMegabytes > 0 ? static_cast<uint64_t>(c.maxCellMegabytes) * 1024 * 1024 : 0;
    settings.maxResidentBytes = c.maxResidentMegabytes > 0 ? static_cast<uint64_t>(c.maxResidentMegabytes) * 1024 * 1024 : 0;
    settings.retireSourceObjects = playing;
    return settings;
}
}

OpResult InspectWorldCells(OpContext& context, const OpArgs& args)
{
    std::string error;
    auto* node = PartitionNode(context.ctx, args.GetString("partition"), error);
    if (!node) return OpResult::Err("NO_PARTITION", error);
    return OpResult::Data(PartitionData(context.ctx, *node, *node->GetComponent<Partition>()));
}

OpResult ConfigureWorldCells(OpContext& context, const OpArgs& args)
{
    auto& ctx = context.ctx;
    if (!Authoring(ctx) || !context.undo.IsRecordingEnabled()) return OpResult::Err("EDIT_REQUIRED", "Partition configuration requires an editable scene and Undo recording");
    std::string error;
    const std::string requested = args.GetString("partition");
    auto* node = PartitionNode(ctx, requested, error);
    if (!node && (!requested.empty() || !ctx.activeScene->GetEntities<Partition>().empty())) return OpResult::Err("BAD_PARTITION", error);
    const bool created = node == nullptr;
    const Partition before = node ? *node->GetComponent<Partition>() : Partition{};
    Partition after = before;
    after.manifestPath = args.GetString("manifest", before.manifestPath.empty() ? "Assets/World/World.worldcells" : before.manifestPath);
    after.gridOrigin = args.GetVec3("origin", before.gridOrigin); after.cellSize = args.GetFloat("cellSize", before.cellSize);
    after.nearDistance = args.GetFloat("nearDistance", before.nearDistance); after.hlodDistance = args.GetFloat("hlodDistance", before.hlodDistance);
    after.hysteresis = args.GetFloat("hysteresis", before.hysteresis);
    after.maxConcurrentLoads = args.GetInt("maxConcurrentReads", before.maxConcurrentLoads);
    after.maxPublishesPerFrame = args.GetInt("maxCommitsPerUpdate", before.maxPublishesPerFrame);
    after.streamInEditor = args.GetBool("streamInEditor", before.streamInEditor);
    if (!scene::ValidateWorldCellGrid({after.gridOrigin, after.cellSize}, error)
        || !scene::ValidateWorldPartitionSettings(RuntimeSettings(after, false), error)) return OpResult::Err("BAD_ARG", error);
    std::string path;
    if (!ProjectAssetPath(ctx, after.manifestPath, path) || util::FileSystem::PathFromUtf8(path).extension() != ".worldcells")
        return OpResult::Err("BAD_PATH", "Manifest must be a .worldcells asset inside this project's Assets directory");
    if (!created && ConfigKey(before) == ConfigKey(after)) {
        auto result = OpResult::Data(PartitionData(ctx, *node, before)); result.noChange = true; return result;
    }
    struct State { std::string id; bool applied = false; scene::EntityID entity; uint64_t lifetime = 0; };
    auto state = std::make_shared<State>();
    if (node) { state->id = node->instanceId; state->entity = node->GetID(); state->lifetime = ctx.activeScene->GetComponentLifetime<Partition>(state->entity); }
    auto* targetScene = ctx.activeScene; const uint64_t generation = targetScene->GetRenderSceneGeneration();
    auto apply = [&ctx, targetScene, generation, state, before, after, created](bool forward, std::string& reason) {
        if (ctx.activeScene != targetScene || targetScene->GetRenderSceneGeneration() != generation || !Authoring(ctx)) { reason = "The edited scene changed"; return false; }
        auto* target = state->id.empty() ? nullptr : targetScene->FindByGuid(state->id);
        if (forward) {
            if (state->applied) { reason = "Configuration is already applied"; return false; }
            if (created) {
                if (target) { reason = "The created partition identity is occupied"; return false; }
                target = targetScene->TryCreateGameObject("World Partition");
                if (!target) { reason = "Scene entity capacity exhausted"; return false; }
                if (state->id.empty()) state->id = target->instanceId; else target->instanceId = state->id;
                target->AddComponent<Partition>(after); state->entity = target->GetID();
                state->lifetime = targetScene->GetComponentLifetime<Partition>(state->entity);
            } else {
                auto* component = target ? target->GetComponent<Partition>() : nullptr;
                if (!component || target->GetID() != state->entity || targetScene->GetComponentLifetime<Partition>(state->entity) != state->lifetime || ConfigKey(*component) != ConfigKey(before)) { reason = "Partition configuration changed externally"; return false; }
                ApplyConfig(*component, after);
            }
            state->applied = true;
        } else {
            auto* component = target ? target->GetComponent<Partition>() : nullptr;
            if (!state->applied || !component || target->GetID() != state->entity || targetScene->GetComponentLifetime<Partition>(state->entity) != state->lifetime || ConfigKey(*component) != ConfigKey(after)) { reason = "Partition configuration changed externally"; return false; }
            if (created) {
                size_t components = 0; scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() { if (target->GetComponent<T>()) ++components; });
                if (components != 1 || target->GetChildCount() || target->GetParent() || target->name != "World Partition" || target->tag!="Untagged" || target->layer!=0 || !target->activeSelf() || target->runtimeGenerated || !target->prefabAssetPath.empty() || target->transform.position != math::Vector3::ZERO || target->transform.scale != math::Vector3::ONE || target->transform.rotation != math::Quaternion::Identity()) { reason = "Created partition has additional edits"; return false; }
                if (!targetScene->DestroyGameObject(target->GetID())) { reason = "Partition could not be removed"; return false; }
            } else ApplyConfig(*component, before);
            state->applied = false;
        }
        ctx.sceneDirty = true; return true;
    };
    auto command = std::make_unique<CheckedLambdaCommand>("Configure World Partition",
        [apply](std::string& reason) { return apply(true, reason); }, [apply](std::string& reason) { return apply(false, reason); });
    if (!command->TryExecute(error)) return OpResult::Err("CONFIGURE_FAILED", error);
    auto* configured = targetScene->FindByGuid(state->id);
    auto result = OpResult::Data(PartitionData(ctx, *configured, *configured->GetComponent<Partition>()));
    result.data.Set("created", created); result.command = std::move(command); return result;
}

OpResult InspectWorldStreaming(OpContext& context, const OpArgs& args)
{
    if(!args.GetString("partition").empty()){
        std::string error;
        if(!PartitionNode(context.ctx,args.GetString("partition"),error))return OpResult::Err("NO_PARTITION",error);
    }
    OpData data = OpData::MakeObject();
    const auto* runtime = context.ctx.activeScene ? scene::FindWorldPartitionRuntime(*context.ctx.activeScene) : nullptr;
    if (!runtime) { data.Set("configured", false); data.Set("cells", OpData::MakeArray()); return OpResult::Data(std::move(data)); }
    const auto status = runtime->Status();
    data.Set("configured", status.configured); data.Set("manifest", status.manifestPath); data.Set("revision", std::to_string(status.revision));
    data.Set("source", status.source); data.Set("near", status.residentNear); data.Set("hlod", status.residentHlod);
    data.Set("inFlight", status.loadsInFlight); data.Set("residentBytes", std::to_string(status.residentBytes));
    data.Set("staleDropped", std::to_string(status.staleCompletionsDropped)); data.Set("diagnostic", status.diagnostic);
    OpData cells = OpData::MakeArray();
    for (const auto& cell : status.cells) {
        OpData row = OpData::MakeObject(); row.Set("x", cell.id.x); row.Set("z", cell.id.z);
        row.Set("desired", scene::WorldCellRepresentationName(cell.desired)); row.Set("resident", scene::WorldCellRepresentationName(cell.resident));
        row.Set("state", scene::WorldCellLoadStateName(cell.state)); row.Set("objects", cell.objectCount);
        row.Set("bytes", std::to_string(cell.residentBytes)); row.Set("diagnostic", cell.diagnostic); cells.Push(std::move(row));
    }
    data.Set("cells", std::move(cells)); return OpResult::Data(std::move(data));
}

OpResult ControlWorldStreaming(OpContext& context, const OpArgs& args)
{
    auto& ctx = context.ctx;
    if (!ctx.activeScene) return OpResult::Err("NO_SCENE", "No active scene");
    std::string error; auto* node = PartitionNode(ctx, args.GetString("partition"), error);
    if (!node) return OpResult::Err("NO_PARTITION", error);
    const auto& component = *node->GetComponent<Partition>();
    const bool playing = ctx.playMode && !ctx.playMode->IsInEditor();
    const std::string action = args.GetString("action", "update");
    const auto* existingRuntime = scene::FindWorldPartitionRuntime(*ctx.activeScene);
    const auto currentSource = existingRuntime && existingRuntime->Status().configured
        ? existingRuntime->Status().source : component.sourcePosition;
    const auto source = args.GetVec3("source", currentSource);
    if (!std::isfinite(source.x) || !std::isfinite(source.y) || !std::isfinite(source.z)) return OpResult::Err("BAD_ARG", "Streaming source must be finite");
    if(action!="reset"&&action!="update"&&action!="load_near"&&action!="load_hlod"&&action!="unload"&&action!="clear_override"&&action!="retry")return OpResult::Err("BAD_ARG","Unknown streaming action");
    auto& runtime = scene::GetWorldPartitionRuntime(*ctx.activeScene);
    if (action == "reset") { runtime.Reset(); return InspectWorldStreaming(context, args); }
    std::string manifestFile;scene::WorldPartitionManifest manifest;
    if(!ProjectAssetPath(ctx,component.manifestPath,manifestFile)||!scene::LoadWorldPartitionManifest(manifestFile,manifest,error))return OpResult::Err("OPEN_FAILED",error.empty()?"Invalid project manifest":error);
    if(manifest.grid.origin!=component.gridOrigin||manifest.grid.cellSize!=component.cellSize)return OpResult::Err("GRID_CHANGED","Rebake cells after changing the common grid");
    if (!runtime.Status().configured && !runtime.Open(*ctx.activeScene, component.manifestPath, RuntimeSettings(component, playing), error)) return OpResult::Err("OPEN_FAILED", error);
    const scene::WorldCellId cell{args.GetInt("cellX"), args.GetInt("cellZ")};
    if (action == "load_near" || action == "load_hlod" || action == "unload") {
        const auto desired = action == "load_near" ? scene::WorldCellRepresentation::NEAR
            : action == "load_hlod" ? scene::WorldCellRepresentation::HLOD : scene::WorldCellRepresentation::UNLOADED;
        if (!runtime.RequestCell(cell, desired, error)) return OpResult::Err("REQUEST_FAILED", error);
    } else if (action == "clear_override") {
        if (!runtime.ClearCellOverride(cell)) return OpResult::Err("NO_OVERRIDE", "No override exists for this cell");
    } else if (action == "retry") runtime.RetryFailed();
    else if (action != "update") return OpResult::Err("BAD_ARG", "Unknown streaming action");
    runtime.Update(source, ctx.resources);
    return InspectWorldStreaming(context, args);
}

} /// @note namespace fbzz::editor
