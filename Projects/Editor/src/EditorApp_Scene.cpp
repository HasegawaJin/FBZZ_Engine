/// @file    EditorApp_Scene.cpp
/// @brief   シーンの新規作成・開く・保存・ダーティ追跡・ホットリロード。
/// @author  Hasegawa Jin
/// @date    2026-05-31

/// @note シーン I/O とダーティ追跡に特化した分離ファイル。ライフサイクル管理や UI (MenuBar) とは関心が異なる。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Editor/EditorApp.hpp>
#include <Editor/Util/LocalClock.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ModalDialog.hpp>
#include <Editor/Util/FileDialog.hpp>
#include <Editor/Util/Localization.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/Selection.hpp>
#include <Editor/Util/Toast.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Concurrency/TaskSystem.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/ShaderCompileDiagnostics.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>
#include <Windows.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <vector>

namespace fbzz::editor {

namespace {

const FileFilter SCENE_FILTER{ "Scene", "*.scene" };
constexpr float HOT_RELOAD_TREE_POLL_INTERVAL = 0.5f;

/// @brief 拡張子がなければ ".scene" を付与する
std::string WithFbzzExtension(const std::string& path)
{
    if (path.empty() || !util::FileSystem::GetExtension(path).empty()) return path;
    return path + ".scene";
}

/// @brief Windows の 100ns 更新時刻をヘッダー非依存の整数表現へ変換する。
/// @see FILETIME structure https://learn.microsoft.com/en-us/windows/win32/api/minwinbase/ns-minwinbase-filetime
std::uint64_t ToWriteTimestamp(const FILETIME& fileTime)
{
    return (static_cast<std::uint64_t>(fileTime.dwHighDateTime) << 32)
        | fileTime.dwLowDateTime;
}

/// @brief 指定パス自身の Windows 更新時刻を取得する。
/// @note std::filesystem::file_time_type は実装依存の clock を使うため、Windows の 100ns 時刻を正本として比較する。
bool TryGetWriteTime(const std::filesystem::path& path, std::uint64_t& out)
{
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        return false;

    out = ToWriteTimestamp(info.ftLastWriteTime);
    return true;
}

/// @brief HLSL ツリー指紋へ 64bit 値を FNV-1a で混ぜる。
/// @note 更新時刻の最大値だけでは、ファイル削除や時刻を維持した改名を検知できない。
void MixShaderFingerprint(std::uint64_t& fingerprint, std::uint64_t value)
{
    for (int byteIndex = 0; byteIndex < 8; ++byteIndex) {
        fingerprint ^= static_cast<std::uint8_t>(value >> (byteIndex * 8));
        fingerprint *= 1099511628211ull;
    }
}

/// @brief HLSL/HLSLI と統合スクリプトのパス・更新時刻・サイズから決定的なツリー指紋を作る。
/// @note 追加・更新・削除・改名のすべてを 1 つの比較で検知し、CSO/meta は監視対象から除外する。
std::uint64_t GetShaderSourceFingerprint(const std::filesystem::path& root)
{
    std::vector<std::filesystem::path> sourcePaths;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        if (error) {
            error.clear();
            iterator.increment(error);
            continue;
        }

        const std::filesystem::path path = iterator->path();
        if (iterator->is_directory(error)) {
            const std::wstring directoryName = path.filename().wstring();
            if (directoryName == L"compiled" || directoryName == L"compiled_dx12")
                iterator.disable_recursion_pending();
            iterator.increment(error);
            continue;
        }

        const std::wstring extension = path.extension().wstring();
        if (extension == L".hlsl" || extension == L".hlsli"
            || path.filename() == L"compile_shaders.ps1")
            sourcePaths.push_back(path);
        iterator.increment(error);
    }
    if (sourcePaths.empty()) return 0;

    std::sort(sourcePaths.begin(), sourcePaths.end());
    std::uint64_t fingerprint = 1469598103934665603ull;
    MixShaderFingerprint(fingerprint, sourcePaths.size());
    for (const std::filesystem::path& path : sourcePaths) {
        const std::wstring relativePath = path.lexically_relative(root).generic_wstring();
        for (const wchar_t codeUnit : relativePath)
            MixShaderFingerprint(fingerprint, static_cast<std::uint16_t>(codeUnit));

        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) continue;
        MixShaderFingerprint(fingerprint, info.ftLastWriteTime.dwHighDateTime);
        MixShaderFingerprint(fingerprint, info.ftLastWriteTime.dwLowDateTime);
        MixShaderFingerprint(fingerprint, info.nFileSizeHigh);
        MixShaderFingerprint(fingerprint, info.nFileSizeLow);
    }
    return fingerprint;
}

/// @brief 開いたシーンの中で、アセット定義の方が新しくなっている Prefab インスタンスを揃える。
/// @note 伝播経路 (Apply / Prefab 編集の保存 / ディスク監視) は «今開いているシーン» しか 触らないため、閉じていた間に更新された .prefab は古い複製のまま焼き直される — 開くときにここで塞ぐ。作り直しは EntityID を振り直すので、シーンより後に 書かれた .prefab だけに絞り、毎回全部走らせて dirty が常態化するのを避ける。
int ReconcileStalePrefabInstances(scene::Scene& scene,
                                  const std::string& scenePath,
                                  const std::string& projectRoot)
{
    std::uint64_t sceneTime = 0;
    if (!TryGetWriteTime(util::FileSystem::PathFromUtf8(scenePath), sceneTime)) return 0;

    std::vector<std::string> assetPaths;
    for (const auto& gameObject : scene.GameObjects()) {
        const std::string& assetPath = gameObject.prefabAssetPath;
        if (assetPath.empty()) continue;
        if (std::find(assetPaths.begin(), assetPaths.end(), assetPath) != assetPaths.end())
            continue;
        assetPaths.push_back(assetPath);
    }

    int updated = 0;
    for (const std::string& assetPath : assetPaths) {
        const std::string diskPath = ToProjectAssetDiskPath(projectRoot, assetPath);
        std::uint64_t assetTime = 0;
        if (!TryGetWriteTime(util::FileSystem::PathFromUtf8(diskPath), assetTime)) continue;
        if (assetTime <= sceneTime) continue;

        updated += PrefabSerializer::PropagateToInstances(
            scene, assetPath, scene::EntityID{}, projectRoot);
    }
    return updated;
}

