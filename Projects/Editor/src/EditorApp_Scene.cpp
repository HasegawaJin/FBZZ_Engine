// FBZZ Engine
// EditorApp_Scene.cpp | fbzz::editor
// シーンの新規作成・開く・保存・ダーティ追跡・ホットリロード
//
// WHY: EditorApp.cpp が肥大化しないよう、シーン I/O とダーティ追跡を分離した。
//      これらはいずれも「シーンファイル」という単一の概念を中心とした処理群であり、
//      ライフサイクル管理 (Init/Shutdown/BeginFrame) や UI (MenuBar) とは関心が異なる。
#include <Editor/EditorApp.hpp>
#include <Editor/ToolchainLocator.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/ModalDialog.hpp>
#include <Editor/Util/FileDialog.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/ScriptCodeGen.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>
#include <Windows.h>
#include <algorithm>
#include <filesystem>
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

// ディレクトリ配下で最も新しい更新時刻を返す。
// WHY: Windows では既存ファイルの中身を書き換えても親ディレクトリの更新時刻は変わらない。
//      Scripts/ や shaders/ のディレクトリ時刻だけを監視すると、ホットリロード対象の .hpp / .hlsl
//      編集を検知できないため、ツリー内の各エントリを確認する。
FILETIME GetLatestWriteTimeInTree(const std::filesystem::path& root)
{
    FILETIME latest{};
    if (!TryGetWriteTime(root, latest))
        return latest;

    for (const std::filesystem::path& path : util::FileSystem::ListFilesRecursive(root)) {
        FILETIME ft{};
        if (TryGetWriteTime(path, ft) && CompareFileTime(&ft, &latest) > 0)
            latest = ft;
    }

    return latest;
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

std::filesystem::path GetScriptScanRoot(const std::filesystem::path& scriptsSourceDir)
{
    if (scriptsSourceDir.filename() == L"Scripts")
        return scriptsSourceDir.parent_path();
    return scriptsSourceDir;
}

// HLSL の再コンパイル結果を現在開いているプロジェクトへ反映する。
// WHY: compile_shaders.bat はエンジンソース側 Assets/shaders/compiled に CSO を出力する。
//      しかし実行中の renderer はプロジェクト側 Assets/shaders/compiled を読むため、
//      ReloadAllShaders() の前に CSO を同期しないと古いバイナリを再ロードしてしまう。
bool SyncCompiledShadersToProject(const std::filesystem::path& hlslSourceDir,
                                  const std::string& projectRoot)
{
    if (hlslSourceDir.empty() || projectRoot.empty()) return false;

    const std::filesystem::path src = hlslSourceDir / L"compiled";
    const std::filesystem::path dst = util::FileSystem::PathFromUtf8(projectRoot) /
                                      L"Assets" / L"shaders" / L"compiled";

    if (!util::FileSystem::Exists(src)) return false;
    if (util::FileSystem::SamePath(src, dst)) return true;
    return util::FileSystem::CopyDirectoryRecursive(src, dst);
}

// HLSL の再コンパイル結果を renderer の実際の読込先へ反映する。
// WHY: EditorLauncher / sandbox は起動時にカレントディレクトリを exe 隣へ変更する。
//      DX11Shader は "assets/shaders/compiled/..." を相対パスで開くため、
//      ReloadAllShaders() の前に exe 隣の Assets にも CSO を同期する必要がある。
bool SyncCompiledShadersToRuntimeAssets(const std::filesystem::path& hlslSourceDir)
{
    if (hlslSourceDir.empty()) return false;

    const std::filesystem::path src = hlslSourceDir / L"compiled";
    const std::filesystem::path dst = util::FileSystem::GetCurrentDirectory() /
                                      L"Assets" / L"shaders" / L"compiled";
    if (dst.empty()) return false;
    if (!util::FileSystem::Exists(src)) return false;
    if (util::FileSystem::SamePath(src, dst)) return true;
    return util::FileSystem::CopyDirectoryRecursive(src, dst);
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
    if (!m_ctx.activeScene) return;
    m_ctx.activeScene->Clear();
    m_undoStack.Clear();
    m_ctx.selectedEntities.clear();
    m_ctx.graphLayouts.clear();
    m_ctx.editorHiddenGuids.clear();
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
    if (!m_ctx.activeScene) return false;

    std::string path;
    if (!FileDialog::OpenFile(m_hwnd, { SCENE_FILTER }, path)) return false;
    return OpenScenePath(path);
}

bool EditorApp::OpenScenePath(const std::string& path)
{
    if (!m_ctx.activeScene || path.empty()) return false;

    if (!SceneIO::Load(*m_ctx.activeScene, path)) {
        FBZZ_LOG_ERROR("Open scene failed: %s", path.c_str());
        return false;
    }
    // WHY: SceneSerializer はローカル position のみ復元し worldPosition はゼロのまま。
    //      次フレームの TransformEditorPreview まで待つと 1 フレーム間オブジェクトが
    //      原点に表示されるため、ここで即時フラッシュして最初のフレームも正しくする。
    scene::FlushWorldTransforms(*m_ctx.activeScene);

    // Undo コマンドは読込前シーンの EntityID と状態を保持するため、
    // 別シーンへ持ち越さず読込成功時点で破棄する。
    m_undoStack.Clear();
    m_settings.lastScenePath = path;
    m_ctx.currentScenePath = path;
    m_ctx.selectedEntities.clear();
    m_ctx.editorHiddenGuids.clear();
    RebuildEditorUIFromScene();
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

    RemoveEditorHiding();
    const bool ok = SceneIO::Save(*m_ctx.activeScene, m_settings.lastScenePath);
    RestoreEditorHiding();
    if (!ok) {
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

    RemoveEditorHiding();
    const bool ok = SceneIO::Save(*m_ctx.activeScene, path);
    RestoreEditorHiding();
    if (!ok) {
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

    // WHY: Sceneファイル監視の前提が満たされない場合でも、初回Scripts DLLビルドと
    //      進行中コンパイルは完了させる。ここで早期returnすると、Standaloneを別途
    //      ビルドするまでEditor Playにスクリプトが登録されない状態になるため。
    TickScriptCompile();
    TickHlslCompile();

    if (m_settings.lastScenePath.empty() || !m_ctx.activeScene) return;
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
        if (!SceneIO::Load(*m_ctx.activeScene, m_settings.lastScenePath))
            FBZZ_LOG_WARN("Hot reload failed: %s", m_settings.lastScenePath.c_str());
        else {
            m_ctx.selectedEntities.clear();
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

// WHY: GameHub プロジェクトは初回開封時に cmake configure が済んでいない場合がある。
//      build.config がないと ToolchainLocator が失敗してスクリプトが動かないため、
//      CMakePresets.json が存在すれば自動で cmake --preset fbzz-vs を実行する。
bool TryCMakeConfigure(const std::string& projectRoot, const std::string& engineRoot)
{
    wchar_t cmakeBuf[MAX_PATH]{};
    if (!SearchPathW(nullptr, L"cmake.exe", nullptr, MAX_PATH, cmakeBuf, nullptr)) {
        FBZZ_LOG_WARN("ScriptDll: cmake.exe が PATH に見つかりません。cmake --preset fbzz-vs を手動実行してください。");
        return false;
    }

    if (!engineRoot.empty()) {
        const int sz = MultiByteToWideChar(CP_UTF8, 0, engineRoot.c_str(), -1, nullptr, 0);
        if (sz > 0) {
            std::wstring w(static_cast<size_t>(sz - 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, engineRoot.c_str(), -1, w.data(), sz);
            SetEnvironmentVariableW(L"FBZZ_ENGINE_ROOT", w.c_str());
        }
    }

    const int projSz = MultiByteToWideChar(CP_UTF8, 0, projectRoot.c_str(), -1, nullptr, 0);
    std::wstring projRootW;
    if (projSz > 0) {
        projRootW.resize(static_cast<size_t>(projSz - 1));
        MultiByteToWideChar(CP_UTF8, 0, projectRoot.c_str(), -1, projRootW.data(), projSz);
    }

    // WHY: --preset の cacheVariables に "$env{FBZZ_ENGINE_ROOT}" があっても
    //      既に CMakeCache.txt が存在する場合はキャッシュ値が優先される。
    //      -D で明示的に上書きすることで既存キャッシュがあっても正しいパスが使われる。
    const int engSz = MultiByteToWideChar(CP_UTF8, 0, engineRoot.c_str(), -1, nullptr, 0);
    std::wstring engineRootW;
    if (engSz > 0) {
        engineRootW.resize(static_cast<size_t>(engSz - 1));
        MultiByteToWideChar(CP_UTF8, 0, engineRoot.c_str(), -1, engineRootW.data(), engSz);
    }
    std::wstring cmd = std::wstring(L"\"") + cmakeBuf + L"\" --preset fbzz-vs";
    if (!engineRootW.empty())
        cmd += L" -DFBZZ_ENGINE_ROOT:PATH=\"" + engineRootW + L"\"";
    // WHY: cmake --preset の cacheVariables は -D で上書きできる (cmake docs: "preset variables can be overridden using normal -D options")。
    //      CMakePresets.json に "Debug;Release" しか書かれていないプロジェクトでも
    //      エディタが Development 構成で VS プロジェクトを生成させるために明示的に上書きする。
    //      スペースを含むフラグは引数全体を "" で括ることで CreateProcessW に正しく渡せる。
    cmd += L" -DCMAKE_CONFIGURATION_TYPES=Debug;Release;Development";
    cmd += L" \"-DCMAKE_CXX_FLAGS_DEVELOPMENT=/Zi /O2 /Ob1 /FS\"";
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

    // WHY: cmake configure はメインスレッドをブロックしない。
    //      バックグラウンドで実行し、完了後にエディタを再起動するよう促す。
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    FBZZ_LOG_INFO("ScriptDll: cmake --preset fbzz-vs をバックグラウンドで起動しました。完了後にエディタを再起動してください。");
    return false;
}

} // namespace

void EditorApp::InitScriptDll()
{
    if (!m_ctx.hotReloadEnabled) return;

    ToolchainLocator::Result toolchain = ToolchainLocator::Locate(
        util::FileSystem::PathFromUtf8(m_ctx.projectBuildRoot));

    // WHY: GameHub プロジェクトは初回開封時に cmake configure が済んでいない場合がある。
    //      CMakePresets.json が存在するなら自動 configure を実行し再度解決を試みる。
    if (!toolchain.found && !m_ctx.projectRoot.empty()) {
        const std::filesystem::path presetsJson = util::FileSystem::PathFromUtf8(m_ctx.projectRoot) / L"CMakePresets.json";
        if (util::FileSystem::Exists(presetsJson)) {
            if (TryCMakeConfigure(m_ctx.projectRoot, m_ctx.engineRoot))
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
        m_hlslSourceDir        = engineRoot / L"Assets" / L"shaders";
        m_compileShadersScript = m_hlslSourceDir / L"compile_shaders.bat";

        m_ctx.scriptsSourceDir = util::FileSystem::PathToUtf8(m_scriptsSourceDir);
        m_ctx.hlslSourceDir    = util::FileSystem::PathToUtf8(m_hlslSourceDir);

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
    if (!m_scriptDllPath.empty() && util::FileSystem::Exists(m_scriptDllPath)) {
        if (m_scriptDll.Load(m_scriptDllPath)) {
            FBZZ_LOG_INFO("ScriptDll: loaded %ls (%d types)",
                m_scriptDllPath.wstring().c_str(),
                static_cast<int>(scene::ScriptFactory::RegisteredTypeNames().size()));
        } else {
            FBZZ_LOG_WARN("ScriptDll: load failed; scripts are kept as serialized data until DLL rebuild succeeds");
            if (toolchain.found) {
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
    // WHY: cmake --build の post-build は EXE が再ビルドされたときのみ実行される。
    //      ビルドなしでエディタを起動した場合、SandboxProject/Assets/ は古い状態のままになる。
    //      InitScriptDll() でエンジンソースのパスが分かった時点で Assets/ を即時同期することで、
    //      cmake を実行せずともスクリプト・カスタムシェーダーが AssetBrowser に表示される。
    {
        const std::filesystem::path engineRoot = toolchain.buildDir.parent_path().parent_path();
        const std::filesystem::path projectAssetsDir =
            util::FileSystem::PathFromUtf8(m_ctx.projectRoot) / L"Assets";
        const std::filesystem::path engineAssetsDir  = engineRoot / L"Assets";

        // プロジェクト側が別ディレクトリの場合のみ同期する (同一なら不要)
        if (!m_ctx.projectRoot.empty() &&
            util::FileSystem::Exists(engineAssetsDir) &&
            !util::FileSystem::SamePath(engineAssetsDir, projectAssetsDir))
        {
            // Scripts/ を同期
            const std::filesystem::path srcScripts = engineAssetsDir / L"Scripts";
            const std::filesystem::path dstScripts = projectAssetsDir / L"Scripts";
            if (util::FileSystem::Exists(srcScripts)) {
                util::FileSystem::EnsureDirectory(dstScripts);
                for (const auto& path : util::FileSystem::ListFiles(srcScripts)) {
                    util::FileSystem::CopyFile(path, dstScripts / path.filename());
                }
            }

            // shaders/Material/Custom/ を同期
            const std::filesystem::path srcCustom = engineAssetsDir / L"shaders" / L"Material" / L"Custom";
            const std::filesystem::path dstCustom = projectAssetsDir / L"shaders" / L"Material" / L"Custom";
            if (util::FileSystem::Exists(srcCustom)) {
                util::FileSystem::EnsureDirectory(dstCustom);
                for (const auto& path : util::FileSystem::ListFiles(srcCustom)) {
                    util::FileSystem::CopyFile(path, dstCustom / path.filename());
                }
            }

            FBZZ_LOG_INFO("InitScriptDll: synced Assets/Scripts/ and shaders/Material/Custom/");
        }
    }

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
    if (!m_hlslSourceDir.empty()) {
        m_lastHlslWriteTime = GetLatestWriteTimeInTree(m_hlslSourceDir);
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
        // WHY: 初回ビルド (DLL 未存在) はエンジン libs がまだないため依存ターゲットも含めてビルドする。
        //      ホットリロード時はエディタがエンジン DLL をロック中のためスキップする。
        config.skipDeps      = !m_scriptInitialBuild;
        m_scriptInitialBuild = false;

        if (!m_scriptCompiler.Start(config)) {
            m_ctx.scriptReloadBusy = false;
            SetHotReloadState(EditorContext::HotReloadState::Failed, "Script: failed to start compile");
            return;
        }
        FBZZ_LOG_DEBUG("ScriptDll: starting compile: target=%s cfg=%s",
            config.target.c_str(), config.configuration.c_str());
        SetHotReloadState(EditorContext::HotReloadState::Compiling, "Scripts: compiling...");
        m_ctx.hotReloadProgress = SCRIPT_PROGRESS_BUILD_BEGIN;
    }

    if (m_scriptCompiler.GetState() == Compiler::State::Building) {
        m_ctx.scriptReloadBusy = true;
        m_scriptCompiler.Tick();
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
            m_ctx.selectedEntities.clear();
            for (const auto& guid : savedGuids) {
                if (auto* go = scene::GameObject::FindByGuid(guid))
                    m_ctx.selectedEntities.push_back(go->GetID());
            }
            SetHotReloadState(EditorContext::HotReloadState::Done, "Scripts: hot reload complete");
            m_ctx.hotReloadProgress = 1.0f;
            m_ctx.hotReloadDoneTimer = 3.0f;
        } else {
            m_ctx.selectedEntities.clear();
            SetHotReloadState(EditorContext::HotReloadState::Failed, "Scripts: reload failed");
            m_ctx.hotReloadDoneTimer = 5.0f;
        }
        m_scriptCompiler.Reset();
        m_ctx.scriptReloadBusy = false;
        return;
    }

    if (m_scriptCompiler.GetState() == Compiler::State::Failed) {
        const std::string msg = "Scripts: compile error (exit=" +
                                std::to_string(m_scriptCompiler.GetExitCode()) + ")";
        FBZZ_LOG_ERROR("ScriptDll: %s\n%s", msg.c_str(), m_scriptCompiler.GetLog().c_str());
        SetHotReloadState(EditorContext::HotReloadState::Failed, msg);
        m_ctx.hotReloadDoneTimer = 8.0f;
        m_scriptCompiler.Reset();
        m_ctx.scriptReloadBusy = false;
    }
}

// =============================================================================
// HLSL ホットリロード
// =============================================================================

void EditorApp::CheckHlslDirty()
{
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
    const FILETIME ft = GetLatestWriteTimeInTree(m_hlslSourceDir);
    if (IsEmptyFileTime(ft))
        return;

    if (IsEmptyFileTime(m_lastHlslWriteTime)) {
        m_lastHlslWriteTime = ft;
        return;
    }
    if (CompareFileTime(&ft, &m_lastHlslWriteTime) == 0) return;

    m_lastHlslWriteTime  = ft;
    m_hlslCompilePending = true;
    m_hlslDebounceTimer  = 0.5f;
    FBZZ_LOG_DEBUG("HLSL: change detected in %s; recompiling after 500 ms debounce",
                   m_ctx.hlslSourceDir.c_str());
}

void EditorApp::TickHlslCompile()
{
    if (m_hlslCompilePending) {
        m_hlslDebounceTimer -= 0.016f;
        if (m_hlslDebounceTimer > 0.0f) return;

        m_hlslCompilePending = false;

        // WHY: HLSL コンパイルは cmake --build ではなく compile_shaders.bat を直接実行する。
        //      Compiler クラスは cmake --build 用だが、exePath を空にして
        //      target に bat 実行コマンドを渡す方法は複雑なため、
        //      cmake --build で compile_shaders ターゲットを指定して間接実行する。
        ToolchainLocator::Result toolchain = ToolchainLocator::Locate(
            util::FileSystem::PathFromUtf8(m_ctx.projectBuildRoot));
        if (!toolchain.found) {
            SetHotReloadState(EditorContext::HotReloadState::Failed, "HLSL: toolchain not resolved");
            return;
        }

        Compiler::Config config;
        config.cmakeExe      = toolchain.cmakeExe;
        config.buildDir      = toolchain.buildDir;
        config.exePath       = std::filesystem::path{};
        config.target        = "compile_shaders";
        config.configuration = FBZZ_CMAKE_CONFIG;

        if (!m_hlslCompiler.Start(config)) {
            SetHotReloadState(EditorContext::HotReloadState::Failed, "HLSL: failed to start compile");
            return;
        }
        SetHotReloadState(EditorContext::HotReloadState::Compiling, "HLSL: compiling shaders...");
        m_ctx.hotReloadProgress = -1.0f;
    }

    if (m_hlslCompiler.GetState() == Compiler::State::Building) {
        m_hlslCompiler.Tick();
        return;
    }

    if (m_hlslCompiler.GetState() == Compiler::State::Done) {
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

} // namespace fbzz::editor
