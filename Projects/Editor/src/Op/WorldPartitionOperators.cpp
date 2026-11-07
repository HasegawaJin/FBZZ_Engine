/// @file    WorldPartitionOperators.cpp
/// @brief   World cell, PCG and HLOD operations project the shared authoring services.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include "../Tools/WorldAuthoring.hpp"
#include <limits>
#include <utility>

namespace fbzz::editor {
namespace {

OpParam Parameter(const char* name, OpParamType type, const char* description,
                  OpValue defaultValue = {})
{
    OpParam parameter;
    parameter.name = name;
    parameter.type = type;
    parameter.desc = description;
    parameter.required = std::holds_alternative<std::monostate>(defaultValue);
    parameter.defaultValue = std::move(defaultValue);
    return parameter;
}

OpParam Optional(const char* name, OpParamType type, const char* description)
{
    auto parameter = Parameter(name, type, description);
    parameter.required = false;
    return parameter;
}

OpParam Number(const char* name, OpParamType type, const char* description,
               OpValue defaultValue, float minimum, float maximum)
{
    auto parameter = Parameter(name, type, description, std::move(defaultValue));
    parameter.hasRange = true;
    parameter.minValue = minimum;
    parameter.maxValue = maximum;
    return parameter;
}

OpParam OptionalNumber(const char* name, OpParamType type, const char* description,
                       float minimum, float maximum)
{
    auto parameter = Optional(name, type, description);
    parameter.hasRange = true;
    parameter.minValue = minimum;
    parameter.maxValue = maximum;
    return parameter;
}

OpParam Partition()
{
    return Optional("partition", OpParamType::NodeId,
        "WorldPartition node GUID. Omit to resolve the unique partition in the current Scene.");
}

bool HasContext(const OpContext& context)
{
    return context.ctx.activeScene && !context.ctx.projectRoot.empty()
        && (!context.ctx.playMode || !context.ctx.playMode->HasPendingRestore());
}

bool CanAuthor(const OpContext& context)
{
    return HasContext(context) && !context.ctx.managedBuildBusy
        && (!context.ctx.playMode || context.ctx.playMode->IsInEditor());
}

std::vector<OpParam> PreviewParameters()
{
    auto partition = Partition();
    partition.desc = "WorldPartition node GUID. Omit to use the unique controller; without a controller use the profile grid. Multiple controllers require an explicit GUID.";
    return {
        Parameter("volume", OpParamType::NodeId, "PCGVolume node GUID."),
        std::move(partition),
        Optional("boundsMin", OpParamType::Vec3, "Optional world-space minimum [m]; supply boundsMax together to limit regeneration."),
        Optional("boundsMax", OpParamType::Vec3, "Optional world-space maximum [m]; supply boundsMin together to limit regeneration."),
        Number("maxPoints", OpParamType::Int, "Maximum returned preview samples, default 256; zero returns counts only.", 256, 0, 1024),
    };
}

} /// @note namespace

void RegisterWorldPartitionOperators(OperatorRegistry& registry)
{
    const auto add = [&registry](const char* id, const char* description, OpKind kind,
                                std::vector<OpParam> parameters, OpExec execute,
                                bool streaming = false, bool needsScene = true) {
        EditorOperator operation;
        operation.id = id;
        operation.label = id;
        operation.category = "World";
        operation.desc = description;
        operation.kind = kind;
        operation.params = std::move(parameters);
        operation.exec = std::move(execute);
        if (kind == OpKind::Mutation) operation.undoLabel = id;
        operation.poll = [kind, streaming, needsScene](const OpContext& context, const OpArgs&) {
            const bool projectReady = !context.ctx.projectRoot.empty() && !context.ctx.managedBuildBusy
                && (!context.ctx.playMode || !context.ctx.playMode->HasPendingRestore());
            const bool available = needsScene
                ? (streaming ? HasContext(context) : CanAuthor(context))
                : projectReady && (!context.ctx.playMode || context.ctx.playMode->IsInEditor());
            return available && (kind != OpKind::Mutation || context.undo.IsRecordingEnabled());
        };
        registry.Register(std::move(operation));
    };

    add("worldcell.inspect", "Inspect partition configuration, authored cells and manifest references without loading cells.",
        OpKind::Query, {Partition()}, InspectWorldCells);

    add("worldcell.configure", "Configure a partition with checked Undo; omitted fields preserve existing values. Create one when no target exists using documented creation defaults. Distances and origin use world metres.",
        OpKind::Mutation, {
            Optional("partition", OpParamType::NodeId, "Existing partition node GUID; omit to resolve or create a partition."),
            Optional("manifest", OpParamType::String, "Project-relative world manifest path; creation default Assets/World/World.worldcells."),
            Optional("origin", OpParamType::Vec3, "World grid origin [m]; creation default [0,0,0]."),
            OptionalNumber("cellSize", OpParamType::Float, "Square cell width [m]; creation default 256; valid range 1..65536.", 1, 65536),
            OptionalNumber("nearDistance", OpParamType::Float, "Near-cell loading radius [m]; creation default 384.", 0, 10000000),
            OptionalNumber("hlodDistance", OpParamType::Float, "HLOD loading radius [m]; creation default 1536; must cover nearDistance.", 0, 10000000),
            OptionalNumber("hysteresis", OpParamType::Float, "Unload distance margin [m]; creation default 64.", 0, 10000000),
            OptionalNumber("maxConcurrentReads", OpParamType::Int, "Concurrent cell reads; creation default 2.", 1, 64),
            OptionalNumber("maxCommitsPerUpdate", OpParamType::Int, "Completed cell publications per update; creation default 1.", 1, 1024),
            Optional("streamInEditor", OpParamType::Bool, "Enable explicit editor streaming; creation default false."),
        }, ConfigureWorldCells);

    add("pcg.profile.inspect", "Inspect a saved PCG profile; an omitted path returns its schema and an authoring example.",
        OpKind::Query, {Parameter("path", OpParamType::String, "Project-relative profile path; default empty returns schema/example.", std::string{})},
        InspectWorldPcgProfile, false, false);
    add("pcg.profile.save", "Validate and save a PCG profile. Existing different content requires its previous fingerprint in expected.",
        OpKind::Action, {
            Parameter("path", OpParamType::String, "Project-relative profile output path."),
            Parameter("text", OpParamType::String, "Complete profile text using the codec/schema returned by pcg.profile.inspect."),
            Optional("expected", OpParamType::String, "Previous profile fingerprint for compare-and-swap when replacing different content."),
        }, SaveWorldPcgProfile, false, false);

    add("pcg.volume.configure", "Create or configure a PCG volume through checked Undo; omitted fields preserve existing values. New volumes require profile. BoundsMin and boundsMax must be supplied together.",
        OpKind::Mutation, {
            Optional("volume", OpParamType::NodeId, "Existing PCGVolume node GUID; omit to create one."),
            Optional("profile", OpParamType::String, "Project-relative saved PCG profile path; required when creating a volume."),
            Optional("boundsMin", OpParamType::Vec3, "World-space bounds minimum [m]; must accompany boundsMax."),
            Optional("boundsMax", OpParamType::Vec3, "World-space bounds maximum [m]; must accompany boundsMin."),
            Optional("useProfileBounds", OpParamType::Bool, "True clears a bounds override and uses the profile bounds; cannot accompany boundsMin/boundsMax."),
            OptionalNumber("seed", OpParamType::Int, "Deterministic seed override; explicit -1 clears the override and uses the profile seed.", -1, static_cast<float>(std::numeric_limits<int>::max())),
            Optional("protectManual", OpParamType::Bool, "Preserve manually modified generated instances; creation default true."),
            Optional("enabled", OpParamType::Bool, "Enable this generation volume; creation default true."),
        }, ConfigureWorldPcgVolume);

    add("pcg.preview", "Compute deterministic generation without changing the Scene; returns counts, bounded samples and expected fingerprint.",
        OpKind::Query, PreviewParameters(), PreviewWorldPcg);
    auto generateParameters = PreviewParameters();
    generateParameters.push_back(Parameter("expected", OpParamType::String,
        "Fingerprint from the same pcg.preview plan; stale inputs are rejected before any changes."));
    add("pcg.generate", "Apply a current preview plan in one checked Undo; preserve protected manual edits and reject stale expected fingerprints.",
        OpKind::Mutation, std::move(generateParameters), GenerateWorldPcg);
    add("pcg.detach", "Detach a generated instance from PCG ownership through Undo, preserving it during future regeneration.",
        OpKind::Mutation, {Parameter("node", OpParamType::NodeId, "Generated node GUID to preserve as manual content.")}, DetachWorldPcg);

    add("worldcell.bake", "Persist authored cell payloads and manifest; optional HLOD build shares the production baker. This file action is not Undoable.",
        OpKind::Action, {
            Partition(),
            Parameter("includeScene", OpParamType::Bool, "Include eligible Scene content in the bake, default false.", false),
            Parameter("buildHlod", OpParamType::Bool, "Build HLOD payloads together, default true.", true),
            Number("maxCells", OpParamType::Int, "Maximum baked cells, default 4096; reject excess before publication.", 4096, 1, 4096),
            Number("maxInstances", OpParamType::Int, "Maximum baked instances, default 1048576.", 1048576, 1, 1048576),
        }, BakeWorldCells);
    add("hlod.build", "Build persistent HLOD payloads from the partition's cell manifest with bounded work.",
        OpKind::Action, {
            Partition(),
            Number("maxGroups", OpParamType::Int, "Maximum HLOD groups, default 65536.", 65536, 1, 65536),
            Number("maxInstances", OpParamType::Int, "Maximum source instances, default 1048576.", 1048576, 1, 1048576),
            Optional("expected", OpParamType::String, "Expected manifest fingerprint; reject stale input before publication."),
        }, BuildWorldHlod);

    add("worldcell.stream_status", "Inspect asynchronous cell queues, published content and failures in Edit or Play without advancing streaming.",
        OpKind::Query, {Partition()}, InspectWorldStreaming, true);
    auto action = Parameter("action", OpParamType::String, "Streaming update or per-cell override. Edit rejects operations that retire authored sources.");
    action.enumValues = {"update", "load_near", "load_hlod", "unload", "clear_override", "retry", "reset"};
    add("worldcell.stream_control", "Control asynchronous streaming in Edit or Play; destructive authoring-source retirement requires Play.",
        OpKind::Action, {
            Partition(), std::move(action),
            Parameter("cellX", OpParamType::Int, "Signed grid X index for a per-cell override, default 0.", 0),
            Parameter("cellZ", OpParamType::Int, "Signed grid Z index for a per-cell override, default 0.", 0),
            Optional("source", OpParamType::Vec3, "Optional world-space streaming source position [m]."),
        }, ControlWorldStreaming, true);
}

} /// @note namespace fbzz::editor