/// @brief HLSL の再コンパイル結果を現在開いているプロジェクトへ反映する。
/// @note compile_shaders.ps1 はエンジンソース側へ CSO を出力するが、実行中の renderer は プロジェクト側 Assets/shaders を読むため、ReloadAllShaders() の前に同期が要る。
bool SyncCompiledShadersToProject(const std::filesystem::path& hlslSourceDir,
                                  const std::string& projectRoot)
{
    if (hlslSourceDir.empty() || projectRoot.empty()) return false;

    const std::filesystem::path shaderDestination =
        util::FileSystem::PathFromUtf8(projectRoot) / L"Assets" / L"shaders";
    bool copiedAny = false;
    for (const std::filesystem::path directory : {L"compiled", L"compiled_dx12"}) {
    }
    return copiedAny;
}

/// @brief HLSL の再コンパイル結果を renderer の実際の読込先へ反映する。
/// @note EditorLauncher / sandbox は起動時にカレントディレクトリを exe 隣へ変更し、Renderer は バックエンド別 compiled ディレクトリを相対パスで開くため、ここにも同期が要る。
bool SyncCompiledShadersToRuntimeAssets(const std::filesystem::path& hlslSourceDir)
{
    if (hlslSourceDir.empty()) return false;

    const std::filesystem::path shaderDestination =
        util::FileSystem::GetCurrentDirectory() / L"Assets" / L"shaders";
    if (shaderDestination.empty()) return false;
    bool copiedAny = false;
    for (const std::filesystem::path directory : {L"compiled", L"compiled_dx12"}) {
        const std::filesystem::path src = hlslSourceDir / directory;
        const std::filesystem::path dst = shaderDestination / directory;
        if (!util::FileSystem::Exists(src)) continue;
        if (!util::FileSystem::SamePath(src, dst)
            && !util::FileSystem::CopyDirectoryRecursive(src, dst)) return false;
        copiedAny = true;
    }
    return copiedAny;
}

}

/// @name シーン ダーティ追跡

/// @brief Play 中にシーンファイルを読み書きしようとしたら断る。
/// @note 走っているのはスナップショットを撮った後のシーンで、遷移していれば別のシーンファイルの 中身ですらある。currentScenePath へ書くと、開いていたシーンが丸ごと潰れる。
bool EditorApp::RejectSceneIOWhilePlaying(const char* action)
{
    if (m_playMode.IsInEditor()) return false;

    std::string message = std::string(action) + ": stop play mode first";
    if (!m_ctx.playSceneName.empty())
        message += " (running: " + m_ctx.playSceneName + ")";
    FBZZ_LOG_WARN("%s", message.c_str());
    Toast::Error(message);
    return true;
}

void EditorApp::CaptureCleanScene()
{
    if (!m_ctx.editScene) {
        m_dirtyTracker.Reset();
        m_ctx.sceneDirty = false;
        return;
    }

    CacheSceneWriteTime();
    m_dirtyTracker.CaptureClean(*m_ctx.editScene);
    m_ctx.sceneDirty = false;
    m_dirtyPollTimer = 0.0f;
    UpdateWindowTitle();
}

void EditorApp::RefreshSceneDirtyState(bool force)
{
    if (!m_ctx.editScene) return;
    if (m_ctx.playMode && !m_ctx.playMode->IsInEditor()) return;

    /// @note Evaluate() はシーン全体を serialize して hash 化する重い処理 (~80ms)。MarkDirty() が呼ばれた時点で dirty 確定なので、IsDirty()==true なら Evaluate() を呼ばず即 return する。 Evaluate() はフォールバック専用 (MarkDirty() が漏れた編集を 0.5 秒周期で拾う)。
    if (!force && m_dirtyTracker.IsDirty()) {
        /// @note IsDirty()==true → dirty 確定。sceneDirty との同期だけ行う。
        if (!m_ctx.sceneDirty) {
            m_ctx.sceneDirty = true;
            UpdateWindowTitle();
        }
        return;
    }

    /// @note 両方 clean → 評価不要
    if (!force && !m_ctx.sceneDirty)
        return;

    /// @note フォールバック：MarkDirty() が漏れた場合のみ 0.5 秒周期で hash 評価
    m_dirtyPollTimer += ImGui::GetIO().DeltaTime;
    if (!force && m_dirtyPollTimer < 0.5f)
        return;
    m_dirtyPollTimer = 0.0f;

    const bool wasDirty = m_ctx.sceneDirty;
    m_ctx.sceneDirty = m_dirtyTracker.Evaluate(*m_ctx.editScene);
    if (wasDirty != m_ctx.sceneDirty)
        UpdateWindowTitle();
}

void EditorApp::MarkSceneDirty()
{
    /// @note Dirty tracking belongs to the parked Edit document; temporary Play changes never authorize its save state.
    if (m_ctx.playMode && !m_ctx.playMode->IsInEditor()) return;
    /// @note Prefab 編集モード中の変更は「シーン」ではなく「プレファブアセット」への変更。 ここで sceneDirty を立てると、編集面を抜けて元シーンへ戻った後も未保存扱いが残る。 退避したシーンの dirty 状態は ExitPrefabEditMode がそのまま復元する。
    if (m_ctx.InPrefabEditMode()) {
        if (!m_ctx.prefabEditDirty) {
            m_ctx.prefabEditDirty = true;
            UpdateWindowTitle();
        }
        return;
    }

    m_dirtyTracker.MarkDirty();
    if (!m_ctx.sceneDirty) {
        m_ctx.sceneDirty = true;
        UpdateWindowTitle();
    }
}

/// @name 未保存確認ダイアログ

void EditorApp::ConfirmDiscardUnsaved(const std::string& actionName, std::function<void()> action)
{
    /// @note Prefab 編集モード中はシーンの新規作成 / 差し替えを受け付けない。m_scene の中身を 作り替えると、入っているプレファブと退避してあるシーンを取り違えて壊すため。
    if (m_ctx.InPrefabEditMode()) {
        FBZZ_LOG_WARN("%s: close the prefab edit mode first", actionName.c_str());
        return;
    }

    const bool assetsDirty = AssetDirtyRegistry::HasAny();
    if (!m_ctx.sceneDirty && !assetsDirty) {
        if (action) action();
        return;
    }

    std::string msg;
    if (m_ctx.sceneDirty) msg += "The current scene has unsaved changes.";
    if (assetsDirty) {
        if (!msg.empty()) msg += "\n\n";
        const auto& dirty = AssetDirtyRegistry::GetAll();
        msg += "Unsaved asset(s): " + std::to_string(static_cast<int>(dirty.size()));
        for (const auto& a : dirty)
            msg += "\n  [" + a.typeLabel + "] " + a.displayPath;
    }

    ModalDialog::OpenUnsavedChanges(actionName, msg,
        [this, action]() {
            AssetDirtyRegistry::SaveAll();
            if (m_ctx.sceneDirty && !SaveScene()) return false;
            if (action) action();
            return true;
        },
        [action]() {
            AssetDirtyRegistry::DiscardAll();
            if (action) action();
        });
}

