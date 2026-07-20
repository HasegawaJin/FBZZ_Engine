// FBZZ Engine
// Inspector/InspectorPanel.cpp | fbzz::editor
// 選択 Entity / Asset の Inspector ルーティング
#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/Panels/AnimationGraphInspector.hpp>
#include <Editor/Panels/AnimationPreviewPanel.hpp>
#include "InspectorAnimation.hpp"
#include "InspectorCommon.hpp"
#include "InspectorCore.hpp"
#include "InspectorEffects.hpp"
#include "InspectorEnvironment.hpp"
#include "InspectorLighting.hpp"
#include "InspectorMaterial.hpp"
#include "InspectorNavigation.hpp"
#include "InspectorPhysics.hpp"
#include "InspectorRendering.hpp"
#include "InspectorTerrainWater.hpp"
#include "InspectorUI.hpp"
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <imgui_internal.h>

namespace fbzz::editor {

namespace {

// 複数選択中の Transform 一括編集
void DrawMultiSelectInspector(EditorContext& ctx, const std::vector<scene::EntityID>& ids)
{
    // 有効な GO のみ収集
    std::vector<scene::GameObject*> gos;
    gos.reserve(ids.size());
    for (auto id : ids)
        if (auto* g = ctx.activeScene->GetGameObject(id))
            gos.push_back(g);
    if (gos.empty()) return;

    ImGui::TextColored({ 0.7f, 0.85f, 1.0f, 1.0f },
        "%zu objects selected", gos.size());
    ImGui::Separator();
    ImGui::Spacing();

    // ── Transform (一括) ──────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Spacing();

        // 値が全エンティティで一致するか調べる
        const auto& refT = gos.front()->transform;
        bool posAllSame = true, rotAllSame = true, scaleAllSame = true;
        for (std::size_t i = 1; i < gos.size(); ++i) {
            const auto& t = gos[i]->transform;
            if (t.position.x != refT.position.x || t.position.y != refT.position.y || t.position.z != refT.position.z)
                posAllSame = false;
            if (t.rotation.x != refT.rotation.x || t.rotation.y != refT.rotation.y ||
                t.rotation.z != refT.rotation.z || t.rotation.w != refT.rotation.w)
                rotAllSame = false;
            if (t.scale.x != refT.scale.x || t.scale.y != refT.scale.y || t.scale.z != refT.scale.z)
                scaleAllSame = false;
        }

        // 値が異なる場合は "---" を DisplayFormat に使い、灰色表示する
        struct MultiTransformEdit {
            std::vector<std::string> guids;
            std::vector<scene::Transform> before;
            bool active = false;
        };
        static MultiTransformEdit edit;

        auto trackMultiEdit = [&](bool changed, const char* description) {
            if (ImGui::IsItemActivated()) {
                edit.guids.clear();
                edit.before.clear();
                for (auto* g : gos) {
                    edit.guids.push_back(g->instanceId);
                    edit.before.push_back(g->transform);
                }
                edit.active = true;
            }
            if (!changed && !ImGui::IsItemDeactivatedAfterEdit()) return;
            if (!ImGui::IsItemDeactivatedAfterEdit()) {
                // WHY: 編集したフィールドの反映は各フィールド (Position/Rotation/Scale) の
                //      changed ハンドラ側で行っている。ここで transform 全体をコピーすると、
                //      Position を編集しただけで他オブジェクトの回転・スケールまで
                //      primary の値に潰れてしまう (Unity は編集した値だけを揃える)。
                return;
            }
            // drag 完了: push undo command
            if (edit.active && ctx.undoStack && ctx.activeScene) {
                const std::vector<std::string> guids = edit.guids;
                const std::vector<scene::Transform> before = edit.before;
                std::vector<scene::Transform> after;
                after.reserve(gos.size());
                for (auto* g : gos) after.push_back(g->transform);
                scene::Scene* scene = ctx.activeScene;
                const auto markDirty = ctx.markSceneDirty;
                auto applyAll = [scene, guids, markDirty](const std::vector<scene::Transform>& values) {
                    for (std::size_t i = 0; i < guids.size(); ++i)
                        if (auto* target = scene->FindByGuid(guids[i]))
                            target->transform = values[i];
                    if (markDirty) markDirty();
                };
                ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                    description,
                    [applyAll, after]() { applyAll(after); },
                    [applyAll, before]() { applyAll(before); }));
                if (ctx.markSceneDirty) ctx.markSceneDirty();
            }
            edit.active = false;
        };

