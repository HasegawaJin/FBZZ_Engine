// FBZZ Engine
// Inspector/InspectorPanel.cpp | fbzz::editor
// 選択 Entity / Asset の Inspector ルーティング
#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/Panels/AnimationGraphInspector.hpp>
#include <Editor/Panels/AnimationPreviewPanel.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include "InspectorAnimation.hpp"
#include "InspectorCommon.hpp"
#include "InspectorCore.hpp"
#include "InspectorEffects.hpp"
#include "InspectorEnvironment.hpp"
#include "InspectorLighting.hpp"
#include "InspectorMaterial.hpp"
#include "InspectorMultiEdit.hpp"
#include "InspectorNavigation.hpp"
#include "InspectorPhysics.hpp"
#include "InspectorRendering.hpp"
#include "InspectorTerrainWater.hpp"
#include "InspectorUI.hpp"
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
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
        ImVec4 warningBg = EditorTheme::Color(ThemeColor::Warning);
        warningBg.w = 0.16f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, warningBg);
        ImGui::BeginChild("##play_mode_warning",
            { 0.0f, ImGui::GetTextLineHeightWithSpacing() + 8.0f }, false,
            ImGuiWindowFlags_NoScrollbar);
        ImGui::SetCursorPos({ 8.0f, 4.0f });
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning),
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
            ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(ThemeColor::AccentActive));

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

            // WHY: SaveSelection だけだと元の GO が通常オブジェクトのまま残り、
            //      「プレファブを作ったのに繋がっていない」(Apply/Revert が出ない) 状態になる。
            //      保存と同時にインスタンスとして接続する。
            std::vector<scene::EntityID> connectedRoots;
            if (PrefabSerializer::SaveSelectionAndConnect(
                    *ctx.activeScene, ctx.selectedEntities, savePath, connectedRoots)) {
                ctx.requestAssetBrowserRefresh = true;
                if (ctx.markSceneDirty) ctx.markSceneDirty();
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Save selected object as prefab to Assets/Prefabs and link it");
    }

    // プレファブインスタンスには出所プレファブ名と Apply / Revert ボタンを表示する。
    // WHY: Unity の Inspector ヘッダーと同等の UX。選択中の GO が特定の .prefab
    //      から生成されたインスタンスであることをユーザーに明示し、同期操作へ
    //      素早くアクセスできるようにする。
    if (!go->prefabAssetPath.empty() && ctx.activeScene) {
        const std::string displayName =
            util::FileSystem::GetFilename(go->prefabAssetPath);

        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Accent));
        ImGui::TextUnformatted(("Prefab: " + displayName).c_str());
        ImGui::PopStyleColor();

        // シーン内の同一プレファブのインスタンス数。Apply の波及範囲を事前に見せる。
        const int instanceCount =
            PrefabSerializer::CountInstances(*ctx.activeScene, go->prefabAssetPath);

        ImGui::SameLine();
        if (ImGui::SmallButton("Apply")) {
            if (PrefabSerializer::Apply(*ctx.activeScene, go->GetID(), ctx.projectRoot)) {
                // WHY: アセットを書き換えただけでは、既に配置済みの他インスタンスは
                //      古い定義のまま残る。プレファブの意味を成すよう、その場で揃える。
                const int updated = PrefabSerializer::PropagateToInstances(
                    *ctx.activeScene, go->prefabAssetPath, go->GetID(), ctx.projectRoot);
                ctx.requestAssetBrowserRefresh = true;
                if (ctx.markSceneDirty) ctx.markSceneDirty();
                FBZZ_LOG_INFO("Prefab applied: %s (%d other instance(s) updated)",
                              displayName.c_str(), updated);
                // 他インスタンスが作り直され EntityID が変わっているため、
                // このフレームの描画は打ち切る。
                if (updated > 0) return;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(instanceCount > 1
                ? "Write instance state back to the .prefab asset,\n"
                  "then update every other instance in this scene"
                : "Write instance state back to the source .prefab asset");

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

        // 同じプレファブのインスタンスをまとめて選ぶ動線。
        // WHY: 「このプレファブは今どこに何個置かれているか」を確かめる手段が無かった。
        if (instanceCount > 1) {
            ImGui::SameLine();
            char selectLabel[64];
            std::snprintf(selectLabel, sizeof(selectLabel), "Select All (%d)", instanceCount);
            if (ImGui::SmallButton(selectLabel)) {
                const std::string prefabPath = go->prefabAssetPath;
                std::vector<scene::EntityID> instances;
                for (auto& candidate : ctx.activeScene->GameObjects())
                    if (candidate.prefabAssetPath == prefabPath)
                        instances.push_back(candidate.GetID());
                if (!instances.empty()) ctx.selectedEntities = instances;
                return; // 選択が複数になったので、この後の単体 Inspector は描かない
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Select every instance of this prefab in the open scene");
        }

        // ── Overrides: このインスタンスがアセット定義とどこで違うか ────────────
        //
        // WHY: これが見えないと「Apply したら何がアセットへ行くのか」「Revert したら
        //      何が消えるのか」が分からず、どちらのボタンも怖くて押せない。
        //
        // 差分の算出はシーン全体のシリアライズを伴うため毎フレームは回さない。
        // Undo のリビジョン (= 何か編集された) と選択が変わったときだけ取り直す。
        {
            struct OverrideCache {
                std::string       guid;
                std::size_t       undoRevision = static_cast<std::size_t>(-1);
                PrefabOverrideSet set;
                bool              valid = false;
            };
            static OverrideCache cache;

            const std::size_t revision =
                ctx.undoStack ? ctx.undoStack->GetRevision() : 0;
            if (cache.guid != go->instanceId || cache.undoRevision != revision) {
                cache.guid         = go->instanceId;
                cache.undoRevision = revision;
                cache.valid = ComputePrefabOverrides(*ctx.activeScene, go->GetID(),
                                                     ctx.projectRoot, cache.set);
                if (!cache.valid) cache.set = {};
            }

            const int overrideCount = static_cast<int>(cache.set.entries.size());
            char overridesLabel[64];
            std::snprintf(overridesLabel, sizeof(overridesLabel),
                          "Overrides (%d)", overrideCount);

            if (overrideCount > 0)
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      EditorTheme::Color(ThemeColor::AccentSoft));
            if (ImGui::SmallButton(overridesLabel))
                ImGui::OpenPopup("##prefab_overrides");
            if (overrideCount > 0) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(overrideCount > 0
                    ? "Properties on this instance that differ from the prefab asset"
                    : "This instance matches the prefab asset");

            if (ImGui::BeginPopup("##prefab_overrides")) {
                if (!cache.valid) {
                    ImGui::TextDisabled("Could not read the prefab asset.");
                } else if (cache.set.entries.empty()) {
                    ImGui::TextDisabled("No overrides — this instance matches the asset.");
                } else {
                    ImGui::TextDisabled("%d overridden propert%s",
                                        overrideCount, overrideCount == 1 ? "y" : "ies");
                    ImGui::Separator();

                    // 「1 件だけ元に戻す」。その 1 件を除いた差分で作り直すのが実装。
                    const PrefabOverride* revertRequest = nullptr;

                    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_BordersInnerH
                                                     | ImGuiTableFlags_RowBg
                                                     | ImGuiTableFlags_SizingStretchProp;
                    if (ImGui::BeginTable("##ov_list", 4, kFlags, { 620.0f, 0.0f })) {
                        ImGui::TableSetupColumn("Object",   ImGuiTableColumnFlags_WidthStretch, 0.9f);
                        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                        ImGui::TableSetupColumn("Prefab -> Instance",
                                                                 ImGuiTableColumnFlags_WidthStretch, 1.8f);
                        ImGui::TableSetupColumn("",         ImGuiTableColumnFlags_WidthFixed, 60.0f);
                        ImGui::TableHeadersRow();

                        int rowId = 0;
                        for (const PrefabOverride& entry : cache.set.entries) {
                            ImGui::TableNextRow();
                            ImGui::PushID(rowId++);

                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextUnformatted(entry.objectName.c_str());

                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextColored(EditorTheme::Color(ThemeColor::Accent),
                                               "%s", entry.path.c_str());

                            ImGui::TableSetColumnIndex(2);
                            ImGui::TextDisabled("%s", entry.prefabValue.c_str());
                            ImGui::SameLine();
                            ImGui::TextUnformatted("->");
                            ImGui::SameLine();
                            ImGui::TextUnformatted(entry.instanceValue.c_str());

                            ImGui::TableSetColumnIndex(3);
                            if (ImGui::SmallButton("Revert")) revertRequest = &entry;

                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }

                    ImGui::Separator();
                    if (ImGui::Button("Revert All")) {
                        std::vector<scene::EntityID> newRoots;
                        if (PrefabSerializer::Revert(*ctx.activeScene, go->GetID(),
                                                     newRoots, ctx.projectRoot)) {
                            if (!newRoots.empty()) ctx.selectedEntities = newRoots;
                            if (ctx.markSceneDirty) ctx.markSceneDirty();
                            cache.valid = false;
                            cache.guid.clear();
                            ImGui::EndPopup();
                            return;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Apply All to Prefab")) {
                        if (PrefabSerializer::Apply(*ctx.activeScene, go->GetID(), ctx.projectRoot)) {
                            const int updated = PrefabSerializer::PropagateToInstances(
                                *ctx.activeScene, go->prefabAssetPath, go->GetID(), ctx.projectRoot);
                            ctx.requestAssetBrowserRefresh = true;
                            if (ctx.markSceneDirty) ctx.markSceneDirty();
                            cache.guid.clear();
                            FBZZ_LOG_INFO("Prefab applied: %s (%d other instance(s) updated)",
                                          displayName.c_str(), updated);
                            ImGui::EndPopup();
                            if (updated > 0) return;
                        }
                    }

                    // 1 件だけ戻す: その 1 件を除いた差分でインスタンスを作り直す。
                    // WHY: ライブなコンポーネントへ値を書き戻すと型ごとの分岐が必要になる。
                    //      「差分集合を編集して展開し直す」なら経路が 1 本で済む。
                    if (revertRequest) {
                        const PrefabOverrideSet kept = WithoutEntry(cache.set, *revertRequest);
                        std::vector<scene::EntityID> newRoots;
                        if (PrefabSerializer::Revert(*ctx.activeScene, go->GetID(),
                                                     newRoots, ctx.projectRoot, &kept)) {
                            if (!newRoots.empty()) ctx.selectedEntities = newRoots;
                            if (ctx.markSceneDirty) ctx.markSceneDirty();
                            cache.guid.clear();
                            ImGui::EndPopup();
                            return;
                        }
                    }
                }
                ImGui::EndPopup();
            }
        }

        // プレファブ本体を隔離シーンで開く動線。
        ImGui::SameLine();
        if (ImGui::SmallButton("Open Prefab")) {
            ctx.requestOpenPrefabEdit = go->prefabAssetPath;
            return;   // このフレーム中にシーンが差し替わるため描画を打ち切る
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Open the .prefab on its own so edits apply to every instance");
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

    // ── Animator を持つ GameObject は Inspector 最下部にプレビューを出す ──
    // WHY: Unity と同じ動線。Animator 付きオブジェクトを選ぶだけでモーションを
    //   確認できるようにする。SkinnedMeshRenderer は子に分かれている構成も
    //   あるため、判定は Animator の有無だけで行う。
    // 対象未解決のときに空の "Preview" 見出しだけ残らないよう、
    // 直前フレームの解決結果 (HasAnimationPreviewTarget) で出し分ける。
    if (go->GetComponent<scene::AnimatorComponent>() && HasAnimationPreviewTarget()) {
        FBZZ_PROFILE_SCOPE("Inspector::AnimationPreview");
        ImGui::Spacing();
        ImGui::SeparatorText("Preview");
        DrawAnimationPreviewWidget(ctx, 240.0f);
    }
}

} // namespace fbzz::editor
