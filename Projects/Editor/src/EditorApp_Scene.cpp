// FBZZ Engine
// EditorApp_Scene.cpp | fbzz::editor
// シーンの新規作成・開く・保存・ダーティ追跡・ホットリロード
//
// WHY: EditorApp.cpp が肥大化しないよう、シーン I/O とダーティ追跡を分離した。
//      これらはいずれも「シーンファイル」という単一の概念を中心とした処理群であり、
//      ライフサイクル管理 (Init/Shutdown/BeginFrame) や UI (MenuBar) とは関心が異なる。
#include <Editor/EditorApp.hpp>
#include <Editor/ToolchainLocator.hpp>
#include <Editor/Util/ModalDialog.hpp>
#include <Editor/Util/FileDialog.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <Windows.h>
#include <filesystem>
#include <vector>

namespace fbzz::editor {

namespace {

const FileFilter SCENE_FILTER{ "FBZZ Scene", "*.fbzz" };

// 拡張子がなければ ".fbzz" を付与する
std::string WithFbzzExtension(const std::string& path)
{
    if (path.empty() || !util::FileSystem::GetExtension(path).empty()) return path;
    return path + ".fbzz";
}

// UTF-8 のプロジェクトパスを Windows の filesystem path へ変換する。
// WHY: EditorContext は UI / TOML と相性の良い UTF-8 文字列でパスを保持する一方、
//      std::filesystem は Windows 環境で wide path を使う方が日本語パスに強い。
std::wstring Utf8ToWidePath(const std::string& text)
{
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    return wide;
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
    if (!GetFileAttributesExW(path.wstring().c_str(), GetFileExInfoStandard, &info))
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

    std::error_code ec;
    const std::filesystem::recursive_directory_iterator end;
    std::filesystem::recursive_directory_iterator it(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        ec);

    while (!ec && it != end) {
        FILETIME ft{};
        if (TryGetWriteTime(it->path(), ft) && CompareFileTime(&ft, &latest) > 0)
            latest = ft;

        it.increment(ec);
    }

    return latest;
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
    const std::filesystem::path dst = std::filesystem::path(Utf8ToWidePath(projectRoot)) /
                                      L"Assets" / L"shaders" / L"compiled";

    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) return false;
    if (std::filesystem::equivalent(src, dst, ec)) return true;
    ec.clear();
    std::filesystem::create_directories(dst, ec);
    if (ec) return false;

