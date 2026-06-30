// FBZZ Engine
// InspectorCore.cpp | fbzz::editor
// Transform / Script の Inspector 描画
#include "InspectorCore.hpp"
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <imgui_internal.h>

namespace fbzz::editor {

namespace {

bool TransformEquals(const scene::Transform& lhs, const scene::Transform& rhs)
{
    return lhs.position.x == rhs.position.x &&
           lhs.position.y == rhs.position.y &&
           lhs.position.z == rhs.position.z &&
           lhs.rotation.x == rhs.rotation.x &&
           lhs.rotation.y == rhs.rotation.y &&
           lhs.rotation.z == rhs.rotation.z &&
           lhs.rotation.w == rhs.rotation.w &&
           lhs.scale.x == rhs.scale.x &&
           lhs.scale.y == rhs.scale.y &&
           lhs.scale.z == rhs.scale.z;
}

// ImGui の連続ドラッグを1つの Transform コマンドへまとめる。
// WHY: 値が変化する各フレームを履歴へ積むと、1回のドラッグを戻すために
//      Ctrl+Z が何十回も必要になるため、Activated～Deactivated を1操作とする。
void TrackTransformEdit(scene::GameObject& go, EditorContext& ctx, const char* description)
{
    struct ActiveEdit {
        scene::EntityID id;
        scene::Transform before;
        bool active = false;
    };
    static ActiveEdit edit;

    if (ImGui::IsItemActivated()) {
        edit.id = go.GetID();
        edit.before = go.transform;
        edit.active = true;
    }

    if (!ImGui::IsItemDeactivatedAfterEdit() || !edit.active || edit.id != go.GetID())
        return;

    const scene::Transform before = edit.before;
    const scene::Transform after = go.transform;
    scene::Scene* scene = ctx.activeScene;
    const std::string instanceId = go.instanceId;
    const auto markDirty = ctx.markSceneDirty;

    if (ctx.undoStack && scene && !TransformEquals(before, after)) {
        auto apply = [scene, instanceId, markDirty](const scene::Transform& value) {
            if (auto* target = scene->FindByGuid(instanceId)) {
                target->transform = value;
                if (markDirty) markDirty();
            }
        };
        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
            description,
            [apply, after]() { apply(after); },
            [apply, before]() { apply(before); }));
    }

    edit.active = false;
    if (!TransformEquals(before, after) && ctx.markSceneDirty) ctx.markSceneDirty();
}

} // namespace

void DrawTransformInspectors(scene::GameObject* go, EditorContext& ctx)
{
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& t = go->transform;
        ImGui::Spacing();

        const bool isUI = go->GetComponent<scene::UIImage>() || go->GetComponent<scene::UIText>();

        if (isUI) {
            const float itemW = (ImGui::GetContentRegionAvail().x
                                 - ImGui::CalcTextSize("X").x * 2
                                 - ImGui::GetStyle().ItemSpacing.x * 3) * 0.5f;

            ImGui::Text("Pos");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(itemW);
            ImGui::DragFloat("##px", &t.position.x, 1.0f, 0.0f, 0.0f, "X %.0f");
            TrackTransformEdit(*go, ctx, "Change Position X");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(itemW);
            ImGui::DragFloat("##py", &t.position.y, 1.0f, 0.0f, 0.0f, "Y %.0f");
            TrackTransformEdit(*go, ctx, "Change Position Y");

            math::Vector3 euler = widgets::QuatToEulerDeg(t.rotation);
            float rotZ = euler.z;
            ImGui::Text("Rot");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragFloat("##rz", &rotZ, 0.5f, -360.0f, 360.0f, "Z %.1f deg"))
                t.rotation = widgets::EulerDegToQuat({ euler.x, euler.y, rotZ });
            TrackTransformEdit(*go, ctx, "Change Rotation");

            if (go->GetComponent<scene::UIImage>()) {
                ImGui::Text("Size");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(itemW);
                ImGui::DragFloat("##sw", &t.scale.x, 1.0f, 1.0f, 0.0f, "W %.0f");
                TrackTransformEdit(*go, ctx, "Change Width");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(itemW);
                ImGui::DragFloat("##sh", &t.scale.y, 1.0f, 1.0f, 0.0f, "H %.0f");
                TrackTransformEdit(*go, ctx, "Change Height");
            }
        } else {
            float pos[3] = { t.position.x, t.position.y, t.position.z };
            if (ImGui::DragFloat3("Position", pos, 0.1f))
                t.position = { pos[0], pos[1], pos[2] };
            TrackTransformEdit(*go, ctx, "Change Position");

            widgets::DragQuatEuler3("Rotation", t.rotation, 0.5f);
            TrackTransformEdit(*go, ctx, "Change Rotation");

            float scale[3] = { t.scale.x, t.scale.y, t.scale.z };
            if (ImGui::DragFloat3("Scale", scale, 0.01f, 0.001f, 1000.0f))
                t.scale = { scale[0], scale[1], scale[2] };
            TrackTransformEdit(*go, ctx, "Change Scale");
        }

        ImGui::Spacing();
    }

}

