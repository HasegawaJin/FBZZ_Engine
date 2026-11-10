/// @file    InspectorMaterial.cpp
/// @brief   Material Component の Inspector 描画。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include "InspectorMaterial.hpp"
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include "../../Util/MaterialAuthoringEditing.hpp"

namespace fbzz::editor {

namespace {

/// @brief 編集された .mat を未保存アセットとして登録する。
/// @note インライン編集は AssetManager 上の実体だけを書き換える。登録しないと未保存プロンプトや
/// @note Save All に出ず、Double Sided 等の .mat 側パラメータが警告なしに消える。
void RegisterMaterialDirty(const std::string& path, asset::AssetHandle<asset::MaterialAsset> handle, const EditorContext& ctx)
{
    if (path.empty() || !handle.IsValid()) return;
    const std::string diskPath = MaterialAssetDiskPath(ctx, path);
    AssetDirtyRegistry::Register(
        diskPath, NormalizeAssetPath(path), "MAT",
        [diskPath, handle]() {
            return material_edit_detail::SaveAssetSnapshot(diskPath, handle);
        });
}

/// @brief マテリアルスロット 1 つぶんの .mat 編集 UI (shader / textures / params / 保存)。
/// @note MaterialComponent でなく MaterialSlot を受けるのは、Element 1 以降でも同じ UI を
/// @note その場で展開するため。コンポーネント限定だとスロット 0 決め打ちに戻ってしまう。
void DrawMaterialSlotBody(const material_edit_detail::SlotTarget& target, EditorContext& ctx)
{
            const auto* slot = target.Resolve();
            if (!slot) return;
            const auto materialHandle = slot->GetMaterialAsset();
            const std::string materialPath = slot->GetMaterialPath();
            auto materialEdit = material_edit_detail::BeginAssetEdit(materialHandle);
            if (!materialEdit) return;
            auto& mat = materialEdit->Draft();
            struct MaterialUndoTracker {
                ImGuiID activeId = 0;
                asset::AssetHandle<asset::MaterialAsset> target;
                asset::MaterialAsset::AuthoringSnapshotPtr expected;
                asset::MaterialAssetAuthoring before;
                bool active = false;
                bool changed = false;
            };
            static MaterialUndoTracker undo;
            const auto initialPublication = asset::ReadMaterialAuthoring(materialHandle);
            if (undo.target != materialHandle || (undo.expected && undo.expected != initialPublication)) {
                undo.active = false;
                undo.changed = false;
                undo.target = materialHandle;
            }
            undo.expected = initialPublication;
            const bool canRecordUndo = CanRecordEditorUndo(ctx);
            asset::MaterialAssetAuthoring materialBeforeDraw;
            if (canRecordUndo)
                materialBeforeDraw = mat;
            else
                undo.active = false;
            const ImGuiID activeBefore = ImGui::GetActiveID();
            DrawMaterialAuthoringFields(mat, ctx.resources, ctx.projectRoot);

            const auto commitResult = material_edit_detail::FinishAssetEdit(materialHandle, *materialEdit);
            bool authoringConflict = commitResult == material_edit_detail::CommitResult::CONFLICT;
            const bool materialDirty = commitResult == material_edit_detail::CommitResult::COMMITTED;
            const auto committedPublication = asset::ReadMaterialAuthoring(materialHandle);
            if (authoringConflict) { undo.active = false; undo.changed = false; }
            if (materialDirty) {
                if (auto* live = target.Resolve()) live->material.reset();
                /// @note AssetBrowser のサムネイルを値変更のたびに追従させる (保存待ちにしない)。
                ctx.BumpMaterialPreviewRevision(NormalizeAssetPath(materialPath));
                if (ctx.activeScene) {
                    const std::string changedPath = NormalizeAssetPath(materialPath);
                    for (auto [terrain] : ctx.activeScene->View<scene::TerrainComponent>()) {
                        for (const auto& lm : terrain.layerMaterials)
                            if (NormalizeAssetPath(lm) == changedPath) {
                                terrain.RequestSplatRebuild();
                                terrain.RequestMaterialRebuild();
                                break;
                            }
                    }
                    for (auto [water] : ctx.activeScene->View<scene::WaterComponent>()) {
                        if (NormalizeAssetPath(water.materialPath) == changedPath) {
                            water.texDirty = true;
                            water.foamDirty = true;
                        }
                    }
                }
                RegisterMaterialDirty(materialPath, materialHandle, ctx);
            }

            if (asset::ReadMaterialAuthoring(materialHandle) != committedPublication) {
                authoringConflict = true;
                undo.active = false;
                undo.changed = false;
            }
            undo.expected = asset::ReadMaterialAuthoring(materialHandle);
            const ImGuiID activeAfter = ImGui::GetActiveID();
            auto pushMaterialCommand = [&](const asset::MaterialAssetAuthoring& before,
                                           const asset::MaterialAssetAuthoring& after) {
                if (!ctx.undoStack) return;
                const auto handle = materialHandle;
                const auto markDirty = ctx.markSceneDirty;
                EditorContext* context = &ctx;
                const std::string relPath = NormalizeAssetPath(materialPath);
                const std::string diskPath = MaterialAssetDiskPath(ctx, materialPath);
                auto apply = [handle, markDirty, context, relPath, diskPath](
                                 const asset::MaterialAssetAuthoring& value) {
                    if (material_edit_detail::ReplaceAssetAuthoring(handle, value)) {
                        context->BumpMaterialPreviewRevision(relPath);
                        if (markDirty) markDirty();
                        /// @note Undo / Redo はメモリ上の値だけを戻す。保存済みでも
                        /// @note ここでディスクとずれるので、改めて未保存として積み直す。
                        if (!diskPath.empty()) {
                            AssetDirtyRegistry::Register(
                                diskPath, relPath, "MAT",
                                [handle, diskPath]() {
                                    return material_edit_detail::SaveAssetSnapshot(diskPath, handle);
                                });
                        }
                    }
                };
                ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                    "Edit Material Asset",
                    [apply, after]() { apply(after); },
                    [apply, before]() { apply(before); }));
            };
            if (!canRecordUndo || authoringConflict) {
                undo.active = false;
                undo.changed = false;
            } else if (!undo.active && activeAfter != 0 && activeAfter != activeBefore) {
                undo.activeId = activeAfter;
                undo.before = materialBeforeDraw;
                undo.active = true;
                undo.changed = materialDirty;
            } else if (undo.active && activeAfter == undo.activeId) {
                undo.changed |= materialDirty;
            } else if (undo.active && activeAfter != undo.activeId) {
                if (undo.changed) pushMaterialCommand(undo.before, mat);
                undo.active = false;
                undo.changed = false;
            } else if (!undo.active && materialDirty && activeAfter == 0) {
                pushMaterialCommand(materialBeforeDraw, mat);
            }

