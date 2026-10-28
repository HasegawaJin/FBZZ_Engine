/// @file    TargetPointReferenceInspector.cpp
/// @brief   共通配置点の接続・明示解除・選択と作成を登録済みOpから表示する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <Editor/Tools/TargetPointReferenceEditing.hpp>
#include <Editor/Tools/ControlPointEditing.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Engine/Scene/Components/CameraRigComponents.hpp>
#include <Engine/Scene/Components/ConstraintComponents.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Scene/Components/MotionWarpComponent.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/Components/VFXBeamComponent.hpp>
#include <Engine/Scene/Components/VFXLineComponent.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <imgui.h>
#include <cstdint>
#include <string>

namespace fbzz::editor {
namespace {

struct IdScope {
    explicit IdScope(const char* id) { ImGui::PushID(id); }
    explicit IdScope(int id) { ImGui::PushID(id); }
    ~IdScope() { ImGui::PopID(); }
};

struct InspectorState {
    uint64_t generation = 0;
    std::string owner, message;
    int anchorMode = 0;
    bool anchorModeInitialized = false;
};

OpArgs Args(const scene::GameObject& owner, const char* kind, const char* role = "target", int chainIndex = 0)
{
    OpArgs args; args.Set("node", owner.instanceId); args.Set("kind", std::string(kind));
    args.Set("role", std::string(role)); args.Set("chainIndex", chainIndex); return args;
}

std::string Guid(scene::Scene& scene, const scene::EntityRef& reference)
{
    if (!reference.guid.empty()) return reference.guid;
    const auto* object = scene.GetGameObject(reference.id);
    return object ? object->instanceId : std::string{};
}

bool Invoke(EditorContext& ctx, InspectorState& state, const char* id, const OpArgs& args)
{
    const auto result = InvokeOperator(ctx, id, args);
    state.message = result.ok ? std::string{} : result.message;
    return true;
}

bool Button(EditorContext& ctx, InspectorState& state, const char* label, const char* id, const OpArgs& args)
{
    ImGui::BeginDisabled(!CanInvokeOperator(ctx, id, args));
    const bool pressed = ImGui::SmallButton(label); ImGui::EndDisabled();
    return pressed && Invoke(ctx, state, id, args);
}

int AnchorModeIndex(scene::TransformConstraintMode mode)
{
    if (mode == scene::TransformConstraintMode::LookAt) return 1;
    if (mode == scene::TransformConstraintMode::Parent) return 2;
    return 0;
}

const char* AnchorModeName(scene::TransformConstraintMode mode)
{
    if (mode == scene::TransformConstraintMode::LookAt) return "LookAt";
    if (mode == scene::TransformConstraintMode::Parent) return "Parent";
    if (mode == scene::TransformConstraintMode::Position) return "Position";
    return "未対応の制約方式";
}

bool DrawSlot(EditorContext& ctx, InspectorState& state, const char* label, const OpArgs& ownerArgs,
    const std::string& guid, const char* guidance, const char* unboundGuidance,
    bool markerOnly = false)
{
    IdScope id(label);
    auto* target = guid.empty() ? nullptr : ctx.activeScene->FindByGuid(guid);
    const char* current = target ? target->name.c_str() : guid.empty() ? "未接続" : "参照先が見つかりません";
    if (guid.empty()) {
        ImGui::TextDisabled("状態: 未接続");
        ImGui::TextWrapped("%s", unboundGuidance);
    } else if (!target) {
        ImGui::TextWrapped("状態: 参照先が見つかりません。保存GUIDを保持しています: %s", guid.c_str());
        ImGui::TextWrapped("同じSceneの参照先を選び直すか、不要なら参照を外してください。");
    } else if (markerOnly && !target->GetComponent<scene::TargetPointComponent>()) {
        ImGui::TextWrapped("状態: %s はTargetPointではありません。この用途はTargetPoint専用です。", target->name.c_str());
    } else {
        ImGui::TextWrapped("状態: 接続済み — %s", target->name.c_str());
    }
    ImGui::TextWrapped("%s", guidance);
    ImGui::BeginDisabled(!IsControlPointAuthoringAllowed(ctx));
    bool handled = false;
    if (ImGui::BeginCombo(label, current)) {
        for (auto& point : ctx.activeScene->GameObjects()) {
            if (point.runtimeGenerated || (markerOnly && !point.GetComponent<scene::TargetPointComponent>())) continue;
            IdScope pointId(point.instanceId.c_str());
            if (ImGui::Selectable(point.name.c_str(), point.instanceId == guid)) {
                auto args = ownerArgs; args.Set("target", point.instanceId);
                handled = Invoke(ctx, state, "target_point.bind_reference", args); break;
            }
        }
        ImGui::EndCombo();
    }
    if (!handled && !guid.empty()) {
        auto clear = ownerArgs; clear.Set("target", std::string{});
        handled = Button(ctx, state, "参照を外す", "target_point.bind_reference", clear);
        ImGui::SameLine();
        if (!handled && target) handled = Button(ctx, state, "参照先を選択", "target_point.select_reference", ownerArgs);
        if (!target) ImGui::TextDisabled("GUID: %s", guid.c_str());
    }
    if (!handled) handled = Button(ctx, state, "TargetPointを作成して接続", "target_point.create_and_bind", ownerArgs);
    ImGui::EndDisabled();
    return handled;
}

bool DrawAnchor(EditorContext& ctx, InspectorState& state, scene::GameObject& owner, const char* kind)
{
    IdScope id(kind);
    ImGui::SeparatorText(kind[0] == 'l' ? "Lightの配置基準" : "FlowFieldの配置基準");
    ImGui::TextWrapped("配置や注視はTransformConstraintのTargetへ接続します。点を作成するか既存のTargetPointを選ぶと、その制約にも接続されます。");
    ImGui::TextWrapped("制約がownerのTransformを動かすため、owner自身や子ではなく、独立したRootまたは兄弟位置に点を置きます。");
    auto* constraint = owner.GetComponent<scene::TransformConstraintComponent>();
    const auto targetGuid = constraint ? Guid(*ctx.activeScene, constraint->target) : std::string{};
    auto* target = targetGuid.empty() ? nullptr : ctx.activeScene->FindByGuid(targetGuid);
    if (!state.anchorModeInitialized) {
        if (constraint) state.anchorMode = AnchorModeIndex(constraint->mode);
        state.anchorModeInitialized = true;
    }
    if (!constraint || targetGuid.empty()) {
        ImGui::TextDisabled("状態: 未接続 — 配置基準を選ぶか作成してください。");
    } else if (!target) {
        ImGui::TextWrapped("状態: 接続先が見つかりません。保存GUIDを保持しています: %s", targetGuid.c_str());
    } else {
        ImGui::TextWrapped("状態: 接続済み — %s (%s)", target->name.c_str(),
            target->GetComponent<scene::TargetPointComponent>() ? "TargetPoint" : "通常のGameObject");
        auto selectArgs = Args(owner, "constraint");
        if (Button(ctx, state, "接続先を選択", "target_point.select_reference", selectArgs)) return true;
        if (target->GetComponent<scene::TargetPointComponent>()) {
            auto applyArgs = Args(owner, kind);
            applyArgs.Set("target", target->instanceId);
            applyArgs.Set("mode", std::string(state.anchorMode == 1 ? "LookAt" : state.anchorMode == 2 ? "Parent" : "Position"));
            if (state.anchorMode != AnchorModeIndex(constraint->mode)
                && Button(ctx, state, "接続中の点へ方式を適用", "target_point.configure_anchor", applyArgs)) return true;
            if (state.anchorMode == AnchorModeIndex(constraint->mode)) ImGui::TextDisabled("選択中の方式は現在の設定と同じです。");
        } else {
            ImGui::TextWrapped("方式を変更するにはTargetPointを選んでください。通常のGameObjectへの既存接続は維持されています。");
        }
    }
    if (constraint) ImGui::TextDisabled("現在の方式: %s", AnchorModeName(constraint->mode));
    ImGui::BeginDisabled(!IsControlPointAuthoringAllowed(ctx));
    static constexpr const char* MODES[] = {"位置を追従", "目標へ向ける", "位置と姿勢を追従"};
    ImGui::Combo("適用する方式", &state.anchorMode, MODES, 3);
    ImGui::TextWrapped("方式を選ぶだけでは既存制約は変わりません。「接続中の点へ方式を適用」するか、接続先を選び直すと反映されます。");
    OpArgs args; args.Set("node", owner.instanceId); args.Set("kind", std::string(kind));
    args.Set("mode", std::string(state.anchorMode == 1 ? "LookAt" : state.anchorMode == 2 ? "Parent" : "Position"));
    bool handled = false;
    const char* current = target ? target->name.c_str() : targetGuid.empty() ? "点を選んで接続" : "接続先が見つかりません";
    if (ImGui::BeginCombo("既存のTargetPointへ接続", current)) {
        for (auto& point : ctx.activeScene->GameObjects()) {
            if (point.runtimeGenerated || !point.GetComponent<scene::TargetPointComponent>()) continue;
            IdScope pointId(point.instanceId.c_str());
            if (ImGui::Selectable(point.name.c_str())) {
                args.Set("target", point.instanceId);
                handled = Invoke(ctx, state, "target_point.configure_anchor", args); break;
            }
        }
        ImGui::EndCombo();
    }
    if (!handled) {
        args.Set("target", std::string{});
        handled = Button(ctx, state, "配置基準を作成して接続", "target_point.configure_anchor", args);
    }
    ImGui::EndDisabled();
    if (handled) return true;
    if (!targetGuid.empty()) {
        ImGui::BeginDisabled(!IsControlPointAuthoringAllowed(ctx));
        auto clearArgs = Args(owner, "constraint"); clearArgs.Set("target", std::string{});
        if (Button(ctx, state, "接続を解除", "target_point.bind_reference", clearArgs)) handled = true;
        ImGui::EndDisabled();
    }
    return handled;
}

} /// @note namespace

bool DrawTargetPointReferenceInspector(EditorContext& ctx, scene::GameObject& object)
{
    if (!ctx.activeScene) return false;
    auto* follow = object.GetComponent<scene::CameraFollowComponent>();
    auto* constraint = object.GetComponent<scene::TransformConstraintComponent>();
    auto* beam = object.GetComponent<scene::VFXBeamComponent>();
    auto* line = object.GetComponent<scene::VFXLineComponent>();
    auto* agent = object.GetComponent<scene::NavMeshAgentComponent>();
    auto* solver = object.GetComponent<scene::IKSolverComponent>();
    auto* warp = object.GetComponent<scene::MotionWarpComponent>();
    auto* lod = object.GetComponent<scene::LODGroupComponent>();
    auto* light = object.GetComponent<scene::LightComponent>();
    auto* flow = object.GetComponent<scene::FlowField>();
    const bool hasConstraintSlot = constraint && !light && !flow;
    if (!follow && !hasConstraintSlot && !beam && !line && !agent && (!solver || solver->chains.empty())
        && !warp && !lod && !light && !flow) return false;
    static InspectorState state;
    if (state.generation != ctx.activeScene->GetRenderSceneGeneration() || state.owner != object.instanceId) {
        state = {}; state.generation = ctx.activeScene->GetRenderSceneGeneration(); state.owner = object.instanceId;
    }
    IdScope ownerId(object.instanceId.c_str());
    IdScope sectionId("TargetPointReferences");
    ImGui::SeparatorText("制御点の接続と編集");
    ImGui::TextWrapped("手順: 用途ごとの欄で点を作成して接続するか、既存点を選びます。接続先を選択してViewport上でGizmoを動かすと位置を編集できます。");
    if (!IsControlPointAuthoringAllowed(ctx))
        ImGui::TextWrapped("現在は編集できないScene状態のため、制御点の作成・接続・解除とGizmo編集は無効です。");
    if (!state.message.empty()) ImGui::TextWrapped("%s", state.message.c_str());
    if (follow)
        if (DrawSlot(ctx, state, "Camera追従先", Args(object, "camera_follow"), Guid(*ctx.activeScene, follow->target),
            "既存のGameObjectまたは骨のGameObjectを選べます。カメラを動かす参照なので、追従先はこのCameraの子にせずRootまたは兄弟に置きます。",
            "未接続です。作成または既存の追従先を選んでください。")) return true;
    if (hasConstraintSlot)
        if (DrawSlot(ctx, state, "Transform制約の目標", Args(object, "constraint"), Guid(*ctx.activeScene, constraint->target),
            "既存のGameObjectまたは骨のGameObjectを選べます。制約でownerを動かす場合、目標はownerの子孫にせずRootまたは兄弟に置きます。",
            "未接続です。目標を接続すると選択した制約がownerを駆動します。")) return true;
    if (beam) {
        if (DrawSlot(ctx, state, "Beam始点", Args(object, "vfx_beam", "from"), Guid(*ctx.activeScene, beam->fromEntity),
            "GameObjectや骨のGameObjectを始点にできます。移動する部位に追随させるならその部位の子に配置できます。",
            "未接続です。始点を作成するか既存Objectを選んでください。")) return true;
        if (DrawSlot(ctx, state, "Beam終点", Args(object, "vfx_beam", "to"), Guid(*ctx.activeScene, beam->toEntity),
            "GameObjectや骨のGameObjectを終点にできます。移動する部位に追随させるならその部位の子に配置できます。",
            "未接続です。終点を作成するか既存Objectを選んでください。")) return true;
    }
    if (line) {
        if (DrawSlot(ctx, state, "VFX始点", Args(object, "vfx_line", "from"), Guid(*ctx.activeScene, line->fromEntity),
            "GameObjectや骨のGameObjectを始点にできます。移動する部位に追随させるならその部位の子に配置できます。",
            "未接続です。始点を作成するか既存Objectを選んでください。")) return true;
        if (DrawSlot(ctx, state, "VFX終点", Args(object, "vfx_line", "to"), Guid(*ctx.activeScene, line->toEntity),
            "GameObjectや骨のGameObjectを終点にできます。移動する部位に追随させるならその部位の子に配置できます。",
            "未接続です。終点を作成するか既存Objectを選んでください。")) return true;
    }
    if (agent)
        if (DrawSlot(ctx, state, "追跡対象", Args(object, "nav_agent"), Guid(*ctx.activeScene, agent->target),
            "GameObjectや骨のGameObjectを追跡先にできます。Agent自身の子孫ではなく、独立したRootまたは兄弟位置に置いてください。",
            "未接続です。追跡先を作成するか既存Objectを選んでください。")) return true;
    if (solver) {
        for (size_t index = 0; index < solver->chains.size(); ++index) {
            IdScope chainId(static_cast<int>(index));
            const auto& chain = solver->chains[index];
            ImGui::Text("IK %zu : %s", index, chain.targetName.c_str());
            if (DrawSlot(ctx, state, "IK目標", Args(object, "ik", "target", static_cast<int>(index)), chain.targetReference.guid,
                "Marker、通常のGameObject、または骨のGameObjectを選べます。別キャラクターに追随する点はその骨の子に、固定ゴールは独立位置に置きます。",
                "未接続です。このIKチェーンの目標を作成または選択してください。")) return true;
            if (DrawSlot(ctx, state, "IK Pole", Args(object, "ik", "pole", static_cast<int>(index)), chain.poleReference.guid,
                "Marker、通常のGameObject、または骨のGameObjectを選べます。Poleを動くリグへ追随させる場合はそのリグ側に配置します。",
                "未接続です。このIKチェーンの曲げ方向を決めるPoleを作成または選択してください。")) return true;
        }
    }
    if (warp) {
        auto args = Args(object, "motion_warp");
        if (DrawSlot(ctx, state, "MotionWarp目標", args, warp->targetReference.guid,
            "TargetPointのみ接続できます。Warpを駆動する点なので、Warp対象の子孫ではなく独立したRootまたは兄弟位置に置きます。",
            "未接続です。TargetPointを作成するか既存点を選んでください。", true)) return true;
        const int current = static_cast<int>(warp->targetPointMode);
        if (current < 0 || current > 1) ImGui::TextDisabled("追従方法が無効です");
        int mode = current == 1 ? 1 : 0;
        ImGui::BeginDisabled(!IsControlPointAuthoringAllowed(ctx));
        static constexpr const char* MODES[] = {"開始時の姿勢を使用", "実行中も点へ追従"};
        const bool changed = ImGui::Combo("MotionWarpの追従方法", &mode, MODES, 2);
        ImGui::EndDisabled();
        if (changed) {
            args.Set("target", warp->targetReference.guid); args.Set("mode", std::string(mode == 1 ? "LIVE" : "CAPTURED"));
            return Invoke(ctx, state, "target_point.bind_reference", args);
        }
    }
    if (lod) {
        if (DrawSlot(ctx, state, "LOD距離判定の中心", Args(object, "lod_center"), lod->centerTarget.guid,
            "TargetPointのみ接続できます。LOD対象群と一緒に動かす中心ならそのRootの子に、固定位置を使うなら独立配置にします。",
            lod->centerSourceMode == scene::ControlPointSourceMode::INLINE
                ? "未接続です。現在はowner原点を中心として使っています。中心を変えるにはTargetPointを作成または選択してください。"
                : "未接続です。TargetPointを作成するか既存点を選んでください。", true)) return true;
        if (lod->centerSourceMode == scene::ControlPointSourceMode::INLINE) ImGui::TextDisabled("距離中心: owner原点");
    }
    if (light && DrawAnchor(ctx, state, object, "light")) return true;
    if (flow && DrawAnchor(ctx, state, object, "flow_field")) return true;
    return false;
}

} /// @note namespace fbzz::editor
