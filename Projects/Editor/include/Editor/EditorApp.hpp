// FBZZ Engine
// EditorApp.hpp | fbzz::editor
// エディター全体のライフサイクルを管理する
#pragma once
#include <Editor/Compiler.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <Editor/ScriptDllLoader.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Util/HotkeyManager.hpp>
#include <Editor/Util/ConsoleSink.hpp>
#include <Editor/Util/EditorSettings.hpp>
#include <Editor/Util/SceneDirtyTracker.hpp>
#include <Editor/PlayModeController.hpp>
// TerrainTool は src/ 内の内部ヘッダーなので前方宣言で対応する
// WHY: TerrainTool.hpp は imgui.h に依存しており、EditorApp.hpp に直接インクルードすると
//      Engine 層のヘッダーが imgui に依存してしまう。std::unique_ptr で所有して隠蔽する。
#include <memory>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <Windows.h>

namespace fbzz::renderer { class IRenderer; class ResourceManager; }
namespace fbzz::core     { class Window; }

namespace fbzz::editor {

class ViewportPanel;
class ProjectSettingsPanel;
class BuildSettingsPanel;
class AssetBrowserPanel;
class AnalysisPanel;
class TerrainTool;
class WaterTool;

class EditorApp {
public:
    EditorApp();
    ~EditorApp(); // TerrainTool の完全型が見えるところ (EditorApp.cpp) で定義する

    bool Init(renderer::IRenderer& renderer, renderer::ResourceManager& resources, core::Window& window);
    void Shutdown();
    bool OpenProject(const std::string& projectRoot, const std::string& projectSettingsPath, const std::string& scenePath);

    void BeginFrame();
    void RenderPanels(EditorContext& ctx);
    void EndFrame(renderer::IRenderer& renderer);

    EditorContext& GetContext() { return m_ctx; }

    // Viewport に紐づいたオフスクリーン RT (main.cpp はここに描く)
    renderer::ResourceHandle<renderer::RenderTargetTag> GetViewportRT() const { return m_sceneViewportRT; }
    renderer::ResourceHandle<renderer::RenderTargetTag> GetGameViewportRT() const { return m_gameViewportRT; }
    // WHY: UI Viewport は Game View の完成済み RT を共有し、編集ガイドだけを ImGui で重ねる。
    //      専用 RT を返す API にすると、Clear 済みの空 RT を表示する経路が再発しやすい。
    renderer::ResourceHandle<renderer::RenderTargetTag> GetUIViewportRT() const { return m_gameViewportRT; }

private:
    void BuildMenuBar(EditorContext& ctx);
    void BuildPlayToolbar(EditorContext& ctx);
    void RegisterDefaultHotkeys();
    void ResizeViewportRTsIfNeeded();
    void CheckHotReload();
    void CacheSceneWriteTime();

    // スクリプト DLL ホットリロード
    void InitScriptDll();
    void CheckScriptDirtyAndRebuild();
    void TickScriptCompile();

    // HLSL ホットリロード
    void CheckHlslDirty();
    void TickHlslCompile();

    // ホットリロード共通
    void SetHotReloadState(EditorContext::HotReloadState state, const std::string& msg = "");
    void CaptureCleanScene();
    void RefreshSceneDirtyState(bool force);
    void UpdateWindowTitle();
    void MarkSceneDirty();
    void ConfirmDiscardUnsaved(const std::string& actionName, std::function<void()> action);
    void NewScene();
    void RequestNewScene();
    void RequestOpenSceneFromDialog();
    void RequestOpenScenePath(const std::string& path);
    void RequestExit();
    bool OpenSceneFromDialog();
    bool OpenScenePath(const std::string& path);
    bool SaveScene();
    bool SaveSceneAsDialog();

    EditorContext                        m_ctx;
    std::vector<std::unique_ptr<IPanel>> m_panels;

    UndoStack          m_undoStack;
    HotkeyManager      m_hotkeys;
    ConsoleSink        m_consoleSink;
    EditorSettings     m_settings;
    SceneDirtyTracker  m_dirtyTracker;
    std::string        m_projectRoot;
    std::string        m_projectSettingsPath;
    float              m_dirtyPollTimer = 0.0f;
    bool               m_titleInitialized = false;
    bool               m_lastTitleDirty = false;
    std::string        m_lastTitleScenePath;
    PlayModeController              m_playMode;
    std::unique_ptr<TerrainTool>    m_terrainTool; // pimpl: EditorApp.hpp が imgui に依存しないよう unique_ptr で隠蔽
    std::unique_ptr<WaterTool>      m_waterTool;   // WaterTool も同じ pimpl パターンで隠蔽する

    // スクリプト DLL ホットリロード
    ScriptDllLoader          m_scriptDll;
    std::filesystem::path    m_scriptDllPath;      // SandboxScripts.dll のビルド出力パス
    std::filesystem::path    m_scriptsSourceDir;   // Scripts/ ソースディレクトリ (変更検知用)
    FILETIME                 m_lastScriptWriteTime = {};  // Scripts/ ディレクトリの最終変更時刻
    Compiler                 m_scriptCompiler;
    bool                     m_scriptCompilePending = false; // 変更検知からビルド開始待ち
    float                    m_scriptDebounceTimer  = 0.0f;  // デバウンス用タイマー (秒)

    // HLSL ホットリロード
    std::filesystem::path    m_hlslSourceDir;      // Assets/shaders/ ディレクトリ
    std::filesystem::path    m_compileShadersScript; // compile_shaders.bat パス
    FILETIME                 m_lastHlslWriteTime = {};
    Compiler                 m_hlslCompiler;
    bool                     m_hlslCompilePending = false;
    float                    m_hlslDebounceTimer  = 0.0f;

    FILETIME                                 m_lastSceneWriteTime = {};
    HWND                                     m_hwnd          = nullptr;
    renderer::IRenderer*                     m_renderer      = nullptr;
    renderer::ResourceManager*               m_resources     = nullptr;
    ViewportPanel*                           m_sceneViewportPanel     = nullptr;
    ViewportPanel*                           m_gameViewportPanel      = nullptr;
    ViewportPanel*                           m_uiViewportPanel        = nullptr;
    ProjectSettingsPanel*                    m_projectSettingsPanel   = nullptr;
    BuildSettingsPanel*                      m_buildSettingsPanel     = nullptr;
    AssetBrowserPanel*                       m_assetBrowserPanel      = nullptr;
    AnalysisPanel*                           m_analysisPanel          = nullptr;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_sceneViewportRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_gameViewportRT;
};

} // namespace fbzz::editor