    std::filesystem::copy(src, dst,
        std::filesystem::copy_options::recursive |
        std::filesystem::copy_options::overwrite_existing,
        ec);
    return !ec;
}

// HLSL の再コンパイル結果を renderer の実際の読込先へ反映する。
// WHY: EditorLauncher / sandbox は起動時にカレントディレクトリを exe 隣へ変更する。
//      DX11Shader は "assets/shaders/compiled/..." を相対パスで開くため、
//      ReloadAllShaders() の前に exe 隣の Assets にも CSO を同期する必要がある。
bool SyncCompiledShadersToRuntimeAssets(const std::filesystem::path& hlslSourceDir)
{
    if (hlslSourceDir.empty()) return false;

    const std::filesystem::path src = hlslSourceDir / L"compiled";
    std::error_code ec;
    const std::filesystem::path dst = std::filesystem::current_path(ec) /
                                      L"Assets" / L"shaders" / L"compiled";
    if (ec) return false;
    if (!std::filesystem::exists(src, ec)) return false;
    if (std::filesystem::equivalent(src, dst, ec)) return true;
    ec.clear();
    std::filesystem::create_directories(dst, ec);
    if (ec) return false;

    std::filesystem::copy(src, dst,
        std::filesystem::copy_options::recursive |
        std::filesystem::copy_options::overwrite_existing,
        ec);
    return !ec;
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

    m_settings.lastScenePath = path;
    m_ctx.currentScenePath = path;
    m_ctx.selectedEntities.clear();
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

    if (!SceneIO::Save(*m_ctx.activeScene, m_settings.lastScenePath)) {
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

    if (!SceneIO::Save(*m_ctx.activeScene, path)) {
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

    // スクリプト・HLSL ホットリロードの状態を進める
    TickScriptCompile();
    TickHlslCompile();

    // Done / Failed の表示タイマーを進める
    if (m_ctx.hotReloadDoneTimer > 0.0f) {
        // WHY: dt が取れないのでフレームごとの固定値 (≈16ms) で近似する
        m_ctx.hotReloadDoneTimer -= 0.016f;
        if (m_ctx.hotReloadDoneTimer <= 0.0f) {
            m_ctx.hotReloadDoneTimer = 0.0f;
            m_ctx.hotReloadState    = EditorContext::HotReloadState::Idle;
            m_ctx.hotReloadMessage.clear();
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

    auto Utf8ToWideLocal = [](const std::string& s) -> std::wstring {
        if (s.empty()) return {};
        const int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        if (sz <= 0) return {};
        std::wstring w(static_cast<size_t>(sz - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), sz);
        return w;
    };
    auto WideToUtf8Local = [](const std::filesystem::path& p) -> std::string {
        const std::wstring& w = p.wstring();
        const int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (sz <= 0) return {};
        std::string s(static_cast<size_t>(sz - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), sz, nullptr, nullptr);
        for (char& c : s) if (c == '\\') c = '/';
        return s;
    };

    ToolchainLocator::Result toolchain = ToolchainLocator::Locate(
        std::filesystem::path(m_ctx.projectBuildRoot));

    // WHY: GameHub プロジェクトは初回開封時に cmake configure が済んでいない場合がある。
    //      CMakePresets.json が存在するなら自動 configure を実行し再度解決を試みる。
    if (!toolchain.found && !m_ctx.projectRoot.empty()) {
        const std::filesystem::path presetsJson = std::filesystem::path(Utf8ToWideLocal(m_ctx.projectRoot)) / L"CMakePresets.json";
        std::error_code ec;
        if (std::filesystem::exists(presetsJson, ec)) {
            if (TryCMakeConfigure(m_ctx.projectRoot, m_ctx.engineRoot))
                toolchain = ToolchainLocator::Locate(std::filesystem::path(m_ctx.projectBuildRoot));
        }
    }

    if (toolchain.found) {
        // WHY: Release Editor が Debug DLL をロードすると CRT ミスマッチ (MSVCRT vs MSVCRTD) が
        //      発生し、DLL 境界を越えた std::function ファクトリ呼び出しでクラッシュする。
        //      Editor のビルド構成と DLL の構成を一致させることで ABI を統一する。
#ifdef NDEBUG
        if (!toolchain.scriptsDllRelease.empty()) {
            m_scriptDllPath = toolchain.scriptsDllRelease;
        } else {
            m_scriptDllPath = toolchain.exeRelease.parent_path() / L"SandboxScripts.dll";
        }
#else
        if (!toolchain.scriptsDllDebug.empty()) {
            m_scriptDllPath = toolchain.scriptsDllDebug;
        } else {
            m_scriptDllPath = toolchain.exeDebug.parent_path() / L"SandboxScripts.dll";
        }
#endif

        const std::filesystem::path engineRoot = toolchain.buildDir.parent_path().parent_path();
        m_scriptsSourceDir     = engineRoot / L"Assets" / L"Scripts";
        m_hlslSourceDir        = engineRoot / L"Assets" / L"shaders";
        m_compileShadersScript = m_hlslSourceDir / L"compile_shaders.bat";

        m_ctx.scriptsSourceDir = WideToUtf8Local(m_scriptsSourceDir);
        m_ctx.hlslSourceDir    = WideToUtf8Local(m_hlslSourceDir);

        if (!m_ctx.projectTargetName.empty()) {
            const std::filesystem::path projRoot(Utf8ToWidePath(m_ctx.projectRoot));
            const std::wstring targetW = Utf8ToWidePath(m_ctx.projectTargetName);
            m_ctx.scriptsDllCppPath    = WideToUtf8Local(projRoot / L"Src" / (targetW + L"ScriptsDll.cpp"));
            m_ctx.scriptsStaticCppPath = WideToUtf8Local(projRoot / L"Src" / L"GameMain.cpp");
        } else {
            m_ctx.scriptsDllCppPath    = WideToUtf8Local(
                engineRoot / L"Projects" / L"Sandbox" / L"src" / L"SandboxScriptsDll.cpp");
            m_ctx.scriptsStaticCppPath = WideToUtf8Local(
                engineRoot / L"Projects" / L"Sandbox" / L"src" / L"SandboxScripts.cpp");
        }
    } else {
        // WHY: ToolchainLocator の失敗は cmake/build.config が未生成の場合に起こる。
        //      hot reload は無効になるが、DLL が既にビルド済みならスクリプトは Play 中に動く。
        //      build.config が部分的に読めた場合 (cmake なし等) は scriptsDllDebug が設定済み。
        //      そうでなければ build_root を 2 段まで検索して DLL を探す。
        FBZZ_LOG_WARN("ScriptDll: ToolchainLocator failed - script hot reload is disabled");

#ifdef NDEBUG
        if (!toolchain.scriptsDllRelease.empty()) {
            m_scriptDllPath = toolchain.scriptsDllRelease;
        }
#else
        if (!toolchain.scriptsDllDebug.empty()) {
            m_scriptDllPath = toolchain.scriptsDllDebug;
        }
#endif
        else if (!m_ctx.projectBuildRoot.empty()) {
            const std::wstring dllName = Utf8ToWideLocal(
                m_ctx.projectTargetName.empty()
                    ? "SandboxScripts.dll"
                    : (m_ctx.projectTargetName + "Scripts.dll"));
            const std::filesystem::path buildRoot(Utf8ToWideLocal(m_ctx.projectBuildRoot));
            std::error_code ec;
            for (const auto& sub1 : std::filesystem::directory_iterator(buildRoot, ec)) {
                if (std::filesystem::exists(sub1.path() / dllName, ec)) {
                    m_scriptDllPath = sub1.path() / dllName;
                    break;
                }
                if (!sub1.is_directory(ec)) continue;
                for (const auto& sub2 : std::filesystem::directory_iterator(sub1.path(), ec)) {
                    if (!sub2.is_directory(ec)) continue;
                    if (std::filesystem::exists(sub2.path() / dllName, ec)) {
                        m_scriptDllPath = sub2.path() / dllName;
                        break;
                    }
                }
                if (!m_scriptDllPath.empty()) break;
            }
        }

        // GameHub プロジェクトなら toolchain なしでも scriptsDllCppPath を設定できる
        if (!m_ctx.projectTargetName.empty()) {
            const std::filesystem::path projRoot(Utf8ToWidePath(m_ctx.projectRoot));
            const std::wstring targetW = Utf8ToWidePath(m_ctx.projectTargetName);
            m_ctx.scriptsDllCppPath    = WideToUtf8Local(projRoot / L"Src" / (targetW + L"ScriptsDll.cpp"));
            m_ctx.scriptsStaticCppPath = WideToUtf8Local(projRoot / L"Src" / L"GameMain.cpp");
        }
    }

    // DLL ロード (toolchain の成否に関わらず実行)
    m_ctx.scriptsDllPath = WideToUtf8Local(m_scriptDllPath);
    if (!m_scriptDllPath.empty() && std::filesystem::exists(m_scriptDllPath)) {
        (void)m_scriptDll.Load(m_scriptDllPath);
        FBZZ_LOG_INFO("ScriptDll: loaded %ls (%d types)",
            m_scriptDllPath.wstring().c_str(),
            static_cast<int>(scene::ScriptFactory::RegisteredTypeNames().size()));
    } else if (toolchain.found) {
        // WHY: cmake configure 直後は DLL がまだ存在しない。
        //      初回ビルドを TickScriptCompile() に委譲することで非同期ビルドを起動する。
        FBZZ_LOG_INFO("ScriptDll: DLL が未ビルドです。初回ビルドを開始します...");
        m_scriptCompilePending = true;
        m_scriptInitialBuild   = true;   // 初回ビルドは依存ターゲットも含めてビルドする
        m_scriptDebounceTimer  = 0.0f;
        m_ctx.scriptReloadBusy = true;
        SetHotReloadState(EditorContext::HotReloadState::Compiling, "Scripts: initial build...");
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
            std::filesystem::path(Utf8ToWideLocal(m_ctx.projectRoot)) / L"Assets";
        const std::filesystem::path engineAssetsDir  = engineRoot / L"Assets";

        // プロジェクト側が別ディレクトリの場合のみ同期する (同一なら不要)
        std::error_code ec;
        if (!m_ctx.projectRoot.empty() &&
            std::filesystem::exists(engineAssetsDir, ec) &&
            std::filesystem::canonical(engineAssetsDir, ec) !=
            std::filesystem::canonical(projectAssetsDir, ec))
        {
            // Scripts/ を同期
            const std::filesystem::path srcScripts = engineAssetsDir / L"Scripts";
            const std::filesystem::path dstScripts = projectAssetsDir / L"Scripts";
            if (std::filesystem::exists(srcScripts, ec)) {
                std::filesystem::create_directories(dstScripts, ec);
                for (const auto& entry : std::filesystem::directory_iterator(srcScripts, ec)) {
                    if (!entry.is_regular_file(ec)) continue;
                    std::filesystem::copy_file(
                        entry.path(),
                        dstScripts / entry.path().filename(),
                        std::filesystem::copy_options::overwrite_existing, ec);
                }
            }

            // shaders/Material/Custom/ を同期
            const std::filesystem::path srcCustom = engineAssetsDir / L"shaders" / L"Material" / L"Custom";
            const std::filesystem::path dstCustom = projectAssetsDir / L"shaders" / L"Material" / L"Custom";
            if (std::filesystem::exists(srcCustom, ec)) {
                std::filesystem::create_directories(dstCustom, ec);
                for (const auto& entry : std::filesystem::directory_iterator(srcCustom, ec)) {
                    if (!entry.is_regular_file(ec)) continue;
                    std::filesystem::copy_file(
                        entry.path(),
                        dstCustom / entry.path().filename(),
                        std::filesystem::copy_options::overwrite_existing, ec);
                }
            }

            FBZZ_LOG_INFO("InitScriptDll: synced Assets/Scripts/ and shaders/Material/Custom/");
        }
    }

    // 最終更新時刻をキャッシュする (初回は変更なしと判定)
    if (!m_scriptsSourceDir.empty()) {
        m_lastScriptWriteTime = GetLatestWriteTimeInTree(m_scriptsSourceDir);
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

    // Scripts ツリー全体の最終変更時刻を確認する。
    const FILETIME ft = GetLatestWriteTimeInTree(m_scriptsSourceDir);
    if (IsEmptyFileTime(ft))
        return;

    if (IsEmptyFileTime(m_lastScriptWriteTime)) {
        m_lastScriptWriteTime = ft;
        return;
    }
    if (CompareFileTime(&ft, &m_lastScriptWriteTime) == 0) return;

    m_lastScriptWriteTime  = ft;
    m_scriptCompilePending = true;
    m_scriptDebounceTimer  = 0.5f;  // 500ms デバウンス
    m_ctx.scriptReloadBusy = true;
    SetHotReloadState(EditorContext::HotReloadState::Compiling, "Scripts: waiting for changes...");
    FBZZ_LOG_DEBUG("ScriptDll: change detected in %s; rebuilding after 500 ms debounce",
                   m_ctx.scriptsSourceDir.c_str());
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
            std::filesystem::path(m_ctx.projectBuildRoot));
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
        config.target        = m_scriptDllPath.stem().string();
        // WHY: Editor と Scripts DLL の CRT を統一して ABI ミスマッチを防ぐ。
        //      Release Editor には Release DLL、Debug Editor には Debug DLL を使う。
#ifdef NDEBUG
        config.configuration = "Release";
#else
        config.configuration = "Debug";
#endif
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
    }

    if (m_scriptCompiler.GetState() == Compiler::State::Building) {
        m_ctx.scriptReloadBusy = true;
        m_scriptCompiler.Tick();
        return;
    }

    if (m_scriptCompiler.GetState() == Compiler::State::Done) {
        m_ctx.scriptReloadBusy = true;
        SetHotReloadState(EditorContext::HotReloadState::Reloading, "Scripts: reloading...");

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
            std::filesystem::path(m_ctx.projectBuildRoot));
        if (!toolchain.found) {
            SetHotReloadState(EditorContext::HotReloadState::Failed, "HLSL: toolchain not resolved");
            return;
        }

        Compiler::Config config;
        config.cmakeExe      = toolchain.cmakeExe;
        config.buildDir      = toolchain.buildDir;
        config.exePath       = std::filesystem::path{};
        config.target        = "compile_shaders";
        config.configuration = "Debug";

        if (!m_hlslCompiler.Start(config)) {
            SetHotReloadState(EditorContext::HotReloadState::Failed, "HLSL: failed to start compile");
            return;
        }
        SetHotReloadState(EditorContext::HotReloadState::Compiling, "HLSL: compiling shaders...");
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

} // namespace fbzz::editor
