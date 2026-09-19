/// @file    EditorApp_Scene.cpp
/// @brief   シーンの新規作成・開く・保存・ダーティ追跡・ホットリロード。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// @note シーン I/O とダーティ追跡に特化した分離ファイル。ライフサイクル管理や UI (MenuBar) とは関心が異なる。
#include <Editor/EditorApp.hpp>
#include <Editor/ToolchainLocator.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ModalDialog.hpp>
#include <Editor/Util/FileDialog.hpp>
#include <Editor/Util/Localization.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/ScriptCodeGen.hpp>
#include <Editor/Util/Selection.hpp>
#include <Editor/Util/Toast.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/ShaderCompileDiagnostics.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
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
#include <ctime>
#include <filesystem>
#include <string_view>
#include <vector>

namespace fbzz::editor {

namespace {

const FileFilter SCENE_FILTER{ "Scene", "*.scene" };
constexpr float HOT_RELOAD_TREE_POLL_INTERVAL = 0.5f;
constexpr float SCRIPT_PROGRESS_BUILD_BEGIN = 0.05f;
constexpr float SCRIPT_PROGRESS_BUILD_END   = 0.90f;

/// 拡張子がなければ ".scene" を付与する
std::string WithFbzzExtension(const std::string& path)
{
    if (path.empty() || !util::FileSystem::GetExtension(path).empty()) return path;
    return path + ".scene";
}

/// FILETIME が未初期化のゼロ値かどうかを判定する。
bool IsEmptyFileTime(const FILETIME& ft)
{
    return ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0;
}

/// @brief 指定パス自身の Windows 更新時刻を取得する。
/// @note std::filesystem::file_time_type は実装依存の clock を使うため、既存コードの
///       CompareFileTime と同じ FILETIME に揃えて扱う。
bool TryGetWriteTime(const std::filesystem::path& path, FILETIME& out)
{
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        return false;

    out = info.ftLastWriteTime;
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

/// @brief Scripts DLL の鮮度判定に使う、ユーザー編集ソースの最新更新時刻を返す。
/// @note 新方式では .generated.hpp は生成しないが、旧プロジェクト互換のため残存ファイルは
///       鮮度判定から除外する (生成物のタイムスタンプで誤って再ビルド要と判定しないため)。
FILETIME GetLatestScriptSourceWriteTime(const std::filesystem::path& root)
{
    FILETIME latest{};
    for (const std::filesystem::path& path : util::FileSystem::ListFilesRecursive(root)) {
        const std::wstring extension = path.extension().wstring();
        if (extension != L".hpp" && extension != L".cpp" && extension != L".inl")
            continue;
        if (path.filename().wstring().ends_with(L".generated.hpp"))
            continue;

        FILETIME ft{};
        if (TryGetWriteTime(path, ft) && CompareFileTime(&ft, &latest) > 0)
            latest = ft;
    }
    return latest;
}

/// @brief 開いたシーンの中で、アセット定義の方が新しくなっている Prefab インスタンスを揃える。
/// @note 伝播経路 (Apply / Prefab 編集の保存 / ディスク監視) は «今開いているシーン» しか
///       触らないため、閉じていた間に更新された .prefab は古い複製のまま焼き直される —
///       開くときにここで塞ぐ。作り直しは EntityID を振り直すので、シーンより後に
///       書かれた .prefab だけに絞り、毎回全部走らせて dirty が常態化するのを避ける。
int ReconcileStalePrefabInstances(scene::Scene& scene,
                                  const std::string& scenePath,
                                  const std::string& projectRoot)
{
    FILETIME sceneTime{};
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
        FILETIME assetTime{};
        if (!TryGetWriteTime(util::FileSystem::PathFromUtf8(diskPath), assetTime)) continue;
        if (CompareFileTime(&assetTime, &sceneTime) <= 0) continue;

        updated += PrefabSerializer::PropagateToInstances(
            scene, assetPath, scene::EntityID{}, projectRoot);
    }
    return updated;
}

std::filesystem::path GetScriptScanRoot(const std::filesystem::path& scriptsSourceDir)
{
    if (scriptsSourceDir.filename() == L"Scripts")
        return scriptsSourceDir.parent_path();
    return scriptsSourceDir;
}

/// @brief HLSL の再コンパイル結果を現在開いているプロジェクトへ反映する。
/// @note compile_shaders.ps1 はエンジンソース側へ CSO を出力するが、実行中の renderer は
///       プロジェクト側 Assets/shaders を読むため、ReloadAllShaders() の前に同期が要る。
bool SyncCompiledShadersToProject(const std::filesystem::path& hlslSourceDir,
                                  const std::string& projectRoot)
{
    if (hlslSourceDir.empty() || projectRoot.empty()) return false;

    const std::filesystem::path shaderDestination =
        util::FileSystem::PathFromUtf8(projectRoot) / L"Assets" / L"shaders";
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

/// @brief HLSL の再コンパイル結果を renderer の実際の読込先へ反映する。
/// @note EditorLauncher / sandbox は起動時にカレントディレクトリを exe 隣へ変更し、Renderer は
///       バックエンド別 compiled ディレクトリを相対パスで開くため、ここにも同期が要る。
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

} // namespace

/// シーン ダーティ追跡

/// @brief Play 中にシーンファイルを読み書きしようとしたら断る。
/// @note 走っているのはスナップショットを撮った後のシーンで、遷移していれば別のシーンファイルの
///       中身ですらある。currentScenePath へ書くと、開いていたシーンが丸ごと潰れる。
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