        // Position
        float pos[3] = { refT.position.x, refT.position.y, refT.position.z };
        if (!posAllSame)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        const bool posChanged = ImGui::DragFloat3(
            posAllSame ? "Position" : "Position (---)", pos, 0.1f);
        if (!posAllSame) ImGui::PopStyleColor();
        if (posChanged) {
            gos.front()->transform.position = { pos[0], pos[1], pos[2] };
            for (std::size_t i = 1; i < gos.size(); ++i)
                gos[i]->transform.position = gos.front()->transform.position;
        }
        trackMultiEdit(posChanged, "Move (Multi)");

        // Rotation (primary euler)
        {
            math::Vector3 euler = widgets::QuatToEulerDeg(refT.rotation);
            float rot[3] = { euler.x, euler.y, euler.z };
            if (!rotAllSame)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
            const bool rotChanged = ImGui::DragFloat3(
                rotAllSame ? "Rotation" : "Rotation (---)", rot, 0.5f);
            if (!rotAllSame) ImGui::PopStyleColor();
            if (rotChanged) {
                const auto newRot = widgets::EulerDegToQuat({ rot[0], rot[1], rot[2] });
                gos.front()->transform.rotation = newRot;
                for (std::size_t i = 1; i < gos.size(); ++i)
                    gos[i]->transform.rotation = newRot;
            }
            trackMultiEdit(rotChanged, "Rotate (Multi)");
        }

        // Scale
        float scale[3] = { refT.scale.x, refT.scale.y, refT.scale.z };
        if (!scaleAllSame)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        const bool scaleChanged = ImGui::DragFloat3(
            scaleAllSame ? "Scale" : "Scale (---)", scale, 0.01f, 0.001f, 1000.0f);
        if (!scaleAllSame) ImGui::PopStyleColor();
        if (scaleChanged) {
            gos.front()->transform.scale = { scale[0], scale[1], scale[2] };
            for (std::size_t i = 1; i < gos.size(); ++i)
                gos[i]->transform.scale = gos.front()->transform.scale;
        }
        trackMultiEdit(scaleChanged, "Scale (Multi)");

        ImGui::Spacing();
    }

    // ── 共通コンポーネント一覧 ───────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::TextDisabled("Common Components");
    ImGui::Separator();

    // 全エンティティが持つコンポーネント名を列挙する
    // (単純に primary の全コンポーネントを確認し、他にも存在するかチェック)
    auto checkAll = [&](auto check) {
        for (auto* g : gos) if (!check(g)) return false;
        return true;
    };
    if (checkAll([](scene::GameObject* g) { return g->GetComponent<scene::MeshRenderer>() != nullptr; }))
        ImGui::BulletText("Mesh Renderer");
    if (checkAll([](scene::GameObject* g) { return g->GetComponent<scene::SkinnedMeshRenderer>() != nullptr; }))
        ImGui::BulletText("Skinned Mesh Renderer");
    if (checkAll([](scene::GameObject* g) { return g->GetComponent<scene::MaterialComponent>() != nullptr; }))
        ImGui::BulletText("Material");
    if (checkAll([](scene::GameObject* g) { return g->GetComponent<scene::LightComponent>() != nullptr; }))
        ImGui::BulletText("Light");
    if (checkAll([](scene::GameObject* g) { return g->GetComponent<scene::RigidBodyComponent>() != nullptr; }))
        ImGui::BulletText("Rigidbody");
    if (checkAll([](scene::GameObject* g) { return g->GetComponent<scene::AnimatorComponent>() != nullptr; }))
        ImGui::BulletText("Animator");
    if (checkAll([](scene::GameObject* g) { return g->GetComponent<scene::AudioSourceComponent>() != nullptr; }))
        ImGui::BulletText("Audio Source");
    if (checkAll([](scene::GameObject* g) { return g->GetComponent<scene::ScriptComponent>() != nullptr; }))
        ImGui::BulletText("Script");
}

