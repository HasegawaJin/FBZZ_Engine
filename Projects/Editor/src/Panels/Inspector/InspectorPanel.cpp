// FBZZ Engine
// Inspector/InspectorPanel.cpp | fbzz::editor
// 選択 Entity / Asset の Inspector ルーティング
#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/Panels/AnimationGraphInspector.hpp>
#include "InspectorAnimation.hpp"
#include "InspectorAudio.hpp"
#include "InspectorCommon.hpp"
#include "InspectorCore.hpp"
#include "InspectorEffects.hpp"
#include "InspectorEnvironment.hpp"
#include "InspectorLighting.hpp"
#include "InspectorMaterial.hpp"
#include "InspectorPhysics.hpp"
#include "InspectorRendering.hpp"
#include "InspectorTerrainWater.hpp"
#include "InspectorUI.hpp"
#include <Engine/Profiler/ProfileScope.hpp>
#include <imgui_internal.h>

namespace fbzz::editor {

namespace {

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

    if (!go) {
        ImGui::TextDisabled("Nothing selected");
        return;
    }

    // Animation Graph の要素選択中は、GameObject 全体ではなく選択要素の詳細を表示する。
    // WHY: Graph は遷移関係の操作に専念し、State / Transition の設定は Inspector に集約する。
    {
        FBZZ_PROFILE_SCOPE("Inspector::AnimationGraphSelection");
        if (DrawAnimationGraphInspector(ctx, *go))
            return;
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
            // 保存先: <projectRoot>/Assets/Prefabs/<name>.fbzzprefab (重複時は連番付き)
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
            std::string savePath = base + ".fbzzprefab";
            for (int i = 1; util::FileSystem::Exists(savePath) && i < 10000; ++i)
                savePath = base + " " + std::to_string(i) + ".fbzzprefab";

            if (PrefabSerializer::SaveSelection(*ctx.activeScene, ctx.selectedEntities, savePath))
                ctx.requestAssetBrowserRefresh = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Save selected object as prefab to Assets/Prefabs");
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
        for (int i = 0; i < (int)ps.tags.size(); ++i)
            if (go->tag == ps.tags[i]) { tagIdx = i; break; }
        const char* tagLabel = ps.tags.empty() ? "(none)" : ps.tags[tagIdx].c_str();
        ImGui::SetNextItemWidth(comboW);
        if (ImGui::BeginCombo("##tag", tagLabel)) {
            for (int i = 0; i < (int)ps.tags.size(); ++i) {
                bool selected = (i == tagIdx);
                if (ImGui::Selectable(ps.tags[i].c_str(), selected)) {
                    const std::string before = go->tag;
                    go->tag = ps.tags[i];
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
        const std::string currentLayerLabel = ps.layerNames[layerIdx].empty()
            ? ("User Layer " + std::to_string(layerIdx))
            : ps.layerNames[layerIdx];
        if (ImGui::BeginCombo("##layer", currentLayerLabel.c_str())) {
            for (int i = 0; i < 32; ++i) {
                const std::string layerLabel = ps.layerNames[i].empty()
                    ? ("User Layer " + std::to_string(i))
                    : ps.layerNames[i];
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
      DrawTransformInspector(go, ctx); }
    if (ctx.mapEditingMode && ctx.mapInspectorFilter) {
        const bool hasMapComponent =
            go->GetComponent<scene::TerrainComponent>()
            || go->GetComponent<scene::WaterComponent>()
            || go->GetComponent<scene::TerrainDetailComponent>()
            || go->GetComponent<scene::FoliageComponent>();
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
    { FBZZ_PROFILE_SCOPE("Inspector::Rendering");
      DrawRenderingInspectors(go, ctx, m_componentClipboard, m_componentClipboardType); }
    { FBZZ_PROFILE_SCOPE("Inspector::Animation");
      DrawAnimationInspectors(go, ctx, m_componentClipboard, m_componentClipboardType); }
    { FBZZ_PROFILE_SCOPE("Inspector::Material");
      DrawMaterialInspectors(go, ctx, m_componentClipboard, m_componentClipboardType); }
    { FBZZ_PROFILE_SCOPE("Inspector::Lighting");
      DrawLightingInspectors(go, ctx, m_componentClipboard, m_componentClipboardType); }
    { FBZZ_PROFILE_SCOPE("Inspector::Effects");
      DrawEffectsInspectors(go, ctx, m_componentClipboard, m_componentClipboardType); }
    { FBZZ_PROFILE_SCOPE("Inspector::Audio");
      DrawAudioInspectors(go, ctx, m_componentClipboard, m_componentClipboardType); }
    { FBZZ_PROFILE_SCOPE("Inspector::Physics");
      DrawPhysicsInspectors(go, ctx, m_componentClipboard, m_componentClipboardType); }
    { FBZZ_PROFILE_SCOPE("Inspector::Environment");
      DrawEnvironmentInspectors(go, ctx, m_componentClipboard, m_componentClipboardType); }
    { FBZZ_PROFILE_SCOPE("Inspector::UI");
      DrawUIInspectors(go, ctx, m_componentClipboard, m_componentClipboardType); }
    { FBZZ_PROFILE_SCOPE("Inspector::TerrainWater");
      DrawTerrainWaterInspectors(go, ctx, m_componentClipboard, m_componentClipboardType); }
    DrawScriptInspectors(go, ctx);

    ImGui::Spacing();
    { FBZZ_PROFILE_SCOPE("Inspector::AddComponent");
      DrawAddComponentMenu(*go, m_addComponentFilter, ctx); }
}

} // namespace fbzz::editor