/// @name 新規シーン

void EditorApp::NewScene()
{
    if (!m_ctx.editScene) return;
    if (RejectSceneIOWhilePlaying("New Scene")) return;
    m_ctx.editScene->Clear();
    m_undoStack.Clear();
    ClearEntitySelection(m_ctx);
    m_ctx.graphLayouts.clear();
    m_ctx.editorHiddenGuids.clear();
    /// @note ロックは EntityID で覚えている。新しいシーンは同じ番号を配り直すので、 残したままだと「まだ何も触っていないのに選べないオブジェクト」ができる。
    m_ctx.lockedEntities.clear();
    m_ctx.editorSceneState.Clear();
    RebuildEditorUIFromScene();
    m_settings.lastScenePath.clear();
    m_ctx.currentScenePath.clear();
    m_lastSceneWriteTime = 0;
    CaptureCleanScene();
    FBZZ_LOG_INFO("New scene created");
}

void EditorApp::RequestNewScene()
{
    ConfirmDiscardUnsaved("New Scene", [this]() { NewScene(); });
}

/// @name シーンを開く

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
    if (!m_ctx.editScene) return false;

    std::string path;
    if (!FileDialog::OpenFile(m_hwnd, { SCENE_FILTER }, path)) return false;
    return OpenScenePath(path);
}

bool EditorApp::OpenScenePath(const std::string& path)
{
    if (!m_ctx.editScene || path.empty()) return false;
    if (RejectSceneIOWhilePlaying("Open Scene")) return false;

    if (!SceneIO::Load(*m_ctx.editScene, path)) {
        FBZZ_LOG_ERROR("Open scene failed: %s", path.c_str());
        return false;
    }
    /// @note SceneSerializer はローカル position のみ復元し worldPosition はゼロのまま。 次フレームの TransformEditorPreview まで待つと 1 フレーム間オブジェクトが 原点に表示されるため、ここで即時フラッシュして最初のフレームも正しくする。
    scene::FlushWorldTransforms(*m_ctx.editScene);

    /// @note Undo コマンドは読込前シーンの EntityID と状態を保持するため、 別シーンへ持ち越さず読込成功時点で破棄する。
    m_undoStack.Clear();
    m_settings.lastScenePath = path;
    m_ctx.currentScenePath = path;
    ClearEntitySelection(m_ctx);
    ApplyEditorViewStateFromSceneMeta();
    RebuildEditorUIFromScene();
    CaptureCleanScene();
    CaptureSceneDiskStamp();

    /// @note 閉じている間に更新された .prefab をここで取り込む。 清書後の状態を基準に採ってから走らせるので、揃え直した結果はそのまま 「ディスクとは違う = 保存が要る」として出る。
    if (const int updated =
            ReconcileStalePrefabInstances(*m_ctx.editScene, path, m_ctx.projectRoot);
        updated > 0) {
        scene::FlushWorldTransforms(*m_ctx.editScene);
        ClearEntitySelection(m_ctx);
        RebuildEditorUIFromScene();
        MarkSceneDirty();
        Toast::Info("Prefab updated: " + std::to_string(updated) + " instance(s)");
        FBZZ_LOG_INFO("Opened scene: prefab assets were newer; %d instance(s) updated", updated);
    }

    AddRecentScene(path);
    Toast::Info("Opened: " + util::FileSystem::GetFilename(path));
    FBZZ_LOG_INFO("Opened scene: %s", path.c_str());
    return true;
}

/// @name シーンを保存

bool EditorApp::SaveScene()
{
    if (!m_ctx.editScene) return false;
    if (RejectSceneIOWhilePlaying("Save Scene")) return false;
    /// @note Prefab 編集モード中は m_scene の中身がプレファブなので、シーンとして保存すると 元のシーンファイルをプレファブの内容で上書きしてしまう。Ctrl+S は反射で押されるため、 ここで止めて「プレファブを保存」へ同じキーのまま読み替える。
    if (m_ctx.InPrefabEditMode()) return SavePrefabEdit();
    if (m_settings.lastScenePath.empty()) return SaveSceneAsDialog();

    /// @note 読み込んだ後に他人 (AI・外部エディタ・git) がファイルを書いていたら、黙って 踏まない。Ctrl+S は反射で押される操作なので、ここで止めないと相手の編集が 一度も画面に出ないまま消える。
    if (IsSceneStaleOnDisk()) {
        m_staleSaveConfirmPending = true;
        return false;
    }

    CaptureEditorViewStateToSceneMeta();
    RemoveEditorHiding();
    const bool ok = SceneIO::Save(*m_ctx.editScene, m_settings.lastScenePath);
    RestoreEditorHiding();
    if (!ok) {
        FBZZ_LOG_ERROR("Save scene failed: %s", m_settings.lastScenePath.c_str());
        return false;
    }
    SaveProjectSettingsNow();

    m_ctx.currentScenePath = m_settings.lastScenePath;
    CaptureCleanScene();
    CaptureSceneDiskStamp();
    AddRecentScene(m_settings.lastScenePath);
    Toast::Success("Saved: " + util::FileSystem::GetFilename(m_settings.lastScenePath));
    FBZZ_LOG_INFO("Saved scene: %s", m_settings.lastScenePath.c_str());
    return true;
}

bool EditorApp::SaveSceneAsDialog()
{
    if (!m_ctx.editScene) return false;

    std::string path;
    if (!FileDialog::SaveFile(m_hwnd, { SCENE_FILTER }, path)) return false;
    return SaveScenePath(path);
}

