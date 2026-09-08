/// @file    EditorApp_Scene.cpp
/// @brief   シーンの新規作成・開く・保存・ダーティ追跡・ホットリロード。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// WHY: EditorApp.cpp が肥大化しないよう、シーン I/O とダーティ追跡を分離した。
/// これらはいずれも「シーンファイル」という単一の概念を中心とした処理群であり、
/// ライフサイクル管理 (Init/Shutdown/BeginFrame) や UI (MenuBar) とは関心が異なる。
#include <Editor/EditorApp.hpp>
#include <Editor/ToolchainLocator.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ModalDialog.hpp>
#include <Editor/Util/FileDialog.hpp>
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
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace fbzz::editor {

namespace {

const FileFilter SCENE_FILTER{ "Scene", "*.scene" };
constexpr float HOT_RELOAD_TREE_POLL_INTERVAL = 0.5f;
constexpr float SCRIPT_PROGRESS_BUILD_BEGIN = 0.05f;
constexpr float SCRIPT_PROGRESS_BUILD_END   = 0.90f;

// 拡張子がなければ ".scene" を付与する
std::string WithFbzzExtension(const std::string& path)
{
    if (path.empty() || !util::FileSystem::GetExtension(path).empty()) return path;
    return path + ".scene";
}

// FILETIME が未初期化のゼロ値かどうかを判定する。
bool IsEmptyFileTime(const FILETIME& ft)
{
    return ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0;
}

// 指定パス自身の Windows 更新時刻を取得する。
// WHY: std::filesystem::file_time_type は実装依存の clock を使うため、既存コードの
//      CompareFileTime と同じ FILETIME に揃えて扱う。
bool TryGetWriteTime(const std::filesystem::path& path, FILETIME& out)
{
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        return false;

    out = info.ftLastWriteTime;
    return true;
}

// HLSLツリー指紋へ64bit値をFNV-1aで混ぜる。
// WHY: 更新時刻の最大値だけでは、ファイル削除や時刻を維持した改名を検知できない。
void MixShaderFingerprint(std::uint64_t& fingerprint, std::uint64_t value)
{
    for (int byteIndex = 0; byteIndex < 8; ++byteIndex) {
        fingerprint ^= static_cast<std::uint8_t>(value >> (byteIndex * 8));
        fingerprint *= 1099511628211ull;
    }
}

// HLSL/HLSLIと統合スクリプトのパス・更新時刻・サイズから決定的なツリー指紋を作る。
// WHAT: 追加・更新・削除・改名のすべてを一つの比較で検知し、CSO/metaは監視対象から除外する。
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

// Scripts DLLの鮮度判定に使う、ユーザー編集ソースの最新更新時刻を返す。
// WHY: 新方式では .generated.hpp は生成しないが、旧プロジェクト互換のため残存ファイルは
//      鮮度判定から除外する (生成物のタイムスタンプで誤って再ビルド要と判定しないため)。
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

// 開いたシーンの中で、アセット定義の方が新しくなっている Prefab インスタンスを揃える。
//
// WHY «開くとき» に要るか: 伝播の経路 (Apply / Prefab 編集の保存 / ディスク監視) は
//     どれも «今開いているシーン» しか触らない。一方シーンファイルはインスタンスを
//     展開済みの完全な状態で持つので、更新のときに閉じていたシーンは古い複製を
//     抱えたまま固定され、次に開いて保存すると古さがそのまま焼き直される。
//     «プレファブなのに片方のステージだけ直らない» はここで塞がないと消えない。
// WHY 更新時刻で絞るか: 作り直しは EntityID を振り直すので、開くたびに全部走らせると
//     «開いただけで dirty» が常態化する。シーンより後に書かれた .prefab だけで足りる。
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

// HLSL の再コンパイル結果を現在開いているプロジェクトへ反映する。
// WHY: compile_shaders.ps1 はエンジンソース側へDX11/DX12別のCSOを出力する。
//      しかし実行中の renderer はプロジェクト側 Assets/shaders を読むため、
//      ReloadAllShaders() の前に CSO を同期しないと古いバイナリを再ロードしてしまう。
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

// HLSL の再コンパイル結果を renderer の実際の読込先へ反映する。
// WHY: EditorLauncher / sandbox は起動時にカレントディレクトリを exe 隣へ変更する。
//      Rendererはバックエンド別compiledディレクトリを相対パスで開くため、
//      ReloadAllShaders() の前に exe 隣の Assets にも CSO を同期する必要がある。
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

// =============================================================================
// シーン ダーティ追跡
// =============================================================================

// Play 中にシーンファイルを読み書きしようとしたら断る。
// WHY: 走っているのはスナップショットを撮ったあとのシーンで、遷移していれば
//      別のシーンファイルの中身ですらある。それを currentScenePath へ書けば、
//      開いていたシーンが最後に走っていたシーンで丸ごと潰れる。
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

    // WHY: Evaluate() はシーン全体を serialize して hash 化する重い処理（~80ms）。
    // MarkDirty() が呼ばれた時点で dirty 確定なので、IsDirty()==true の場合は
    // Evaluate() を呼ばず fast path で即 return する。
    // Evaluate() はフォールバック専用：MarkDirty() が漏れた編集を 0.5 秒周期で拾う場合にのみ実行する。
    if (!force && m_dirtyTracker.IsDirty()) {
        // IsDirty()==true → dirty 確定。sceneDirty との同期だけ行う。
        if (!m_ctx.sceneDirty) {
            m_ctx.sceneDirty = true;
            UpdateWindowTitle();
        }
        return;
    }

    // 両方 clean → 評価不要
    if (!force && !m_ctx.sceneDirty)
        return;

    // フォールバック：MarkDirty() が漏れた場合のみ 0.5 秒周期で hash 評価
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
    // Prefab 編集モード中の変更は「シーン」ではなく「プレファブアセット」への変更。
    // WHY: ここで sceneDirty を立ててしまうと、編集モードを抜けて元のシーンへ戻った
    //      あとも未保存扱いが残り、触っていないシーンの保存を促すことになる。
    //      退避したシーンの dirty 状態は ExitPrefabEditMode がそのまま復元する。
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

// =============================================================================
// 未保存確認ダイアログ
// =============================================================================

void EditorApp::ConfirmDiscardUnsaved(const std::string& actionName, std::function<void()> action)
{
    // Prefab 編集モード中はシーンの新規作成 / 差し替えを受け付けない。
    // WHY: これらは m_scene の中身を作り替えるが、そこに入っているのはプレファブで、
    //      退避してあるシーンを取り違えて壊す。先に編集面から出てもらう。
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

// =============================================================================
// 新規シーン
// =============================================================================

void EditorApp::NewScene()
{
    if (!m_ctx.editScene) return;
    if (RejectSceneIOWhilePlaying("New Scene")) return;
    m_ctx.editScene->Clear();
    m_undoStack.Clear();
    ClearEntitySelection(m_ctx);
    m_ctx.graphLayouts.clear();
    m_ctx.editorHiddenGuids.clear();
    // ロックは EntityID で覚えている。新しいシーンは同じ番号を配り直すので、
    // 残したままだと「まだ何も触っていないのに選べないオブジェクト」ができる。
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
    // WHY: SceneSerializer はローカル position のみ復元し worldPosition はゼロのまま。
    //      次フレームの TransformEditorPreview まで待つと 1 フレーム間オブジェクトが
    //      原点に表示されるため、ここで即時フラッシュして最初のフレームも正しくする。
    scene::FlushWorldTransforms(*m_ctx.editScene);

    // Undo コマンドは読込前シーンの EntityID と状態を保持するため、
    // 別シーンへ持ち越さず読込成功時点で破棄する。
    m_undoStack.Clear();
    m_settings.lastScenePath = path;
    m_ctx.currentScenePath = path;
    ClearEntitySelection(m_ctx);
    ApplyEditorViewStateFromSceneMeta();
    RebuildEditorUIFromScene();
    CaptureCleanScene();
    CaptureSceneDiskStamp();

    // 閉じている間に更新された .prefab をここで取り込む。
    // 清書後の状態を基準に採ってから走らせるので、揃え直した結果はそのまま
    // 「ディスクとは違う = 保存が要る」として出る。
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

// =============================================================================
// シーンを保存
// =============================================================================

bool EditorApp::SaveScene()
{
    if (!m_ctx.editScene) return false;
    if (RejectSceneIOWhilePlaying("Save Scene")) return false;
    // Prefab 編集モード中は m_scene の中身がプレファブなので、シーンとして保存すると
    // 元のシーンファイルをプレファブの内容で上書きしてしまう。
    // WHY: Ctrl+S は反射的に押される操作なので、ここで止めないと確実に事故になる。
    //      同じキーで「プレファブを保存」へ読み替える。
    if (m_ctx.InPrefabEditMode()) return SavePrefabEdit();
    if (m_settings.lastScenePath.empty()) return SaveSceneAsDialog();

    // 読み込んだ後に他人 (AI・外部エディタ・git) がファイルを書いていたら、黙って
    // 踏まない。Ctrl+S は反射で押される操作なので、ここで止めないと相手の編集が
    // 一度も画面に出ないまま消える。
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
    m_ctx.projectSettings.Save(m_projectSettingsPath);

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

// 指定パスへ保存する実体。ダイアログ経由と AI (Command Bus の scene.save) が共有する。
// WHY: 保存は「書き出す」だけでは終わらず、lastScenePath / currentScenePath の更新、
//      クリーン状態の再取得、Recent への追加までが 1 つの操作。AI 側で書き出しだけ
//      真似ると、保存したのに dirty のままという食い違いが残る。
bool EditorApp::SaveScenePath(const std::string& requestedPath)
{
    if (!m_ctx.editScene || requestedPath.empty()) return false;
    if (RejectSceneIOWhilePlaying("Save Scene")) return false;
    const std::string path = WithFbzzExtension(requestedPath);

    // 別名保存は衝突しない。同じファイルを上書きするときだけ、読み込み後に他人が
    // 書いていないかを見る。
    // モーダルは出さない。この関数は AI (scene.save) も通り、クリック待ちにするとバスの
    // drain が止まる。黙って踏むよりは失敗を返す。
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
    m_ctx.projectSettings.Save(m_projectSettingsPath);

    m_settings.lastScenePath = path;
    m_ctx.currentScenePath = path;
    CaptureCleanScene();
    CaptureSceneDiskStamp();
    AddRecentScene(path);
    Toast::Success("Saved: " + util::FileSystem::GetFilename(path));
    FBZZ_LOG_INFO("Saved scene: %s", path.c_str());
    return true;
}

// =============================================================================
// 最近開いたシーン
// =============================================================================

void EditorApp::AddRecentScene(const std::string& path)
{
    if (path.empty()) return;
    auto& recent = m_settings.recentScenes;
    // 同一パス (大小・区切り無視) を除去してから先頭へ差し込む。
    recent.erase(std::remove_if(recent.begin(), recent.end(),
        [&](const std::string& p) { return util::FileSystem::SamePathText(p, path); }),
        recent.end());
    recent.insert(recent.begin(), path);
    if (static_cast<int>(recent.size()) > EditorSettings::kMaxRecentScenes)
        recent.resize(EditorSettings::kMaxRecentScenes);
}

// =============================================================================
// オートセーブ / クラッシュ復旧
// =============================================================================

std::string EditorApp::AutoSaveDir() const
{
    if (m_ctx.projectRoot.empty()) return {};
    return m_ctx.projectRoot + "/Library/AutoSave";
}

std::string EditorApp::AutoSavePath() const
{
    const std::string dir = AutoSaveDir();
    if (dir.empty()) return {};
    // 現在シーン名を基にした固定パス。無題シーンは "Untitled" を使う。
    // WHY: path::string() は非 ASCII で例外を投げうるため、UTF-8 変換ユーティリティを使う。
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
    // 生存しているセッションの印。クリーンシャットダウンで消す。残っていればクラッシュとみなす。
    util::FileSystem::WriteText(SessionLockPath(), m_ctx.currentScenePath);
}

void EditorApp::ClearSessionLock()
{
    const std::string lock = SessionLockPath();
    if (!lock.empty() && util::FileSystem::Exists(lock))
        util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(lock));
    // 正常終了時はオートセーブの中間ファイルも掃除する (残すと次回誤検知する)。
    const std::string autos = AutoSavePath();
    if (!autos.empty() && util::FileSystem::Exists(autos))
        util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(autos));
}

void EditorApp::TickAutoSave(float dt)
{
    if (!m_settings.autoSaveEnabled) return;
    if (!m_ctx.editScene) return;
    // Play 中は編集シーンを触らない。ダーティでなければ何もしない。
    if (m_ctx.playMode && !m_ctx.playMode->IsInEditor()) return;
    if (!m_ctx.sceneDirty) { m_autoSaveTimer = 0.0f; return; }

    m_autoSaveTimer += dt;
    const float interval = static_cast<float>(std::max(30, m_settings.autoSaveIntervalSec));
    if (m_autoSaveTimer < interval) return;
    m_autoSaveTimer = 0.0f;

    const std::string path = AutoSavePath();
    if (path.empty()) return;
    util::FileSystem::EnsureDirectory(AutoSaveDir());

    // 本保存 (SaveScene) とは別の中間ファイルへ書き出す。dirty 状態や lastScenePath は変えない。
    CaptureEditorViewStateToSceneMeta();
    RemoveEditorHiding();
    const bool ok = SceneIO::Save(*m_ctx.editScene, path);
    RestoreEditorHiding();
    if (ok) {
        Toast::Info("Auto-saved");
        FBZZ_LOG_DEBUG("AutoSave: wrote %s", path.c_str());
    } else {
        FBZZ_LOG_WARN("AutoSave failed: %s", path.c_str());
    }
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
                // 復旧直後は未保存状態にして、ユーザーに保存を促す。
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

    // WHY: Sceneファイル監視の前提が満たされない場合でも、初回Scripts DLLビルドと
    //      進行中コンパイルは完了させる。ここで早期returnすると、Standaloneを別途
    //      ビルドするまでEditor Playにスクリプトが登録されない状態になるため。
    TickScriptCompile();
    TickHlslCompile();

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

    // Done / Failed の表示タイマーを進める
    if (m_ctx.hotReloadDoneTimer > 0.0f) {
        // WHY: dt が取れないのでフレームごとの固定値 (≈16ms) で近似する
        m_ctx.hotReloadDoneTimer -= 0.016f;
        if (m_ctx.hotReloadDoneTimer <= 0.0f) {
            m_ctx.hotReloadDoneTimer = 0.0f;
            m_ctx.hotReloadState    = EditorContext::HotReloadState::Idle;
            m_ctx.hotReloadMessage.clear();
            m_ctx.hotReloadProgress = -1.0f;
        }
    }
}

// =============================================================================
// スクリプト DLL ホットリロード
// =============================================================================

namespace {

// CMakeCache.txt に記録された FBZZ_SDK_ROOT の値を 1 行分そのまま取り出す。
// 見つからない / cache が読めない場合は空文字列を返す。
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

// find_package(FBZZ CONFIG) が解決できる実体が残っている SDK root かを判定する。
// WHY: SDK ディレクトリを削除・改名しても cache の値だけは残るため、値の一致だけでは
//      「使える SDK を指しているか」を保証できない。
bool IsUsableSdkRoot(const std::string& sdkRoot)
{
    if (sdkRoot.empty()) return false;
    return util::FileSystem::Exists(
        util::FileSystem::PathFromUtf8(sdkRoot) / L"cmake" / L"FBZZ" / L"FBZZConfig.cmake");
}

bool CMakeCacheUsesSdkRoot(const std::filesystem::path& buildDir, const std::string& sdkRoot)
{
    if (sdkRoot.empty()) return true;
    const std::string cachedRoot = ReadCachedSdkRoot(buildDir);
    if (cachedRoot.empty()) return false;

    // 行末までを含めた値の完全一致で比較する (区切り文字・大小文字は正規化)。
    // 部分一致だと ".../SDK/0.1.0" が ".../SDK/0.1.0-dev.dirty" へ前方一致し、
    // 消えた SDK を指す cache を「一致」と誤判定して reconfigure が永久にスキップされる。
    if (!util::FileSystem::SamePathText(cachedRoot, sdkRoot)) return false;

    // 値が一致していても SDK 実体が無ければ configure は必ず失敗するので stale 扱いにする。
    return IsUsableSdkRoot(cachedRoot);
}

// コピーされたテンプレートのCMakeCacheは生成元を指すため、configure前に破棄する。
// WHY: CMakeはCMAKE_HOME_DIRECTORYが現在のプロジェクトと異なるキャッシュを安全上再利用せず、
//      -DでSDKパスだけ上書きしてもexit=1になる。
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

    // 対象を project/Build/VS に固定し、プロジェクト外のパスを削除しない。
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

// WHY: GameHub プロジェクトは初回開封時、または共有 SDK 移行直後に cache が古い場合がある。
//      stale な Scripts.dll を先に読むと偽の ABI 詳細を出すため、ロード前に configure を完了させる。
bool TryCMakeConfigure(const std::string& projectRoot, const std::string& engineRoot)
{
    if (!RemoveForeignCMakeCache(projectRoot)) return false;

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

    // WHY: --preset の cacheVariables に "$env{FBZZ_SDK_ROOT}" があっても
    //      既に CMakeCache.txt が存在する場合はキャッシュ値が優先される。
    //      -D で明示的に上書きすることで既存キャッシュがあっても正しいパスが使われる。
    const std::wstring engineRootW = util::StringUtils::ToWide(engineRoot);
    std::wstring cmd = std::wstring(L"\"") + cmakeBuf + L"\" --preset fbzz-vs";
    if (!engineRootW.empty())
        cmd += L" -DFBZZ_SDK_ROOT:PATH=\"" + engineRootW + L"\"";
    // WHY: cmake --preset の cacheVariables は -D で上書きできる (cmake docs: "preset variables can be overridden using normal -D options")。
    //      CMakePresets.json に "Debug;Release" しか書かれていないプロジェクトでも
    //      エディタが Development 構成で VS プロジェクトを生成させるために明示的に上書きする。
    //      スペースを含むフラグは引数全体を "" で括ることで CreateProcessW に正しく渡せる。
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

    // 初回または SDK 切替時だけ同期的に待つ。
    // WHY: configure と Script build を並行起動すると generate.stamp と CMakeCache が競合し、
    //      正常な SDK でも ABI エラーと compile error を繰り返すため。
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

// Scripts ビルドの失敗ログが「コンパイルエラー」ではなく「SDK を解決できない configure 失敗」かを判定する。
// WHY: cmake --build は generate.stamp が古いと暗黙に configure をやり直すため、
//      壊れた CMakeCache は compile error の顔をして毎回同じ内容で失敗し続ける。
//      この形の失敗だけは再ビルドではなく cache の作り直しでしか復帰できない。
bool LooksLikeSdkConfigureFailure(const std::string& log)
{
    return log.find("FBZZConfig.cmake")            != std::string::npos
        || log.find("FBZZ_SDK_ROOT")               != std::string::npos
        || log.find("CMake Configure step failed") != std::string::npos;
}

} // namespace

void EditorApp::InitScriptDll()
{
    if (!m_ctx.hotReloadEnabled) return;

    // HLSL の監視先は C++ ツールチェーンと無関係に決まる。必要なのは PowerShell と
    // シェーダーツリー同梱の compile_shaders.ps1 だけで、cmake は 1 度も通らない。
    // ツールチェーン分岐の中で決めていた頃は、build.config を持たない (あるいは
    // configure に失敗した) プロジェクトでシェーダーのリロードまで道連れに死んでいた。
    InitHlslHotReload();

    ToolchainLocator::Result toolchain = ToolchainLocator::Locate(
        util::FileSystem::PathFromUtf8(m_ctx.projectBuildRoot));
    bool sdkCacheRefreshed = false;

    // build.config があっても、共有 SDK 移行前の cache なら configure をやり直す。
    if (!m_ctx.projectRoot.empty()) {
        const std::filesystem::path presetsJson = util::FileSystem::PathFromUtf8(m_ctx.projectRoot) / L"CMakePresets.json";
        const bool sdkCacheMatches = toolchain.found && CMakeCacheUsesSdkRoot(toolchain.buildDir, m_ctx.engineRoot);
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
        // WHY: ABI を統一するため、Editor と同じビルド構成の Scripts DLL を使う。
        //      FBZZ_CMAKE_CONFIG は cmake が $<CONFIG> を焼き込んだ文字列 ("Debug"/"Release"/"Development")。
        //      #ifdef NDEBUG では Development (NDEBUG なし・最適化 ON) を Debug と誤判定するため使わない。
        static constexpr std::string_view kConfig = FBZZ_CMAKE_CONFIG;
        if constexpr (kConfig == "Development") {
            m_scriptDllPath = !toolchain.scriptsDllDevelopment.empty()
                ? toolchain.scriptsDllDevelopment
                // WHY: SandboxScripts.dll は exe 整理後 Binaries/<Config>/Sandbox/ に出力される。
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

        // 新方式: Reflect() はヘッダ内の FBZZ_REFLECT が生成するため、起動時の
        //         .generated.hpp 一括生成 (旧 FHT) は不要になった。

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
        // WHY: ToolchainLocator の失敗は cmake/build.config が未生成の場合に起こる。
        //      hot reload は無効になるが、DLL が既にビルド済みならスクリプトは Play 中に動く。
        //      build.config が部分的に読めた場合 (cmake なし等) は scriptsDllDebug が設定済み。
        //      そうでなければ build_root を 2 段まで検索して DLL を探す。
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

        // GameHub プロジェクトなら toolchain なしでも scriptsDllCppPath を設定できる
        if (!m_ctx.projectTargetName.empty()) {
            const std::filesystem::path projRoot = util::FileSystem::PathFromUtf8(m_ctx.projectRoot);
            const std::wstring targetW = util::StringUtils::ToWide(m_ctx.projectTargetName);
            m_ctx.scriptsDllCppPath    = util::FileSystem::PathToUtf8(projRoot / L"Src" / (targetW + L"ScriptsDll.cpp"));
            m_ctx.scriptsStaticCppPath = util::FileSystem::PathToUtf8(projRoot / L"Src" / L"GameMain.cpp");
        }
    }

    // DLL ロード (toolchain の成否に関わらず実行)
    m_ctx.scriptsDllPath = util::FileSystem::PathToUtf8(m_scriptDllPath);
    if (!m_ctx.scriptsSourceDir.empty()) {
        // WHY: Scripts/ の実ファイルを登録の正とし、手動追加・削除を Unity 風に自動反映する。
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
                // 拒否した DLL は消してから再ビルドを予約する。
                //
                // 消さないと «Engine をエクスポート不変で再リンクしただけ» のときに
                // 抜け出せなくなる。ローダーは «Engine.dll より古い» と拒否し続けるが、
                // ヘッダーもインポートライブラリも変わっていないので MSBuild は
                // «更新不要» と正しく判断して何もせず、DLL の日時も進まない。
                // 出力を消せばリンクだけは必ず走り、日時が進んで膠着が解ける。
                std::error_code removeError;
                std::filesystem::remove(m_scriptDllPath, removeError);
                if (removeError) {
                    FBZZ_LOG_WARN("ScriptDll: 古い DLL を削除できませんでした: %s",
                                  removeError.message().c_str());
                }

                // WHY: Engine 側の Scene / Component レイアウトだけが変わった場合、
                //      スクリプトソースのタイムスタンプ比較では古い DLL を検出できない。
                //      ABI 不一致でロードを拒否した時点で依存ターゲット込みの再ビルドを予約する。
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
        // WHY: cmake configure 直後は DLL がまだ存在しない。
        //      初回ビルドを TickScriptCompile() に委譲することで非同期ビルドを起動する。
        FBZZ_LOG_INFO("ScriptDll: DLL が未ビルドです。初回ビルドを開始します...");
        m_scriptCompilePending = true;
        m_scriptInitialBuild   = true;   // 初回ビルドは依存ターゲットも含めてビルドする
        m_scriptDebounceTimer  = 0.0f;
        m_ctx.scriptReloadBusy = true;
        SetHotReloadState(EditorContext::HotReloadState::Compiling, "Scripts: initial build...");
        m_ctx.hotReloadProgress = 0.0f;
    } else {
        FBZZ_LOG_WARN("ScriptDll: %ls not found; generate it with cmake --build",
                      m_scriptDllPath.wstring().c_str());
    }

    if (!toolchain.found) return;

    // ── 起動時 Assets 即時同期 ──────────────────────────────────────────────
    // 最終更新時刻をキャッシュする (初回は変更なしと判定)
    if (!m_scriptsSourceDir.empty()) {
        const std::filesystem::path scriptScanRoot = GetScriptScanRoot(m_scriptsSourceDir);
        m_lastScriptWriteTime = GetLatestScriptSourceWriteTime(scriptScanRoot);

        // WHY: 既存DLLをロードできても、ソースより古ければSceneManagerScript等の修正が
        //      Editor Playへ反映されない。Standaloneビルドへ依存せず起動時に自動更新する。
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

// 監視するシェーダーツリーを決め、それがプロジェクトの持ち物かを判定する。
//
// WHY 所有者を見るか: AssetManager::ResolveAssetPath は «プロジェクト → 共有 SDK» の順に
//   探すので、自前の Assets/Shaders を持たないプロジェクトでは SDK 側の実体が返る。
//   そこへ書き戻すと、その SDK を使う他のプロジェクトの CSO まで書き換わる。
//   逆に自前のツリーを持っているなら書き換えて困る相手はいないので、止める理由もない。
//   従来は FBZZ_SOURCE_TREE_BUILD というビルド構成で切っており、«自前のシェーダーを
//   持つ SDK プロジェクト» が巻き添えで無効化されていた。判断すべきはビルド構成ではなく
//   «今から書き込む先が誰のものか» なので、実行時の所在で決める。
void EditorApp::InitHlslHotReload()
{
    m_hlslProjectOwned      = false;
    m_hlslSourceDir         = util::FileSystem::PathFromUtf8(
        asset::AssetManager::ResolveAssetPath("Assets/Shaders"));
    m_compileShadersScript  = m_hlslSourceDir / L"compile_shaders.ps1";
    m_ctx.hlslSourceDir     = util::FileSystem::PathToUtf8(m_hlslSourceDir);

    if (m_ctx.projectRoot.empty() || m_ctx.hlslSourceDir.empty()) return;

    // 比較の前に絶対化して '..' を畳む。ResolveAssetPath は基準パスを継ぎ足すだけなので、
    // 相対のまま比べると «プロジェクトの中» を «外» と読み違える。
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
    // 走らせるのは同梱スクリプトなので、無いなら «自前のツリー» とは呼べない。
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
        // WHY: 起動していない間の削除もstampでは判定できないため、差分スクリプトを一度走らせる。
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

    // WHY: Assets/ 配下のスクリプト候補確認はディスク I/O と path 確保を伴うため、
    //      BeginFrame 毎に走らせるとエディター操作が CPU ボトルネック化する。
    // WHAT: ホットリロードの体感遅延として許容できる 0.5 秒間隔に制限し、
    //       ファイル保存後の再ビルドは既存のデバウンスでまとめる。
    m_scriptDirtyPollTimer += ImGui::GetIO().DeltaTime;
    if (m_scriptDirtyPollTimer < HOT_RELOAD_TREE_POLL_INTERVAL) return;
    m_scriptDirtyPollTimer = 0.0f;

    // Assets/ 配下のスクリプト候補の最終変更時刻を確認する。
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
    m_scriptDebounceTimer  = 0.5f;  // 500ms デバウンス
    m_ctx.scriptReloadBusy = true;
    SetHotReloadState(EditorContext::HotReloadState::Compiling, "Scripts: waiting for changes...");
    m_ctx.hotReloadProgress = 0.0f;
    FBZZ_LOG_DEBUG("ScriptDll: change detected in %s; rebuilding after 500 ms debounce",
                   m_ctx.scriptsSourceDir.c_str());
    // 新方式: Reflect() はヘッダ内生成のため、ビルド前の .generated.hpp 一括生成は不要。
}

void EditorApp::TickScriptCompile()
{
    // デバウンスタイマーを消費してからビルド開始する
    if (m_scriptCompilePending) {
        m_ctx.scriptReloadBusy = true;
        m_scriptDebounceTimer -= 0.016f;
        if (m_scriptDebounceTimer > 0.0f) return;

        m_scriptCompilePending = false;

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
        // WHY: DLL ファイル名 (例: SandboxScripts.dll) から cmake ターゲット名を導出する。
        //      ハードコードすると GameHub プロジェクト (MyGameScripts 等) で壊れる。
        config.target        = util::FileSystem::PathToUtf8(m_scriptDllPath.stem());
        // WHY: cmake --config に Editor と同じ構成を渡して ABI を統一する。
        //      FBZZ_CMAKE_CONFIG を使う理由: Development は NDEBUG なし・最適化 ON の第三の構成であり、
        //      #ifdef NDEBUG では正しく判定できない。
        config.configuration = FBZZ_CMAKE_CONFIG;
        config.sdkRoot       = m_ctx.engineRoot;
        // WHY: 初回ビルド (DLL 未存在) はエンジン libs がまだないため依存ターゲットも含めてビルドする。
        //      ホットリロード時はエディタがエンジン DLL をロック中のためスキップする。
        config.skipDeps      = !m_scriptInitialBuild;
        m_scriptInitialBuild = false;

        if (!m_scriptCompiler.Start(config)) {
            m_ctx.scriptReloadBusy = false;
            SetHotReloadState(EditorContext::HotReloadState::Failed, "Script: failed to start compile");
            return;
        }
        // ビルドコンソールへ新規ビルドを通知する (診断・ライブログ・履歴の起点)。
        m_buildConsole.BeginBuild(BuildRecord::Kind::Script);
        FBZZ_LOG_DEBUG("ScriptDll: starting compile: target=%s cfg=%s",
            config.target.c_str(), config.configuration.c_str());
        SetHotReloadState(EditorContext::HotReloadState::Compiling, "Scripts: compiling...");
        m_ctx.hotReloadProgress = SCRIPT_PROGRESS_BUILD_BEGIN;
    }

    if (m_scriptCompiler.GetState() == Compiler::State::Building) {
        m_ctx.scriptReloadBusy = true;
        m_scriptCompiler.Tick();
        // コンパイラの stdout 差分を取り込み、現在コンパイル中ファイルと診断を更新する。
        m_buildConsole.IngestFullLog(m_scriptCompiler.GetLog());
        // WHAT: 総コンパイル単位を取得できないため、残り幅に比例して増える段階進捗を使う。
        // WHY: 90% を上限にすることで、ビルド完了前にリロード段階へ到達したように見せない。
        const float deltaTime = ImGui::GetIO().DeltaTime;
        m_ctx.hotReloadProgress +=
            (SCRIPT_PROGRESS_BUILD_END - m_ctx.hotReloadProgress) * std::min(deltaTime * 0.7f, 1.0f);
        m_ctx.hotReloadProgress = std::min(m_ctx.hotReloadProgress, SCRIPT_PROGRESS_BUILD_END - 0.01f);
        return;
    }

    if (m_scriptCompiler.GetState() == Compiler::State::Done) {
        m_ctx.scriptReloadBusy = true;
        // コンパイル成功を確定する (この後の DLL リロードは別工程として扱う)。
        m_buildConsole.IngestFullLog(m_scriptCompiler.GetLog());
        m_buildConsole.EndBuild(true, 0);
        SetHotReloadState(EditorContext::HotReloadState::Reloading, "Scripts: reloading...");
        m_ctx.hotReloadProgress = SCRIPT_PROGRESS_BUILD_END;

        // リロード前に選択中エンティティの instanceId を保存する。
        // WHY: Reload() はシーンを move で置き換えるため EntityID が変わるが、
        //      instanceId (UUID v4) はシリアライズ経由で保持されるため復元に使える。
        std::vector<std::string> savedGuids;
        if (m_ctx.activeScene) {
            for (auto id : m_ctx.selectedEntities) {
                if (auto* go = m_ctx.activeScene->GetGameObject(id))
                    savedGuids.push_back(go->instanceId);
            }
        }

        if (m_ctx.activeScene && m_scriptDll.Reload(*m_ctx.activeScene, m_scriptDllPath)) {
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
            SetHotReloadState(EditorContext::HotReloadState::Failed, "Scripts: reload failed");
            m_ctx.hotReloadDoneTimer = 5.0f;
        }
        m_scriptCompiler.Reset();
        m_ctx.scriptReloadBusy = false;
        return;
    }

    if (m_scriptCompiler.GetState() == Compiler::State::Failed) {
        // 失敗ログを取り込み、診断を確定する。Build Output パネルへ件数と file:line が並ぶ。
        const std::string buildLog = m_scriptCompiler.GetLog();
        m_buildConsole.IngestFullLog(buildLog);
        m_buildConsole.EndBuild(false, m_scriptCompiler.GetExitCode());
        const int errs = m_buildConsole.Latest() ? m_buildConsole.Latest()->errorCount : 0;
        const std::string msg = errs > 0
            ? "Scripts: " + std::to_string(errs) + " error(s)"
            : "Scripts: compile error (exit=" + std::to_string(m_scriptCompiler.GetExitCode()) + ")";
        FBZZ_LOG_ERROR("ScriptDll: %s\n%s", msg.c_str(), buildLog.c_str());
        m_scriptCompiler.Reset();

        // 自己修復: SDK を解決できない CMakeCache は、放置すると起動のたびに同じ失敗を出す。
        // configure をやり直して cache を作り直し、その場で 1 度だけ再ビルドを予約する。
        // WHY: 1 セッションにつき 1 回に制限するのは、SDK 自体が本当に無い場合に
        //      configure 失敗 → 再ビルド → 同じ失敗、の無限ループへ落ちないようにするため。
        if (!m_scriptSdkRecoveryDone
            && !m_ctx.projectRoot.empty()
            && LooksLikeSdkConfigureFailure(buildLog)) {
            m_scriptSdkRecoveryDone = true;
            FBZZ_LOG_WARN("ScriptDll: SDK を解決できない CMakeCache を検出しました。configure をやり直します");
            if (TryCMakeConfigure(m_ctx.projectRoot, m_ctx.engineRoot)) {
                m_scriptCompilePending = true;
                m_scriptDebounceTimer  = 0.0f;
                // 依存ターゲットも含めた初回ビルド扱いにする (cache 再生成後は中間物が失われている)。
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

// =============================================================================
// HLSL ホットリロード
// =============================================================================

void EditorApp::CheckHlslDirty()
{
    // 共有 SDK の shader を実行中に書き換えてはならない。判定は InitHlslHotReload が持つ。
    if (!m_hlslProjectOwned) return;
    if (!m_ctx.hotReloadEnabled) return;
    if (m_hlslSourceDir.empty()) return;
    if (m_ctx.playMode && !m_ctx.playMode->IsInEditor()) return;
    if (m_hlslCompiler.GetState() == Compiler::State::Building) return;
    if (m_hlslCompilePending) return;

    // WHY: HLSL 監視も Scripts と同じくツリー全体を列挙する。
    //      シェーダー保存検知は即時性よりフレーム安定性を優先し、EditorBegin の常時 5ms 負荷を避ける。
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

        // WHY: ホットリロードは未Configureのゲームプロジェクトでも動く必要があるため、
        //      正式ビルド用CMakeターゲットを経由せず、差分対応PowerShellを直接非同期実行する。
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
        if (renderer::ResourceManager::Active())
            renderer::ResourceManager::Active()->ReloadAllShaders();
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
    m_ctx.hotReloadState   = state;
    m_ctx.hotReloadMessage = msg;
    if (!msg.empty()) FBZZ_LOG_INFO("[HotReload] %s", msg.c_str());
}

// =============================================================================
// エディタ専用非表示の一時解除 / 再適用
// =============================================================================

void EditorApp::RemoveEditorHiding()
{
    if (!m_ctx.activeScene || m_ctx.editorHiddenGuids.empty()) return;
    for (const auto& [guid, wasActive] : m_ctx.editorHiddenGuids) {
        if (auto* go = m_ctx.activeScene->FindByGuid(guid))
            go->SetActive(wasActive);  // 非表示前の状態を復元
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
    // 保存順が毎回変わると .meta の差分が無意味に膨らむ (unordered_map の走査順は不定)。
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

    // .scene には非表示を解除した状態の activeSelf が入っている (Save 前に
    // RemoveEditorHiding が戻す)。つまり「隠す前の値」はいま読んだ値そのもの。
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