template<typename T, typename Setter>
void PushGameObjectPropertyCommand(EditorContext& ctx,
                                   scene::EntityID id,
                                   const char* description,
                                   T before,
                                   T after,
                                   Setter setter)
{
    if (!ctx.undoStack || !ctx.undoStack->IsRecordingEnabled() ||
        !ctx.activeScene || before == after) return;

    scene::Scene* scene = ctx.activeScene;
    scene::GameObject* gameObject = scene->GetGameObject(id);
    if (!gameObject) return;
    const std::string instanceId = gameObject->instanceId;
    const auto markDirty = ctx.markSceneDirty;
    auto apply = [scene, instanceId, setter, markDirty](const T& value) {
        if (auto* target = scene->FindByGuid(instanceId)) {
            setter(*target, value);
            if (markDirty) markDirty();
        }
    };
    ctx.undoStack->Push(std::make_unique<LambdaCommand>(
        description,
        [apply, after]() { apply(after); },
        [apply, before]() { apply(before); }));
    if (ctx.markSceneDirty) ctx.markSceneDirty();
}

} // namespace

void InspectorPanel::OnShutdown()
{
    // WHY: Inspector はロック中の EntityID / AssetPath と、表示中 Material のハンドルを
    //      フレームをまたいで保持する。終了時は Scene / AssetManager / ImGui の破棄順が
    //      通常フレームと異なるため、古い参照状態を残すと終了中の描画・破棄で無効な
    //      アセット情報へ触れる可能性がある。
    // WHAT: Panel 自身が所有する一時状態をすべて null 状態へ戻し、後続のグローバル
    //       リソース破棄に依存しない状態にする。
    m_componentClipboard.reset();
    m_componentClipboardType = nullptr;
    m_locked = false;
    m_lockedEntityId = {};
    m_inspectedAssetPath.clear();
    m_inspectedMat = {};
}

