/// @file    EditorApp_Prefab.cpp
/// @brief   Prefab 編集モード — .prefab を隔離状態で開いて直接編集する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note Scene オブジェクトは差し替えず m_scene の中身だけを入れ替える。ProjectRuntime /
///       SceneManager / レンダーパスが握るポインタを一切張り替えずに済ませるため。
#include <Editor/EditorApp.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/ModalDialog.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>

#include <algorithm>
#include <string>
#include <vector>

namespace fbzz::editor {

namespace {

/// @brief 編集シーンのルート GO をすべて集める。
/// @note プレファブは複数ルートを持ちうるため、保存対象は「親を持たない全 GO」とする。
std::vector<scene::EntityID> CollectRoots(scene::Scene& scene)
{
    std::vector<scene::EntityID> roots;
    for (auto& go : scene.GameObjects()) {
        if (go.runtimeGenerated) continue;
        if (go.GetParent() == nullptr) roots.push_back(go.GetID());
    }
    return roots;
}

} // namespace

void EditorApp::EnterPrefabEditMode(const std::string& assetRelPath)
{
    if (assetRelPath.empty() || !m_ctx.activeScene) return;

    /// @note Play 中の編集は状態が混ざるので受け付けない。
    if (!m_playMode.IsInEditor()) {
        FBZZ_LOG_WARN("Prefab edit: stop play mode first");
        return;
    }
    /// @note 既に編集中なら、いったん保存して閉じてから開き直す。
    if (m_ctx.InPrefabEditMode()) ExitPrefabEditMode(true);

    const std::string diskPath = ToProjectAssetDiskPath(m_ctx.projectRoot, assetRelPath);
    if (!util::FileSystem::Exists(diskPath)) {
        FBZZ_LOG_ERROR("Prefab edit: asset not found: %s", diskPath.c_str());
        return;
    }

    /// @name 編集中シーンを退避
    /// @note 保存を強制せず「メモリ上に退避」し、閉じたときに dirty も含め元の状態へ戻す。
    ///       非表示を外してから採るのは、隠す前の activeSelf を記録するため —
    ///       隠したまま false を焼くと表示へ戻す手立てが無くなる。
    CaptureEditorViewStateToSceneMeta();
    RemoveEditorHiding();
    m_prefabEditStashedScene     = SceneIO::Serialize(*m_ctx.activeScene);
    RestoreEditorHiding();
    if (m_prefabEditStashedScene.empty()) {
        FBZZ_LOG_ERROR("Prefab edit: failed to snapshot the current scene");
        return;
    }
    m_prefabEditStashedScenePath = m_ctx.currentScenePath;
    m_prefabEditStashedDirty     = m_ctx.sceneDirty;
    m_prefabEditStashedSelection.clear();
    for (const scene::EntityID id : m_ctx.selectedEntities)
        if (auto* go = m_ctx.activeScene->GetGameObject(id))
            m_prefabEditStashedSelection.push_back(go->instanceId);

    /// @name プレファブだけの状態にする
    m_ctx.activeScene->Clear();
    ClearEntitySelection(m_ctx);
    m_ctx.lockedEntities.clear();
    m_ctx.editorHiddenGuids.clear();
    m_undoStack.Clear();

    std::vector<scene::EntityID> roots;
    if (!PrefabSerializer::Instantiate(*m_ctx.activeScene, diskPath, roots) || roots.empty()) {
        FBZZ_LOG_ERROR("Prefab edit: failed to open %s", assetRelPath.c_str());
        /// @note 開けなかったので退避したシーンを戻す。
        SceneIO::Deserialize(*m_ctx.activeScene, m_prefabEditStashedScene);
        m_prefabEditStashedScene.clear();
        ApplyEditorViewStateFromSceneMeta();
        RebuildEditorUIFromScene();
        return;
    }

    /// @note 編集面は「プレファブ本体」を触るため prefabAssetPath は消す (残すと自インスタンスに
    ///       見え Apply/Revert/Overrides が壊れる)。prefabSourceId は残す — SaveSelection が
    ///       保存時に「アセットへ書く id」として使うため、消すと override の対応付けが切れる。
    for (auto& go : m_ctx.activeScene->GameObjects())
        go.prefabAssetPath.clear();

    m_ctx.prefabEditPath  = assetRelPath;
    m_ctx.prefabEditDirty = false;
    m_prefabEditSaved     = false;
    m_prefabEditDiskPath  = diskPath;
    SelectEntity(m_ctx, roots.front());

    RebuildEditorUIFromScene();
    UpdateWindowTitle();
    FBZZ_LOG_INFO("Editing prefab: %s", assetRelPath.c_str());
}

bool EditorApp::SavePrefabEdit()
{
    if (!m_ctx.InPrefabEditMode() || !m_ctx.activeScene) return false;

    const std::vector<scene::EntityID> roots = CollectRoots(*m_ctx.activeScene);
    if (roots.empty()) {
        FBZZ_LOG_WARN("Prefab edit: nothing to save (no root objects)");
        return false;
    }

    if (!PrefabSerializer::SaveSelection(*m_ctx.activeScene, roots, m_prefabEditDiskPath)) {
        FBZZ_LOG_ERROR("Prefab edit: save failed: %s", m_prefabEditDiskPath.c_str());
        return false;
    }

    m_ctx.prefabEditDirty = false;
    m_prefabEditSaved     = true;
    UpdateWindowTitle();
    return true;
}

void EditorApp::ExitPrefabEditMode(bool save)
{
    if (!m_ctx.InPrefabEditMode() || !m_ctx.activeScene) return;

    const std::string editedPath = m_ctx.prefabEditPath;
    if (save) SavePrefabEdit();
    /// @note このセッション中に一度でも保存していれば、アセットは既に変わっている。
    const bool saved = m_prefabEditSaved;

    /// @name 退避していたシーンを戻す
    m_ctx.activeScene->Clear();
    ClearEntitySelection(m_ctx);
    m_undoStack.Clear();

    if (!m_prefabEditStashedScene.empty() &&
        !SceneIO::Deserialize(*m_ctx.activeScene, m_prefabEditStashedScene)) {
        /// @note ここで失敗すると編集中だったシーンを失う。握り潰さず必ず知らせる。
        FBZZ_LOG_ERROR("Prefab edit: failed to restore the previous scene");
    }

    m_ctx.currentScenePath = m_prefabEditStashedScenePath;
    m_ctx.sceneDirty       = m_prefabEditStashedDirty;
    /// @note 編集に入るとき退避した非表示 / ロックをシーンごと戻す。
    ApplyEditorViewStateFromSceneMeta();

    /// @note 選択は EntityID ではなく GUID で戻す (再構築で ID が変わるため)。
    {
        std::vector<scene::EntityID> restored;
        for (const std::string& guid : m_prefabEditStashedSelection)
            if (auto* go = m_ctx.activeScene->FindByGuid(guid))
                restored.push_back(go->GetID());
        SelectEntities(m_ctx, std::move(restored), SelectionReveal::Skip);
    }

    m_ctx.prefabEditPath.clear();
    m_ctx.prefabEditDirty = false;
    m_prefabEditStashedScene.clear();
    m_prefabEditStashedSelection.clear();
    m_prefabEditDiskPath.clear();
    m_prefabEditSaved = false;

    /// @name 保存したなら、戻ってきたシーンのインスタンスへ反映する
    /// @note 反映しないと「直したのに変わらない」ことになる。各インスタンスの個別調整は保つ。
    if (saved) {
        const int updated = PrefabSerializer::PropagateToInstances(
            *m_ctx.activeScene, editedPath, scene::EntityID{}, m_ctx.projectRoot);
        if (updated > 0) {
            MarkSceneDirty();
            FBZZ_LOG_INFO("Prefab saved: %s (%d instance(s) updated)",
                          editedPath.c_str(), updated);
        }
        m_ctx.requestAssetBrowserRefresh = true;
    }

    RebuildEditorUIFromScene();
    UpdateWindowTitle();
}

/// ディスク上で書き換わった .prefab を、シーン内のインスタンスへ反映する。
void EditorApp::ProcessPrefabDiskReloads()
{
    if (m_ctx.pendingPrefabReloads.empty()) return;

    /// @note 監視イベントは 1 回の保存で複数回来ることがあるので、まとめてから処理する。
    std::vector<std::string> paths;
    paths.swap(m_ctx.pendingPrefabReloads);
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

    /// @note 編集モード中は「編集面のシーン」しか開いていないので、反映する相手がいない。
    ///       抜けたときに ExitPrefabEditMode が伝播する。
    if (m_ctx.InPrefabEditMode() || !m_ctx.activeScene) return;
    /// @note Play 中にシーンを作り直すと実行中の状態が壊れる。
    if (!m_playMode.IsInEditor()) return;

    for (const std::string& diskPath : paths) {
        /// @note 自分の Apply / Prefab 保存で起きた変更は、既に反映済みなので無視する。
        if (PrefabSerializer::WasSelfWrittenRecently(diskPath)) continue;

        const std::string relPath = NormalizeAssetPath(diskPath);
        if (PrefabSerializer::CountInstances(*m_ctx.activeScene, relPath) == 0) continue;

        const int updated = PrefabSerializer::PropagateToInstances(
            *m_ctx.activeScene, relPath, scene::EntityID{}, m_ctx.projectRoot);
        if (updated > 0) {
            /// @note 作り直しで EntityID が変わるため、古い ID を掴んだままの選択は捨てる。
            ClearEntitySelection(m_ctx);
            MarkSceneDirty();
            FBZZ_LOG_INFO("Prefab changed on disk: %s (%d instance(s) updated)",
                          relPath.c_str(), updated);
        }
    }
}

void EditorApp::ProcessPrefabEditRequests()
{
    ProcessPrefabDiskReloads();

    if (!m_ctx.requestOpenPrefabEdit.empty()) {
        const std::string path = m_ctx.requestOpenPrefabEdit;
        m_ctx.requestOpenPrefabEdit.clear();
        EnterPrefabEditMode(path);
        /// @note 開いた直後に保存/終了要求まで処理しない
        return;
    }
    if (m_ctx.requestSavePrefabEdit) {
        m_ctx.requestSavePrefabEdit = false;
        SavePrefabEdit();
    }
    if (m_ctx.requestClosePrefabEdit) {
        m_ctx.requestClosePrefabEdit = false;
        /// @note 未保存の変更があるなら黙って捨てない。「戻る」だけで作業が消えるのは取り返しがつかない。
        if (m_ctx.prefabEditDirty) {
            const std::string name = util::FileSystem::GetFilename(m_ctx.prefabEditPath);
            ModalDialog::OpenUnsavedChanges(
                "Close Prefab",
                "The prefab '" + name + "' has unsaved changes.",
                [this]() {
                    if (!SavePrefabEdit()) return false;
                    /// @note 保存済みなので再保存はしない
                    ExitPrefabEditMode(false);
                    return true;
                },
                [this]() { ExitPrefabEditMode(false); });
        } else {
            ExitPrefabEditMode(false);
        }
    }
}

void EditorApp::DrawPrefabEditBar(EditorContext& ctx)
{
    if (!ctx.InPrefabEditMode()) return;

    /// @note 編集モードは「今どのシーンを触っているか」の認識が最も狂いやすい。
    ///       画面上部の帯で対象と戻り道を常時明示する (Unity のパンくずと同じ役割)。
    const std::string name = util::FileSystem::GetFilename(ctx.prefabEditPath);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Color(ThemeColor::AccentSoft));
    ImGui::BeginChild("##prefab_edit_bar", { 0.0f, ImGui::GetFrameHeight() + 8.0f }, false);
    ImGui::Spacing();

    if (ImGui::SmallButton("< Back to Scene"))
        ctx.requestClosePrefabEdit = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Return to the scene you were editing (unsaved prefab changes are lost)");

    ImGui::SameLine();
    ImGui::TextColored(EditorTheme::Color(ThemeColor::Accent), "Prefab: %s%s",
                       name.c_str(), ctx.prefabEditDirty ? " *" : "");

    ImGui::SameLine();
    if (ImGui::SmallButton("Save Prefab"))
        ctx.requestSavePrefabEdit = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Write the current state back to the .prefab asset");

    ImGui::SameLine();
    if (ImGui::SmallButton("Save and Close")) {
        ctx.requestSavePrefabEdit = true;
        ctx.requestClosePrefabEdit = true;
    }

    ImGui::SameLine();
    ImGui::TextDisabled("Editing the asset itself - every instance follows on save");

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

} // namespace fbzz::editor