/// @brief 指定パスへ保存する実体。ダイアログ経由と AI (Command Bus の scene.save) が共有する。
/// @note 保存は「書き出す」だけでは終わらず、lastScenePath / currentScenePath の更新、 クリーン状態の再取得、Recent への追加までが 1 つの操作。書き出しだけ真似ると、 保存したのに dirty のままという食い違いが残る。
bool EditorApp::SaveScenePath(const std::string& requestedPath)
{
    if (!m_ctx.editScene || requestedPath.empty()) return false;
    if (RejectSceneIOWhilePlaying("Save Scene")) return false;
    const std::string path = WithFbzzExtension(requestedPath);

    /// @note 別名保存は衝突しない。同じファイルを上書きするときだけ、読み込み後に他人が 書いていないかを見る。モーダルは出さない — この関数は AI (scene.save) も通り、 クリック待ちにするとバスの drain が止まるため、黙って踏むより失敗を返す。
    if (util::FileSystem::SamePathText(path, m_ctx.currentScenePath) && IsSceneStaleOnDisk()) {
        FBZZ_LOG_ERROR("Save refused: %s changed on disk after it was opened. "
                       "Reload it, or save through Ctrl+S to choose which version wins.",
                       util::FileSystem::GetFilename(path).c_str());
        Toast::Error("Save refused: " + util::FileSystem::GetFilename(path) +
                     " changed on disk");
        return false;
    }

    CaptureEditorViewStateToSceneMeta();
    RemoveEditorHiding();
    const bool ok = SceneIO::Save(*m_ctx.editScene, path);
    RestoreEditorHiding();
    if (!ok) {
        FBZZ_LOG_ERROR("Save scene failed: %s", path.c_str());
        return false;
    }
    SaveProjectSettingsNow();

    m_settings.lastScenePath = path;
    m_ctx.currentScenePath = path;
    CaptureCleanScene();
    CaptureSceneDiskStamp();
    AddRecentScene(path);
    Toast::Success("Saved: " + util::FileSystem::GetFilename(path));
    FBZZ_LOG_INFO("Saved scene: %s", path.c_str());
    return true;
}

/// @name 最近開いたシーン

void EditorApp::AddRecentScene(const std::string& path)
{
    if (path.empty()) return;
    auto& recent = m_settings.recentScenes;
    /// @note 同一パス (大小・区切り無視) を除去してから先頭へ差し込む。
    recent.erase(std::remove_if(recent.begin(), recent.end(),
        [&](const std::string& p) { return util::FileSystem::SamePathText(p, path); }),
        recent.end());
    recent.insert(recent.begin(), path);
    if (static_cast<int>(recent.size()) > EditorSettings::kMaxRecentScenes)
        recent.resize(EditorSettings::kMaxRecentScenes);
}

/// @name オートセーブ / クラッシュ復旧

std::string EditorApp::AutoSaveDir() const
{
    if (m_ctx.projectRoot.empty()) return {};
    return m_ctx.projectRoot + "/Library/AutoSave";
}

std::string EditorApp::AutoSavePath() const
{
    const std::string dir = AutoSaveDir();
    if (dir.empty()) return {};
    /// @note 現在シーン名を基にした固定パス。無題シーンは "Untitled" を使う。 path::string() は非 ASCII で例外を投げうるため、UTF-8 変換ユーティリティを使う。
    const std::string base = m_ctx.currentScenePath.empty()
        ? std::string("Untitled")
        : util::FileSystem::PathToUtf8(
              util::FileSystem::PathFromUtf8(m_ctx.currentScenePath).stem());
    return dir + "/" + base + ".autosave.scene";
}

std::string EditorApp::SessionLockPath() const
{
    const std::string dir = AutoSaveDir();
    if (dir.empty()) return {};
    return dir + "/.session_active";
}

void EditorApp::WriteSessionLock()
{
    const std::string dir = AutoSaveDir();
    if (dir.empty()) return;
    util::FileSystem::EnsureDirectory(dir);
    /// @note 生存しているセッションの印。クリーンシャットダウンで消す。残っていればクラッシュとみなす。
    util::FileSystem::WriteText(SessionLockPath(), m_ctx.currentScenePath);
}

void EditorApp::ClearSessionLock()
{
    const std::string lock = SessionLockPath();
    if (!lock.empty() && util::FileSystem::Exists(lock))
        util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(lock));
    /// @note 正常終了時は中間ファイルを全部消す。途中で開き直したシーンの分も残すと次回誤検知する。
    const std::string dir = AutoSaveDir();
    if (dir.empty() || !util::FileSystem::Exists(dir)) return;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(util::FileSystem::PathFromUtf8(dir), ec)) {
        const std::string name = util::FileSystem::PathToUtf8(entry.path().filename());
        if (name.ends_with(".autosave.scene"))
            util::FileSystem::RemoveAll(entry.path());
    }
}

void EditorApp::DetectCrashRecovery()
{
    m_crashRecoveryChecked = false;
    m_pendingRecoveryAutoSave.clear();

    const std::string lock     = SessionLockPath();
    const std::string autosave = AutoSavePath();
    if (lock.empty() || autosave.empty()) return;
    if (!util::FileSystem::Exists(lock) || !util::FileSystem::Exists(autosave)) return;

    /// @note 本体を後から保存していれば、オートセーブの方が古い。古いものを «復旧» として出さない。
    if (!m_ctx.currentScenePath.empty() && util::FileSystem::Exists(m_ctx.currentScenePath)) {
        const auto sceneTime    = util::FileSystem::LastWriteTime(util::FileSystem::PathFromUtf8(m_ctx.currentScenePath));
        const auto autosaveTime = util::FileSystem::LastWriteTime(util::FileSystem::PathFromUtf8(autosave));
        if (autosaveTime <= sceneTime) return;
    }
    m_pendingRecoveryAutoSave = autosave;
    FBZZ_LOG_WARN("CrashRecovery: previous session did not exit cleanly; found %s", autosave.c_str());
}

namespace {

/// @brief 値が動かなくなってから書き出すまでの待ち [s]。スライダーを引いている間に何度も書かない。
constexpr float kProjectSettingsSaveDelay = 1.0f;
/// @brief ToToml() の比較間隔 [s]。直列化は数百行なので毎フレームは回さない。
constexpr float kProjectSettingsPollInterval = 0.25f;

}