            ImGui::Separator();
            const bool canSave = !materialPath.empty() && !authoringConflict;
            if (!canSave)
                ImGui::BeginDisabled();
            if (ImGui::Button("Save .mat")) {
                const std::string diskPath = MaterialAssetDiskPath(ctx, materialPath);
                if (material_edit_detail::SaveAssetSnapshot(diskPath, materialHandle)) {
                    AssetDirtyRegistry::MarkClean(diskPath);
                    ctx.requestAssetBrowserRefresh = true;
                }
            }
            if (!canSave)
                ImGui::EndDisabled();
}

/// @brief マテリアル配列の 1 行。D&D / クリックで辿る / 中身のインライン編集をここで完結させる。
/// @note スロット番号は submesh との対応そのものでユーザーが決める値ではないため、番号入力は出さない。
/// @note widgets::AssetPathField が D&D 受理・Asset Browser への ping・Inspector 遷移を内包している。
void DrawMaterialElementRow(const material_edit_detail::SlotTarget& target,
                            size_t slotIndex,
                            bool isFallbackTarget,
                            EditorContext& ctx)
{
    auto* initial = target.Resolve();
    if (!initial) return;
    auto slotEdit = initial->BeginAuthoringEdit();
    ImGui::PushID(static_cast<int>(slotIndex));
    const std::string label = "Element " + std::to_string(slotIndex);
    const std::string originalPath = slotEdit.Draft().materialPath;
    const bool fieldAccepted = widgets::AssetPathField(label.c_str(), slotEdit.Draft().materialPath, ".mat", ctx.projectRoot);
    if (slotEdit.Draft().materialPath != originalPath)
        slotEdit.Draft().materialAsset = {};
    if (material_edit_detail::FinishSlotPathEdit(target, slotEdit, fieldAccepted) == material_edit_detail::CommitResult::COMMITTED)
        if (target.Resolve() && ctx.markSceneDirty) ctx.markSceneDirty();
    const auto* displaySlot = target.Resolve();
    const auto snapshot = displaySlot ? displaySlot->ReadAuthoring() : scene::MaterialSlot::AuthoringSnapshotPtr{};
    const auto materialHandle = displaySlot ? displaySlot->GetMaterialAsset() : asset::AssetHandle<asset::MaterialAsset>{};
    const std::string path = snapshot ? snapshot->Data().materialPath : std::string{};
    if (!path.empty() && !materialHandle.IsValid()) {
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Danger), "Missing: %s", path.c_str());
    } else if (path.empty() && isFallbackTarget) {
        ImGui::TextDisabled("未割当 — Element 0 のマテリアルで描画されます");
    }
    bool visible = displaySlot && displaySlot->visible;
    if (ImGui::Checkbox("Visible", &visible)) {
        if (auto* live = target.Resolve(); live && live->visible != visible) {
            live->visible = visible;
            if (ctx.markSceneDirty) ctx.markSceneDirty();
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("この submesh を描画するか");
    if (materialHandle.IsValid() && ImGui::TreeNode("Edit")) {
        DrawMaterialSlotBody(target, ctx);
        ImGui::TreePop();
    }
    ImGui::Separator();
    ImGui::PopID();
}

/// @note この GameObject の Renderer が描く submesh 数。Renderer が無ければ 0。
size_t RendererSubmeshCount(scene::GameObject& go)
{
    if (const auto* smr = go.GetComponent<scene::SkinnedMeshRenderer>())
        return smr->SubmeshCount();
    if (const auto* mr = go.GetComponent<scene::MeshRenderer>())
        return mr->mesh ? 1u : 0u;
    return 0u;
}

} /// @note namespace

void DrawMaterialInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    DrawComponentSection<scene::MaterialComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Material",
        [](scene::MaterialComponent& mc, EditorContext& ctx) {
            scene::GameObject* owner = ctx.GetSelectedGO();
            const size_t submeshCount = owner ? RendererSubmeshCount(*owner) : 0u;
            scene::Scene* scene = ctx.activeScene;
            const uint64_t sceneGeneration = scene ? scene->GetRenderSceneGeneration() : 0;
            const std::string ownerId = owner ? owner->instanceId : std::string{};

            /// @note 行数を submesh 数へ自動追従させる。手動の Add Slot / Match Submesh Count は廃止した。
            /// @note ずれた状態はフォールバック描画という分かりにくい挙動を生むだけだった。
            if (submeshCount > 0 && mc.SlotCount() != submeshCount)
                mc.ResizeSlots(submeshCount);

            /// @note Renderer を持たない GameObject (VFX の Mesh 出力・デカール等) は
            /// @note submesh の概念が無いので、単一マテリアルとして 1 行だけ出す。
            if (submeshCount == 0) {
                ImGui::SeparatorText("Material");
                if (!ownerId.empty()) DrawMaterialElementRow(material_edit_detail::SlotTarget(ctx, scene, sceneGeneration, ownerId, 0), 0, false, ctx);
                return;
            }

            ImGui::SeparatorText("Materials");
            const size_t slotCount = mc.SlotCount();
            ImGui::TextDisabled("Size  %zu", slotCount);
            for (size_t i = 0; i < slotCount; ++i) {
                /// @note Element 0 自身はフォールバック先なので注記の対象にしない。
                if (!ownerId.empty()) DrawMaterialElementRow(material_edit_detail::SlotTarget(ctx, scene, sceneGeneration, ownerId, i), i, i > 0, ctx);
            }
        });
}


} /// @note namespace fbzz::editor