void InspectorPanel::OnRenderContent(EditorContext& ctx)
{
    FBZZ_PROFILE_SCOPE("Inspector::Render");
    widgets::DrawAssetPickerModal(ctx.resources, ctx.imguiRenderer);

    if (ctx.mapEditingMode) {
        ImGui::TextColored({ 0.35f, 0.88f, 0.48f, 1.0f }, "MAP MODE");
        ImGui::SameLine();
        ImGui::Checkbox("Map Components Only", &ctx.mapInspectorFilter);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Show only Transform and Map-related components\nUncheck to see all components in Map Mode");
        ImGui::Separator();
    }

    // 起動時に一度だけ、保存済み折り畳み状態を ImGui StateStorage へ復元する
    if (!m_sectionStateRestored && !ctx.inspectorSectionState.empty()) {
        if (ImGuiWindow* win = ImGui::FindWindowByName("Inspector")) {
            for (const auto& [key, open] : ctx.inspectorSectionState)
                win->StateStorage.SetInt(key, open ? 1 : 0);
            m_sectionStateRestored = true;
        }
    }

    // ------------------------------------------------------------------
    // ロック解決
    // Entity ロック中は m_lockedEntityId、Asset ロック中は m_inspectedAssetPath を表示する。
    // ロック先が破棄 / 削除されていた場合は自動解除する。
    // ------------------------------------------------------------------
    scene::GameObject* selectedGo = ctx.GetSelectedGO();
    scene::GameObject* go = nullptr;
    const bool hasSelectedAsset = !ctx.selectedAssetPath.empty();
    std::string assetPathToInspect = ctx.selectedAssetPath;
    const bool assetLocked = m_locked && !m_lockedEntityId.IsValid() && !m_inspectedAssetPath.empty();

    if (m_locked && m_lockedEntityId.IsValid()) {
        if (ctx.activeScene)
            go = ctx.activeScene->GetGameObject(m_lockedEntityId);
        if (!go) {
            // 破棄 / シーン切り替えで無効になった場合は自動解除
            m_locked = false;
            m_lockedEntityId = {};
        }
    } else if (assetLocked) {
        assetPathToInspect = m_inspectedAssetPath;
    } else {
        go = selectedGo;
    }

    // --- Play 中の編集警告バナー ---
    // WHY: Play 中の Inspector 編集は Stop 時のスナップショット復元で巻き戻る。
    //      Unity が Play 中に UI を tint して知らせるのと同様、目立つバナーで注意を促す。
    if (ctx.playMode && !ctx.playMode->IsInEditor()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.35f, 0.22f, 0.05f, 0.55f));
        ImGui::BeginChild("##play_mode_warning",
            { 0.0f, ImGui::GetTextLineHeightWithSpacing() + 8.0f }, false,
            ImGuiWindowFlags_NoScrollbar);
        ImGui::SetCursorPos({ 8.0f, 4.0f });
        ImGui::TextColored({ 1.0f, 0.8f, 0.35f, 1.0f },
            "PLAY MODE \xe2\x80\x94 changes will be lost on Stop");
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    // ------------------------------------------------------------------
    // ロックボタン (右端に配置)
    // WHY: ボタン押下で m_locked が変化するため、PushStyleColor / PopStyleColor の
    //      対応を保証するには押下前の状態を wasLocked に固定しておく必要がある。
    //      m_locked を Push 判定と Pop 判定の両方で使うと片方が空振りしてクラッシュする。
    // ------------------------------------------------------------------
    {
        // ボタン描画前の状態を保存して Push/Pop を必ず対称にする
        const bool wasLocked = m_locked;
        const char* label    = wasLocked ? "Unlock" : "Lock";
        const float padX     = ImGui::GetStyle().FramePadding.x;
        const float btnW     = ImGui::CalcTextSize(label).x + padX * 2.0f;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btnW);

        if (wasLocked)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.55f, 0.85f, 1.0f));

        const bool canLock = wasLocked || hasSelectedAsset || go;
        if (!canLock)
            ImGui::BeginDisabled();
        if (ImGui::Button(label)) {
            if (wasLocked) {
                m_locked         = false;
                m_lockedEntityId = {};
                if (assetLocked) {
                    m_inspectedAssetPath.clear();
                    m_inspectedMat = {};
                }
            } else if (hasSelectedAsset) {
                m_locked          = false;
                m_lockedEntityId  = {};
                // Asset ロックは EntityID を INVALID にした m_locked と、既存の inspected path で表す。
                // WHY: InspectorPanel のデータメンバを増やすと、増分ビルドで古い確保サイズが残った時に
                //      std::string メンバ破損を起こしやすいため、既存メンバだけで状態を持つ。
                m_locked = true;
                m_inspectedAssetPath = ctx.selectedAssetPath;
                m_inspectedMat = {};
            } else if (go) {
                m_locked         = true;
                m_lockedEntityId = go->GetID();
            }
        }
        if (!canLock)
            ImGui::EndDisabled();

        if (wasLocked) ImGui::PopStyleColor();  // wasLocked で対称を保証

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(wasLocked
                ? "Unlock — follow selection changes"
                : "Lock Inspector to current selection");

        // ロック中はロック先の名前をバナー表示
        if (wasLocked && go) {
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
            ImGui::TextDisabled("Locked: %s", go->name.c_str());
        } else if (wasLocked && assetLocked) {
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
            const std::string lockedAssetName = util::FileSystem::GetFilename(m_inspectedAssetPath);
            ImGui::TextDisabled("Locked: %s", lockedAssetName.c_str());
        }
    }

    ImGui::Spacing();

    // アセット選択中、または Asset Inspector ロック中 → アセットインスペクターへ
    if ((!m_locked || assetLocked) && !assetPathToInspect.empty()) {
        FBZZ_PROFILE_SCOPE("Inspector::Asset");
        DrawAssetInspector(ctx, assetPathToInspect);
        return;
    }

    // 複数選択中 (ロックなし) は Multi-select Inspector を表示
    if (!m_locked && ctx.selectedEntities.size() > 1 && ctx.activeScene) {
        DrawMultiSelectInspector(ctx, ctx.selectedEntities);
        return;
    }

    if (!go) {
        // 空状態ガイド: 何を選べば編集できるかを案内し、初見の迷いを消す。
        ImGui::Spacing();
        ImGui::TextDisabled("Nothing selected");
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Select a GameObject in the Scene or Hierarchy to edit its components here.");
        ImGui::Spacing();
        ImGui::TextDisabled("Tip: press Ctrl+K to jump to any object or asset, F1 for shortcuts.");
        ImGui::PopTextWrapPos();
        return;
    }

    // Animation Graph の要素選択中は、GameObject 全体ではなく選択要素の詳細を表示する。
    // WHY: Graph は遷移関係の操作に専念し、State / Transition の設定は Inspector に集約する。
    {
        FBZZ_PROFILE_SCOPE("Inspector::AnimationGraphSelection");
        if (DrawAnimationGraphInspector(ctx, *go)) {
            // Unity と同じく、State / Transition の詳細の下でアニメーションを直接確認できる。
            ImGui::SeparatorText("Preview");
            DrawAnimationPreviewWidget(ctx, 240.0f);
            return;
        }
    }

    // ── Save as Prefab ───────────────────────────────────────────────────────
    // WHY: Hierarchy のコンテキストメニューを使わずに Inspector から直接 Prefab 化できる動線。
    //      Unity の Inspector ヘッダーと同様に最上部に配置する。
    {
        static constexpr const char* kSaveLabel = "Save as Prefab";
        const float btnW = ImGui::CalcTextSize(kSaveLabel).x
                         + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btnW);
        if (ImGui::SmallButton(kSaveLabel) && !ctx.selectedEntities.empty() && ctx.activeScene) {
            // 保存先: <projectRoot>/Assets/Prefabs/<name>.prefab (重複時は連番付き)
            const std::string assetRoot = ctx.projectRoot.empty()
                ? "Assets"
                : ctx.projectRoot + "/Assets";
            const std::string prefabDir = assetRoot + "/Prefabs";
            util::FileSystem::EnsureDirectory(prefabDir);

            // ファイル名として使えない文字をアンダースコアに置換する
            std::string safeName;
            for (char c : go->name) {
                const bool ok = std::isalnum(static_cast<unsigned char>(c))
                             || c == '_' || c == '-' || c == ' ';
                safeName += ok ? c : '_';
            }
            if (safeName.empty()) safeName = "Prefab";

            const std::string base = prefabDir + "/" + safeName;
            std::string savePath = base + ".prefab";
            for (int i = 1; util::FileSystem::Exists(savePath) && i < 10000; ++i)
                savePath = base + " " + std::to_string(i) + ".prefab";

            if (PrefabSerializer::SaveSelection(*ctx.activeScene, ctx.selectedEntities, savePath))
                ctx.requestAssetBrowserRefresh = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Save selected object as prefab to Assets/Prefabs");
    }

    // プレファブインスタンスには出所プレファブ名と Apply / Revert ボタンを表示する。
    // WHY: Unity の Inspector ヘッダーと同等の UX。選択中の GO が特定の .prefab
    //      から生成されたインスタンスであることをユーザーに明示し、同期操作へ
    //      素早くアクセスできるようにする。
    if (!go->prefabAssetPath.empty() && ctx.activeScene) {
        const std::string displayName =
            util::FileSystem::GetFilename(go->prefabAssetPath);

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
        ImGui::TextUnformatted(("Prefab: " + displayName).c_str());
        ImGui::PopStyleColor();

        ImGui::SameLine();
        if (ImGui::SmallButton("Apply")) {
            PrefabSerializer::Apply(*ctx.activeScene, go->GetID(), ctx.projectRoot);
            ctx.requestAssetBrowserRefresh = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Write instance state back to the source .prefab asset");

        ImGui::SameLine();
        if (ImGui::SmallButton("Revert")) {
            std::vector<scene::EntityID> newRoots;
            if (PrefabSerializer::Revert(*ctx.activeScene, go->GetID(), newRoots, ctx.projectRoot)) {
                if (!newRoots.empty()) ctx.selectedEntities = newRoots;
                if (ctx.markSceneDirty) ctx.markSceneDirty();
                return; // 古い GO を描画し続けないよう早期リターン
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Discard instance changes and restore from the source .prefab asset");
    }

    char nameBuf[256];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", go->name.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    static scene::EntityID namingEntity;
    static std::string nameBeforeEdit;
    const std::string nameAtFrameStart = go->name;
    if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf)))
        go->name = nameBuf;
    if (ImGui::IsItemActivated()) {
        namingEntity = go->GetID();
        nameBeforeEdit = nameAtFrameStart;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && namingEntity == go->GetID()) {
        PushGameObjectPropertyCommand(
            ctx, go->GetID(), "Rename GameObject", nameBeforeEdit, go->name,
            [](scene::GameObject& target, const std::string& value) { target.name = value; });
        namingEntity = {};
        nameBeforeEdit.clear();
    }

    auto& ps = ctx.projectSettings;

    {
        const float spacing   = ImGui::GetStyle().ItemSpacing.x;
        const float labelTagW = ImGui::CalcTextSize("Tag").x   + spacing;
        const float labelLayW = ImGui::CalcTextSize("Layer").x + spacing;
        const float comboW    = (ImGui::GetContentRegionAvail().x - labelTagW - labelLayW - spacing) * 0.5f;

        // Tag
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Tag");
        ImGui::SameLine();
        int tagIdx = 0;
        for (int i = 0; i < (int)ps.game.tags.size(); ++i)
            if (go->tag == ps.game.tags[i]) { tagIdx = i; break; }
        const char* tagLabel = ps.game.tags.empty() ? "(none)" : ps.game.tags[tagIdx].c_str();
        ImGui::SetNextItemWidth(comboW);
        if (ImGui::BeginCombo("##tag", tagLabel)) {
            for (int i = 0; i < (int)ps.game.tags.size(); ++i) {
                bool selected = (i == tagIdx);
                if (ImGui::Selectable(ps.game.tags[i].c_str(), selected)) {
                    const std::string before = go->tag;
                    go->tag = ps.game.tags[i];
                    PushGameObjectPropertyCommand(
                        ctx, go->GetID(), "Change Tag", before, go->tag,
                        [](scene::GameObject& target, const std::string& value) { target.tag = value; });
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::Separator();
            if (ImGui::Selectable("Add Tag..."))
                ctx.requestOpenProjectSettings = true;
            ImGui::EndCombo();
        }

        // Layer
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Layer");
        ImGui::SameLine();
        int layerIdx = go->layer & 31;
        ImGui::SetNextItemWidth(-1.0f);
        const std::string currentLayerLabel = ps.game.layerNames[layerIdx].empty()
            ? ("User Layer " + std::to_string(layerIdx))
            : ps.game.layerNames[layerIdx];
        if (ImGui::BeginCombo("##layer", currentLayerLabel.c_str())) {
            for (int i = 0; i < 32; ++i) {
                const std::string layerLabel = ps.game.layerNames[i].empty()
                    ? ("User Layer " + std::to_string(i))
                    : ps.game.layerNames[i];
                const bool selected = i == layerIdx;
                if (ImGui::Selectable(layerLabel.c_str(), selected)) {
                    const int before = go->layer;
                    go->layer = i;
                    PushGameObjectPropertyCommand(
                        ctx, go->GetID(), "Change Layer", before, go->layer,
                        [](scene::GameObject& target, int value) { target.layer = value; });
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();

    { FBZZ_PROFILE_SCOPE("Inspector::Transform");
      DrawTransformInspectors(go, ctx); }
    if (ctx.mapEditingMode && ctx.mapInspectorFilter) {
        const bool hasMapComponent =
            go->GetComponent<scene::TerrainComponent>()
            || go->GetComponent<scene::WaterComponent>()
            || go->GetComponent<scene::TerrainDetailComponent>()
            || go->GetComponent<scene::FoliageComponent>()
            || go->GetComponent<scene::TerrainGridComponent>();
        if (!hasMapComponent) {
            ImGui::TextDisabled("No Map component on this GameObject.");
            ImGui::TextDisabled("Disable Map Components Only to inspect everything.");
            return;
        }
        { FBZZ_PROFILE_SCOPE("Inspector::TerrainWater");
          DrawTerrainWaterInspectors(
              go, ctx, m_componentClipboard, m_componentClipboardType); }
        return;
    }
    const auto drawAutomatic = [&](scene::ComponentCategory category) {
        DrawAutomaticInspectors(
            category, go, ctx, m_componentClipboard, m_componentClipboardType);
    };

    { FBZZ_PROFILE_SCOPE("Inspector::Rendering");
      DrawRenderingInspectors(go, ctx, m_componentClipboard, m_componentClipboardType); }
    { FBZZ_PROFILE_SCOPE("Inspector::Material");
      DrawMaterialInspectors(go, ctx, m_componentClipboard, m_componentClipboardType); }
    drawAutomatic(scene::ComponentCategory::Rendering);
    { FBZZ_PROFILE_SCOPE("Inspector::Lighting");
      DrawLightingInspectors(go, ctx, m_componentClipboard, m_componentClipboardType); }
    drawAutomatic(scene::ComponentCategory::Lighting);
    { FBZZ_PROFILE_SCOPE("Inspector::Physics");
      DrawPhysicsInspectors(go, ctx, m_componentClipboard, m_componentClipboardType); }
    drawAutomatic(scene::ComponentCategory::Physics);
    { FBZZ_PROFILE_SCOPE("Inspector::Animation");
      DrawAnimationInspectors(go, ctx, m_componentClipboard, m_componentClipboardType); }
    drawAutomatic(scene::ComponentCategory::Animation);
    drawAutomatic(scene::ComponentCategory::Audio);
    { FBZZ_PROFILE_SCOPE("Inspector::Effects");
      DrawEffectsInspectors(go, ctx, m_componentClipboard, m_componentClipboardType); }
    drawAutomatic(scene::ComponentCategory::Effects);
    { FBZZ_PROFILE_SCOPE("Inspector::Environment");
      DrawEnvironmentInspectors(go, ctx, m_componentClipboard, m_componentClipboardType); }
    drawAutomatic(scene::ComponentCategory::Environment);
    { FBZZ_PROFILE_SCOPE("Inspector::Navigation");
      DrawNavigationInspectors(go, ctx, m_componentClipboard, m_componentClipboardType); }
    drawAutomatic(scene::ComponentCategory::Navigation);
    { FBZZ_PROFILE_SCOPE("Inspector::AutomaticUI");
      drawAutomatic(scene::ComponentCategory::UI); }
    { FBZZ_PROFILE_SCOPE("Inspector::TerrainWater");
      DrawTerrainWaterInspectors(go, ctx, m_componentClipboard, m_componentClipboardType); }
    drawAutomatic(scene::ComponentCategory::Terrain);
    drawAutomatic(scene::ComponentCategory::Misc);
    DrawScriptInspectors(go, ctx);

    ImGui::Spacing();
    { FBZZ_PROFILE_SCOPE("Inspector::AddComponent");
      DrawAddComponentMenu(*go, m_addComponentFilter, ctx); }
}

} // namespace fbzz::editor