bool EditorApp::SaveProjectSettingsNow()
{
    if (m_projectSettingsPath.empty()) return false;
    std::string toml = m_ctx.projectSettings.ToToml();
    if (!util::FileSystem::WriteText(m_projectSettingsPath, toml)) {
        m_ctx.projectSettingsSaveState = EditorContext::SettingsSaveState::Failed;
        m_projectSettingsLastToml   = toml;
        m_projectSettingsFailedToml = std::move(toml);
        FBZZ_LOG_ERROR("ProjectSettings: failed to write %s", m_projectSettingsPath.c_str());
        return false;
    }
    m_projectSettingsLastToml   = toml;
    m_projectSettingsSavedToml  = std::move(toml);
    m_projectSettingsIdleTime   = 0.0f;
    m_ctx.projectSettingsSaveState  = EditorContext::SettingsSaveState::Saved;
    m_ctx.projectSettingsSavedClock = LocalClockNow();
    return true;
}

void EditorApp::TickProjectSettingsAutoSave(float dt)
{
    if (m_projectSettingsPath.empty()) return;

    if (m_ctx.requestProjectSettingsSave) {
        m_ctx.requestProjectSettingsSave = false;
        SaveProjectSettingsNow();
        return;
    }

    m_projectSettingsIdleTime  += dt;
    m_projectSettingsPollTimer += dt;
    if (m_projectSettingsPollTimer < kProjectSettingsPollInterval) return;
    m_projectSettingsPollTimer = 0.0f;

    std::string toml = m_ctx.projectSettings.ToToml();
    if (toml == m_projectSettingsSavedToml) {
        /// @note Undo で保存済みの内容へ戻った場合もここで «保存済み» に戻す。
        if (m_ctx.projectSettingsSaveState == EditorContext::SettingsSaveState::Pending)
            m_ctx.projectSettingsSaveState = EditorContext::SettingsSaveState::Saved;
        m_projectSettingsLastToml = std::move(toml);
        return;
    }
    if (toml != m_projectSettingsLastToml) {
        m_projectSettingsLastToml = std::move(toml);
        m_projectSettingsIdleTime = 0.0f;
    }
    /// @note 書けなかった内容のままなら再試行しない (毎ポーリングで Console を埋めない)。次の変更か Retry を待つ。
    if (m_ctx.projectSettingsSaveState == EditorContext::SettingsSaveState::Failed
        && m_projectSettingsLastToml == m_projectSettingsFailedToml)
        return;
    m_ctx.projectSettingsSaveState = EditorContext::SettingsSaveState::Pending;

    /// @note 掴んでいる最中 (ドラッグ・文字入力) は書かない。離してから待ちを数える。
    if (ImGui::IsAnyItemActive()) { m_projectSettingsIdleTime = 0.0f; return; }
    if (m_projectSettingsIdleTime < kProjectSettingsSaveDelay) return;
    SaveProjectSettingsNow();
}

namespace {

/// @brief オートセーブの何秒前から通知を出すか。
constexpr float kAutoSaveWarningSec = 10.0f;
/// @brief 間隔の下限 [s]。短すぎると保存の引っかかりが操作を邪魔し続ける。
constexpr int kAutoSaveMinIntervalSec = 60;

}

void EditorApp::TickAutoSave(float dt)
{
    const auto stopCounting = [this](bool resetTimer) {
        if (resetTimer) m_autoSaveTimer = 0.0f;
        m_autoSaveWaitingForIdle = false;
        m_ctx.sceneAutoSaveRemainingSec = -1.0f;
    };

    /// @note 変更が無ければ数え直す。Unreal と同じく «前回保存してからの経過» で測る。
    if (!m_ctx.sceneAutoSaveEnabled || !m_ctx.editScene || m_ctx.InPrefabEditMode() || !m_ctx.sceneDirty) {
        stopCounting(true);
        return;
    }
    /// @note Play 中は編集シーンを触らない。タイマーは戻さず止めるだけで、Stop 後に続きから数える。
    if (m_ctx.playMode && !m_ctx.playMode->IsInEditor()) {
        stopCounting(false);
        return;
    }

    m_autoSaveTimer += dt;
    const float interval = static_cast<float>((std::max)(kAutoSaveMinIntervalSec, m_ctx.sceneAutoSaveIntervalSec));
    const float remaining = interval - m_autoSaveTimer;
    m_ctx.sceneAutoSaveRemainingSec = (std::max)(remaining, 0.0f);
    if (remaining > 0.0f) return;

    /// @note ドラッグや文字入力の最中に保存で引っかかると操作が飛ぶ。手を離すまで待つ。
    const ImGuiIO& io = ImGui::GetIO();
    const bool interacting = ImGui::IsAnyItemActive()
        || io.MouseDown[ImGuiMouseButton_Left] || io.MouseDown[ImGuiMouseButton_Right]
        || io.MouseDown[ImGuiMouseButton_Middle] || io.WantTextInput;
    m_autoSaveWaitingForIdle = interacting;
    if (interacting) return;

    AutoSaveNow();
}

bool EditorApp::AutoSaveNow()
{
    m_autoSaveTimer = 0.0f;
    m_autoSaveWaitingForIdle = false;
    if (!m_ctx.editScene) return false;

    const std::string path = AutoSavePath();
    if (path.empty()) return false;
    util::FileSystem::EnsureDirectory(AutoSaveDir());

    /// @note 本保存 (SaveScene) とは別の中間ファイルへ書く。dirty 状態や lastScenePath は変えない。
    CaptureEditorViewStateToSceneMeta();
    RemoveEditorHiding();
    const bool ok = SceneIO::Save(*m_ctx.editScene, path);
    RestoreEditorHiding();
    if (ok) {
        /// @note 開いた後にシーンを切り替えていても、印が今のシーンを指すように書き直す。
        WriteSessionLock();
        Toast::Push(Toast::Level::Info, "Auto-saved (backup in Library/AutoSave)", 2.5f);
        FBZZ_LOG_DEBUG("AutoSave: wrote %s", path.c_str());
    } else {
        Toast::Error("Auto-save failed. See Console.");
        FBZZ_LOG_WARN("AutoSave failed: %s", path.c_str());
    }
    return ok;
}

