/// @file    PlacementOperators.cpp
/// @brief   ワールド空間の配置計画、競合検査付き適用と制作コンテキスト。
/// @author  Hasegawa Jin
/// @date    2026-11-06
/// @see Docs/design/ai-authoring-tools.md
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/ManagedSdkBinding.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::editor {
namespace {

constexpr std::size_t MAX_PLACEMENT_TARGETS = 128;

struct PlacementEdit {
    std::string guid;
    scene::EntityID entity;
    scene::EntityID parent;
    scene::Transform before;
    scene::Transform parentWorld;
    math::Vector3 afterLocal;
    math::Vector3 afterWorld;
};

struct PlacementPlan {
    scene::Scene* scene = nullptr;
    std::uint64_t generation = 0;
    std::vector<PlacementEdit> edits;
    OpData data = OpData::MakeObject();
    std::string expected;
};

bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool Finite(const math::Quaternion& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z)
        && std::isfinite(value.w) && value.Length() > 0.000001f;
}

bool Same(const math::Vector3& left, const math::Vector3& right)
{
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

bool Same(const math::Quaternion& left, const math::Quaternion& right)
{
    return left.x == right.x && left.y == right.y && left.z == right.z && left.w == right.w;
}

/// @note Query は未同期の派生 world を書き換えず、local 正本から一時値だけを構築する。
/// @see Docs/design/script-transform-contract.md
bool ReadWorld(scene::GameObject& object, scene::Transform& output)
{
    std::vector<scene::GameObject*> ancestors;
    for (auto* current = &object; current != nullptr; current = current->GetParent()) {
        if (ancestors.size() >= 4096 || std::find(ancestors.begin(), ancestors.end(), current) != ancestors.end()) return false;
        if (!Finite(current->transform.position) || !Finite(current->transform.scale)
            || !Finite(current->transform.rotation)) return false;
        ancestors.push_back(current);
    }
    scene::Transform world;
    bool hasParent = false;
    for (auto iterator = ancestors.rbegin(); iterator != ancestors.rend(); ++iterator) {
        scene::Transform next = (*iterator)->transform;
        scene::UpdateWorldTransform(next, hasParent ? &world : nullptr);
        if (!Finite(next.worldPosition) || !Finite(next.worldScale) || !Finite(next.worldRotation)) return false;
        world = next;
        hasParent = true;
    }
    output = world;
    return true;
}

float Coordinate(const math::Vector3& value, int axis)
{
    return axis == 0 ? value.x : axis == 1 ? value.y : value.z;
}

void SetCoordinate(math::Vector3& value, int axis, float coordinate)
{
    if (axis == 0) value.x = coordinate;
    else if (axis == 1) value.y = coordinate;
    else value.z = coordinate;
}

OpData RotationData(const math::Quaternion& rotation)
{
    auto value = OpData::MakeArray();
    value.Push(rotation.x); value.Push(rotation.y); value.Push(rotation.z); value.Push(rotation.w);
    return value;
}

OpData TransformData(const scene::Transform& transform)
{
    auto value = OpData::MakeObject();
    value.Set("position", transform.position);
    value.Set("rotation", RotationData(transform.rotation));
    value.Set("scale", transform.scale);
    value.Set("worldPosition", transform.worldPosition);
    value.Set("worldRotation", RotationData(transform.worldRotation));
    value.Set("worldScale", transform.worldScale);
    return value;
}

/// @note 静的 CPU 頂点の実際の底面を使う。スキニング後の形状、子メッシュと Collider は対象外。
bool MeshBottom(scene::GameObject& object, const scene::Transform& world, float& minimum)
{
    const auto* renderer = object.GetComponent<scene::MeshRenderer>();
    if (renderer == nullptr || renderer->mesh == nullptr || renderer->mesh->isSkinned
        || renderer->mesh->cpuVertices.empty()) return false;
    minimum = (std::numeric_limits<float>::max)();
    for (const auto& vertex : renderer->mesh->cpuVertices) {
        const math::Vector3 scaled{vertex.position.x * world.worldScale.x,
            vertex.position.y * world.worldScale.y, vertex.position.z * world.worldScale.z};
        const math::Vector3 position = world.worldPosition + world.worldRotation * scaled;
        if (!Finite(position)) return false;
        minimum = (std::min)(minimum, position.y);
    }
    return std::isfinite(minimum);
}

bool Editable(const OpContext& context)
{
    return context.ctx.activeScene != nullptr && !context.ctx.managedBuildBusy
        && (context.ctx.editScene == nullptr || context.ctx.activeScene == context.ctx.editScene)
        && (context.ctx.playMode == nullptr || (context.ctx.playMode->IsInEditor()
            && !context.ctx.playMode->HasPendingRestore()));
}

OpResult PreparePlacement(OpContext& context, const OpArgs& args, PlacementPlan& plan)
{
    const auto* nodes = args.GetData("nodes");
    if (nodes == nullptr || !nodes->IsArray() || nodes->AsArray().empty()
        || nodes->AsArray().size() > MAX_PLACEMENT_TARGETS)
        return OpResult::Err("BAD_ARG", "nodes は 1..128 個の一意な NodeId 配列です");
    plan.scene = context.ctx.activeScene;
    plan.generation = plan.scene->GetRenderSceneGeneration();
    std::vector<scene::GameObject*> objects;
    for (const auto& node : nodes->AsArray()) {
        if (!node.IsString() || node.AsString().empty()) return OpResult::Err("BAD_ARG", "nodes の各要素は空でない NodeId です");
        auto* object = plan.scene->FindByGuid(node.AsString());
        if (object == nullptr || object->IsRuntimeGenerated()) return OpResult::Err("NO_NODE", "編集可能な対象がありません: " + node.AsString());
        if (std::find(objects.begin(), objects.end(), object) != objects.end()) return OpResult::Err("BAD_ARG", "nodes に重複があります");
        objects.push_back(object);
    }
    for (auto* object : objects)
        for (auto* parent = object->GetParent(); parent != nullptr; parent = parent->GetParent())
            if (std::find(objects.begin(), objects.end(), parent) != objects.end())
                return OpResult::Err("OVERLAPPING_HIERARCHY", "親と子孫を同時に配置できません");

    const std::string operation = args.GetString("operation");
    const std::string axisName = args.GetString("axis", "x");
    const int axis = axisName == "x" ? 0 : axisName == "y" ? 1 : 2;
    if (operation == "align" && !args.Has("value")) return OpResult::Err("BAD_ARG", "align はワールド座標 value が必要です");
    if (operation == "ground" && !args.Has("planeHeight")) return OpResult::Err("BAD_ARG", "ground はワールド水平面 planeHeight が必要です");
    if (operation == "distribute" && objects.size() < 3) return OpResult::Err("BAD_ARG", "distribute は 3 個以上の対象が必要です");
    const float value = args.GetFloat("value");
    const float planeHeight = args.GetFloat("planeHeight");
    const float offset = args.GetFloat("offset");
    const std::string anchor = args.GetString("anchor", "meshBottom");
    for (auto* object : objects) {
        PlacementEdit edit;
        edit.guid = object->instanceId;
        edit.entity = object->GetID();
        if (!ReadWorld(*object, edit.before)) return OpResult::Err("INVALID_TRANSFORM", "Transform が非有限または不正です: " + edit.guid);
        if (auto* parent = object->GetParent()) {
            edit.parent = parent->GetID();
            if (!ReadWorld(*parent, edit.parentWorld)) return OpResult::Err("INVALID_TRANSFORM", "親 Transform が不正です: " + edit.guid);
            const auto scale = edit.parentWorld.worldScale;
            if (std::abs(scale.x) <= 0.000001f || std::abs(scale.y) <= 0.000001f || std::abs(scale.z) <= 0.000001f)
                return OpResult::Err("SINGULAR_PARENT", "親のゼロスケールを逆算できません: " + edit.guid);
        }
        edit.afterWorld = edit.before.worldPosition;
        if (operation == "align") SetCoordinate(edit.afterWorld, axis, value);
        else if (operation == "ground") {
            float bottom = edit.before.worldPosition.y;
            if (anchor == "meshBottom" && !MeshBottom(*object, edit.before, bottom))
                return OpResult::Err("NO_STATIC_MESH", "meshBottom は対象自身の静的 CPU メッシュが必要です。原点なら anchor=origin を明示してください: " + edit.guid);
            edit.afterWorld.y += planeHeight + offset - bottom;
        }
        plan.edits.push_back(std::move(edit));
    }
    if (operation == "distribute") {
        std::sort(plan.edits.begin(), plan.edits.end(), [axis](const auto& left, const auto& right) {
            const float a = Coordinate(left.before.worldPosition, axis), b = Coordinate(right.before.worldPosition, axis);
            return a == b ? left.guid < right.guid : a < b;
        });
        const double start = Coordinate(plan.edits.front().before.worldPosition, axis);
        const double end = Coordinate(plan.edits.back().before.worldPosition, axis);
        for (std::size_t index = 1; index + 1 < plan.edits.size(); ++index)
            SetCoordinate(plan.edits[index].afterWorld, axis,
                static_cast<float>(start + (end - start) * static_cast<double>(index) / static_cast<double>(plan.edits.size() - 1)));
    }
    auto changes = OpData::MakeArray();
    auto guard = OpData::MakeArray();
    int changed = 0;
    for (auto& edit : plan.edits) {
        scene::Transform after = edit.before;
        if (!Same(edit.afterWorld, edit.before.worldPosition))
            scene::SetWorldPose(after, edit.parent.IsValid() ? &edit.parentWorld : nullptr,
                edit.afterWorld, edit.before.worldRotation, edit.before.worldScale);
        after.rotation = edit.before.rotation;
        after.scale = edit.before.scale;
        edit.afterLocal = after.position;
        if (!Finite(edit.afterWorld) || !Finite(edit.afterLocal)) return OpResult::Err("INVALID_TRANSFORM", "配置結果が非有限です");
        const bool willChange = !Same(edit.before.position, edit.afterLocal);
        if (willChange) ++changed;
        auto change = OpData::MakeObject();
        change.Set("id", edit.guid);
        change.Set("beforeWorld", edit.before.worldPosition);
        change.Set("afterWorld", edit.afterWorld);
        change.Set("beforeLocal", edit.before.position);
        change.Set("afterLocal", edit.afterLocal);
        change.Set("changed", willChange);
        changes.Push(std::move(change));
        auto key = OpData::MakeObject();
        key.Set("id", edit.guid);
        key.Set("entityIndex", static_cast<std::int64_t>(edit.entity.index));
        key.Set("entityGeneration", static_cast<std::int64_t>(edit.entity.generation));
        key.Set("parentIndex", static_cast<std::int64_t>(edit.parent.index));
        key.Set("parentGeneration", static_cast<std::int64_t>(edit.parent.generation));
        key.Set("before", TransformData(edit.before));
        key.Set("parentWorld", TransformData(edit.parentWorld));
        key.Set("afterLocal", edit.afterLocal);
        guard.Push(std::move(key));
    }
    auto expected = OpData::MakeObject();
    expected.Set("sceneGeneration", std::to_string(plan.generation));
    expected.Set("operation", operation);
    expected.Set("axis", axisName);
    expected.Set("anchor", anchor);
    expected.Set("value", value);
    expected.Set("planeHeight", planeHeight);
    expected.Set("offset", offset);
    expected.Set("targets", std::move(guard));
    if (!WriteOpDataJson(expected, plan.expected)) return OpResult::Err("INVALID_TRANSFORM", "配置計画を表現できません");
    plan.data.Set("schemaVersion", 1);
    plan.data.Set("sceneGeneration", std::to_string(plan.generation));
    plan.data.Set("operation", operation);
    plan.data.Set("space", "world");
    plan.data.Set("surface", operation == "ground" ? "horizontal-plane" : "none");
    plan.data.Set("anchor", operation == "ground" ? anchor : "origin");
    plan.data.Set("targetCount", static_cast<std::int64_t>(plan.edits.size()));
    plan.data.Set("changedCount", changed);
    plan.data.Set("changes", std::move(changes));
    plan.data.Set("expected", plan.expected);
    return OpResult::Ok();
}

/// @note Undo/Redo は対象寿命・親姿勢・編集値を全件検査してから位置だけを一括変更する。
bool ApplyPlacement(EditorContext& context, const PlacementPlan& plan, bool forward, std::string& reason)
{
    if (context.activeScene != plan.scene || context.activeScene == nullptr
        || context.activeScene->GetRenderSceneGeneration() != plan.generation
        || context.managedBuildBusy
        || (context.editScene != nullptr && context.activeScene != context.editScene)
        || (context.playMode != nullptr && (!context.playMode->IsInEditor() || context.playMode->HasPendingRestore()))) {
        reason = "配置対象の Scene または Edit 状態が変わりました"; return false;
    }
    std::vector<scene::GameObject*> targets;
    for (const auto& edit : plan.edits) {
        auto* object = context.activeScene->GetGameObject(edit.entity);
        scene::Transform world;
        if (object == nullptr || object->instanceId != edit.guid || !ReadWorld(*object, world)
            || !Same(world.position, forward ? edit.before.position : edit.afterLocal)
            || !Same(world.rotation, edit.before.rotation) || !Same(world.scale, edit.before.scale)) {
            reason = "配置対象の寿命または Transform が変わりました: " + edit.guid; return false;
        }
        auto* parent = object->GetParent();
        if ((parent != nullptr ? parent->GetID() : scene::EntityID{}) != edit.parent) {
            reason = "配置対象の親が変わりました: " + edit.guid; return false;
        }
        if (parent != nullptr) {
            scene::Transform parentWorld;
            if (!ReadWorld(*parent, parentWorld) || !Same(parentWorld.worldPosition, edit.parentWorld.worldPosition)
                || !Same(parentWorld.worldRotation, edit.parentWorld.worldRotation) || !Same(parentWorld.worldScale, edit.parentWorld.worldScale)) {
                reason = "配置対象の親姿勢が変わりました: " + edit.guid; return false;
            }
        }
        targets.push_back(object);
    }
    for (std::size_t index = 0; index < targets.size(); ++index)
        targets[index]->transform.position = forward ? plan.edits[index].afterLocal : plan.edits[index].before.position;
    for (auto* object : targets) scene::FlushWorldTransforms(*object);
    context.sceneDirty = true;
    if (context.markSceneDirty) context.markSceneDirty();
    reason.clear();
    return true;
}

std::vector<OpParam> PlacementParams()
{
    OpParam operation; operation.name = "operation"; operation.type = OpParamType::String;
    operation.desc = "align=指定座標へ原点整列、distribute=原点の両端を保った等間隔、ground=明示した水平面への配置";
    operation.enumValues = {"align", "distribute", "ground"};
    OpParam nodes; nodes.name = "nodes"; nodes.type = OpParamType::Data;
    nodes.desc = "1..128 個の一意な NodeId 配列。親子同時指定は不可";
    OpParam axis; axis.name = "axis"; axis.type = OpParamType::String;
    axis.desc = "align/distribute のワールド軸"; axis.required = false; axis.defaultValue = std::string("x"); axis.enumValues = {"x", "y", "z"};
    OpParam value; value.name = "value"; value.type = OpParamType::Float;
    value.desc = "align のワールド座標 (m)"; value.required = false;
    OpParam height; height.name = "planeHeight"; height.type = OpParamType::Float;
    height.desc = "ground が使う明示した水平面のワールド Y (m)。Terrain/Collider の問い合わせは行わない"; height.required = false;
    OpParam anchor; anchor.name = "anchor"; anchor.type = OpParamType::String;
    anchor.desc = "ground の基準。meshBottom は対象自身の静的 CPU メッシュ底面、origin は原点";
    anchor.required = false; anchor.defaultValue = std::string("meshBottom"); anchor.enumValues = {"meshBottom", "origin"};
    OpParam offset; offset.name = "offset"; offset.type = OpParamType::Float;
    offset.desc = "ground の水平面からの垂直オフセット (m)"; offset.required = false; offset.defaultValue = 0.0f;
    return {operation, nodes, axis, value, height, anchor, offset};
}

OpResult AuthoringContext(OpContext& context)
{
    auto data = OpData::MakeObject();
    data.Set("schemaVersion", 1);
    data.Set("projectRoot", context.ctx.projectRoot);
    data.Set("engineRoot", context.ctx.engineRoot);
    data.Set("scenePath", context.ctx.currentScenePath);
    data.Set("scriptsSourceDir", context.ctx.scriptsSourceDir);
    data.Set("sceneGeneration", context.ctx.activeScene != nullptr
        ? OpData(std::to_string(context.ctx.activeScene->GetRenderSceneGeneration())) : OpData{});
    data.Set("nodeCount", context.ctx.activeScene != nullptr ? static_cast<std::int64_t>(context.ctx.activeScene->GameObjectCount()) : 0);
    data.Set("sceneDirty", context.ctx.sceneDirty);
    data.Set("managedBuildBusy", context.ctx.managedBuildBusy);
    data.Set("undoRevision", static_cast<std::int64_t>(context.undo.GetRevision()));
    data.Set("undoCursor", static_cast<std::int64_t>(context.undo.GetCursor()));
    const auto* play = context.ctx.playMode;
    data.Set("mode", play == nullptr || play->IsInEditor() ? "edit" : play->IsPaused() ? "paused" : "play");
    data.Set("pendingRestore", play != nullptr && play->HasPendingRestore());
    auto managed = OpData::MakeObject();
    ManagedSdkBinding binding;
    std::string diagnostic;
    const bool resolved = ResolveManagedSdkBinding(ManagedEditorExecutable(), util::FileSystem::PathFromUtf8(context.ctx.projectRoot),
        util::FileSystem::PathFromUtf8(context.ctx.engineRoot), FBZZ_CMAKE_CONFIG, binding, diagnostic);
    std::error_code error;
    const bool available = resolved && std::filesystem::is_regular_file(binding.apiPath, error) && !error;
    managed.Set("apiPath", available ? OpData(util::FileSystem::PathToUtf8(binding.apiPath)) : OpData{});
    managed.Set("diagnostic", available ? "" : diagnostic.empty() ? "Script API assembly is unavailable" : diagnostic);
    data.Set("managed", std::move(managed));
    return OpResult::Data(std::move(data));
}

} /// @note namespace

void RegisterPlacementOperators(OperatorRegistry& registry)
{
    EditorOperator context;
    context.id = "authoring.context"; context.label = "Inspect Authoring Context"; context.category = "GameObject";
    context.desc = "現在の project/Scene 寿命と起動 Editor に整合する Script API 配置を返す。Scene 世代は内容 revision ではない";
    context.kind = OpKind::Query;
    context.exec = [](OpContext& c, const OpArgs&) { return AuthoringContext(c); };
    registry.Register(std::move(context));
    for (const bool apply : {false, true}) {
        EditorOperator op;
        op.id = apply ? "authoring.placement.apply" : "authoring.placement.preview";
        op.label = apply ? "Apply Placement" : "Preview Placement"; op.category = "GameObject";
        op.desc = "明示対象のワールド配置を計画・適用する。水平面への接地、原点整列、原点等間隔に対応する";
        op.kind = apply ? OpKind::Mutation : OpKind::Query;
        op.undoLabel = apply ? "Authoring Placement" : "";
        op.params = PlacementParams();
        if (apply) {
            OpParam expected; expected.name = "expected"; expected.type = OpParamType::String;
            expected.desc = "preview が返した expected。その後の Scene・対象・引数変更は競合として拒否する";
            op.params.push_back(std::move(expected));
        }
        op.poll = [apply](const OpContext& c, const OpArgs&) { return Editable(c) && (!apply || c.undo.IsRecordingEnabled()); };
        op.exec = [apply](OpContext& c, const OpArgs& args) {
            auto plan = std::make_shared<PlacementPlan>();
            if (auto result = PreparePlacement(c, args, *plan); !result.ok) return result;
            if (apply && args.GetString("expected") != plan->expected)
                return OpResult::Err("PLACEMENT_CONFLICT", "配置計画が変わりました。preview を取得し直してください");
            OpResult result = OpResult::Data(plan->data);
            result.data.Set("applied", apply);
            if (!apply) return result;
            result.noChange = plan->data.Find("changedCount")->AsInt() == 0;
            if (result.noChange) return result;
            std::string reason;
            if (!ApplyPlacement(c.ctx, *plan, true, reason)) return OpResult::Err("PLACEMENT_CONFLICT", std::move(reason));
            auto* editorContext = &c.ctx;
            result.command = std::make_unique<CheckedLambdaCommand>("Authoring Placement",
                [editorContext, plan](std::string& error) { return ApplyPlacement(*editorContext, *plan, true, error); },
                [editorContext, plan](std::string& error) { return ApplyPlacement(*editorContext, *plan, false, error); });
            return result;
        };
        registry.Register(std::move(op));
    }
}

} /// @note namespace fbzz::editor
