/// @file    ControlPointInspector.cpp
/// @brief   制御点の接続、変換、Bakeを登録済みOpから編集するInspector。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <Editor/Tools/ControlPointEditing.hpp>
#include <Editor/Tools/AdditionalControlPointEditing.hpp>
#include <Editor/Tools/TargetPointReferenceEditing.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/ClothComponent.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/ControlPointAdapters.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <imgui.h>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace fbzz::editor {
namespace {
struct IdScope {
    explicit IdScope(const char* id) { ImGui::PushID(id); }
    explicit IdScope(int id) { ImGui::PushID(id); }
    ~IdScope() { ImGui::PopID(); }
};

struct PanelState {
    uint64_t scene = 0;
    std::string owner, message, target;
    math::Vector3 newPosition{};
    int particle = 0;
    float radius = 0.0f;
    math::Vector3 offset{};
    uint32_t attachmentId = 0;
    std::vector<std::string> referencingOwners;
    bool connectionsChecked = false;
    bool motionInitialized = false;
    uint64_t motionLifetime = 0;
    physics::ClothSettings motionBaseline;
    physics::ClothSettings motionDraft;
};

OpArgs OwnerArgs(const scene::GameObject& object, const char* kind)
{
    OpArgs result; result.Set("node", object.instanceId); result.Set("kind", std::string(kind)); return result;
}

bool Invoke(EditorContext& ctx, PanelState& state, const char* id, const OpArgs& args)
{
    const auto result = InvokeOperator(ctx, id, args);
    state.message = result.ok ? std::string{} : result.message;
    if (result.ok && std::string_view(id) == "target_point.inspect_connections") {
        state.referencingOwners.clear();
        if (const auto* owners = result.data.Find("owners"); owners && owners->IsArray()) {
            for (const auto& owner : owners->AsArray()) {
                if (const auto* guid = owner.Find("guid"); guid && guid->IsString())
                    state.referencingOwners.push_back(guid->AsString());
            }
        }
        state.connectionsChecked = true;
    }
    if (result.ok && std::string_view(id) == "control_points.create_cloth_anchor") {
        if (const auto* target = result.data.Find("target"); target && target->IsString()) {
            state.target = target->AsString();
            if (auto* point = ctx.activeScene->FindByGuid(state.target)) RevealInHierarchy(ctx, point->GetID());
        }
        if (const auto* binding = result.data.Find("id"); binding && binding->IsInt())
            state.attachmentId = static_cast<uint32_t>(binding->AsInt());
    }
    if (result.ok && (std::string_view(id) == "control_points.convert" || std::string_view(id) == "control_points.add")) {
        if (const auto* targets = result.data.Find("targets"); targets && targets->IsArray() && !targets->AsArray().empty()) {
            if (auto* point = ctx.activeScene->FindByGuid(targets->AsArray().front().AsString()))
                RevealInHierarchy(ctx, point->GetID());
        }
    }
    return true;
}

bool Button(EditorContext& ctx, PanelState& state, const char* label, const char* id, const OpArgs& args)
{
    ImGui::BeginDisabled(!CanInvokeOperator(ctx, id, args));
    const bool pressed = ImGui::Button(label); ImGui::EndDisabled();
    return pressed && Invoke(ctx, state, id, args);
}

bool DrawMarkerConnections(EditorContext& ctx, scene::GameObject& object, PanelState& state)
{
    IdScope scope("MarkerConnections");
    ImGui::SeparatorText("配置点 (TargetPoint)");
    ImGui::TextWrapped("Scene Viewで点を選び、移動Gizmoをドラッグします。離すと確定し、Escで取り消せます。");
    ImGui::TextWrapped("姿勢を使う目標点は回転・拡縮ツールでも編集できます。Waterの幅・水深は利用側の制御点欄で変更します。");
    ImGui::TextWrapped("親子関係は配置の基準です。利用側のInspectorでこの点を参照して初めて接続されます。");
    OpArgs query; query.Set("target", object.instanceId);
    if (Button(ctx, state, state.connectionsChecked ? "接続先を再確認" : "接続先を確認",
        "target_point.inspect_connections", query)) return true;
    if (state.connectionsChecked) {
        ImGui::TextDisabled("確認時点の参照を持つオブジェクト: %zu", state.referencingOwners.size());
        if (state.referencingOwners.empty())
            ImGui::TextWrapped("接続が見つかりません。利用側を選び、制御点または目標の欄でこのTargetPointを指定してください。");
        for (const auto& guid : state.referencingOwners) {
            IdScope ownerId(guid.c_str());
            auto* owner = ctx.activeScene->FindByGuid(guid);
            ImGui::TextUnformatted(owner ? owner->name.c_str() : "参照元が見つかりません");
            if (!owner) continue;
            ImGui::SameLine();
            OpArgs select; select.Set("target", guid);
            if (Button(ctx, state, "利用側を選択", "control_points.select", select)) return true;
        }
    }
    return false;
}

bool ChooseTarget(EditorContext& ctx, PanelState& state, const OpArgs& ownerArgs,
                  const scene::ObjectReference& reference, uint32_t id)
{
    auto* target = reference.guid.empty() ? nullptr : ctx.activeScene->FindByGuid(reference.guid);
    const char* label = target ? target->name.c_str() : reference.guid.empty() ? "未接続" : "参照先が見つかりません";
    if (ImGui::BeginCombo("TargetPoint", label)) {
        bool handled = false;
        for (auto& point : ctx.activeScene->GameObjects()) {
            if (!point.GetComponent<scene::TargetPointComponent>()) continue;
            IdScope pointId(point.instanceId.c_str());
            if (ImGui::Selectable(point.name.c_str(), point.instanceId == reference.guid)) {
                auto args = ownerArgs; args.Set("id", static_cast<int>(id)); args.Set("target", point.instanceId);
                handled = Invoke(ctx, state, "control_points.bind", args); break;
            }
        }
        ImGui::EndCombo();
        if (handled) return true;
    }
    if (target) {
        OpArgs args; args.Set("target", reference.guid);
        if (Button(ctx, state, "点を選択", "control_points.select", args)) return true;
    }
    return false;
}

template<typename Component>
bool DrawPointOwner(EditorContext& ctx, scene::GameObject& object, Component& component,
                    const char* kind, PanelState& state)
{
    IdScope scope(kind);
    const auto args = OwnerArgs(object, kind);
    const bool objects = component.pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS;
    size_t pointCount = component.pointBindings.size();
    if (!objects) {
        if constexpr (std::is_same_v<Component, scene::WaterComponent>) pointCount = component.splinePoints.size();
        else pointCount = component.points.size();
    }
    ImGui::SeparatorText(std::is_same_v<Component, scene::WaterComponent> ? "Waterの形状点" : "Splineの制御点");
    ImGui::TextDisabled("位置ソース: %s / %zu 点", objects ? "TargetPoint" : "数値点", pointCount);
    OpArgs edit; edit.Set("owner", ctx.controlPointOwnerGuid == object.instanceId ? std::string{} : object.instanceId);
    ImGui::BeginDisabled(!objects && ctx.controlPointOwnerGuid != object.instanceId);
    const bool editingChanged = Button(ctx, state, ctx.controlPointOwnerGuid == object.instanceId ? "点編集を終了" : "点を編集", "control_points.edit", edit);
    ImGui::EndDisabled();
    if (editingChanged) return true;
    ImGui::SameLine();
    auto convert = args; convert.Set("source", std::string(objects ? "INLINE" : "POINT_OBJECTS"));
    if (Button(ctx, state, objects ? "数値点へ戻す" : "TargetPointへ変換", "control_points.convert", convert)) return true;
    if (!objects) {
        ImGui::TextWrapped("「TargetPointへ変換」で、親の子にControlPointsと各点を作成して自動接続します。その後「点を編集」を押してください。");
        return false;
    }
    ImGui::TextWrapped("Hierarchyの親 → ControlPoints → Pointを選択するか、Scene Viewの点をクリックして移動Gizmoで編集します。");
    ImGui::TextWrapped("既存の点へ接続する場合は、下の各行を開いてTargetPointを選択します。点の順序はこの行の並びです。");
    const bool baked = component.pointRuntimeMode == scene::ControlPointRuntimeMode::BAKED_STATIC;
    ImGui::TextDisabled("実行時: %s", baked ? "静的ベイク" : "点へ追従 (LIVE)");
    auto runtime = args; runtime.Set("mode", std::string(baked ? "LIVE" : "BAKED_STATIC"));
    if (baked && Button(ctx, state, "実行中も点へ追従", "control_points.set_runtime", runtime)) return true;
    if (baked) ImGui::SameLine();
    if (Button(ctx, state, baked ? "ベイクを更新" : "静的ベイク", "control_points.bake", args)) return true;
    if (baked) ImGui::TextDisabled("実行時はベイクした形状を使います。");
    for (size_t index = 0; index < component.pointBindings.size(); ++index) {
        const auto& row = component.pointBindings[index];
        IdScope pointId(static_cast<int>(row.id));
        if (!ImGui::TreeNode("Point", "点 %zu", index + 1)) continue;
        bool handled = ChooseTarget(ctx, state, args, row.target, row.id);
        auto action = args; action.Set("id", static_cast<int>(row.id));
        if (!handled && ImGui::SmallButton("参照を外す")) handled = Invoke(ctx, state, "control_points.unbind", action);
        ImGui::SameLine();
        if (!handled && ImGui::SmallButton("行を削除")) handled = Invoke(ctx, state, "control_points.remove", action);
        ImGui::SameLine();
        if (!handled && index > 0 && ImGui::SmallButton("上へ")) { action.Set("toIndex", static_cast<int>(index - 1)); handled = Invoke(ctx, state, "control_points.reorder", action); }
        ImGui::SameLine();
        if (!handled && index + 1 < component.pointBindings.size() && ImGui::SmallButton("下へ")) { action.Set("toIndex", static_cast<int>(index + 1)); handled = Invoke(ctx, state, "control_points.reorder", action); }
        if constexpr (std::is_same_v<Component, scene::WaterComponent>) {
            if (!handled) {
                float width = row.width, depth = row.depth, speed = row.flowSpeed;
                if (component.surfaceShape == scene::WaterSurfaceShape::SPLINE_RIVER
                    && ImGui::InputFloat("幅 (ローカル)", &width, 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) { action.Set("width", width); handled = Invoke(ctx, state, "control_points.set_water_attributes", action); }
                if (!handled && ImGui::InputFloat("水深 (m)", &depth, 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) { action.Set("depth", depth); handled = Invoke(ctx, state, "control_points.set_water_attributes", action); }
                if (!handled && ImGui::InputFloat("流速 (m/s)", &speed, 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) { action.Set("flowSpeed", speed); handled = Invoke(ctx, state, "control_points.set_water_attributes", action); }
            }
        }
        ImGui::TreePop();
        if (handled) return true;
    }
    if constexpr (std::is_same_v<Component, scene::WaterComponent>) {
        if (component.surfaceShape == scene::WaterSurfaceShape::SPLINE_LAKE) {
            float position[]{state.newPosition.x, state.newPosition.z};
            ImGui::InputFloat2("新しい点のローカルXZ", position);
            state.newPosition = {position[0], component.lakePlaneHeight, position[1]};
        } else ImGui::InputFloat3("新しい点のローカル位置", &state.newPosition.x);
    } else ImGui::InputFloat3("新しい点のローカル位置", &state.newPosition.x);
    auto add = args; add.Set("localPosition", state.newPosition);
    return Button(ctx, state, "点を追加", "control_points.add", add);
}

bool SameClothMotion(const physics::ClothSettings& first, const physics::ClothSettings& second)
{
    return first.bendCompliance == second.bendCompliance && first.stretchCompliance == second.stretchCompliance
        && first.damping == second.damping && first.windVelocity == second.windVelocity;
}

bool DrawClothMotion(EditorContext& ctx, scene::GameObject& object, const scene::ClothComponent& cloth, PanelState& state)
{
    IdScope scope("clothMotion");
    const auto lifetime = ctx.activeScene->GetComponentLifetime<scene::ClothComponent>(object.GetID());
    if (!state.motionInitialized || state.motionLifetime != lifetime || !SameClothMotion(state.motionBaseline, cloth.settings)) {
        state.motionBaseline = state.motionDraft = cloth.settings;
        state.motionInitialized = true;
        state.motionLifetime = lifetime;
    }
    ImGui::SeparatorText("Cloth 揺れ方");
    ImGui::TextWrapped("曲げ・伸びは右ほど柔らかく、揺れの収まりは右ほど早く静止します。適用後、Playで確認できます。");
    ImGui::SliderFloat("曲がりやすさ", &state.motionDraft.bendCompliance, 0.0f, 0.1f, "%.5f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("伸びやすさ", &state.motionDraft.stretchCompliance, 0.0f, 0.001f, "%.8f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("揺れの収まり", &state.motionDraft.damping, 0.0f, 20.0f, "%.2f");
    ImGui::InputFloat3("風 (ワールド m/s)", &state.motionDraft.windVelocity.x);
    if (ImGui::SmallButton("標準値を下書きに戻す")) state.motionDraft = physics::ClothSettings{};
    OpArgs motion; motion.Set("node", object.instanceId);
    motion.Set("bendCompliance", state.motionDraft.bendCompliance);
    motion.Set("stretchCompliance", state.motionDraft.stretchCompliance);
    motion.Set("damping", state.motionDraft.damping);
    motion.Set("windVelocity", state.motionDraft.windVelocity);
    ImGui::BeginDisabled(SameClothMotion(state.motionDraft, cloth.settings));
    const bool handled = Button(ctx, state, "揺れ方を適用", "cloth.set_motion", motion);
    ImGui::EndDisabled();
    if (handled && state.message.empty()) {
        state.motionBaseline = state.motionDraft = cloth.settings;
    }
    return handled;
}

bool DrawClothAnchors(EditorContext& ctx, scene::GameObject& object, scene::ClothComponent& cloth, PanelState& state)
{
    IdScope scope("cloth");
    ImGui::SeparatorText("Cloth 固定先");
    ImGui::TextWrapped("画面上で点を選ぶ場合は「固定点を画面で編集」→「固定先を配置」を選び、布の点をクリックしてください。固定先をGizmoで動かせます。");
    if (Button(ctx, state, ctx.clothPinPainting ? "固定点の画面編集を終了" : "固定点を画面で編集", "cloth.paint_mode", {})) return true;
    ImGui::TextWrapped("固定する質点を指定し、「TargetPointを作成して固定」で点の作成と接続を行います。固定先を選択するとGizmoで移動できます。");
    ImGui::TextWrapped("既存のTargetPoint、Bone、通常のGameObjectも固定先として選べます。布のすべての物理質点を子オブジェクトにする必要はありません。");
    ImGui::TextDisabled("接続数: %zu", cloth.attachments.size());
    for (const auto& row : cloth.attachments) {
        IdScope rowId(row.bindingId ? static_cast<int>(row.bindingId) : row.particle);
        auto* target = row.targetReference.guid.empty() ? ctx.activeScene->GetGameObject(row.target) : ctx.activeScene->FindByGuid(row.targetReference.guid);
        ImGui::Text("質点 %d : %s", row.particle, target ? target->name.c_str()
            : row.targetReference.guid.empty() && !row.target.IsValid() ? "未接続" : "参照先が見つかりません");
        if (row.topologySignature == 0) ImGui::TextDisabled("旧接続：再接続すると現在の質点構成を検証します。");
        if (ImGui::SmallButton("接続を編集")) {
            state.attachmentId = row.bindingId; state.particle = row.particle; state.radius = row.maxDistance;
            state.offset = row.localPosition; state.target = target ? target->instanceId : row.targetReference.guid;
        }
        ImGui::SameLine();
        if (target) {
            OpArgs select; select.Set("target", target->instanceId);
            if (Button(ctx, state, "固定先を選択", "control_points.select", select)) return true;
            ImGui::SameLine();
        }
        auto remove = OwnerArgs(object, "cloth"); remove.Set("id", static_cast<int>(row.bindingId)); remove.Set("particle", row.particle);
        if (Button(ctx, state, "接続を解除", "control_points.unbind", remove)) return true;
    }
    ImGui::InputInt("接続する質点", &state.particle);
    ImGui::InputFloat("最大移動距離 (m)", &state.radius);
    ImGui::InputFloat3("固定先のローカルoffset", &state.offset.x);
    auto* selected = state.target.empty() ? nullptr : ctx.activeScene->FindByGuid(state.target);
    if (ImGui::BeginCombo("固定先", selected ? selected->name.c_str() : "TargetPointを選択")) {
        for (auto& point : ctx.activeScene->GameObjects()) {
            IdScope pointId(point.instanceId.c_str());
            if (ImGui::Selectable(point.name.c_str(), point.instanceId == state.target)) state.target = point.instanceId;
        }
        ImGui::EndCombo();
    }
    auto bind = OwnerArgs(object, "cloth"); bind.Set("particle", state.particle); bind.Set("target", state.target);
    if (state.attachmentId != 0) bind.Set("id", static_cast<int>(state.attachmentId));
    bind.Set("localPosition", state.offset); bind.Set("maxDistance", state.radius);
    if (Button(ctx, state, "固定先へ接続 / 再接続", "control_points.bind", bind)) return true;
    if (ImGui::SmallButton("新しい接続")) { state.attachmentId = 0; state.target.clear(); state.offset = {}; }
    ImGui::InputFloat3("作成する固定点のローカル位置", &state.newPosition.x);
    auto create = OwnerArgs(object, "cloth");
    create.Set("particle", state.particle); create.Set("localPosition", state.offset);
    create.Set("maxDistance", state.radius); create.Set("markerPosition", state.newPosition);
    if (state.attachmentId != 0) create.Set("id", static_cast<int>(state.attachmentId));
    return Button(ctx, state, "TargetPointを作成して固定", "control_points.create_cloth_anchor", create);
}
} /// @note namespace

bool DrawControlPointInspector(EditorContext& ctx, scene::GameObject& object)
{
    if (!ctx.activeScene) return false;
    static PanelState state;
    const auto generation = ctx.activeScene->GetRenderSceneGeneration();
    if (state.scene != generation || state.owner != object.instanceId) {
        state = {}; state.scene = generation; state.owner = object.instanceId;
    }
    if (object.GetComponent<scene::TargetPointComponent>()) {
        if (!state.message.empty()) ImGui::TextWrapped("%s", state.message.c_str());
        if (DrawMarkerConnections(ctx, object, state)) return true;
    }
    if (DrawTargetPointReferenceInspector(ctx, object)) return true;
    if (DrawAdditionalControlPointInspector(ctx, object)) return true;
    auto* water = object.GetComponent<scene::WaterComponent>();
    auto* spline = object.GetComponent<scene::SplineComponent>();
    auto* cloth = object.GetComponent<scene::ClothComponent>();
    const bool splineWater = water && water->surfaceShape != scene::WaterSurfaceShape::RECTANGLE;
    if (!water && !spline && !cloth) return false;
    ImGui::SeparatorText("制御点");
    if (!state.message.empty()) ImGui::TextWrapped("%s", state.message.c_str());
    if (water && !splineWater)
        ImGui::TextWrapped("Rectangleの水面は範囲設定で形を決めます。輪郭や流路を点で編集する場合は、WaterのSurface Shape → ShapeをSpline LakeまたはSpline Riverへ変更してください。");
    if (!IsControlPointAuthoringAllowed(ctx))
        ImGui::TextWrapped("制御点の作成・接続・移動は編集モードで行います。Playや復元、スクリプトのビルドが完了してから操作してください。");
    ImGui::BeginDisabled(!IsControlPointAuthoringAllowed(ctx));
    bool handled = false;
    if (splineWater) {
        const auto snapshot = scene::ResolveWaterControlPoints(*ctx.activeScene, object.GetID(), *water);
        if (snapshot.bakeStale) ImGui::TextColored({1.0f, 0.7f, 0.25f, 1.0f}, "点が変更されています。Play前にベイクを更新してください。");
        if (!snapshot.valid) ImGui::TextWrapped("点の接続、湖の高さ、位置や幅・水深を確認してください。");
        handled = DrawPointOwner(ctx, object, *water, "water", state);
        if (!handled && water->pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS
            && water->surfaceShape == scene::WaterSurfaceShape::SPLINE_LAKE) {
            float height = water->lakePlaneHeight;
            if (ImGui::InputFloat("湖の高さ", &height, 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                auto args = OwnerArgs(object, "water"); args.Set("height", height);
                handled = Invoke(ctx, state, "control_points.set_lake_height", args);
            }
        }
    }
    if (!handled && spline) {
        const auto snapshot = scene::ResolveSplinePoints(*ctx.activeScene, object.GetID(), *spline);
        if (snapshot.bakeStale) ImGui::TextColored({1.0f, 0.7f, 0.25f, 1.0f}, "点が変更されています。Play前にベイクを更新してください。");
        if (!snapshot.valid) ImGui::TextWrapped("点の接続とTransformを確認してください。");
        handled = DrawPointOwner(ctx, object, *spline, "spline", state);
    }
    if (!handled && cloth) handled = DrawClothMotion(ctx, object, *cloth, state);
    if (!handled && cloth) handled = DrawClothAnchors(ctx, object, *cloth, state);
    ImGui::EndDisabled();
    return handled;
}
} /// @note namespace fbzz::editor