void EditorApp::DrawAutoSaveNotice()
{
    const float remaining = m_ctx.sceneAutoSaveRemainingSec;
    if (remaining < 0.0f || (remaining > kAutoSaveWarningSec && !m_autoSaveWaitingForIdle)) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    /// @note 右下はトーストが積まれるので、重ならない下辺中央に置く。
    const ImVec2 anchor{ viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                         viewport->WorkPos.y + viewport->WorkSize.y - ImGui::GetFrameHeight() * 2.5f };
    ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, { 0.5f, 1.0f });
    ImGui::SetNextWindowBgAlpha(0.95f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
        | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 12.0f, 8.0f });
    if (ImGui::Begin("##AutoSaveNotice", nullptr, kFlags)) {
        ImGui::AlignTextToFramePadding();
        if (m_autoSaveWaitingForIdle)
            ImGui::TextUnformatted(LOCT("Auto-save will run when you finish the current edit..."));
        else
            ImGui::Text(LOCT("Auto-saving the scene in %d s"), static_cast<int>(std::ceil(remaining)));

        ImGui::SameLine();
        if (ImGui::SmallButton(LOC("Save Now"))) AutoSaveNow();
        ImGui::SameLine();
        /// @note 延期は間隔をまるごとやり直す (Unreal の Cancel と同じ)。
        if (ImGui::SmallButton(LOC("Postpone"))) {
            m_autoSaveTimer = 0.0f;
            m_autoSaveWaitingForIdle = false;
        }

        const float fraction = m_autoSaveWaitingForIdle ? 1.0f : 1.0f - remaining / kAutoSaveWarningSec;
        ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f), { -FLT_MIN, 3.0f }, "");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void EditorApp::ProcessCrashRecovery()
{
    if (m_crashRecoveryChecked) return;
    m_crashRecoveryChecked = true;
    if (m_pendingRecoveryAutoSave.empty()) return;

    const std::string autosavePath = m_pendingRecoveryAutoSave;
    m_pendingRecoveryAutoSave.clear();

    ModalDialog::OpenConfirm(
        "Recover Unsaved Work",
        "The previous session did not exit cleanly.\n"
        "An auto-saved version of the scene was found.\n\n"
        "Restore it? (Choosing No keeps the last saved scene.)",
        [this, autosavePath]() {
            if (!m_ctx.editScene) return;
            if (SceneIO::Load(*m_ctx.editScene, autosavePath)) {
                scene::FlushWorldTransforms(*m_ctx.editScene);
                m_undoStack.Clear();
                ClearEntitySelection(m_ctx);
                ApplyEditorViewStateFromSceneMeta();
                RebuildEditorUIFromScene();
                /// @note 復旧直後は未保存状態にして、ユーザーに保存を促す。
                m_dirtyTracker.MarkDirty();
                m_ctx.sceneDirty = true;
                UpdateWindowTitle();
                Toast::Success("Recovered auto-saved scene");
                FBZZ_LOG_INFO("CrashRecovery: restored %s", autosavePath.c_str());
            } else {
                Toast::Error("Failed to load auto-saved scene");
            }
        });
}

/// @name アプリケーション終了リクエスト

void EditorApp::RequestExit()
{
    ConfirmDiscardUnsaved("Exit", []() { PostQuitMessage(0); });
}

/// @name ホットリロード

void EditorApp::CacheSceneWriteTime()
{
    if (m_settings.lastScenePath.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExA(m_settings.lastScenePath.c_str(), GetFileExInfoStandard, &info))
        m_lastSceneWriteTime = ToWriteTimestamp(info.ftLastWriteTime);
}

void EditorApp::CheckHotReload()
{
    /// @note シーン監視の早期 return より先に、進行中のシェーダーコンパイルを進める。
    TickHlslCompile();

    /// @note 表示タイマーは下の早期 return より前で進める。後ろに置くとシーン未保存・Play 中に «Reload OK» が消えなくなる。
    if (m_ctx.hotReloadDoneTimer > 0.0f) {
        m_ctx.hotReloadDoneTimer -= ImGui::GetIO().DeltaTime;
        if (m_ctx.hotReloadDoneTimer <= 0.0f) {
            m_ctx.hotReloadDoneTimer = 0.0f;
            m_ctx.hotReloadState    = EditorContext::HotReloadState::Idle;
            m_ctx.hotReloadMessage.clear();
            m_ctx.hotReloadProgress = -1.0f;
        }
    }

    if (!m_ctx.hotReloadEnabled) return;
    if (m_settings.lastScenePath.empty() || !m_ctx.editScene) return;
    if (m_ctx.playMode && !m_ctx.playMode->IsInEditor()) return;

    FBZZ_PROFILE_SCOPE("HotReload::Scene");
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExA(m_settings.lastScenePath.c_str(), GetFileExInfoStandard, &info))
        return;

    const std::uint64_t ft = ToWriteTimestamp(info.ftLastWriteTime);
    if (m_lastSceneWriteTime == 0) {
        m_lastSceneWriteTime = ft;
        return;
    }

    if (ft != m_lastSceneWriteTime) {
        m_lastSceneWriteTime = ft;
        if (!SceneIO::Load(*m_ctx.editScene, m_settings.lastScenePath))
            FBZZ_LOG_WARN("Hot reload failed: %s", m_settings.lastScenePath.c_str());
        else {
            ClearEntitySelection(m_ctx);
            ApplyEditorViewStateFromSceneMeta();
            RebuildEditorUIFromScene();
            CaptureCleanScene();
            FBZZ_LOG_INFO("Hot reloaded: %s", m_settings.lastScenePath.c_str());
        }
    }
}

