// FBZZ Engine
// EditorApp_Scene.cpp | fbzz::editor
// シーンの新規作成・開く・保存・ダーティ追跡・ホットリロード
//
// WHY: EditorApp.cpp が肥大化しないよう、シーン I/O とダーティ追跡を分離した。
//      これらはいずれも「シーンファイル」という単一の概念を中心とした処理群であり、
//      ライフサイクル管理 (Init/Shutdown/BeginFrame) や UI (MenuBar) とは関心が異なる。
#include <Editor/EditorApp.hpp>
#include <Editor/Util/ModalDialog.hpp>
#include <Editor/Util/FileDialog.hpp>
#include <Editor/Util/SceneSerializer.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <Windows.h>

namespace fbzz::editor {

namespace {

const FileFilter SCENE_FILTER{ "FBZZ Scene", "*.fbzz" };

// 拡張子がなければ ".fbzz" を付与する
std::string WithFbzzExtension(const std::string& path)
{
    if (path.empty() || !util::FileSystem::GetExtension(path).empty()) return path;
    return path + ".fbzz";
}

} // namespace

// =============================================================================
// シーン ダーティ追跡
// =============================================================================

void EditorApp::CaptureCleanScene()
{
    if (!m_ctx.activeScene) {
        m_dirtyTracker.Reset();
        m_ctx.sceneDirty = false;
        return;
    }

    CacheSceneWriteTime();
    m_dirtyTracker.CaptureClean(*m_ctx.activeScene);
    m_ctx.sceneDirty = false;
    m_dirtyPollTimer = 0.0f;
    UpdateWindowTitle();
}

void EditorApp::RefreshSceneDirtyState(bool force)
{
    if (!m_ctx.activeScene) return;
    if (m_ctx.playMode && !m_ctx.playMode->IsInEditor()) return;

    // WHY: SceneDirtyTracker::Evaluate() はシーン全体を serialize して hash 化するため、
    // 未編集のアイドル状態で 0.5 秒ごとに呼ぶと Release ビルドでは FPS の周期的な落ち込みとして見える。
    // WHAT: 編集操作は MarkDirty() で dirty に遷移させる設計なので、clean 状態では重い再評価を省略する。
    if (!force && !m_ctx.sceneDirty && !m_dirtyTracker.IsDirty())
        return;

    m_dirtyPollTimer += ImGui::GetIO().DeltaTime;
    if (!force && m_dirtyPollTimer < 0.5f && m_ctx.sceneDirty == m_dirtyTracker.IsDirty())
        return;
    m_dirtyPollTimer = 0.0f;

    const bool wasDirty = m_ctx.sceneDirty;
    m_ctx.sceneDirty = m_dirtyTracker.Evaluate(*m_ctx.activeScene);
    if (wasDirty != m_ctx.sceneDirty)
        UpdateWindowTitle();
}

void EditorApp::MarkSceneDirty()
{
    m_dirtyTracker.MarkDirty();
    if (!m_ctx.sceneDirty) {
        m_ctx.sceneDirty = true;
        UpdateWindowTitle();
    }
}

// =============================================================================
// 未保存確認ダイアログ
// =============================================================================

void EditorApp::ConfirmDiscardUnsaved(const std::string& actionName, std::function<void()> action)
{
    if (!m_ctx.sceneDirty) {
        if (action) action();
        return;
    }

    ModalDialog::OpenUnsavedChanges(actionName,
        "The current scene has unsaved changes.",
        [this, action]() {
            if (!SaveScene()) return false;
            if (action) action();
            return true;
        },
        std::move(action));
}

// =============================================================================
// 新規シーン
// =============================================================================

void EditorApp::NewScene()
{
    if (!m_ctx.activeScene) return;
    m_ctx.activeScene->Clear();
    m_ctx.selectedEntities.clear();
    m_settings.lastScenePath.clear();
    m_ctx.currentScenePath.clear();
    m_lastSceneWriteTime = {};
    CaptureCleanScene();
    FBZZ_LOG_INFO("New scene created");
}

void EditorApp::RequestNewScene()
{
    ConfirmDiscardUnsaved("New Scene", [this]() { NewScene(); });
}

// =============================================================================
// シーンを開く
// =============================================================================

void EditorApp::RequestOpenSceneFromDialog()
{
    ConfirmDiscardUnsaved("Open Scene", [this]() { OpenSceneFromDialog(); });
}

void EditorApp::RequestOpenScenePath(const std::string& path)
{
    ConfirmDiscardUnsaved("Open Scene", [this, path]() { OpenScenePath(path); });
}

bool EditorApp::OpenSceneFromDialog()
{
    if (!m_ctx.activeScene) return false;

    std::string path;
    if (!FileDialog::OpenFile(m_hwnd, { SCENE_FILTER }, path)) return false;
    return OpenScenePath(path);
}

bool EditorApp::OpenScenePath(const std::string& path)
{
    if (!m_ctx.activeScene || path.empty()) return false;

    if (!SceneSerializer::Load(*m_ctx.activeScene, path)) {
        FBZZ_LOG_ERROR("Open scene failed: %s", path.c_str());
        return false;
    }

    m_settings.lastScenePath = path;
    m_ctx.currentScenePath = path;
    m_ctx.selectedEntities.clear();
    CaptureCleanScene();
    FBZZ_LOG_INFO("Opened scene: %s", path.c_str());
    return true;
}

// =============================================================================
// シーンを保存
// =============================================================================

bool EditorApp::SaveScene()
{
    if (!m_ctx.activeScene) return false;
    if (m_settings.lastScenePath.empty()) return SaveSceneAsDialog();

    if (!SceneSerializer::Save(*m_ctx.activeScene, m_settings.lastScenePath)) {
        FBZZ_LOG_ERROR("Save scene failed: %s", m_settings.lastScenePath.c_str());
        return false;
    }
    m_ctx.projectSettings.Save(m_projectSettingsPath);

    m_ctx.currentScenePath = m_settings.lastScenePath;
    CaptureCleanScene();
    FBZZ_LOG_INFO("Saved scene: %s", m_settings.lastScenePath.c_str());
    return true;
}

bool EditorApp::SaveSceneAsDialog()
{
    if (!m_ctx.activeScene) return false;

    std::string path;
    if (!FileDialog::SaveFile(m_hwnd, { SCENE_FILTER }, path)) return false;
    path = WithFbzzExtension(path);

    if (!SceneSerializer::Save(*m_ctx.activeScene, path)) {
        FBZZ_LOG_ERROR("Save scene failed: %s", path.c_str());
        return false;
    }
    m_ctx.projectSettings.Save(m_projectSettingsPath);

    m_settings.lastScenePath = path;
    m_ctx.currentScenePath = path;
    CaptureCleanScene();
    FBZZ_LOG_INFO("Saved scene: %s", path.c_str());
    return true;
}

// =============================================================================
// アプリケーション終了リクエスト
// =============================================================================

void EditorApp::RequestExit()
{
    ConfirmDiscardUnsaved("Exit", []() { PostQuitMessage(0); });
}

// =============================================================================
// ホットリロード
// =============================================================================

void EditorApp::CacheSceneWriteTime()
{
    if (m_settings.lastScenePath.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExA(m_settings.lastScenePath.c_str(), GetFileExInfoStandard, &info))
        m_lastSceneWriteTime = info.ftLastWriteTime;
}

void EditorApp::CheckHotReload()
{
    if (!m_ctx.hotReloadEnabled) return;
    if (m_settings.lastScenePath.empty() || !m_ctx.activeScene) return;
    if (m_ctx.playMode && !m_ctx.playMode->IsInEditor()) return;

    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExA(m_settings.lastScenePath.c_str(), GetFileExInfoStandard, &info))
        return;

    const FILETIME& ft = info.ftLastWriteTime;
    // キャッシュが未設定 (初回) の場合はリロードせずに記録だけする
    if (m_lastSceneWriteTime.dwLowDateTime == 0 && m_lastSceneWriteTime.dwHighDateTime == 0) {
        m_lastSceneWriteTime = ft;
        return;
    }

    if (CompareFileTime(&ft, &m_lastSceneWriteTime) != 0) {
        m_lastSceneWriteTime = ft;
        if (!SceneSerializer::Load(*m_ctx.activeScene, m_settings.lastScenePath))
            FBZZ_LOG_WARN("Hot reload failed: %s", m_settings.lastScenePath.c_str());
        else {
            m_ctx.selectedEntities.clear();
            CaptureCleanScene();
            FBZZ_LOG_INFO("Hot reloaded: %s", m_settings.lastScenePath.c_str());
        }
    }
}

} // namespace fbzz::editor
