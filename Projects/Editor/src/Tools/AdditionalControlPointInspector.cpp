/// @file    AdditionalControlPointInspector.cpp
/// @brief   Patrol、Line、Off-Mesh Linkの制御点sourceとordered bindingをInspectorへ表示する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <Editor/Tools/AdditionalControlPointEditing.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Tools/ControlPointEditing.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/Components/NavMeshOffMeshLinkComponent.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/NavigationControlPoints.hpp>
#include <Engine/Scene/PresentationControlPoints.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <imgui.h>
#include <array>
#include <cstdint>
#include <string>
#include <type_traits>

namespace fbzz::editor {
namespace {

struct PanelState {
    std::uint64_t sceneGeneration = 0;
    std::string ownerGuid;
    std::string message;
    math::Vector3 newPosition{};
    std::array<math::Vector3, 2> linkEndpointPositions{};
};

OpArgs OwnerArgs(const scene::GameObject& owner, const char* kind)
{
    OpArgs args;
    args.Set("node", owner.instanceId);
    args.Set("kind", std::string(kind));
    return args;
}

bool Invoke(EditorContext& ctx, PanelState& state, const char* opId, const OpArgs& args)
{
    const auto result = InvokeOperator(ctx, opId, args);
    state.message = result.ok ? std::string{} : result.message;
    return true;
}

bool Button(EditorContext& ctx, PanelState& state, const char* label,
    const char* opId, const OpArgs& args)
{
    ImGui::BeginDisabled(!CanInvokeOperator(ctx, opId, args));
    const bool pressed = ImGui::Button(label);
    ImGui::EndDisabled();
    return pressed && Invoke(ctx, state, opId, args);
}

const char* AuthoringUnavailableReason(const EditorContext& ctx)
{
    if (!ctx.activeScene) return "編集対象Sceneがありません。";
    if (ctx.playMode && !ctx.playMode->IsInEditor()) return "PlayまたはPause中は制御点を編集できません。Playを終了してください。";
    if (ctx.playMode && ctx.playMode->HasPendingRestore()) return "Play Sceneの復元中です。完了後に編集できます。";
    if (ctx.managedBuildBusy) return "Managed buildの実行中は制御点を編集できません。";
    if (!ctx.undoStack || !ctx.undoStack->IsRecordingEnabled()) return "Undo記録が利用できないため、制御点を編集できません。";
    return nullptr;
}

const char* PointSourceLabel(scene::ControlPointSourceMode source)
{
    if (source == scene::ControlPointSourceMode::INLINE) return "数値配列 (INLINE)";
    if (source == scene::ControlPointSourceMode::POINT_OBJECTS) return "TargetPoint参照 (POINT_OBJECTS)";
    return "不正なsource設定";
}

const char* RuntimeModeLabel(scene::ControlPointRuntimeMode mode)
{
    if (mode == scene::ControlPointRuntimeMode::LIVE) return "LIVE";
    if (mode == scene::ControlPointRuntimeMode::BAKED_STATIC) return "BAKED_STATIC";
    return "不正な実行モード";
}

struct IdScope {
    explicit IdScope(const char* value) { ImGui::PushID(value); }
    explicit IdScope(int value) { ImGui::PushID(value); }
    ~IdScope() { ImGui::PopID(); }
};

bool ChooseTarget(EditorContext& ctx, PanelState& state, const OpArgs& ownerArgs,
    const scene::ObjectReference& reference, std::uint32_t id, const char* label)
{
    auto* selected = reference.guid.empty() ? nullptr : ctx.activeScene->FindByGuid(reference.guid);
    const char* preview = selected ? selected->name.c_str()
        : reference.guid.empty() ? "未接続" : "参照先が見つかりません";
    if (ImGui::BeginCombo(label, preview)) {
        bool handled = false;
        for (auto& target : ctx.activeScene->GameObjects()) {
            if (!target.GetComponent<scene::TargetPointComponent>()) continue;
            IdScope targetId(target.instanceId.c_str());
            if (ImGui::Selectable(target.name.c_str(), target.instanceId == reference.guid)) {
                auto args = ownerArgs;
                args.Set("id", static_cast<int>(id));
                args.Set("target", target.instanceId);
                handled = Invoke(ctx, state, "control_points.bind", args);
                break;
            }
        }
        ImGui::EndCombo();
        if (handled) return true;
    }
    if (selected) {
        OpArgs args;
        args.Set("target", selected->instanceId);
        if (Button(ctx, state, "点を選択", "control_points.select", args)) return true;
    }
    return false;
}

template<typename Component>
bool DrawSourceControls(EditorContext& ctx, scene::GameObject& owner,
    Component& component, const char* kind, std::size_t effectivePointCount,
    bool effectivePointsValid, PanelState& state)
{
    const auto ownerArgs = OwnerArgs(owner, kind);
    if constexpr (std::is_same_v<Component, scene::NavMeshPatrolComponent>
        || std::is_same_v<Component, scene::LineRendererComponent>
        || std::is_same_v<Component, scene::NavMeshOffMeshLinkComponent>) {
        const bool objectSource = component.pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS;
        const bool alreadyEditing = ctx.controlPointOwnerGuid == owner.instanceId;
        OpArgs edit;
        edit.Set("owner", alreadyEditing ? std::string{} : owner.instanceId);
        ImGui::BeginDisabled(!objectSource && !alreadyEditing);
        const bool editHandled = Button(ctx, state,
            alreadyEditing ? "点編集を終了" : "点編集を開始", "control_points.edit", edit);
        ImGui::EndDisabled();
        if (editHandled) return true;
        ImGui::SameLine();
    }
    const bool objectSource = component.pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS;
    auto sourceArgs = ownerArgs;
    sourceArgs.Set("source", std::string(objectSource ? "INLINE" : "POINT_OBJECTS"));
    if (Button(ctx, state, objectSource ? "数値へ戻す" : "TargetPointへ変換",
        "control_points.convert", sourceArgs)) return true;

    const std::size_t sourceRowCount = [&] {
        if constexpr (std::is_same_v<Component, scene::LineRendererComponent>)
            return objectSource ? component.pointBindings.size() : component.points.size();
        else if constexpr (std::is_same_v<Component, scene::NavMeshPatrolComponent>)
            return objectSource ? component.waypointBindings.size() : component.waypoints.size();
        else
            return std::size_t{2};
    }();
    ImGui::TextDisabled("位置ソース: %s", PointSourceLabel(component.pointSourceMode));
    if (effectivePointsValid)
        ImGui::TextDisabled("有効点数: %zu", effectivePointCount);
    else
        ImGui::TextColored({1.0f, 0.7f, 0.25f, 1.0f}, "有効点数を解決できません (入力行: %zu)", sourceRowCount);

    if constexpr (std::is_same_v<Component, scene::LineRendererComponent>) {
        const char* space = component.space == scene::LineSpace::Local ? "Local" :
            component.space == scene::LineSpace::World ? "World" : "不正";
        ImGui::TextDisabled("実行モード: %s | 座標系: %s", RuntimeModeLabel(component.pointRuntimeMode), space);
        if (component.space == scene::LineSpace::Local)
            ImGui::TextWrapped("Local LineのTargetPointは所有者の子として作成・接続され、所有者と一緒に移動します。");
        else
            ImGui::TextWrapped("World LineのTargetPointはScene rootの独立したGameObjectとして作成・接続されます。");
    } else if constexpr (std::is_same_v<Component, scene::NavMeshPatrolComponent>) {
        ImGui::TextDisabled("実行モード: %s", RuntimeModeLabel(component.pointRuntimeMode));
        ImGui::TextWrapped("PatrolのTargetPointはScene rootの独立したGameObjectとして作成・接続されます。");
    } else {
        ImGui::TextDisabled("実行モード: NavMesh Bake時に端点を取り込みます (個別のLIVE/BAKED_STATIC設定なし)。");
        ImGui::TextWrapped("Off-Mesh LinkのTargetPointはScene rootの独立したGameObjectとして作成・接続されます。");
    }

    if (!objectSource)
        ImGui::TextWrapped("数値点を使用中です。TargetPointへ変換すると、現在の位置から点GameObjectを作成し、順序付き参照へ接続します。Gizmo編集は変換後に開始できます。");
    else
        ImGui::TextWrapped("参照先を選択して「点を選択」を押すとHierarchyで選べます。点編集を開始するとViewport Gizmoで移動できます。");

    if constexpr (std::is_same_v<Component, scene::NavMeshPatrolComponent>
        || std::is_same_v<Component, scene::LineRendererComponent>) {
        const bool baked = component.pointRuntimeMode == scene::ControlPointRuntimeMode::BAKED_STATIC;
        auto runtime = ownerArgs;
        runtime.Set("mode", std::string(baked ? "LIVE" : "BAKED_STATIC"));
        if (baked && Button(ctx, state, "TargetPointへ追従", "control_points.set_runtime", runtime)) return true;
        if (baked) ImGui::SameLine();
        if (Button(ctx, state, baked ? "ベイクを更新" : "静的ベイク",
            "control_points.bake", ownerArgs)) return true;
        if (baked) ImGui::TextDisabled("実行時は最後に保存した数値snapshotを使用します。");
    }
    return false;
}

bool DrawLineRows(EditorContext& ctx, scene::GameObject& owner,
    scene::LineRendererComponent& line, PanelState& state)
{
    const auto ownerArgs = OwnerArgs(owner, "line");
    for (std::size_t index = 0; index < line.pointBindings.size(); ++index) {
        const auto& row = line.pointBindings[index];
        IdScope rowScope(static_cast<int>(row.id));
        if (!ImGui::TreeNode("LinePoint", "点 %zu · ID %u", index + 1, row.id)) continue;
        bool handled = ChooseTarget(ctx, state, ownerArgs, row.target, row.id, "接続先");
        auto args = ownerArgs;
        args.Set("id", static_cast<int>(row.id));
        if (!handled && ImGui::SmallButton("参照を外す")) handled = Invoke(ctx, state, "control_points.unbind", args);
        ImGui::SameLine();
        if (!handled && ImGui::SmallButton("行を削除")) handled = Invoke(ctx, state, "control_points.remove", args);
        ImGui::SameLine();
        if (!handled && index > 0 && ImGui::SmallButton("上へ")) {
            args.Set("toIndex", static_cast<int>(index - 1));
            handled = Invoke(ctx, state, "control_points.reorder", args);
        }
        ImGui::SameLine();
        if (!handled && index + 1 < line.pointBindings.size() && ImGui::SmallButton("下へ")) {
            args.Set("toIndex", static_cast<int>(index + 1));
            handled = Invoke(ctx, state, "control_points.reorder", args);
        }
        ImGui::TreePop();
        if (handled) return true;
    }
    return false;
}

bool DrawPatrolRows(EditorContext& ctx, scene::GameObject& owner,
    scene::NavMeshPatrolComponent& patrol, PanelState& state)
{
    const auto ownerArgs = OwnerArgs(owner, "patrol");
    for (std::size_t index = 0; index < patrol.waypointBindings.size(); ++index) {
        const auto& row = patrol.waypointBindings[index];
        IdScope rowScope(static_cast<int>(row.id));
        if (!ImGui::TreeNode("PatrolPoint", "経路点 %zu · ID %u", index + 1, row.id)) continue;
        bool handled = ChooseTarget(ctx, state, ownerArgs, row.target, row.id, "接続先");
        auto args = ownerArgs;
        args.Set("id", static_cast<int>(row.id));
        if (!handled && ImGui::SmallButton("参照を外す")) handled = Invoke(ctx, state, "control_points.unbind", args);
        ImGui::SameLine();
        if (!handled && ImGui::SmallButton("行を削除")) handled = Invoke(ctx, state, "control_points.remove", args);
        ImGui::SameLine();
        if (!handled && index > 0 && ImGui::SmallButton("上へ")) {
            args.Set("toIndex", static_cast<int>(index - 1));
            handled = Invoke(ctx, state, "control_points.reorder", args);
        }
        ImGui::SameLine();
        if (!handled && index + 1 < patrol.waypointBindings.size() && ImGui::SmallButton("下へ")) {
            args.Set("toIndex", static_cast<int>(index + 1));
            handled = Invoke(ctx, state, "control_points.reorder", args);
        }
        if (!handled) {
            float wait = row.waitTime, speed = row.speed;
            if (ImGui::InputFloat("待ち時間 (s)", &wait, 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                args.Set("waitTime", wait); args.Set("speed", speed);
                handled = Invoke(ctx, state, "control_points.set_patrol_attributes", args);
            }
            if (!handled && ImGui::InputFloat("移動速度 (m/s)", &speed, 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                args.Set("waitTime", wait); args.Set("speed", speed);
                handled = Invoke(ctx, state, "control_points.set_patrol_attributes", args);
            }
        }
        ImGui::TreePop();
        if (handled) return true;
    }
    return false;
}

bool DrawNavLink(EditorContext& ctx, scene::GameObject& owner,
    scene::NavMeshOffMeshLinkComponent& link, PanelState& state)
{
    const auto ownerArgs = OwnerArgs(owner, "nav_link");
    for (std::uint32_t id = 1; id <= 2; ++id) {
        IdScope endpoint(static_cast<int>(id));
        const char* label = id == 1 ? "始点" : "終点";
        const auto& reference = id == 1 ? link.startTarget : link.endTarget;
        bool handled = ChooseTarget(ctx, state, ownerArgs, reference, id, label);
        auto args = ownerArgs;
        args.Set("id", static_cast<int>(id));
        if (!handled && !reference.guid.empty() && ImGui::SmallButton("参照を外す"))
            handled = Invoke(ctx, state, "control_points.unbind", args);
        if (!handled && reference.guid.empty()) {
            ImGui::InputFloat3(id == 1 ? "新しい始点のワールド位置" : "新しい終点のワールド位置",
                &state.linkEndpointPositions[id - 1].x);
            args.Set("worldPosition", state.linkEndpointPositions[id - 1]);
            handled = Button(ctx, state, id == 1 ? "起点を作成" : "終点を作成", "control_points.add", args);
        }
        if (handled) return true;
    }
    ImGui::TextWrapped("始点と終点をそれぞれ選び、「点を選択」でHierarchyへ移動してからViewport Gizmoで編集します。");
    return false;
}

} /// @note namespace

bool DrawAdditionalControlPointInspector(EditorContext& ctx, scene::GameObject& object)
{
    if (!ctx.activeScene) return false;
    auto* patrol = object.GetComponent<scene::NavMeshPatrolComponent>();
    auto* line = object.GetComponent<scene::LineRendererComponent>();
    auto* link = object.GetComponent<scene::NavMeshOffMeshLinkComponent>();
    if (!patrol && !line && !link) return false;

    static PanelState state;
    const auto generation = ctx.activeScene->GetRenderSceneGeneration();
    if (state.sceneGeneration != generation || state.ownerGuid != object.instanceId) {
        state = {};
        state.sceneGeneration = generation;
        state.ownerGuid = object.instanceId;
    }
    scene::FlushWorldTransforms(*ctx.activeScene);
    ImGui::SeparatorText("制御点");
    if (!state.message.empty()) ImGui::TextWrapped("%s", state.message.c_str());
    const char* unavailableReason = AuthoringUnavailableReason(ctx);
    if (unavailableReason)
        ImGui::TextColored({1.0f, 0.7f, 0.25f, 1.0f}, "編集不可: %s", unavailableReason);
    ImGui::BeginDisabled(unavailableReason != nullptr);
    bool handled = false;
    if (line) {
        ImGui::TextUnformatted("Line Renderer");
        const auto snapshot = scene::ResolveLineControlPoints(*ctx.activeScene, object.GetID(), *line,
            scene::ControlPointReadContext::AUTHORING);
        if (snapshot.bakeStale) ImGui::TextColored({1.0f, 0.7f, 0.25f, 1.0f}, "点が変更されています。実行前にベイクを更新してください。");
        if (!snapshot.valid) ImGui::TextWrapped("Lineの点数、参照先、または座標を確認してください。");
        handled = DrawSourceControls(ctx, object, *line, "line", snapshot.points.size(), snapshot.valid, state);
        if (!handled && line->pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS)
            handled = DrawLineRows(ctx, object, *line, state);
        if (!handled && line->pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS) {
            const bool worldSpace = line->space == scene::LineSpace::World;
            ImGui::InputFloat3(worldSpace ? "新しい点のワールド位置" : "新しい点のOwner Local位置", &state.newPosition.x);
            auto args = OwnerArgs(object, "line");
            args.Set(worldSpace ? "worldPosition" : "localPosition", state.newPosition);
            handled = Button(ctx, state, "点を追加", "control_points.add", args);
        }
    }
    if (!handled && patrol) {
        ImGui::SeparatorText("NavMesh巡回経路");
        const auto snapshot = scene::ResolveNavMeshPatrolPoints(*ctx.activeScene,
            object.GetID(), *patrol, scene::ControlPointReadContext::AUTHORING);
        if (snapshot.bakeStale) ImGui::TextColored({1.0f, 0.7f, 0.25f, 1.0f}, "点が変更されています。実行前にベイクを更新してください。");
        if (!snapshot.valid) ImGui::TextWrapped("巡回点の参照先、座標、または属性を確認してください。");
        handled = DrawSourceControls(ctx, object, *patrol, "patrol", snapshot.points.size(), snapshot.valid, state);
        if (!handled && patrol->pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS)
            handled = DrawPatrolRows(ctx, object, *patrol, state);
        if (!handled && patrol->pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS) {
            ImGui::InputFloat3("新しい巡回点のワールド位置", &state.newPosition.x);
            auto args = OwnerArgs(object, "patrol"); args.Set("worldPosition", state.newPosition);
            args.Set("waitTime", patrol->waitTime); args.Set("speed", 0.0f);
            handled = Button(ctx, state, "巡回点を追加", "control_points.add", args);
        }
    }
    if (!handled && link) {
        ImGui::SeparatorText("NavMesh Off-Mesh Link");
        const auto snapshot = scene::ResolveNavMeshOffMeshLinkEndpoints(*ctx.activeScene, object.GetID(), *link);
        if (!snapshot.valid) ImGui::TextWrapped("始点と終点の参照先または座標を確認してください。");
        handled = DrawSourceControls(ctx, object, *link, "nav_link", snapshot.valid ? 2u : 0u, snapshot.valid, state);
        if (!handled && link->pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS)
            handled = DrawNavLink(ctx, object, *link, state);
    }
    ImGui::EndDisabled();
    return handled;
}

} /// @note namespace fbzz::editor