void DrawScriptInspectors(scene::GameObject* go, EditorContext& ctx)
{
    FBZZ_PROFILE_SCOPE("Inspector::Scripts");

    auto* sc = go->GetComponent<scene::ScriptComponent>();
    if (!sc)
        return;

    struct ScriptUndoTracker {
        ImGuiID activeId = 0;
        std::string before;
        bool active = false;
    };
    static ScriptUndoTracker undo;
    const bool canTrackUndo =
        ctx.activeScene != nullptr &&
        ctx.undoStack != nullptr &&
        ctx.undoStack->IsRecordingEnabled();
    const ImGuiID activeBefore = ImGui::GetActiveID();
    // WHY: Scene 全体の Serialize は高コストなので、Inspector を眺めているだけのフレームでは実行しない。
    //      Mouse/Keyboard による操作開始候補だけを捕捉し、連続編集では tracker の before を再利用する。
    const bool mayStartEdit =
        !undo.active &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
         ImGui::IsKeyPressed(ImGuiKey_Enter) ||
         ImGui::IsKeyPressed(ImGuiKey_Space));
    const std::string beforeDraw = canTrackUndo && mayStartEdit
        ? SceneIO::Serialize(*ctx.activeScene)
        : std::string{};

    int removeIndex = -1;
    for (int i = 0; i < static_cast<int>(sc->scripts.size()); ++i) {
        auto& entry = sc->scripts[static_cast<size_t>(i)];
        ImGui::PushID(i);

        if (entry.script) {
            const char* header = entry.script->GetTypeName();
            ImGui::Checkbox("##en", &entry.script->enabled);
            ImGui::SameLine();

            const bool open = ImGui::CollapsingHeader(header,
                ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

            const float btnW = ImGui::GetFrameHeight();
            ImGui::SameLine(ImGui::GetContentRegionMax().x - btnW);
            if (ImGui::SmallButton("..."))
                ImGui::OpenPopup("##script_opts");

            if (ImGui::BeginPopup("##script_opts")) {
                if (ImGui::MenuItem("Remove Component"))
                    removeIndex = i;
                ImGui::EndPopup();
            }

            if (open) {
                ImGui::Spacing();
                ImGuiReflector reflector;
                reflector.m_projectRoot = ctx.projectRoot; // アセットスロットの "..." パス検索用
                if (ctx.activeScene) {
                    reflector.m_goNameResolver = [scene = ctx.activeScene](scene::EntityID id) -> std::string {
                        auto* target = scene->GetGameObject(id);
                        return target ? target->name : "(Missing)";
                    };
                    // ◎ピッカー用の候補一覧。シーンの全 GameObject を (ID, 名前) で列挙する。
                    reflector.m_goListProvider =
                        [scene = ctx.activeScene]() -> std::vector<std::pair<scene::EntityID, std::string>> {
                            std::vector<std::pair<scene::EntityID, std::string>> out;
                            for (auto& go : scene->GameObjects())
                                out.emplace_back(go.GetID(), go.name);
                            return out;
                        };
                    // 型付き参照 (FBZZ_REF<T>) の型チェック: 対象 GO が typeName の Script を持つか。
                    // typeName が空 (任意 GameObject) なら常に true。
                    reflector.m_refTypeValidator =
                        [scene = ctx.activeScene](scene::EntityID id, const char* typeName) -> bool {
                            if (!typeName || !typeName[0]) return true;
                            auto* target = scene->GetGameObject(id);
                            if (!target) return false;
                            auto* sc = target->GetComponent<scene::ScriptComponent>();
                            if (!sc) return false;
                            for (const auto& entry : sc->scripts)
                                if (entry.script && std::string(entry.script->GetTypeName()) == typeName)
                                    return true;
                            return false;
                        };
                }
                entry.script->Reflect(reflector);
                ImGui::Spacing();
            }
        } else if (entry.serialized && !entry.serialized->type.empty()) {
            ImGui::Checkbox("##en", &entry.serialized->enabled);
            ImGui::SameLine();

            // WHY: DLL ビルド中は "Building..." と表示し、完了後に自動復元されることを示す。
            //      それ以外 (DLL 未ロード・ビルド失敗) は "Missing Script" のままにして問題を明示する。
            const bool isBuilding = ctx.scriptReloadBusy;
            const std::string header = (isBuilding ? "Building... " : "Missing Script: ")
                                       + entry.serialized->type;
            const bool open = ImGui::CollapsingHeader(
                header.c_str(),
                ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

            const float btnW = ImGui::GetFrameHeight();
            ImGui::SameLine(ImGui::GetContentRegionMax().x - btnW);
            if (ImGui::SmallButton("..."))
                ImGui::OpenPopup("##missing_script_opts");

            if (ImGui::BeginPopup("##missing_script_opts")) {
                if (ImGui::MenuItem("Remove Component"))
                    removeIndex = i;
                ImGui::EndPopup();
            }

            if (open) {
                ImGui::Spacing();
                if (isBuilding)
                    ImGui::TextDisabled("Script DLL is building. Fields will be restored on completion.");
                else
                    ImGui::TextDisabled("Script DLL is not loaded. Serialized fields are preserved.");
                ImGui::Spacing();
            }
        }

        ImGui::PopID();
    }

    if (removeIndex >= 0) {
        sc->scripts.erase(sc->scripts.begin() + removeIndex);
        if (sc->scripts.empty())
            go->RemoveComponent<scene::ScriptComponent>();
    }

    if (!canTrackUndo) {
        undo.active = false;
        return;
    }
    const ImGuiID activeAfter = ImGui::GetActiveID();
    auto pushCommand = [&](const std::string& before, const std::string& after) {
        if (before == after) return;
        scene::Scene* scene = ctx.activeScene;
        EditorContext* context = &ctx;
        const auto markDirty = ctx.markSceneDirty;
        auto restore = [scene, context, markDirty](const std::string& snapshot) {
            if (SceneIO::Deserialize(*scene, snapshot)) {
                context->selectedEntities.clear();
                if (markDirty) markDirty();
            }
        };
        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
            "Edit Script",
            [restore, after]() { restore(after); },
            [restore, before]() { restore(before); }));
    };

    if (!undo.active && activeAfter != 0 && activeAfter != activeBefore) {
        undo.activeId = activeAfter;
        undo.before = beforeDraw;
        undo.active = true;
    } else if (undo.active && activeAfter != undo.activeId) {
        pushCommand(undo.before, SceneIO::Serialize(*ctx.activeScene));
        undo.active = false;
    } else if (!undo.active && GImGui && GImGui->ActiveIdHasBeenEditedThisFrame &&
               activeAfter == 0) {
        pushCommand(beforeDraw, SceneIO::Serialize(*ctx.activeScene));
    }
}

} // namespace fbzz::editor