    /// @note Evaluate() はシーン全体を serialize して hash 化する重い処理 (~80ms)。MarkDirty()
    ///       が呼ばれた時点で dirty 確定なので、IsDirty()==true なら Evaluate() を呼ばず即 return する。
    ///       Evaluate() はフォールバック専用 (MarkDirty() が漏れた編集を 0.5 秒周期で拾う)。
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
    /// @note Prefab 編集モード中の変更は「シーン」ではなく「プレファブアセット」への変更。
    ///       ここで sceneDirty を立てると、編集面を抜けて元シーンへ戻った後も未保存扱いが残る。
    ///       退避したシーンの dirty 状態は ExitPrefabEditMode がそのまま復元する。
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

/// 未保存確認ダイアログ

void EditorApp::ConfirmDiscardUnsaved(const std::string& actionName, std::function<void()> action)
{
    /// @note Prefab 編集モード中はシーンの新規作成 / 差し替えを受け付けない。m_scene の中身を
    ///       作り替えると、入っているプレファブと退避してあるシーンを取り違えて壊すため。
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

/// 新規シーン

void EditorApp::NewScene()
{
    if (!m_ctx.editScene) return;
    if (RejectSceneIOWhilePlaying("New Scene")) return;
    m_ctx.editScene->Clear();
    m_undoStack.Clear();
    ClearEntitySelection(m_ctx);
    m_ctx.graphLayouts.clear();
    m_ctx.editorHiddenGuids.clear();
    /// @note ロックは EntityID で覚えている。新しいシーンは同じ番号を配り直すので、
    ///       残したままだと「まだ何も触っていないのに選べないオブジェクト」ができる。
    m_ctx.lockedEntities.clear();
    m_ctx.editorSceneState.Clear();
    RebuildEditorUIFromScene();
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

/// シーンを開く

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
    /// @note SceneSerializer はローカル position のみ復元し worldPosition はゼロのまま。
    ///       次フレームの TransformEditorPreview まで待つと 1 フレーム間オブジェクトが
    ///       原点に表示されるため、ここで即時フラッシュして最初のフレームも正しくする。
    scene::FlushWorldTransforms(*m_ctx.editScene);

    /// @note Undo コマンドは読込前シーンの EntityID と状態を保持するため、
    ///       別シーンへ持ち越さず読込成功時点で破棄する。
    m_undoStack.Clear();
    m_settings.lastScenePath = path;
    m_ctx.currentScenePath = path;
    ClearEntitySelection(m_ctx);
    ApplyEditorViewStateFromSceneMeta();
    RebuildEditorUIFromScene();
    CaptureCleanScene();
    CaptureSceneDiskStamp();

    /// @note 閉じている間に更新された .prefab をここで取り込む。
    ///       清書後の状態を基準に採ってから走らせるので、揃え直した結果はそのまま
    ///       「ディスクとは違う = 保存が要る」として出る。
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

/// シーンを保存

bool EditorApp::SaveScene()
{
    if (!m_ctx.editScene) return false;
    if (RejectSceneIOWhilePlaying("Save Scene")) return false;
    /// @note Prefab 編集モード中は m_scene の中身がプレファブなので、シーンとして保存すると
    ///       元のシーンファイルをプレファブの内容で上書きしてしまう。Ctrl+S は反射で押されるため、
    ///       ここで止めて「プレファブを保存」へ同じキーのまま読み替える。
    if (m_ctx.InPrefabEditMode()) return SavePrefabEdit();
    if (m_settings.lastScenePath.empty()) return SaveSceneAsDialog();

    /// @note 読み込んだ後に他人 (AI・外部エディタ・git) がファイルを書いていたら、黙って
    ///       踏まない。Ctrl+S は反射で押される操作なので、ここで止めないと相手の編集が
    ///       一度も画面に出ないまま消える。
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
/// @note 保存は「書き出す」だけでは終わらず、lastScenePath / currentScenePath の更新、
///       クリーン状態の再取得、Recent への追加までが 1 つの操作。書き出しだけ真似ると、
///       保存したのに dirty のままという食い違いが残る。
bool EditorApp::SaveScenePath(const std::string& requestedPath)
{
    if (!m_ctx.editScene || requestedPath.empty()) return false;
    if (RejectSceneIOWhilePlaying("Save Scene")) return false;
    const std::string path = WithFbzzExtension(requestedPath);

    /// @note 別名保存は衝突しない。同じファイルを上書きするときだけ、読み込み後に他人が
    ///       書いていないかを見る。モーダルは出さない — この関数は AI (scene.save) も通り、
    ///       クリック待ちにするとバスの drain が止まるため、黙って踏むより失敗を返す。
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

/// 最近開いたシーン

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

/// オートセーブ / クラッシュ復旧

std::string EditorApp::AutoSaveDir() const
{
    if (m_ctx.projectRoot.empty()) return {};
    return m_ctx.projectRoot + "/Library/AutoSave";
}

std::string EditorApp::AutoSavePath() const
{
    const std::string dir = AutoSaveDir();
    if (dir.empty()) return {};
    /// @note 現在シーン名を基にした固定パス。無題シーンは "Untitled" を使う。
    ///       path::string() は非 ASCII で例外を投げうるため、UTF-8 変換ユーティリティを使う。
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

std::string LocalClockNow()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char buffer[16];
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
    return buffer;
}

} // namespace

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

} // namespace

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

/// アプリケーション終了リクエスト

void EditorApp::RequestExit()
{
    ConfirmDiscardUnsaved("Exit", []() { PostQuitMessage(0); });
}

/// ホットリロード

void EditorApp::CacheSceneWriteTime()
{
    if (m_settings.lastScenePath.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExA(m_settings.lastScenePath.c_str(), GetFileExInfoStandard, &info))
        m_lastSceneWriteTime = info.ftLastWriteTime;
}

void EditorApp::CheckHotReload()
{
    /// @note Scene ファイル監視の前提が満たされない場合でも、初回 Scripts DLL ビルドと
    ///       進行中コンパイルは完了させる。ここで早期 return すると、Standalone を別途
    ///       ビルドするまで Editor Play にスクリプトが登録されない状態になる。
    TickScriptCompile();
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

    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExA(m_settings.lastScenePath.c_str(), GetFileExInfoStandard, &info))
        return;

    const FILETIME& ft = info.ftLastWriteTime;
    if (m_lastSceneWriteTime.dwLowDateTime == 0 && m_lastSceneWriteTime.dwHighDateTime == 0) {
        m_lastSceneWriteTime = ft;
        return;
    }

    if (CompareFileTime(&ft, &m_lastSceneWriteTime) != 0) {
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

/// スクリプト DLL ホットリロード

namespace {

/// CMakeCache.txt に記録された FBZZ_SDK_ROOT の値を 1 行分そのまま取り出す。
/// 見つからない / cache が読めない場合は空文字列を返す。
std::string ReadCachedSdkRoot(const std::filesystem::path& buildDir)
{
    std::string cacheText;
    if (!util::FileSystem::ReadText(buildDir / L"CMakeCache.txt", cacheText)) return {};

    static constexpr std::string_view kSdkKey = "FBZZ_SDK_ROOT:PATH=";
    const std::size_t keyBegin = cacheText.find(kSdkKey);
    if (keyBegin == std::string::npos) return {};
    const std::size_t valueBegin = keyBegin + kSdkKey.size();
    const std::size_t valueEnd   = cacheText.find_first_of("\r\n", valueBegin);
    return cacheText.substr(valueBegin, valueEnd - valueBegin);
}

/// @brief find_package(FBZZ CONFIG) が解決できる実体が残っている SDK root かを判定する。
/// @note SDK ディレクトリを削除・改名しても cache の値だけは残るため、値の一致だけでは
///       「使える SDK を指しているか」を保証できない。
bool IsUsableSdkRoot(const std::string& sdkRoot)
{
    if (sdkRoot.empty()) return false;
    return util::FileSystem::Exists(
        util::FileSystem::PathFromUtf8(sdkRoot) / L"cmake" / L"FBZZ" / L"FBZZConfig.cmake");
}

/// @brief SDK manifest (fbzz-sdk.toml) の `key = "value"` を 1 件読む。
/// @param sdkRoot SDK root。manifest はこの直下に置かれる。
/// @return manifest が読めない / キーが無いなら空文字列。
/// @note 行頭一致に限定する。コメント中の同名文字列を拾うと、実在しない要求版数で
///       cache を捨ててしまうため。
std::string ReadSdkManifestValue(const std::string& sdkRoot, std::string_view key)
{
    if (sdkRoot.empty()) return {};
    std::string manifest;
    if (!util::FileSystem::ReadText(
            util::FileSystem::PathFromUtf8(sdkRoot) / L"fbzz-sdk.toml", manifest))
        return {};

    for (std::size_t keyBegin = manifest.find(key);
         keyBegin != std::string::npos;
         keyBegin = manifest.find(key, keyBegin + 1)) {
        if (keyBegin != 0 && manifest[keyBegin - 1] != '\n') continue;
        const std::size_t quoteBegin = manifest.find('"', keyBegin + key.size());
        if (quoteBegin == std::string::npos) return {};
        const std::size_t quoteEnd = manifest.find('"', quoteBegin + 1);
        if (quoteEnd == std::string::npos) return {};
        return manifest.substr(quoteBegin + 1, quoteEnd - quoteBegin - 1);
    }
    return {};
}

/// @brief build tree へ固定された MSVC の版数を読む。
/// @return 検出結果が無いなら空文字列。
/// @note CMake はコンパイラ ID を CMakeFiles/<cmake版数>/CMakeCXXCompiler.cmake へ書いた後、
///       再 configure しても検出をやり直さない。VS 更新後もここだけが旧版を指し続ける。
std::string ReadCachedCompilerVersion(const std::filesystem::path& buildDir)
{
    static constexpr std::string_view kKey = "set(CMAKE_CXX_COMPILER_VERSION \"";
    for (const std::filesystem::path& dir :
         util::FileSystem::ListDirectories(buildDir / L"CMakeFiles")) {
        std::string text;
        if (!util::FileSystem::ReadText(dir / L"CMakeCXXCompiler.cmake", text)) continue;
        const std::size_t keyBegin = text.find(kKey);
        if (keyBegin == std::string::npos) continue;
        const std::size_t valueBegin = keyBegin + kKey.size();
        const std::size_t valueEnd   = text.find('"', valueBegin);
        if (valueEnd == std::string::npos) continue;
        return text.substr(valueBegin, valueEnd - valueBegin);
    }
    return {};
}

/// @brief cache が固定したコンパイラ版数が SDK の ABI 契約と一致するかを判定する。
/// @note 判定材料 (manifest / 検出結果) のどちらかを欠く場合は true を返す。
///       確証なく cache を捨てると、SDK と無関係な失敗まで毎回フル configure になる。
bool CMakeCacheToolchainMatchesSdk(const std::filesystem::path& buildDir, const std::string& sdkRoot)
{
    const std::string required = ReadSdkManifestValue(sdkRoot, "compiler_version");
    if (required.empty()) return true;
    const std::string cached = ReadCachedCompilerVersion(buildDir);
    if (cached.empty()) return true;
    return cached == required;
}

/// @brief 既存 cache をそのまま使えるか (SDK root と ABI 契約の両方) を判定する。
/// @return false なら configure をやり直す必要がある。
bool CMakeCacheMatchesSdk(const std::filesystem::path& buildDir, const std::string& sdkRoot)
{
    if (sdkRoot.empty()) return true;
    const std::string cachedRoot = ReadCachedSdkRoot(buildDir);
    if (cachedRoot.empty()) return false;

    /// @note 行末までを含めた値の完全一致で比較する (区切り文字・大小文字は正規化)。
    ///       部分一致だと ".../SDK/0.1.0" が ".../SDK/0.1.0-dev.dirty" へ前方一致し、
    ///       消えた SDK を指す cache を「一致」と誤判定して reconfigure が永久にスキップされる。
    if (!util::FileSystem::SamePathText(cachedRoot, sdkRoot)) return false;

    /// @note 値が一致していても SDK 実体が無ければ configure は必ず失敗するので stale 扱いにする。
    if (!IsUsableSdkRoot(cachedRoot)) return false;

    /// @note SDK は版数完全一致の ABI 契約なので、VS 更新で cache 側だけ旧版のままになると
    ///       find_package(FBZZ) が FATAL_ERROR で落ちる。SDK root が一致していても stale。
    return CMakeCacheToolchainMatchesSdk(buildDir, sdkRoot);
}

/// @brief コピーされたテンプレートの CMakeCache は生成元を指すため、configure 前に破棄する。
/// @note CMake は CMAKE_HOME_DIRECTORY が現在のプロジェクトと異なるキャッシュを安全上再利用せず、
///       -D で SDK パスだけ上書きしても exit=1 になる。
bool RemoveForeignCMakeCache(const std::string& projectRoot)
{
    const std::filesystem::path projectPath =
        util::FileSystem::MakeAbsolute(util::FileSystem::PathFromUtf8(projectRoot));
    const std::filesystem::path buildDir = projectPath / L"Build" / L"VS";
    std::string cacheText;
    if (!util::FileSystem::ReadText(buildDir / L"CMakeCache.txt", cacheText)) return true;

    static constexpr std::string_view kHomeKey = "CMAKE_HOME_DIRECTORY:INTERNAL=";
    const std::size_t valueBegin = cacheText.find(kHomeKey);
    if (valueBegin == std::string::npos) return true;
    const std::size_t pathBegin = valueBegin + kHomeKey.size();
    const std::size_t pathEnd = cacheText.find_first_of("\r\n", pathBegin);
    const std::string cachedSource = cacheText.substr(pathBegin, pathEnd - pathBegin);
    if (util::FileSystem::SamePath(util::FileSystem::PathFromUtf8(cachedSource), projectPath)) return true;

    /// @note 対象を project/Build/VS に固定し、プロジェクト外のパスを削除しない。
    if (!util::FileSystem::IsChildPathText(
            util::FileSystem::PathToUtf8(buildDir),
            util::FileSystem::PathToUtf8(projectPath))) {
        FBZZ_LOG_ERROR("ScriptDll: foreign CMake cache path is outside project: %s",
                       util::FileSystem::PathToUtf8(buildDir).c_str());
        return false;
    }
    if (!util::FileSystem::RemoveAll(buildDir)) {
        FBZZ_LOG_ERROR("ScriptDll: stale CMake cache cleanup failed: %s",
                       util::FileSystem::PathToUtf8(buildDir).c_str());
        return false;
    }
    FBZZ_LOG_INFO("ScriptDll: removed copied CMake cache: %s", cachedSource.c_str());
    return true;
}

/// @brief ツールチェーンが入れ替わった build tree から configure 状態だけを捨てる。
/// @return 削除に失敗したら false。一致していれば何もせず true。
/// @note 中間物 (`<Target>.dir` / x64) は残す。捨てるのは CMakeCache.txt と
///       コンパイラ ID を抱えた `CMakeFiles/<cmake版数>/` だけ。
/// @warning configure のやり直しだけでは回復しない。CMake はコンパイラ ID を
///          再検出しないため、これを消さない限り旧版数で ABI 検査へ入り続ける。
bool RemoveStaleToolchainCache(const std::string& projectRoot, const std::string& sdkRoot)
{
    const std::filesystem::path projectPath =
        util::FileSystem::MakeAbsolute(util::FileSystem::PathFromUtf8(projectRoot));
    const std::filesystem::path buildDir = projectPath / L"Build" / L"VS";
    if (CMakeCacheToolchainMatchesSdk(buildDir, sdkRoot)) return true;

    FBZZ_LOG_INFO("ScriptDll: toolchain changed (cache=%s, SDK=%s); dropping cached compiler id",
                  ReadCachedCompilerVersion(buildDir).c_str(),
                  ReadSdkManifestValue(sdkRoot, "compiler_version").c_str());

    bool ok = util::FileSystem::RemoveAll(buildDir / L"CMakeCache.txt");
    for (const std::filesystem::path& dir :
         util::FileSystem::ListDirectories(buildDir / L"CMakeFiles")) {
        if (!util::FileSystem::Exists(dir / L"CMakeCXXCompiler.cmake")) continue;
        ok = util::FileSystem::RemoveAll(dir) && ok;
    }
    if (!ok) {
        FBZZ_LOG_ERROR("ScriptDll: stale toolchain cache cleanup failed: %s",
                       util::FileSystem::PathToUtf8(buildDir).c_str());
    }
    return ok;
}

/// @brief cache が古ければ configure してから Scripts DLL をロードする。
/// @note GameHub プロジェクトは初回開封時や共有 SDK 移行直後に cache が古い場合がある。
///       stale な Scripts.dll を先に読むと偽の ABI 詳細を出すため、ロード前に完了させる。
bool TryCMakeConfigure(const std::string& projectRoot, const std::string& engineRoot)
{
    if (!RemoveForeignCMakeCache(projectRoot)) return false;
    if (!RemoveStaleToolchainCache(projectRoot, engineRoot)) return false;

    wchar_t cmakeBuf[MAX_PATH]{};
    if (!SearchPathW(nullptr, L"cmake.exe", nullptr, MAX_PATH, cmakeBuf, nullptr)) {
        FBZZ_LOG_WARN("ScriptDll: cmake.exe が PATH に見つかりません。cmake --preset fbzz-vs を手動実行してください。");
        return false;
    }

    if (!engineRoot.empty()) {
        const std::wstring sdkRootW = util::StringUtils::ToWide(engineRoot);
        if (!sdkRootW.empty()) SetEnvironmentVariableW(L"FBZZ_SDK_ROOT", sdkRootW.c_str());
    }

    const std::wstring projRootW = util::StringUtils::ToWide(projectRoot);

    /// @note --preset の cacheVariables に "$env{FBZZ_SDK_ROOT}" があっても、既に
    ///       CMakeCache.txt が存在する場合はキャッシュ値が優先される。-D で明示的に
    ///       上書きすることで既存キャッシュがあっても正しいパスが使われる。
    const std::wstring engineRootW = util::StringUtils::ToWide(engineRoot);
    std::wstring cmd = std::wstring(L"\"") + cmakeBuf + L"\" --preset fbzz-vs";
    if (!engineRootW.empty())
        cmd += L" -DFBZZ_SDK_ROOT:PATH=\"" + engineRootW + L"\"";
    /// @note cmake --preset の cacheVariables は -D で上書きできる。CMakePresets.json に
    ///       "Debug;Release" しか無いプロジェクトでも Development 構成を生成させるため
    ///       明示的に上書きする。スペースを含むフラグは引数全体を "" で括る。
    cmd += L" -DCMAKE_CONFIGURATION_TYPES=Debug;Release;Development";
    cmd += L" \"-DCMAKE_CXX_FLAGS_DEVELOPMENT=/Zi /O2 /Ob2 /FS\"";
    cmd += L" \"-DCMAKE_EXE_LINKER_FLAGS_DEVELOPMENT=/DEBUG:FULL /INCREMENTAL:NO\"";
    cmd += L" \"-DCMAKE_SHARED_LINKER_FLAGS_DEVELOPMENT=/DEBUG:FULL /INCREMENTAL:NO\"";
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(
        nullptr, mutableCmd.data(),
        nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, projRootW.empty() ? nullptr : projRootW.c_str(),
        &si, &pi);

    if (!ok) {
        FBZZ_LOG_WARN("ScriptDll: cmake configure の起動に失敗しました。");
        return false;
    }

    /// @note 初回または SDK 切替時だけ同期的に待つ。configure と Script build を並行起動すると
    ///       generate.stamp と CMakeCache が競合し、正常な SDK でも ABI/compile error を繰り返す。
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exitCode != 0) {
        FBZZ_LOG_ERROR("ScriptDll: SDK cache configure failed (exit=%lu)", exitCode);
        return false;
    }
    FBZZ_LOG_DEBUG("ScriptDll: SDK cache configured");
    return true;
}

/// @brief Scripts ビルドの失敗ログが「コンパイルエラー」ではなく「SDK を解決できない configure 失敗」かを判定する。
/// @note cmake --build は generate.stamp が古いと暗黙に configure をやり直すため、壊れた
///       CMakeCache は compile error の顔で失敗し続ける。cache の作り直しでしか復帰しない。
bool LooksLikeSdkConfigureFailure(const std::string& log)
{
    return log.find("FBZZConfig.cmake")            != std::string::npos
        || log.find("FBZZ_SDK_ROOT")               != std::string::npos
        || log.find("CMake Configure step failed") != std::string::npos;
}

} // namespace

void EditorApp::InitScriptDll()
{
    /// @note HLSL の監視先は C++ ツールチェーンと無関係に決まる。必要なのは PowerShell と
    ///       シェーダーツリー同梱の compile_shaders.ps1 だけで、cmake は 1 度も通らない。
    ///       ツールチェーン分岐の中で決めていた頃は、build.config を持たない (あるいは
    ///       configure に失敗した) プロジェクトでシェーダーのリロードまで道連れに死んでいた。
    if (m_ctx.hotReloadEnabled) InitHlslHotReload();

    ToolchainLocator::Result toolchain = ToolchainLocator::Locate(
        util::FileSystem::PathFromUtf8(m_ctx.projectBuildRoot));
    bool sdkCacheRefreshed = false;

    /// @note build.config があっても、共有 SDK 移行前の cache なら configure をやり直す。
    if (!m_ctx.projectRoot.empty()) {
        const std::filesystem::path presetsJson = util::FileSystem::PathFromUtf8(m_ctx.projectRoot) / L"CMakePresets.json";
        const bool sdkCacheMatches = toolchain.found && CMakeCacheMatchesSdk(toolchain.buildDir, m_ctx.engineRoot);
        if (util::FileSystem::Exists(presetsJson) && !sdkCacheMatches) {
            if (!TryCMakeConfigure(m_ctx.projectRoot, m_ctx.engineRoot)) {
                SetHotReloadState(EditorContext::HotReloadState::Failed, "Scripts: SDK configure failed");
                return;
            }
            sdkCacheRefreshed = true;
            toolchain = ToolchainLocator::Locate(util::FileSystem::PathFromUtf8(m_ctx.projectBuildRoot));
        }
    }

    if (toolchain.found) {
        /// @note ABI を統一するため、Editor と同じビルド構成の Scripts DLL を使う。
        ///       FBZZ_CMAKE_CONFIG は cmake が `$<CONFIG>` を焼き込んだ文字列 ("Debug"/"Release"/"Development")。
        ///       `#ifdef NDEBUG` では Development (NDEBUG なし・最適化 ON) を Debug と誤判定するため使わない。
        static constexpr std::string_view kConfig = FBZZ_CMAKE_CONFIG;
        if constexpr (kConfig == "Development") {
            m_scriptDllPath = !toolchain.scriptsDllDevelopment.empty()
                ? toolchain.scriptsDllDevelopment
                /// @note SandboxScripts.dll は exe 整理後 `Binaries/<Config>/Sandbox/` に出力される。
            : toolchain.buildDir.parent_path() / L"Binaries" / L"Development" / L"Sandbox" / L"SandboxScripts.dll";
        } else if constexpr (kConfig == "Release") {
            m_scriptDllPath = !toolchain.scriptsDllRelease.empty()
                ? toolchain.scriptsDllRelease
                : toolchain.exeRelease.parent_path() / L"SandboxScripts.dll";
        } else {
            m_scriptDllPath = !toolchain.scriptsDllDebug.empty()
                ? toolchain.scriptsDllDebug
                : toolchain.exeDebug.parent_path() / L"SandboxScripts.dll";
        }

        const std::filesystem::path engineRoot = toolchain.buildDir.parent_path().parent_path();
        m_scriptsSourceDir     = engineRoot / L"Assets" / L"Scripts";
        m_ctx.scriptsSourceDir = util::FileSystem::PathToUtf8(m_scriptsSourceDir);

        /// @note 新方式: Reflect() はヘッダ内の FBZZ_REFLECT が生成するため、起動時の
        ///       .generated.hpp 一括生成 (旧 FHT) は不要になった。

        if (!m_ctx.projectTargetName.empty()) {
            const std::filesystem::path projRoot = util::FileSystem::PathFromUtf8(m_ctx.projectRoot);
            const std::wstring targetW = util::StringUtils::ToWide(m_ctx.projectTargetName);
            m_ctx.scriptsDllCppPath    = util::FileSystem::PathToUtf8(projRoot / L"Src" / (targetW + L"ScriptsDll.cpp"));
            m_ctx.scriptsStaticCppPath = util::FileSystem::PathToUtf8(projRoot / L"Src" / L"GameMain.cpp");
        } else {
            m_ctx.scriptsDllCppPath    = util::FileSystem::PathToUtf8(
                engineRoot / L"Projects" / L"Sandbox" / L"src" / L"SandboxScriptsDll.cpp");
            m_ctx.scriptsStaticCppPath = util::FileSystem::PathToUtf8(
                engineRoot / L"Projects" / L"Sandbox" / L"src" / L"SandboxScripts.cpp");
        }
    } else {
        /// @note ToolchainLocator の失敗は cmake/build.config が未生成の場合に起こる。hot reload は
        ///       無効になるが、DLL が既にビルド済みならスクリプトは Play 中に動く。build.config が
        ///       部分的に読めていれば scriptsDllDebug 使用、そうでなければ build_root を 2 段検索する。
        FBZZ_LOG_WARN("ScriptDll: ToolchainLocator failed - script hot reload is disabled");

        {
            static constexpr std::string_view kConfig = FBZZ_CMAKE_CONFIG;
            const std::filesystem::path& preferred =
                (kConfig == "Development") ? toolchain.scriptsDllDevelopment :
                (kConfig == "Release")     ? toolchain.scriptsDllRelease :
                                             toolchain.scriptsDllDebug;
            if (!preferred.empty()) m_scriptDllPath = preferred;
        }
        if (m_scriptDllPath.empty() && !m_ctx.projectBuildRoot.empty()) {
            const std::wstring dllName = util::StringUtils::ToWide(
                m_ctx.projectTargetName.empty()
                    ? "SandboxScripts.dll"
                    : (m_ctx.projectTargetName + "Scripts.dll"));
            const std::filesystem::path buildRoot = util::FileSystem::PathFromUtf8(m_ctx.projectBuildRoot);
            for (const auto& sub1 : util::FileSystem::ListDirectories(buildRoot)) {
                if (util::FileSystem::Exists(sub1 / dllName)) {
                    m_scriptDllPath = sub1 / dllName;
                    break;
                }
                for (const auto& sub2 : util::FileSystem::ListDirectories(sub1)) {
                    if (util::FileSystem::Exists(sub2 / dllName)) {
                        m_scriptDllPath = sub2 / dllName;
                        break;
                    }
                }
                if (!m_scriptDllPath.empty()) break;
            }
        }

        /// @note GameHub プロジェクトなら toolchain なしでも scriptsDllCppPath を設定できる
        if (!m_ctx.projectTargetName.empty()) {
            const std::filesystem::path projRoot = util::FileSystem::PathFromUtf8(m_ctx.projectRoot);
            const std::wstring targetW = util::StringUtils::ToWide(m_ctx.projectTargetName);
            m_ctx.scriptsDllCppPath    = util::FileSystem::PathToUtf8(projRoot / L"Src" / (targetW + L"ScriptsDll.cpp"));
            m_ctx.scriptsStaticCppPath = util::FileSystem::PathToUtf8(projRoot / L"Src" / L"GameMain.cpp");
        }
    }

    /// @note DLL ロード (toolchain の成否に関わらず実行)
    m_ctx.scriptsDllPath = util::FileSystem::PathToUtf8(m_scriptDllPath);
    if (!m_ctx.scriptsSourceDir.empty()) {
        /// @note Scripts/ の実ファイルを登録の正とし、手動追加・削除を Unity 風に自動反映する。
        ScriptCodeGen::SyncScriptRegistry(m_ctx.scriptsSourceDir,
                                          m_ctx.scriptsDllCppPath,
                                          m_ctx.scriptsStaticCppPath);
    }
    if (!sdkCacheRefreshed && !m_scriptDllPath.empty() && util::FileSystem::Exists(m_scriptDllPath)) {
        if (m_scriptDll.Load(m_scriptDllPath)) {
            FBZZ_LOG_INFO("ScriptDll: loaded %ls (%d types)",
                m_scriptDllPath.wstring().c_str(),
                static_cast<int>(scene::ScriptFactory::RegisteredTypeNames().size()));
        } else {
            FBZZ_LOG_INFO("ScriptDll: stale DLL was rejected; rebuild scheduled");
            if (toolchain.found) {
                /// @note Engine 側の Scene / Component レイアウトだけが変わった場合、スクリプト
                ///       ソースのタイムスタンプ比較では古い DLL を検出できない。ABI 不一致で
                ///       ロードを拒否した時点で依存ターゲット込みの再ビルドを予約する。
                m_scriptCompilePending = true;
                m_scriptInitialBuild   = true;
                m_scriptDebounceTimer  = 0.0f;
                m_ctx.scriptReloadBusy = true;
                SetHotReloadState(EditorContext::HotReloadState::Compiling,
                                  "Scripts: ABI mismatch, rebuilding...");
                m_ctx.hotReloadProgress = 0.0f;
                FBZZ_LOG_INFO("ScriptDll: load failed; scheduling dependency rebuild");
            }
        }
    } else if (toolchain.found) {
        /// @note cmake configure 直後は DLL がまだ存在しない。初回ビルドを TickScriptCompile()
        ///       に委譲することで非同期ビルドを起動する。
        FBZZ_LOG_INFO("ScriptDll: DLL が未ビルドです。初回ビルドを開始します...");
        m_scriptCompilePending = true;
        /// @note 初回ビルドは依存ターゲットも含めてビルドする
        m_scriptInitialBuild   = true;
        m_scriptDebounceTimer  = 0.0f;
        m_ctx.scriptReloadBusy = true;
        SetHotReloadState(EditorContext::HotReloadState::Compiling, "Scripts: initial build...");
        m_ctx.hotReloadProgress = 0.0f;
    } else {
        FBZZ_LOG_WARN("ScriptDll: %ls not found; generate it with cmake --build",
                      m_scriptDllPath.wstring().c_str());
    }

    if (!toolchain.found) return;

    /// @name 起動時 Assets 即時同期
    /// @note 最終更新時刻をキャッシュする (初回は変更なしと判定)
    if (!m_scriptsSourceDir.empty()) {
        const std::filesystem::path scriptScanRoot = GetScriptScanRoot(m_scriptsSourceDir);
        m_lastScriptWriteTime = GetLatestScriptSourceWriteTime(scriptScanRoot);

        /// @note 既存 DLL をロードできても、ソースより古ければ SceneManagerScript 等の修正が
        ///       Editor Play へ反映されない。Standalone ビルドへ依存せず起動時に自動更新する。
        FILETIME dllWriteTime{};
        const FILETIME sourceWriteTime = GetLatestScriptSourceWriteTime(scriptScanRoot);
        if (!m_scriptCompilePending
            && !IsEmptyFileTime(sourceWriteTime)
            && (!TryGetWriteTime(m_scriptDllPath, dllWriteTime)
                || CompareFileTime(&sourceWriteTime, &dllWriteTime) > 0)) {
            m_scriptCompilePending = true;
            m_scriptDebounceTimer = 0.0f;
            m_ctx.scriptReloadBusy = true;
            SetHotReloadState(EditorContext::HotReloadState::Compiling,
                              "Scripts: source is newer than DLL...");
            m_ctx.hotReloadProgress = 0.0f;
            FBZZ_LOG_INFO("ScriptDll: source is newer than DLL; scheduling rebuild");
        }
    }
}

/// @brief 監視するシェーダーツリーを決め、それがプロジェクトの持ち物かを判定する。
/// @note AssetManager::ResolveAssetPath は «プロジェクト → 共有 SDK» の順に探すため、自前の
///       Assets/Shaders を持たないプロジェクトでは SDK 側の実体が返る。そこへ書き戻すと他の
///       プロジェクトの CSO まで書き換わるため、実行時のパス所在で書き込み可否を判定する
///       (旧ビルド構成切替は «自前シェーダーを持つ SDK プロジェクト» を巻き添えにしていた)。
void EditorApp::InitHlslHotReload()
{
    m_hlslProjectOwned      = false;
    m_hlslSourceDir         = util::FileSystem::PathFromUtf8(
        asset::AssetManager::ResolveAssetPath("Assets/Shaders"));
    m_compileShadersScript  = m_hlslSourceDir / L"compile_shaders.ps1";
    m_ctx.hlslSourceDir     = util::FileSystem::PathToUtf8(m_hlslSourceDir);

    if (m_ctx.projectRoot.empty() || m_ctx.hlslSourceDir.empty()) return;

    /// @note 比較の前に絶対化して '..' を畳む。ResolveAssetPath は基準パスを継ぎ足すだけなので、
    ///       相対のまま比べると «プロジェクトの中» を «外» と読み違える。
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

void EditorApp::CheckScriptDirtyAndRebuild()
{
    if (!m_ctx.hotReloadEnabled) return;
    if (m_scriptsSourceDir.empty()) return;
    if (m_ctx.playMode && !m_ctx.playMode->IsInEditor()) return;
    if (m_scriptCompiler.GetState() == Compiler::State::Building) return;
    if (m_scriptCompilePending) return;

    /// @note Assets/ 配下のスクリプト候補確認はディスク I/O と path 確保を伴うため、BeginFrame
    ///       毎に走らせると CPU ボトルネック化する。体感遅延として許容できる 0.5 秒間隔に制限し、
    ///       ファイル保存後の再ビルドは既存のデバウンスでまとめる。
    m_scriptDirtyPollTimer += ImGui::GetIO().DeltaTime;
    if (m_scriptDirtyPollTimer < HOT_RELOAD_TREE_POLL_INTERVAL) return;
    m_scriptDirtyPollTimer = 0.0f;

    /// @note Assets/ 配下のスクリプト候補の最終変更時刻を確認する。
    FBZZ_PROFILE_SCOPE("HotReload::ScanScripts");
    const std::filesystem::path scriptScanRoot = GetScriptScanRoot(m_scriptsSourceDir);
    const FILETIME ft = GetLatestScriptSourceWriteTime(scriptScanRoot);
    if (IsEmptyFileTime(ft))
        return;

    if (IsEmptyFileTime(m_lastScriptWriteTime)) {
        m_lastScriptWriteTime = ft;
        return;
    }
    if (CompareFileTime(&ft, &m_lastScriptWriteTime) == 0) return;

    ScriptCodeGen::SyncScriptRegistry(m_ctx.scriptsSourceDir,
                                      m_ctx.scriptsDllCppPath,
                                      m_ctx.scriptsStaticCppPath);
    m_lastScriptWriteTime  = GetLatestScriptSourceWriteTime(scriptScanRoot);
    m_scriptCompilePending = true;
    /// @note 500ms デバウンス
    m_scriptDebounceTimer  = 0.5f;
    m_ctx.scriptReloadBusy = true;
    SetHotReloadState(EditorContext::HotReloadState::Compiling, "Scripts: change detected...");
    m_ctx.hotReloadProgress = 0.0f;
    FBZZ_LOG_DEBUG("ScriptDll: change detected in %s; rebuilding after 500 ms debounce",
                   m_ctx.scriptsSourceDir.c_str());
    /// @note 新方式: Reflect() はヘッダ内生成のため、ビルド前の .generated.hpp 一括生成は不要。
}

void EditorApp::TickScriptCompile()
{
    const bool inEditor = !m_ctx.playMode || m_ctx.playMode->IsInEditor();
    const bool explicitRebuild = m_ctx.requestScriptReload && inEditor &&
        m_scriptCompiler.GetState() != Compiler::State::Building && !m_scriptCompilePending;
    if (explicitRebuild) {
        m_ctx.requestScriptReload = false;
        m_scriptCompilePending = true;
        m_scriptDebounceTimer = 0.0f;
        m_ctx.scriptReloadBusy = true;
        SetHotReloadState(EditorContext::HotReloadState::Compiling, "Scripts: rebuild requested...");
        m_ctx.hotReloadProgress = 0.0f;
        FBZZ_LOG_INFO("ScriptDll: explicit rebuild requested");
    }

    /// @note デバウンスタイマーを消費してからビルド開始する
    if (m_scriptCompilePending) {
        if (!inEditor) return;
        m_ctx.scriptReloadBusy = true;
        m_scriptDebounceTimer -= 0.016f;
        if (m_scriptDebounceTimer > 0.0f) return;

        m_scriptCompilePending = false;

        if (m_scriptDllPath.empty()) {
            m_ctx.scriptReloadBusy = false;
            SetHotReloadState(EditorContext::HotReloadState::Failed, "Scripts: DLL target is not configured");
            FBZZ_LOG_ERROR("ScriptDll: cannot rebuild without a configured Scripts DLL path");
            return;
        }

        ToolchainLocator::Result toolchain = ToolchainLocator::Locate(
            util::FileSystem::PathFromUtf8(m_ctx.projectBuildRoot));
        if (!toolchain.found) {
            m_ctx.scriptReloadBusy = false;
            SetHotReloadState(EditorContext::HotReloadState::Failed, "Script: toolchain not resolved");
            return;
        }

        Compiler::Config config;
        config.cmakeExe      = toolchain.cmakeExe;
        config.buildDir      = toolchain.buildDir;
        config.exePath       = m_scriptDllPath;
        /// @note DLL ファイル名 (例: SandboxScripts.dll) から cmake ターゲット名を導出する。
        ///       ハードコードすると GameHub プロジェクト (MyGameScripts 等) で壊れる。
        config.target        = util::FileSystem::PathToUtf8(m_scriptDllPath.stem());
        /// @note cmake --config に Editor と同じ構成を渡して ABI を統一する。Development は
        ///       NDEBUG なし・最適化 ON の第三の構成であり、`#ifdef NDEBUG` では正しく判定できない。
        config.configuration = FBZZ_CMAKE_CONFIG;
        config.sdkRoot       = m_ctx.engineRoot;
        /// @note 初回ビルド (DLL 未存在) はエンジン libs がまだないため依存ターゲットも含めてビルドする。
        ///       ホットリロード時はエディタがエンジン DLL をロック中のためスキップする。
        config.skipDeps      = explicitRebuild || !m_scriptInitialBuild;
        config.rebuild       = explicitRebuild;
        m_scriptInitialBuild = false;
        m_scriptReloadShown  = false;

        if (!m_ctx.scriptsSourceDir.empty()) {
            ScriptCodeGen::SyncScriptRegistry(m_ctx.scriptsSourceDir,
                m_ctx.scriptsDllCppPath, m_ctx.scriptsStaticCppPath);
            m_lastScriptWriteTime = GetLatestScriptSourceWriteTime(GetScriptScanRoot(m_scriptsSourceDir));
        }

        if (!m_scriptCompiler.Start(config)) {
            m_ctx.scriptReloadBusy = false;
            SetHotReloadState(EditorContext::HotReloadState::Failed, "Script: failed to start compile");
            return;
        }
        /// @note ビルドコンソールへ新規ビルドを通知する (診断・ライブログ・履歴の起点)。
        m_buildConsole.BeginBuild(BuildRecord::Kind::Script);
        FBZZ_LOG_DEBUG("ScriptDll: starting compile: target=%s cfg=%s",
            config.target.c_str(), config.configuration.c_str());
        SetHotReloadState(EditorContext::HotReloadState::Compiling, "Scripts: compiling...");
        m_ctx.hotReloadProgress = SCRIPT_PROGRESS_BUILD_BEGIN;
    }

    if (m_scriptCompiler.GetState() == Compiler::State::Building) {
        m_ctx.scriptReloadBusy = true;
        m_scriptCompiler.Tick();
        /// @note コンパイラの stdout 差分を取り込み、現在コンパイル中ファイルと診断を更新する。
        m_buildConsole.IngestFullLog(m_scriptCompiler.GetLog());
        /// @note 総コンパイル単位を取得できないため、残り幅に比例して増える段階進捗を使う。
        ///       90% を上限にすることで、ビルド完了前にリロード段階へ到達したように見せない。
        const float deltaTime = ImGui::GetIO().DeltaTime;
        m_ctx.hotReloadProgress +=
            (SCRIPT_PROGRESS_BUILD_END - m_ctx.hotReloadProgress) * std::min(deltaTime * 0.7f, 1.0f);
        m_ctx.hotReloadProgress = std::min(m_ctx.hotReloadProgress, SCRIPT_PROGRESS_BUILD_END - 0.01f);
        return;
    }

    if (m_scriptCompiler.GetState() == Compiler::State::Done) {
        if (!inEditor) return;
        m_ctx.scriptReloadBusy = true;
        if (!m_scriptReloadShown) {
            /// @note コンパイル成功を確定する (この後の DLL リロードは別工程として扱う)。
            m_buildConsole.IngestFullLog(m_scriptCompiler.GetLog());
            m_buildConsole.EndBuild(true, 0);
            SetHotReloadState(EditorContext::HotReloadState::Reloading, "Scripts: reloading...");
            m_ctx.hotReloadProgress = SCRIPT_PROGRESS_BUILD_END;
            /// @note ここで 1 フレーム返し、Reloading を描かせてから同期の Reload() で止まる。
            m_scriptReloadShown = true;
            return;
        }
        m_scriptReloadShown = false;

        /// @note リロード前に選択中エンティティの instanceId を保存する。Reload() はシーンを
        ///       move で置き換えるため EntityID が変わるが、instanceId (UUID v4) はシリアライズ
        ///       経由で保持されるため復元に使える。
        std::vector<std::string> savedGuids;
        if (m_ctx.activeScene) {
            for (auto id : m_ctx.selectedEntities) {
                if (auto* go = m_ctx.activeScene->GetGameObject(id))
                    savedGuids.push_back(go->instanceId);
            }
        }

        const bool reloaded = m_ctx.activeScene
            ? m_scriptDll.Reload(*m_ctx.activeScene, m_scriptDllPath)
            : (!m_scriptDll.IsLoaded() && m_scriptDll.Load(m_scriptDllPath));
        if (reloaded) {
            std::vector<scene::EntityID> restored;
            for (const auto& guid : savedGuids) {
                if (auto* go = scene::GameObject::FindByGuid(guid))
                    restored.push_back(go->GetID());
            }
            SelectEntities(m_ctx, std::move(restored), SelectionReveal::Skip);
            SetHotReloadState(EditorContext::HotReloadState::Done, "Scripts: hot reload complete");
            m_ctx.hotReloadProgress = 1.0f;
            m_ctx.hotReloadDoneTimer = 3.0f;
        } else {
            ClearEntitySelection(m_ctx);
            SetHotReloadState(EditorContext::HotReloadState::Failed,
                m_scriptDll.IsLoaded() ? "Scripts: reload failed; previous DLL loaded, see Console"
                                      : "Scripts: unavailable; fix errors and press Rebuild");
            /// @note 消さずに残す。ビルド記録は成功なので、消えると StatusBar が «Scripts OK» に戻って失敗を隠す。
            m_ctx.hotReloadDoneTimer = 0.0f;
        }
        m_scriptCompiler.Reset();
        m_ctx.scriptReloadBusy = false;
        return;
    }

    if (m_scriptCompiler.GetState() == Compiler::State::Failed) {
        /// @note 失敗ログを取り込み、診断を確定する。Build Output パネルへ件数と file:line が並ぶ。
        const std::string buildLog = m_scriptCompiler.GetLog();
        m_buildConsole.IngestFullLog(buildLog);
        m_buildConsole.EndBuild(false, m_scriptCompiler.GetExitCode());
        const int errs = m_buildConsole.Latest() ? m_buildConsole.Latest()->errorCount : 0;
        const std::string msg = errs > 0
            ? "Scripts: " + std::to_string(errs) + " error(s)"
            : "Scripts: compile error (exit=" + std::to_string(m_scriptCompiler.GetExitCode()) + ")";
        FBZZ_LOG_ERROR("ScriptDll: %s\n%s", msg.c_str(), buildLog.c_str());
        m_scriptCompiler.Reset();

        /// @note 自己修復: SDK を解決できない CMakeCache は、放置すると起動のたびに同じ失敗を出す。
        ///       configure をやり直して cache を作り直し、その場で 1 度だけ再ビルドを予約する。
        ///       1 セッション 1 回に制限するのは、SDK が本当に無い場合に configure 失敗 →
        ///       再ビルド → 同じ失敗の無限ループへ落ちないようにするため。
        if (!m_scriptSdkRecoveryDone
            && !m_ctx.projectRoot.empty()
            && LooksLikeSdkConfigureFailure(buildLog)) {
            m_scriptSdkRecoveryDone = true;
            FBZZ_LOG_WARN("ScriptDll: SDK を解決できない CMakeCache を検出しました。configure をやり直します");
            if (TryCMakeConfigure(m_ctx.projectRoot, m_ctx.engineRoot)) {
                m_scriptCompilePending = true;
                m_scriptDebounceTimer  = 0.0f;
                /// @note 依存ターゲットも含めた初回ビルド扱いにする (cache 再生成後は中間物が失われている)。
                m_scriptInitialBuild   = true;
                m_ctx.scriptReloadBusy = true;
                SetHotReloadState(EditorContext::HotReloadState::Compiling,
                                  "Scripts: SDK cache を再構成しました。再ビルド中...");
                m_ctx.hotReloadProgress = 0.0f;
                return;
            }
            FBZZ_LOG_ERROR("ScriptDll: SDK cache の再構成に失敗しました。FBZZ Hub で SDK を選び直してください");
        }

        SetHotReloadState(EditorContext::HotReloadState::Failed, msg);
        m_ctx.hotReloadDoneTimer = 8.0f;
        m_ctx.scriptReloadBusy = false;
    }
}

/// HLSL ホットリロード

void EditorApp::CheckHlslDirty()
{
    /// @note 共有 SDK の shader を実行中に書き換えてはならない。判定は InitHlslHotReload が持つ。
    if (!m_hlslProjectOwned) return;
    if (!m_ctx.hotReloadEnabled) return;
    if (m_hlslSourceDir.empty()) return;
    if (m_ctx.playMode && !m_ctx.playMode->IsInEditor()) return;
    if (m_hlslCompiler.GetState() == Compiler::State::Building) return;
    if (m_hlslCompilePending) return;

    /// @note HLSL 監視も Scripts と同じくツリー全体を列挙する。シェーダー保存検知は即時性より
    ///       フレーム安定性を優先し、EditorBegin の常時 5ms 負荷を避ける。
    m_hlslDirtyPollTimer += ImGui::GetIO().DeltaTime;
    if (m_hlslDirtyPollTimer < HOT_RELOAD_TREE_POLL_INTERVAL) return;
    m_hlslDirtyPollTimer = 0.0f;

    FBZZ_PROFILE_SCOPE("HotReload::ScanHlsl");
    const std::uint64_t fingerprint = GetShaderSourceFingerprint(m_hlslSourceDir);
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

    if (m_hlslCompilePending) {
        m_hlslDebounceTimer -= 0.016f;
        if (m_hlslDebounceTimer > 0.0f) return;

        m_hlslCompilePending = false;

        /// @note ホットリロードは未 Configure のゲームプロジェクトでも動く必要があるため、
        ///       正式ビルド用 CMake ターゲットを経由せず、差分対応 PowerShell を直接非同期実行する。
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
        if (auto* resources = renderer::ResourceManager::Active();
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
        m_ctx.hotReloadTarget = msg.rfind("HLSL", 0) == 0 ? EditorContext::HotReloadTarget::Shaders
                                                          : EditorContext::HotReloadTarget::Scripts;
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
    const bool shaders = m_ctx.hotReloadTarget == EditorContext::HotReloadTarget::Shaders;
    if (!shaders)
        m_ctx.scriptReloadResult = state == State::Done ? EditorContext::ScriptReloadResult::Ok
                                                        : EditorContext::ScriptReloadResult::Failed;
    char text[256];
    if (state == State::Done) {
        std::snprintf(text, sizeof(text), "%s reloaded (%.1fs)",
                      shaders ? "Shaders" : "Scripts",
                      wasBusy ? now - m_ctx.hotReloadStartTime : 0.0);
        Toast::Push(Toast::Level::Success, text, 2.0f);
    } else {
        Toast::Error(msg.empty() ? std::string("Hot reload failed") : msg);
    }
}

/// エディタ専用非表示の一時解除 / 再適用

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

    /// @note .scene には非表示を解除した状態の activeSelf が入っている (Save 前に
    ///       RemoveEditorHiding が戻す)。つまり「隠す前の値」はいま読んだ値そのもの。
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

} // namespace fbzz::editor
