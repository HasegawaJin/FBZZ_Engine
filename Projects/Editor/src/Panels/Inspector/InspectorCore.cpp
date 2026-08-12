// FBZZ Engine
// InspectorCore.cpp | fbzz::editor
// Transform / Script の Inspector 描画
#include "InspectorCore.hpp"
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/ScriptSnapshot.hpp>
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

// ── Transform の値クリップボード (オブジェクト間コピー/ペースト用) ─────────────
// WHY: 「A の Transform を B にそのまま移す」は頻出操作。プロセス内で 1 つ保持すれば足りる。
struct TransformClipboard {
    bool             has = false;
    math::Vector3    position{};
    math::Quaternion rotation{};
    math::Vector3    scale{ 1.0f, 1.0f, 1.0f };
};
TransformClipboard& TransformClip() { static TransformClipboard c; return c; }

// Transform をメニュー操作で書き換えた際の Undo コマンドを積む (連続ドラッグ用の TrackTransformEdit とは別経路)。
//
// WHY: 以前はシーン全体を TOML 化して before/after にしていたが、戻すのが 1 つの
//      GameObject の Transform だけなのにシーン全体を Deserialize で再構築していた。
//      EntityID が振り直されるため選択・ロック・エディタ非表示が毎回消え、
//      大きなシーンでは Paste/Reset のたびに全文シリアライズ 2 回ぶんのヒッチが出ていた。
void PushTransformSnapshotUndo(scene::GameObject& go,
                               EditorContext& ctx,
                               const scene::Transform& before,
                               const char* desc)
{
    if (ctx.markSceneDirty) ctx.markSceneDirty();
    if (!ctx.activeScene || !ctx.undoStack || !ctx.undoStack->IsRecordingEnabled()) return;

    const scene::Transform after = go.transform;
    if (TransformEquals(before, after)) return;

    scene::Scene*     scene      = ctx.activeScene;
    const std::string instanceId = go.instanceId;
    const auto        markDirty  = ctx.markSceneDirty;

    auto apply = [scene, instanceId, markDirty](const scene::Transform& value) {
        if (auto* target = scene->FindByGuid(instanceId)) {
            target->transform = value;
            if (markDirty) markDirty();
        }
    };
    ctx.undoStack->Push(std::make_unique<LambdaCommand>(desc,
        [apply, after]()  { apply(after); },
        [apply, before]() { apply(before); }));
}

// Transform ヘッダー右クリックの Copy / Paste / Reset メニュー。
void DrawTransformHeaderMenu(scene::GameObject& go, EditorContext& ctx)
{
    if (!ImGui::BeginPopupContextItem("##transform_hdr_ctx")) return;
    auto& t = go.transform;
    TransformClipboard& clip = TransformClip();

    if (ImGui::MenuItem("Copy Transform")) {
        clip.position = t.position;
        clip.rotation = t.rotation;
        clip.scale    = t.scale;
        clip.has      = true;
    }
    if (ImGui::MenuItem("Paste Transform", nullptr, false, clip.has)) {
        const scene::Transform before = t;
        t.position = clip.position;
        t.rotation = clip.rotation;
        t.scale    = clip.scale;
        PushTransformSnapshotUndo(go, ctx, before, "Paste Transform");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Reset Transform")) {
        const scene::Transform before = t;
        t.position = math::Vector3::ZERO;
        t.rotation = math::Quaternion::Identity();
        t.scale    = { 1.0f, 1.0f, 1.0f };
        PushTransformSnapshotUndo(go, ctx, before, "Reset Transform");
    }
    ImGui::EndPopup();
}

} // namespace