void EditorApp::InitHlslHotReload()
{
    /// @note 旧プロジェクトの結果を捨てる。TaskSystem のジョブは this を保持せず、破棄時にも待機しない。
    m_hlslSourceScan         = {};
    m_hlslDirtyPollTimer     = 0.0f;
    m_hlslProjectOwned      = false;
    m_hlslSourceDir         = util::FileSystem::PathFromUtf8(
        asset::AssetManager::ResolveAssetPath("Assets/Shaders"));
    m_compileShadersScript  = m_hlslSourceDir / L"compile_shaders.ps1";
    m_ctx.hlslSourceDir     = util::FileSystem::PathToUtf8(m_hlslSourceDir);

    if (m_ctx.projectRoot.empty() || m_ctx.hlslSourceDir.empty()) return;

    /// @note 比較の前に絶対化して '..' を畳む。ResolveAssetPath は基準パスを継ぎ足すだけなので、 相対のまま比べると «プロジェクトの中» を «外» と読み違える。
    const auto absolutize = [](const std::filesystem::path& path) {
        if (path.is_absolute())
            return util::FileSystem::PathToUtf8(path.lexically_normal());
        std::error_code error;
        const std::filesystem::path full = std::filesystem::absolute(path, error);
        return util::FileSystem::PathToUtf8((error ? path : full).lexically_normal());
    };
    const std::string shaderDirAbs  = absolutize(m_hlslSourceDir);
    const std::string projectAbs    = absolutize(util::FileSystem::PathFromUtf8(m_ctx.projectRoot));

    if (!util::FileSystem::IsChildPathText(shaderDirAbs, projectAbs)) {
        FBZZ_LOG_INFO("HLSL: hot reload is off - %s belongs to the shared SDK, not this project. "
                      "Copy Assets/Shaders into the project to edit shaders live.",
                      m_ctx.hlslSourceDir.c_str());
        return;
    }
    /// @note 走らせるのは同梱スクリプトなので、無いなら «自前のツリー» とは呼べない。
    if (!util::FileSystem::Exists(m_compileShadersScript)) {
        FBZZ_LOG_WARN("HLSL: hot reload is off - compile_shaders.ps1 not found in %s",
                      m_ctx.hlslSourceDir.c_str());
        return;
    }

    m_hlslProjectOwned      = true;
    m_hlslSourceFingerprint = GetShaderSourceFingerprint(m_hlslSourceDir);
    if (m_hlslSourceFingerprint != 0) {
        m_hlslCompilePending = true;
        m_hlslDebounceTimer  = 0.0f;
        /// @note 起動していない間の削除も stamp では判定できないため、差分スクリプトを一度走らせる。
        FBZZ_LOG_INFO("HLSL: hot reload is on for %s; verifying the tree once",
                      m_ctx.hlslSourceDir.c_str());
    }
}

