// FBZZ Engine
// Inspector/InspectorPanel.cpp | fbzz::editor
// 選択 Entity / Asset の Inspector ルーティング
#include <Editor/Panels/InspectorPanel.hpp>
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

namespace fbzz::editor {

void InspectorPanel::OnRenderContent(EditorContext& ctx)
{
    // ------------------------------------------------------------------
    // ロック解決
    // ロック中は m_lockedEntityId のオブジェクトを表示する。
    // ロック先が破棄されていた場合は自動解除する。
    // ------------------------------------------------------------------
    scene::GameObject* selectedGo = ctx.GetSelectedGO();
    scene::GameObject* go = nullptr;
    const bool hasSelectedAsset = !ctx.selectedAssetPath.empty();

    if (m_locked) {
        if (ctx.activeScene)
            go = ctx.activeScene->GetGameObject(m_lockedEntityId);
        if (!go) {
            // 破棄 / シーン切り替えで無効になった場合は自動解除
            m_locked = false;
            m_lockedEntityId = {};
        }
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

        const bool canLock = wasLocked || (!hasSelectedAsset && go);
        if (!canLock)
            ImGui::BeginDisabled();
        if (ImGui::Button(label)) {
            if (wasLocked) {
                m_locked         = false;
                m_lockedEntityId = {};
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
        }
    }

    ImGui::Spacing();

    // アセット選択中かつロックなし → アセットインスペクターへ
    if (!m_locked && hasSelectedAsset) {
        DrawAssetInspector(ctx, ctx.selectedAssetPath);
        return;
    }

    if (!go) {
        ImGui::TextDisabled("Nothing selected");
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
    if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf)))
        go->name = nameBuf;

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
                if (ImGui::Selectable(ps.tags[i].c_str(), selected))
                    go->tag = ps.tags[i];
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
                if (ImGui::Selectable(layerLabel.c_str(), selected))
                    go->layer = i;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();

    DrawTransformInspector(go, ctx);

    DrawRenderingInspectors(go, ctx, m_componentClipboard, m_componentClipboardType);
    DrawAnimationInspectors(go, ctx, m_componentClipboard, m_componentClipboardType);
    DrawMaterialInspectors(go, ctx, m_componentClipboard, m_componentClipboardType);
    DrawLightingInspectors(go, ctx, m_componentClipboard, m_componentClipboardType);
    DrawEffectsInspectors(go, ctx, m_componentClipboard, m_componentClipboardType);
    DrawAudioInspectors(go, ctx, m_componentClipboard, m_componentClipboardType);
    DrawPhysicsInspectors(go, ctx, m_componentClipboard, m_componentClipboardType);
    DrawEnvironmentInspectors(go, ctx, m_componentClipboard, m_componentClipboardType);
    DrawUIInspectors(go, ctx, m_componentClipboard, m_componentClipboardType);
    DrawTerrainWaterInspectors(go, ctx, m_componentClipboard, m_componentClipboardType);
    DrawScriptInspectors(go, ctx);

    ImGui::Spacing();
    DrawAddComponentMenu(*go, m_addComponentFilter);
}

} // namespace fbzz::editor