void DrawTransformInspectors(scene::GameObject* go, EditorContext& ctx)
{
    const bool transformOpen = ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen);
    // ヘッダー右クリック: Copy / Paste / Reset (開閉状態に関わらず有効)
    DrawTransformHeaderMenu(*go, ctx);
    if (transformOpen) {
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

    // スクリプト 1 個ぶんの編集を 1 コマンドとして記録する。
    //
    // WHY: 以前はシーン全体を TOML 化して before/after にしていた。スクリプトの実体は
    //      DLL の向こうにあり、型を知らないエディタからは値を取り出せないというのが理由。
    //      だが Script は Reflect() を実装しているので、IReflector を 1 つ用意すれば
    //      型を知らないまま「そのスクリプトだけ」を読み書きできる。
    //      これで Undo が全シーン再構築ではなく値の復元になり、EntityID も選択も維持される。
    struct ScriptUndoTracker {
        ImGuiID     activeId = 0;
        std::string before;        // 編集開始時のスナップショット
        std::string instanceId;    // 対象 GameObject
        std::string typeName;      // 対象スクリプトの型 (index だけだと取り違える)
        int         scriptIndex = -1;
        bool        active = false;
    };
    static ScriptUndoTracker undo;

    const bool canTrackUndo =
        ctx.activeScene != nullptr &&
        ctx.undoStack != nullptr &&
        ctx.undoStack->IsRecordingEnabled();
    const ImGuiID activeBefore = ImGui::GetActiveID();
    // WHY: 眺めているだけのフレームでスナップショットを取らない。操作の開始候補
    //      (クリック / Enter / Space) が来たフレームだけ各スクリプトの現在値を控える。
    const bool mayStartEdit =
        !undo.active &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
         ImGui::IsKeyPressed(ImGuiKey_Enter) ||
         ImGui::IsKeyPressed(ImGuiKey_Space));

    // index → 描画前スナップショット。どのスクリプトが編集対象になるかは
    // ActiveID が確定するまで分からないため、候補フレームでは全件控えておく。
    std::vector<std::string> beforeSnapshots;
    if (canTrackUndo && mayStartEdit) {
        beforeSnapshots.reserve(sc->scripts.size());
        for (const auto& e : sc->scripts)
            beforeSnapshots.push_back(e.script ? CaptureScriptSnapshot(*e.script) : std::string{});
    }
    // ImGui のアイテム ID から「どのスクリプトを描画中だったか」を辿るための記録。
    int editingScriptIndex = -1;

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
                    reflector.m_tagListProvider =
                        [scene = ctx.activeScene]() -> std::vector<std::string> {
                            std::vector<std::string> tags;
                            for (auto& object : scene->GameObjects()) {
                                if (!object.tag.empty() &&
                                    std::find(tags.begin(), tags.end(), object.tag) == tags.end())
                                    tags.push_back(object.tag);
                            }
                            std::sort(tags.begin(), tags.end());
                            return tags;
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
                // WHY: どのスクリプトが編集対象になったかは、その Reflect() の描画中に
                //      ActiveID が確定したかどうかで判別する。index だけを後から推測すると
                //      複数スクリプトを付けた GameObject で取り違える。
                const ImGuiID activeBeforeScript = ImGui::GetActiveID();
                entry.script->Reflect(reflector);
                if (reflector.m_changed)
                    entry.script->OnValidate();
                const ImGuiID activeAfterScript = ImGui::GetActiveID();
                if (activeAfterScript != 0 && activeAfterScript != activeBeforeScript)
                    editingScriptIndex = i;

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

    // 対象スクリプトの現在値を撮る (追跡中のものを Undo コマンドの after にする)。
    const auto captureTracked = [&]() -> std::string {
        if (undo.scriptIndex < 0 ||
            undo.scriptIndex >= static_cast<int>(sc->scripts.size()))
            return {};
        auto& e = sc->scripts[static_cast<std::size_t>(undo.scriptIndex)];
        if (!e.script || e.script->GetTypeName() != undo.typeName) return {};
        return CaptureScriptSnapshot(*e.script);
    };

    auto pushCommand = [&](const std::string& before, const std::string& after) {
        if (before == after || before.empty()) return;

        scene::Scene* scene       = ctx.activeScene;
        const std::string guid    = undo.instanceId;
        const std::string type    = undo.typeName;
        const int         index   = undo.scriptIndex;
        const auto        markDirty = ctx.markSceneDirty;

        // WHY: GameObject* も Script* も Undo までの間に無効化され得るので、
        //      GUID → ScriptComponent → index の順で毎回引き直す。型名も照合して、
        //      間にスクリプトを付け外しされていた場合に別物へ書き込むのを防ぐ。
        auto apply = [scene, guid, type, index, markDirty](const std::string& snapshot) {
            auto* target = scene->FindByGuid(guid);
            if (!target) return;
            auto* comp = target->GetComponent<scene::ScriptComponent>();
            if (!comp || index < 0 || index >= static_cast<int>(comp->scripts.size())) return;
            auto& e = comp->scripts[static_cast<std::size_t>(index)];
            if (!e.script || e.script->GetTypeName() != type) return;
            ApplyScriptSnapshot(*e.script, snapshot);
            e.script->OnValidate();
            if (markDirty) markDirty();
        };

        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
            "Edit Script (" + type + ")",
            [apply, after]()  { apply(after); },
            [apply, before]() { apply(before); }));
    };

    const ImGuiID activeAfter = ImGui::GetActiveID();

    if (!undo.active && activeAfter != 0 && activeAfter != activeBefore &&
        editingScriptIndex >= 0 &&
        editingScriptIndex < static_cast<int>(beforeSnapshots.size())) {
        // 編集開始: 対象スクリプトの控えを Undo の before にする。
        auto& e = sc->scripts[static_cast<std::size_t>(editingScriptIndex)];
        undo.activeId    = activeAfter;
        undo.before      = beforeSnapshots[static_cast<std::size_t>(editingScriptIndex)];
        undo.instanceId  = go->instanceId;
        undo.typeName    = e.script ? e.script->GetTypeName() : "";
        undo.scriptIndex = editingScriptIndex;
        undo.active      = true;
    } else if (undo.active && activeAfter != undo.activeId) {
        // 編集終了 (別のウィジェットへ移った / 入力欄から離れた)。
        pushCommand(undo.before, captureTracked());
        undo.active = false;
    }
}

} // namespace fbzz::editor