void EditorApp::CheckHlslDirty()
{
    if (m_packageBuild.IsRunning()) return;
    /// @note 共有 SDK の shader を実行中に書き換えてはならない。判定は InitHlslHotReload が持つ。
    if (!m_hlslProjectOwned) return;
    if (!m_ctx.hotReloadEnabled) return;
    if (m_hlslSourceDir.empty()) return;
    if (m_ctx.playMode && !m_ctx.playMode->IsInEditor()) return;
    if (m_hlslCompiler.GetState() == Compiler::State::Building) return;
    if (m_hlslCompilePending) return;

    FBZZ_PROFILE_SCOPE("HotReload::PollHlsl");
    if (!m_hlslSourceScan.valid()) {
        m_hlslDirtyPollTimer += ImGui::GetIO().DeltaTime;
        if (m_hlslDirtyPollTimer < HOT_RELOAD_TREE_POLL_INTERVAL) return;
        m_hlslDirtyPollTimer = 0.0f;
        /// @note 間隔を空けても同期 I/O のスパイクは残るため、FBX 走査と同じ共有プールへ送る。
        /// @see Docs/design/shader-reload-lifetime.md
        m_hlslSourceScan = TaskSystem::Submit([root = m_hlslSourceDir]() {
            return GetShaderSourceFingerprint(root);
        });
        return;
    }
    /// @note フレーム内では完了済みの結果だけを受け取り、走査の完了を待たない。
    if (m_hlslSourceScan.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    const std::uint64_t fingerprint = m_hlslSourceScan.get();
    if (fingerprint == 0)
        return;

    if (m_hlslSourceFingerprint == 0) {
        m_hlslSourceFingerprint = fingerprint;
        return;
    }
    if (fingerprint == m_hlslSourceFingerprint) return;

    m_hlslSourceFingerprint = fingerprint;
    m_hlslCompilePending = true;
    m_hlslDebounceTimer  = 0.5f;
    FBZZ_LOG_DEBUG("HLSL: change detected in %s; recompiling after 500 ms debounce",
                   m_ctx.hlslSourceDir.c_str());
}

void EditorApp::TickHlslCompile()
{
    if (!m_hlslProjectOwned) return;
    FBZZ_PROFILE_SCOPE("HotReload::HlslCompile");

    if (m_hlslCompilePending) {
        if (m_packageBuild.IsRunning()) return;
        m_hlslDebounceTimer -= 0.016f;
        if (m_hlslDebounceTimer > 0.0f) return;

        m_hlslCompilePending = false;

        /// @note ホットリロードは未 Configure のゲームプロジェクトでも動く必要があるため、 正式ビルド用 CMake ターゲットを経由せず、差分対応 PowerShell を直接非同期実行する。
        if (!util::FileSystem::Exists(m_compileShadersScript)) {
            SetHotReloadState(EditorContext::HotReloadState::Failed, "HLSL: compile_shaders.ps1 not found");
            return;
        }

        wchar_t systemRoot[MAX_PATH]{};
        const DWORD systemRootLength = GetEnvironmentVariableW(L"SystemRoot", systemRoot, MAX_PATH);
        const std::filesystem::path powerShellPath =
            systemRootLength > 0 && systemRootLength < MAX_PATH
                ? std::filesystem::path(systemRoot) / L"System32" / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe"
                : std::filesystem::path(L"powershell.exe");

        Compiler::Config config;
        config.commandLine = L"\"" + powerShellPath.wstring()
            + L"\" -NoProfile -ExecutionPolicy Bypass -File \""
            + m_compileShadersScript.wstring() + L"\"";
        config.workingDirectory = m_hlslSourceDir;

        renderer::ClearShaderCompileDiagnostics();
        if (!m_hlslCompiler.Start(config)) {
            renderer::ReportShaderCompileDiagnostic(
                "HLSL batch", {}, {}, "compile_shaders.ps1 の起動に失敗しました", true);
            SetHotReloadState(EditorContext::HotReloadState::Failed, "HLSL: failed to start compile");
            return;
        }
        m_buildConsole.BeginBuild(BuildRecord::Kind::Hlsl);
        SetHotReloadState(EditorContext::HotReloadState::Compiling, "HLSL: compiling shaders...");
        m_ctx.hotReloadProgress = -1.0f;
    }

    if (m_hlslCompiler.GetState() == Compiler::State::Building) {
        m_hlslCompiler.Tick();
        m_buildConsole.IngestFullLog(m_hlslCompiler.GetLog());
        return;
    }

    if (m_hlslCompiler.GetState() == Compiler::State::Done) {
        m_buildConsole.IngestFullLog(m_hlslCompiler.GetLog());
        m_buildConsole.EndBuild(true, 0);
        if (!SyncCompiledShadersToProject(m_hlslSourceDir, m_ctx.projectRoot)) {
            FBZZ_LOG_WARN("HLSL: compiled CSO sync failed; renderer may still use stale shader binaries");
        }
        if (!SyncCompiledShadersToRuntimeAssets(m_hlslSourceDir)) {
            FBZZ_LOG_WARN("HLSL: runtime CSO sync failed; renderer may still use stale shader binaries");
        }
        if (auto* resources = m_resources;
            resources && !resources->ReloadAllShaders()) {
            SetHotReloadState(EditorContext::HotReloadState::Failed,
                              "HLSL: reload failed; previous shaders retained");
            m_ctx.hotReloadDoneTimer = 8.0f;
            m_hlslCompiler.Reset();
            return;
        }
        SetHotReloadState(EditorContext::HotReloadState::Done, "HLSL: shader reload complete");
        m_ctx.hotReloadDoneTimer = 3.0f;
        m_hlslCompiler.Reset();
        return;
    }

    if (m_hlslCompiler.GetState() == Compiler::State::Failed) {
        m_buildConsole.IngestFullLog(m_hlslCompiler.GetLog());
        m_buildConsole.EndBuild(false, m_hlslCompiler.GetExitCode());
        renderer::ReportShaderCompileDiagnostic(
            "HLSL batch", {}, {}, m_hlslCompiler.GetLog(), true);
        SetHotReloadState(EditorContext::HotReloadState::Failed,
                          "HLSL: compile error (exit=" +
                          std::to_string(m_hlslCompiler.GetExitCode()) + ")");
        m_ctx.hotReloadDoneTimer = 8.0f;
        m_hlslCompiler.Reset();
    }
}

void EditorApp::SetHotReloadState(EditorContext::HotReloadState state, const std::string& msg)
{
    using State = EditorContext::HotReloadState;
    const State prev = m_ctx.hotReloadState;
    const bool  wasBusy = prev == State::Compiling || prev == State::Reloading;
    const double now = ImGui::GetTime();

    m_ctx.hotReloadState   = state;
    m_ctx.hotReloadMessage = msg;
    /// @note 対象は呼び出し元の文言の接頭辞で決まる (HLSL 側はすべて "HLSL:" で始める)。
    if (!msg.empty()) {
        FBZZ_LOG_INFO("[HotReload] %s", msg.c_str());
    }

    if (state == State::Compiling && !wasBusy) {
        m_ctx.hotReloadStartTime  = now;
        m_ctx.hotReloadDoneTimer  = 0.0f;
    }
    if (state != State::Done && state != State::Failed) return;

    m_ctx.hotReloadFinishTime = now;
    /// @note システムサウンドを使う。非同期で鳴り、エディターが最背面でも届き、音量は OS のミキサーに従う。
    if (m_ctx.hotReloadSound)
        MessageBeep(state == State::Done ? MB_ICONASTERISK : MB_ICONHAND);
    char text[256];
    if (state == State::Done) {
        std::snprintf(text, sizeof(text), "%s reloaded (%.1fs)",
                      "Shaders",
                      wasBusy ? now - m_ctx.hotReloadStartTime : 0.0);
        Toast::Push(Toast::Level::Success, text, 2.0f);
    } else {
        Toast::Error(msg.empty() ? std::string("Hot reload failed") : msg);
    }
}

/// @name エディタ専用非表示の一時解除 / 再適用

void EditorApp::RemoveEditorHiding()
{
    if (!m_ctx.activeScene || m_ctx.editorHiddenGuids.empty()) return;
    for (const auto& [guid, wasActive] : m_ctx.editorHiddenGuids) {
        if (auto* go = m_ctx.activeScene->FindByGuid(guid))
            /// @note 非表示前の状態を復元
            go->SetActive(wasActive);
    }
}

void EditorApp::RestoreEditorHiding()
{
    if (!m_ctx.activeScene || m_ctx.editorHiddenGuids.empty()) return;
    for (const auto& [guid, wasActive] : m_ctx.editorHiddenGuids) {
        (void)wasActive;
        if (auto* go = m_ctx.activeScene->FindByGuid(guid))
            go->SetActive(false);
    }
}

void EditorApp::CaptureEditorViewStateToSceneMeta()
{
    EditorSceneState::InstanceIds hidden;
    hidden.reserve(m_ctx.editorHiddenGuids.size());
    for (const auto& [guid, wasActive] : m_ctx.editorHiddenGuids) {
        (void)wasActive;
        hidden.push_back(guid);
    }
    /// @note 保存順が毎回変わると .meta の差分が無意味に膨らむ (unordered_map の走査順は不定)。
    std::sort(hidden.begin(), hidden.end());
    m_ctx.editorSceneState.SetHiddenObjects(std::move(hidden));

    EditorSceneState::InstanceIds locked;
    if (m_ctx.activeScene) {
        locked.reserve(m_ctx.lockedEntities.size());
        for (const scene::EntityID id : m_ctx.lockedEntities)
            if (const auto* go = m_ctx.activeScene->GetGameObject(id))
                locked.push_back(go->instanceId);
        std::sort(locked.begin(), locked.end());
    }
    m_ctx.editorSceneState.SetLockedObjects(std::move(locked));
}

void EditorApp::ApplyEditorViewStateFromSceneMeta()
{
    m_ctx.editorHiddenGuids.clear();
    m_ctx.lockedEntities.clear();
    if (!m_ctx.activeScene) return;

    /// @note .scene には非表示を解除した状態の activeSelf が入っている (Save 前に RemoveEditorHiding が戻す)。つまり「隠す前の値」はいま読んだ値そのもの。
    for (const std::string& guid : m_ctx.editorSceneState.GetHiddenObjects()) {
        auto* go = m_ctx.activeScene->FindByGuid(guid);
        if (!go) continue;
        m_ctx.editorHiddenGuids[guid] = go->activeSelf();
        go->SetActive(false);
    }
    for (const std::string& guid : m_ctx.editorSceneState.GetLockedObjects())
        if (const auto* go = m_ctx.activeScene->FindByGuid(guid))
            m_ctx.lockedEntities.push_back(go->GetID());
}

}
