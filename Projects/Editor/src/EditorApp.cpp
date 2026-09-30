/// @file    EditorApp.cpp
/// @brief   エディター全体のライフサイクル管理 + DockSpace。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// @note  ファイル構成:
/// @note  EditorApp.cpp         - Init / Shutdown / OpenProject / BeginFrame / EndFrame / RenderPanels
/// @note  EditorApp_Scene.cpp   - シーン I/O・ダーティ追跡・ホットリロード
/// @note  EditorApp_MenuBar.cpp - メインメニューバーの構築・ホットキー登録
#include <Editor/EditorApp.hpp>
#include <Engine/Scene/Systems/RenderSceneExtractor.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/EditorTaskOverlay.hpp>
#include <Editor/Ai/EditorBusDispatcher.hpp>
#include <Editor/Ai/Json.hpp>
#include <Editor/Ai/NamedPipeServer.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Engine/Core/DeveloperMode.hpp>
#include <Editor/Util/Localization.hpp>
#include <Editor/Util/ModalDialog.hpp>
#include <Editor/Util/FileDialog.hpp>
#include <Editor/Util/FluidBakeService.hpp>
#include <Editor/Util/FluidInspector.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/Selection.hpp>
#include <Editor/Panels/SceneHierarchyPanel.hpp>
#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/Panels/PreviewPanel.hpp>
#include <Editor/Panels/AnimationPreview.hpp>
#include <Editor/Panels/AnimationMaskPreviewPanel.hpp>
#include <Editor/Panels/ViewportPanel.hpp>
#include <Editor/Panels/ConsolePanel.hpp>
#include <Editor/Panels/ScriptRequirementsPanel.hpp>
#include <Editor/Panels/BuildOutputPanel.hpp>
#include <Editor/Panels/AssetBrowserPanel.hpp>
#include <Editor/Panels/DependencyViewPanel.hpp>
#include <Editor/Panels/StatusBar.hpp>
#include <Editor/Panels/UndoHistoryPanel.hpp>
#include <Editor/Panels/HotkeyEditorPanel.hpp>
#include <Editor/Panels/ProjectSettingsPanel.hpp>
#include <Editor/Panels/BuildSettingsPanel.hpp>
#include <Editor/Panels/AnalysisPanel.hpp>
#include <Editor/Panels/RenderPassViewerPanel.hpp>
#include <Editor/Panels/AnimationGraphPanel.hpp>
#include <Editor/Panels/BehaviorTreePanel.hpp>
#include <Editor/Panels/SequencePanel.hpp>
#include <Editor/Panels/VFXTimelinePanel.hpp>
#include <Editor/Panels/SfxEditorPanel.hpp>
#include <Editor/Panels/FluidEditorPanel.hpp>
#include "Panels/FluidEditor/FluidEditorExtras.hpp"
#include <Editor/Panels/SpriteEditorPanel.hpp>
#include <Editor/Panels/MapEditorPanel.hpp>
#include <Editor/Panels/IblBakePanel.hpp>
#include <Editor/Panels/VolumeFlipbookBakePanel.hpp>
#include <Editor/Panels/AssetMaintenancePanel.hpp>
#include <Editor/Panels/NavigationPanel.hpp>
#include <Editor/Panels/AiSettingsPanel.hpp>
#include <Editor/Import/ImportCacheStore.hpp>
#include <Editor/Util/Toast.hpp>
#include "Tools/TerrainTool.hpp"
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Audio/AudioManager.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Cursor.hpp>
#include <Engine/Renderer/DebugCamera.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptRuntime.hpp>
#include <Engine/Scene/SceneUtils.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Core/Window.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Input/InputActionMap.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <imgui_impl_win32.h>
#include <toml++/toml.hpp>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

/// @note  imgui_impl_win32.h では #if 0 で隠されているため手動で前方宣言する
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fbzz::editor {

/// @note  デフォルトレイアウトをファイルスコープで定義し、OpenProject() から参照する。
/// @note io.IniFilename は projectRoot 確定後にセットするため Init() では設定しない。
static constexpr const char* DEFAULT_IMGUI_LAYOUT =
    "[Window][##statusbar]\n"
    "Pos=0,970\n"
    "Size=1904,32\n"
    "Collapsed=0\n"
    "\n"
    "[Window][##DockSpaceHost]\n"
    "Pos=0,0\n"
    "Size=1904,993\n"
    "Collapsed=0\n"
    "\n"
    "[Window][Debug##Default]\n"
    "Pos=60,60\n"
    "Size=400,400\n"
    "Collapsed=0\n"
    "\n"
    "[Window][Scene Hierarchy]\n"
    "Pos=0,19\n"
    "Size=209,974\n"
    "Collapsed=0\n"
    "DockId=0x00000001,0\n"
    "\n"
    "[Window][Inspector]\n"
    "Pos=1675,19\n"
    "Size=229,974\n"
    "Collapsed=0\n"
    "DockId=0x00000004,0\n"
    "\n"
    "[Window][Scene]\n"
    "Pos=211,19\n"
    "Size=1462,667\n"
    "Collapsed=0\n"
    "DockId=0x00000007,0\n"
    "\n"
    "[Window][Game]\n"
    "Pos=211,19\n"
    "Size=1462,667\n"
    "Collapsed=0\n"
    "DockId=0x00000007,2\n"
    "\n"
    "[Window][UI]\n"
    "Pos=211,19\n"
    "Size=1462,667\n"
    "Collapsed=0\n"
    "DockId=0x00000007,1\n"
    "\n"
    "[Window][Console]\n"
    "Pos=211,688\n"
    "Size=1462,305\n"
    "Collapsed=0\n"
    "DockId=0x00000005,1\n"
    "\n"
    "[Window][Asset Browser]\n"
    "Pos=211,688\n"
    "Size=1462,305\n"
    "Collapsed=0\n"
    "DockId=0x00000005,0\n"
    "\n"
    "[Window][Project Settings]\n"
    "Pos=211,19\n"
    "Size=1462,594\n"
    "Collapsed=0\n"
    "DockId=0x00000007,3\n"
    "\n"
    "[Window][New Scene]\n"
    "Pos=820,459\n"
    "Size=264,75\n"
    "Collapsed=0\n"
    "\n"
    "[Docking][Data]\n"
    "DockSpace       ID=0xFF535877 Window=0xE26AC72C Pos=0,19 Size=1904,974 Split=X\n"
    "  DockNode      ID=0x00000001 Parent=0xFF535877 SizeRef=209,1042 HiddenTabBar=1 Selected=0xB8729153\n"
    "  DockNode      ID=0x00000006 Parent=0xFF535877 SizeRef=1703,1042 Split=X\n"
    "    DockNode    ID=0x00000003 Parent=0x00000006 SizeRef=1478,1042 Split=Y\n"
    "      DockNode  ID=0x00000007 Parent=0x00000003 SizeRef=1464,683 CentralNode=1 Selected=0xD1EB2482\n"
    "      DockNode  ID=0x00000005 Parent=0x00000003 SizeRef=1464,305 Selected=0x36AF052B\n"
    "    DockNode    ID=0x00000004 Parent=0x00000006 SizeRef=229,1042 HiddenTabBar=1 Selected=0x36DC96AB\n";


namespace {

bool IsEditorUICanvas(const scene::UICanvas& canvas)
{
    return canvas.enabled
        && (canvas.renderMode == scene::UIRenderMode::ScreenSpaceOverlay
            || canvas.renderMode == scene::UIRenderMode::ScreenSpaceCamera);
}

std::string ResolveScenePathForProject(const std::string& projectRoot, const std::string& scenePath)
{
    if (scenePath.empty()) return {};
    const std::filesystem::path rootPath = util::FileSystem::PathFromUtf8(projectRoot);
    std::filesystem::path candidate = util::FileSystem::PathFromUtf8(scenePath);
    if (!candidate.is_absolute())
        candidate = rootPath / candidate;
    return util::FileSystem::PathToUtf8(util::FileSystem::MakeAbsolute(candidate));
}

bool IsScenePathInsideProject(const std::string& projectRoot, const std::string& scenePath)
{
    if (projectRoot.empty() || scenePath.empty()) return false;
    if (util::StringUtils::ToLower(util::FileSystem::GetExtension(scenePath)) != ".scene") return false;
    if (!util::FileSystem::Exists(scenePath)) return false;

    const std::filesystem::path rootPath = util::FileSystem::MakeAbsolute(util::FileSystem::PathFromUtf8(projectRoot));
    const std::filesystem::path sceneFs  = util::FileSystem::MakeAbsolute(util::FileSystem::PathFromUtf8(scenePath));
    return util::FileSystem::IsChildPathText(
        util::FileSystem::PathToUtf8(sceneFs),
        util::FileSystem::PathToUtf8(rootPath));
}

void LoadRuntimeBuildMetadata(EditorContext& ctx)
{
    ctx.projectBuildRoot.clear();
    ctx.standaloneTargetName = "SandboxStandalone";
    if (ctx.projectRoot.empty()) return;

    std::string projectText;
    if (!util::FileSystem::ReadText(ctx.projectRoot + "/.fbzz_proj", projectText))
        return;

    toml::parse_result result = toml::parse(projectText);
    if (!result) return;

    const toml::table& table = result.table();
    const std::string buildRoot = table["project"]["build_root"].value_or(std::string{});
    if (!buildRoot.empty() && buildRoot.rfind("{{", 0) != 0) {
        std::filesystem::path path = util::FileSystem::PathFromUtf8(buildRoot);
        if (!path.is_absolute())
            path = util::FileSystem::PathFromUtf8(ctx.projectRoot) / path;
        ctx.projectBuildRoot = util::FileSystem::PathToUtf8(path.lexically_normal());
    }

    const std::string standaloneTarget = table["project"]["standalone_target_name"].value_or(std::string{});
    if (!standaloneTarget.empty() && standaloneTarget.rfind("{{", 0) != 0) {
        ctx.standaloneTargetName = standaloneTarget;
    }

    const std::string targetName = table["project"]["target_name"].value_or(std::string{});
    if (!targetName.empty() && targetName.rfind("{{", 0) != 0) {
        if (standaloneTarget.empty())
            ctx.standaloneTargetName = targetName + "Standalone";
        ctx.projectTargetName    = targetName;
    }

    /// @note SDKの実パスはマシン固有なのでGameHubが環境変数で渡す。
    /// @note        .fbzz_projにはportableなsdk_idだけを保存し、旧sdk_root/rootは移行用に限って読む。
    std::string engineRoot;
    const DWORD sdkRootSize = GetEnvironmentVariableW(L"FBZZ_SDK_ROOT", nullptr, 0);
    if (sdkRootSize > 1) {
        std::wstring sdkRoot(sdkRootSize, L'\0');
        const DWORD written = GetEnvironmentVariableW(
            L"FBZZ_SDK_ROOT", sdkRoot.data(), sdkRootSize);
        if (written > 0 && written < sdkRootSize) {
            sdkRoot.resize(written);
            engineRoot = util::FileSystem::PathToUtf8(std::filesystem::path(sdkRoot));
        }
    }
    if (engineRoot.empty()) {
        engineRoot = table["engine"]["sdk_root"].value_or(
            table["engine"]["root"].value_or(std::string{}));
    }
    if (!engineRoot.empty() && engineRoot.rfind("{{", 0) != 0) {
        std::filesystem::path path = util::FileSystem::PathFromUtf8(engineRoot);
        if (!path.is_absolute())
            path = util::FileSystem::PathFromUtf8(ctx.projectRoot) / path;
        ctx.engineRoot = util::FileSystem::PathToUtf8(path.lexically_normal());
    }
}

} /// @note namespace

/// @note  初期化 / 終了

/// @note  コンストラクタ・デストラクタをここで定義する。EditorApp.hpp は TerrainTool を
/// @note  前方宣言だけにしており、unique_ptr のデストラクタは完全型を要求するため。
EditorApp::EditorApp()
{
    SceneIO::SetEditorSceneState(&m_ctx.editorSceneState);
}
EditorApp::~EditorApp() = default;

bool EditorApp::StartAiCommandBus()
{
    if (m_aiPipeServer && m_aiPipeServer->IsRunning()) {
        m_ctx.aiCommandBusRunning = true;
        return true;
    }

    auto dispatcher = std::make_unique<ai::EditorBusDispatcher>(m_ctx);
    auto server = std::make_unique<ai::NamedPipeServer>();
    if (!server->Start(L"\\\\.\\pipe\\FBZZEditorCommandBus")) {
        m_ctx.aiCommandBusRunning = false;
        FBZZ_LOG_ERROR("AI Command Bus を開始できませんでした");
        return false;
    }

    m_aiDispatcher = std::move(dispatcher);
    m_aiPipeServer = std::move(server);
    m_ctx.aiCommandBusRunning = true;
    FBZZ_LOG_INFO("AI Command Bus を開始しました");
    return true;
}

std::string EditorApp::HandleAiRequest(const std::string& request)
{
    if (!m_aiDispatcher) return {};
    return m_aiDispatcher->Handle(request);
}

void EditorApp::StopAiCommandBus()
{
    if (m_aiPipeServer) {
        m_aiPipeServer->Stop();
        m_aiPipeServer.reset();
    }
    m_aiDispatcher.reset();
    m_ctx.aiCommandBusRunning = false;
}

bool EditorApp::Init(renderer::IRenderer& renderer, renderer::IImGuiRenderer& imguiRenderer, renderer::ResourceManager& resources, core::Window& window)
{
    m_hwnd          = window.GetHandle();
    m_window        = &window;
    m_renderer      = &renderer;
    m_imguiRenderer = &imguiRenderer;
    m_resources     = &resources;

    /// @note ゲーム入力のアクション層は編集中は止め、Play 開始時に PlayModeController が有効化する。
    /// @note        既定は有効 (Standalone でそのまま遊べる状態) なので、エディタ側で明示的に落とす。
    input::InputActionMap::SetEnabled(false);

    /// @note Play 中のゲームが display プロキシで窓の形態や解像度を変えても、Editor の窓には
    /// @note        通さない (Editor は常にウィンドウモード)。要求値は覚えられるので、Option 画面の
    /// @note        チェックやドロップダウンは Standalone と同じように動く。
    core::Application::Get().SetEditorHosted(true);

    window.SetWndProcHook([this](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> bool {
        if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp) != 0)
            return true;
        if (msg == WM_CLOSE) {
            RequestExit();
            return true;
        }
        return false;
    });

    /// @name エクスプローラーからの外部ファイル D&D
    /// @note AssetBrowser 側は EditorContext を読むだけの消費者なので、ここで
    /// @note        プラットフォーム層と EditorContext を接続する。
    /// @note        座標: Window はクライアント座標で通知するが、ImGui は Multi-Viewport 有効時に
    /// @note        OS デスクトップ座標で当たり判定する。ここで screen 空間へ揃える。
    const auto toScreenSpace = [this](int clientX, int clientY, float& outX, float& outY) {
        POINT p{ static_cast<LONG>(clientX), static_cast<LONG>(clientY) };
        ClientToScreen(m_hwnd, &p);
        outX = static_cast<float>(p.x);
        outY = static_cast<float>(p.y);
    };

    window.SetFileDropCallback(
        [this, toScreenSpace](const std::vector<std::string>& paths, int x, int y) {
            if (paths.empty()) return;
            /// @note 同一フレームで複数回ドロップされることはないが、未消費分は失わず連結する。
            m_ctx.droppedExternalFiles.insert(
                m_ctx.droppedExternalFiles.end(), paths.begin(), paths.end());
            toScreenSpace(x, y, m_ctx.droppedExternalFilesX, m_ctx.droppedExternalFilesY);
            m_ctx.externalDragActive = false;
        });

    window.SetFileDragOverCallback([this, toScreenSpace](int x, int y) {
        m_ctx.externalDragActive = true;
        toScreenSpace(x, y, m_ctx.externalDragX, m_ctx.externalDragY);
    });

    window.SetFileDragLeaveCallback([this]() {
        m_ctx.externalDragActive = false;
    });

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuizmo::SetImGuiContext(ImGui::GetCurrentContext());
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    /// @note 通常パネルをメイン HWND の外や別モニターへドラッグできるよう、OS Multi-Viewport を有効化する。
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    /// @note ビューポートの絵は DrawList へ直接描くので ImGui アイテムを持たず、既定 (false) だと
    /// @note        ゲーム画面全体が «ウィンドウの空き地» になる。Play 中にクリックした瞬間ウィンドウ移動が
    /// @note        始まり、カーソル中央戻しと共鳴して窓が暴走するため、掴む場所をタイトルバー/タブへ限る。
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    /// @note IniFilename は OpenProject() で projectRoot が確定してから設定する。Init() 時点
    /// @note        では projectRoot が空なので nullptr にし、最初の NewFrame() での自動ロードを防ぐ。
    io.IniFilename = nullptr;

    EditorTheme::Apply();
    /// @note 追加 OS Window とメイン Window の見た目を連続させ、境界移動時の角丸差をなくす。
    if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    imguiRenderer.ImGuiInit(m_hwnd);

    m_ctx.hotkeyManager = &m_hotkeys;
    m_ctx.operators     = &m_operators;
    m_ctx.undoStack   = &m_undoStack;
    m_ctx.buildConsole = &m_buildConsole;
    m_ctx.playMode    = &m_playMode;
    m_ctx.startAiCommandBus = [this]() { return StartAiCommandBus(); };
    m_ctx.stopAiCommandBus = [this]() { StopAiCommandBus(); };
    m_ctx.playtest      = &m_playtest;
    m_ctx.inputRecorder = &m_inputRecorder;
    m_ctx.renderer    = &renderer;
    m_ctx.imguiRenderer = &imguiRenderer;
    m_ctx.resources   = &resources;
    auto& application = core::Application::Get();
    m_ctx.memorySystem = &application.GetMemorySystem();
    m_ctx.memoryLeakDiff = &m_memoryLeakDiff;
    /// @note パネルの OnInit より前に繋ぐ (Volume Flipbook Baker パネルが共有 Baker を使う)。
    m_fluidBake = std::make_unique<FluidBakeService>();
    m_ctx.fluidBake = m_fluidBake.get();
    BindFluidInspectorContext(&m_ctx);
    /// @note EditorはApplication所有とは別のProjectRuntimeを更新するため、音響を明示的に接続する。
    m_runtime.GetSceneManager().SetAudioManager(application.GetAudioManager());
    m_terrainTool     = std::make_unique<TerrainTool>();
    m_ctx.terrainTool = m_terrainTool.get();
    m_ctx.markSceneDirty  = [this]() { MarkSceneDirty(); };
    m_ctx.requestOpenScene = [this](const std::string& path) { RequestOpenScenePath(path); };
    /// @note AI (Command Bus) からの入出力はモーダル確認を挟まない実体を直接呼ぶ。
    /// @note        未保存変更の扱いは EditorBusDispatcher 側が discardUnsaved 引数で判定する。
    m_ctx.openScenePathImmediate = [this](const std::string& path) { return OpenScenePath(path); };
    m_ctx.saveScenePathImmediate = [this](const std::string& path) {
        return path.empty() ? SaveScene() : SaveScenePath(path);
    };

    m_panels.push_back(std::make_unique<SceneHierarchyPanel>());
    m_panels.push_back(std::make_unique<InspectorPanel>());
    /// @note Animation / Material / VFX を同じ選択導線で確認できる共通プレビュー。
    m_panels.push_back(std::make_unique<PreviewPanel>());
    m_panels.push_back(std::make_unique<AnimationMaskPreviewPanel>());
    {
        /// @note .animcontroller は「開く」操作でこのパネルへ渡す (BehaviorTree と同じ方式)。
        auto animationGraph = std::make_unique<AnimationGraphPanel>();
        AnimationGraphPanel* animationGraphPtr = animationGraph.get();
        m_ctx.openAnimationGraph = [animationGraphPtr](const std::string& path) {
            animationGraphPtr->RequestOpen(path);
        };
        m_panels.push_back(std::move(animationGraph));
    }
    {
        /// @note .behaviortree はダブルクリックでこのパネルへ渡す。以前は作れるのに
        /// @note        開く手段が無く、TOML を手書きするしかなかった。
        auto behaviorTree = std::make_unique<BehaviorTreePanel>();
        BehaviorTreePanel* behaviorTreePtr = behaviorTree.get();
        m_ctx.openBehaviorTree = [behaviorTreePtr](const std::string& path) {
            behaviorTreePtr->RequestOpen(path);
        };
        m_panels.push_back(std::move(behaviorTree));
    }
    {
        /// @note .synth はダブルクリックでこのパネルへ渡す。Inspector にも同じ値は出るが、
        /// @note        プリセット / Randomize / 波形プレビューはここにしかない。
        auto sfxEditor = std::make_unique<SfxEditorPanel>();
        SfxEditorPanel* sfxEditorPtr = sfxEditor.get();
        m_ctx.openSfxEditor = [sfxEditorPtr](const std::string& path) {
            sfxEditorPtr->RequestOpen(path);
        };
        m_panels.push_back(std::move(sfxEditor));
    }
    {
        /// @note .sequence はダブルクリックでこのパネルへ渡す。尺を目で詰める面はここにしかない。
        auto sequence = std::make_unique<SequencePanel>();
        SequencePanel* sequencePtr = sequence.get();
        m_ctx.openSequence = [sequencePtr](const std::string& path) {
            sequencePtr->RequestOpen(path);
        };
        m_panels.push_back(std::move(sequence));
    }
    {
        /// @note .fluid は asset.open が ctx.requestOpenFluidEditor に積み、パネル自身が読んで消す。
        /// @note        未保存の確認をパネルが持つので、SFX Editor のような «開く» 関数は渡さない。
        auto fluidEditor = std::make_unique<FluidEditorPanel>();
        m_fluidEditorPanel = fluidEditor.get();
        m_panels.push_back(std::move(fluidEditor));
    }
    /// @note .vfx はプレハブ編集モードで開き、尺の詰めだけこのパネルが受け持つ。
    /// @note        アセットを渡す必要は無い (シーン上の VFX ルートを自分で見つける)。
    m_panels.push_back(std::make_unique<VFXTimelinePanel>());
    {
        auto spriteEditor = std::make_unique<SpriteEditorPanel>();
        SpriteEditorPanel* spriteEditorPtr = spriteEditor.get();
        m_ctx.openSpriteEditor = [spriteEditorPtr](const std::string& metaPath) {
            spriteEditorPtr->Open(metaPath);
        };
        m_panels.push_back(std::move(spriteEditor));
    }
    /// @note VFX Editor は別プロセスで専用 Preview World を所有する。Editor Scene へ Preview
    /// @note        Entity が混入する経路をプロセス境界で完全に断つため。
    {
        auto vp = std::make_unique<ViewportPanel>(ViewportPanel::Kind::Scene);
        m_sceneViewportPanel = vp.get();
        m_panels.push_back(std::move(vp));
    }
    {
        auto vp = std::make_unique<ViewportPanel>(ViewportPanel::Kind::Game);
        m_gameViewportPanel = vp.get();
        m_panels.push_back(std::move(vp));
    }
    {
        auto vp = std::make_unique<ViewportPanel>(ViewportPanel::Kind::UI);
        m_uiViewportPanel = vp.get();
        m_panels.push_back(std::move(vp));
    }
    {
        auto console = std::make_unique<ConsolePanel>(m_consoleSink);
        m_consolePanel = console.get();
        m_panels.push_back(std::move(console));
    }
    m_panels.push_back(std::make_unique<ScriptRequirementsPanel>());
    {
        auto buildOutput = std::make_unique<BuildOutputPanel>();
        m_buildOutputPanel = buildOutput.get();
        m_panels.push_back(std::move(buildOutput));
    }
    /// @note Asset Browser は Unity の Project ウィンドウと同じく複数開ける。m_panels は
    /// @note        描画ループが走査中に push_back すると再配置でポインタが無効化されるため、
    /// @note        全枚数を最初から作り、2 枚目以降は非表示で常駐させ View > Panels で出し入れする。
    for (std::size_t i = 0; i < AssetBrowserPanel::kMaxInstances; ++i) {
        auto assets = std::make_unique<AssetBrowserPanel>("Assets", i);
        if (i == 0) m_assetBrowserPanel = assets.get();
        m_assetBrowserPanels.push_back(assets.get());
        m_panels.push_back(std::move(assets));
    }
    m_statusBar = std::make_unique<StatusBar>();
    m_panels.push_back(std::make_unique<UndoHistoryPanel>());
    m_panels.push_back(std::make_unique<HotkeyEditorPanel>());
    {
        auto ps = std::make_unique<ProjectSettingsPanel>();
        m_projectSettingsPanel = ps.get();
        m_panels.push_back(std::move(ps));
    }
    {
        auto bs = std::make_unique<BuildSettingsPanel>();
        m_buildSettingsPanel = bs.get();
        m_panels.push_back(std::move(bs));
    }
    {
        auto analysis = std::make_unique<AnalysisPanel>();
        m_analysisPanel = analysis.get();
        m_panels.push_back(std::move(analysis));
    }
    {
        auto viewer = std::make_unique<RenderPassViewerPanel>();
        m_renderPassViewerPanel = viewer.get();
        m_panels.push_back(std::move(viewer));
    }
    {
        auto mapEditor = std::make_unique<MapEditorPanel>();
        m_mapEditorPanel = mapEditor.get();
        m_panels.push_back(std::move(mapEditor));
    }
    m_panels.push_back(std::make_unique<DependencyViewPanel>());
    {
        auto iblBake = std::make_unique<IblBakePanel>();
        m_iblBakePanel = iblBake.get();
        m_panels.push_back(std::move(iblBake));
    }
    {
        auto volumeFlipbook = std::make_unique<VolumeFlipbookBakePanel>();
        m_volumeFlipbookBakePanel = volumeFlipbook.get();
        m_panels.push_back(std::move(volumeFlipbook));
    }
    {
        auto navigation = std::make_unique<NavigationPanel>();
        m_navigationPanel = navigation.get();
        m_panels.push_back(std::move(navigation));
    }
    {
        auto maintenance = std::make_unique<AssetMaintenancePanel>();
        m_assetMaintenancePanel = maintenance.get();
        m_panels.push_back(std::move(maintenance));
    }
    {
        auto aiSettings = std::make_unique<AiSettingsPanel>();
        m_aiSettingsPanel = aiSettings.get();
        m_panels.push_back(std::move(aiSettings));
    }

    for (auto& panel : m_panels) {
        panel->visible = panel->GetDefaultVisibility();
        panel->OnInit(m_ctx);
    }

    /// @note 操作の登録が先。ホットキーは operator id へキーを割り当てるだけなので、
    /// @note        レジストリが空だと 1 つも解決できない。
    RegisterBuiltinOperators();
    RegisterDefaultHotkeys();
    /// @note 保存済みオーバーライドの適用は OpenProject()。ここではまだ設定を読んでいない。

    /// @note 初回 RT をウィンドウサイズで生成する
    m_sceneViewportRT = resources.CreateRenderTarget(window.GetWidth(), window.GetHeight());
    m_gameViewportRT  = resources.CreateRenderTarget(window.GetWidth(), window.GetHeight());
    if (m_sceneViewportPanel) {
        m_sceneViewportPanel->hdrRT     = m_sceneViewportRT;
        m_sceneViewportPanel->renderer  = &renderer;
        m_sceneViewportPanel->resources = &resources;
    }
    if (m_gameViewportPanel) {
        m_gameViewportPanel->hdrRT     = m_gameViewportRT;
        m_gameViewportPanel->renderer  = &renderer;
        m_gameViewportPanel->resources = &resources;
    }
    if (m_uiViewportPanel) {
        /// @note UI Viewport は Game View の完成フレームを背景として共有する。UI 専用 RT を
        /// @note        別描画すると Clear 順や RenderGraph 経路の差で青い空 RT が表示されるため、
        /// @note        編集用ガイドとギズモだけを ImGui 側で重ねる。
        m_uiViewportPanel->hdrRT     = m_gameViewportRT;
        m_uiViewportPanel->renderer  = &renderer;
        m_uiViewportPanel->resources = &resources;
    }

    /// @note シーンはここで生成し activeScene にバインドする。
    /// @note        OpenProject() が activeScene を参照するため Init() で確立しておく必要がある。
    m_scene = std::make_unique<scene::Scene>();
    m_ctx.activeScene = m_scene.get();
    m_ctx.editScene   = m_scene.get();
    /// @note AI(EditorBusDispatcher)のphysicsクエリが参照するProjectRuntimeを共有する。EditorAppが所有。
    m_ctx.runtime = &m_runtime;
    /// @note AIのconsole.logsクエリが読むログシンクを共有する。EditorAppが所有。
    m_ctx.consoleSink = &m_consoleSink;
    /// @note VFX Preview は編集 Scene と別の SceneManager で駆動し、生成物を Scene 保存・Undo から隔離する。

    FBZZ_LOG_INFO("EditorApp init done");
    UpdateWindowTitle();
    InstallNativeMenuBar();
    return true;
}

void EditorApp::Shutdown()
{
    /// @note 接続中の AI ワーカーを Scene / Panel より先に停止し、破棄済み状態への要求を防ぐ。
    StopAiCommandBus();
    /// @note 正常終了の印。ここまで来なかった起動は次回の OpenProject が異常終了とみなす。
    ClearSessionLock();
    /// @note AI が止まった後に、レンダラーと ResourceManager が生きているうちに GPU 資源を返す。
    if (m_fluidBake) m_fluidBake->Shutdown(m_ctx);
    BindFluidInspectorContext(nullptr);

    /// @note Map Mode / Play レイアウトはパネルの visible を一時的に潰す。この直後の Map Mode
    /// @note        復帰処理で mapEditingMode が落ちるため、判定できるのは今だけ。
    CaptureNormalPanelVisibility();

    /// @note Map Mode の Dock を imgui_layout.ini へ保存すると次回起動も専用配置になる。
    /// @note        終了経路でも通常 Workspace をメモリから戻してから ImGui を破棄する。
    if (m_ctx.mapEditingMode && !m_normalLayoutIni.empty()) {
        ImGui::GetIO().IniFilename = m_normalIniFilename;
        ImGui::ClearIniSettings();
        ImGui::LoadIniSettingsFromMemory(
            m_normalLayoutIni.data(), m_normalLayoutIni.size());
        m_ctx.mapEditingMode = false;
    }

    /// @note DLL 仮想デストラクタが DLL コードを参照するため、パネル・シーンより先にアンロードする。
    m_scriptDll.Unload(m_ctx.activeScene);

    for (auto& panel : m_panels)
        panel->OnShutdown();
    ShutdownAnimationPreview();

    CaptureEditorSettingsFromContext();

    m_settings.Save(m_ctx.projectRoot + "/Assets/EditorConfig/editor_settings.toml", m_ctx.projectRoot);
    SaveProjectSettingsNow();
    m_sceneViewportRT = {};
    m_gameViewportRT  = {};
    m_imguiRenderer->ImGuiShutdown();
    ImGui::DestroyContext();
}

void EditorApp::CaptureEditorSettingsFromContext()
{
    /// @name EditorContext → EditorSettings への書き戻し
    /// @note パネルは EditorContext のライブ値を直接変更する。ここで書き戻さないと、
    /// @note        起動時に読んだ初期値がそのまま保存される。
    m_settings.showGrid           = m_ctx.showGrid;
    m_settings.gridSize           = m_ctx.gridSize;
    m_settings.snapEnabled = m_ctx.snapEnabled;
    m_settings.snapPos    = m_ctx.snapPos;
    m_settings.snapRot    = m_ctx.snapRot;
    m_settings.snapScale  = m_ctx.snapScale;
    m_settings.gizmoMode          = static_cast<int>(m_ctx.gizmoMode);
    m_settings.gizmoSpace         = static_cast<int>(m_ctx.gizmoSpace);
    m_settings.gizmoPivot         = static_cast<int>(m_ctx.gizmoPivot);
    m_settings.showLightRange     = m_ctx.showLightRange;
    m_settings.showVFXGizmos      = m_ctx.showVFXGizmos;
    m_settings.showFlowFields     = m_ctx.showFlowFields;
    m_settings.showFlowSamples    = m_ctx.showFlowSamples;
    m_settings.showPhysicsVolumes = m_ctx.showPhysicsVolumes;
    m_settings.showWaterFlow      = m_ctx.showWaterFlow;
    m_settings.showRagdoll        = m_ctx.showRagdoll;
    m_settings.showSkeleton       = m_ctx.showSkeleton;
    m_settings.skeletonSelectedOnly = m_ctx.skeletonSelectedOnly;
    m_settings.showScriptGizmos   = m_ctx.showScriptGizmos;
    m_settings.showConstraints    = m_ctx.showConstraints;
    m_settings.showRigidBodies    = m_ctx.showRigidBodies;
    m_settings.showIK             = m_ctx.showIK;
    m_settings.showSpringBones    = m_ctx.showSpringBones;
    m_settings.showAttachments    = m_ctx.showAttachments;
    m_settings.showVFXPaths       = m_ctx.showVFXPaths;
    m_settings.showTerrainBounds  = m_ctx.showTerrainBounds;
    m_settings.showLODBounds      = m_ctx.showLODBounds;
    m_settings.showSceneIcons     = m_ctx.showSceneIcons;
    m_settings.hiddenSceneIcons   = m_ctx.hiddenSceneIcons;
    m_settings.showStats          = m_ctx.showStats;
    m_settings.sceneViewOcclusionCulling = m_ctx.sceneViewOcclusionCulling;
    m_settings.hotReloadEnabled   = m_ctx.hotReloadEnabled;
    m_settings.hotReloadSound     = m_ctx.hotReloadSound;
    m_settings.autoSaveEnabled     = m_ctx.sceneAutoSaveEnabled;
    m_settings.autoSaveIntervalSec = m_ctx.sceneAutoSaveIntervalSec;
    m_settings.aiCommandBusEnabled = m_ctx.aiCommandBusEnabled;
    m_settings.developerMode       = core::DeveloperMode::Preference();
    m_settings.showTerrainTool    = m_ctx.showTerrainTool;
    m_settings.gameViewportAspect = static_cast<int>(m_ctx.gameViewportAspect);
    m_settings.playFocusMode      = static_cast<int>(m_ctx.playFocusMode);
    m_settings.cameraSpeed           = m_ctx.cameraSpeed;
    m_settings.cameraSensitivity     = m_ctx.cameraSensitivity;
    if (m_ctx.editorCamera) {
        m_settings.cameraLastPx = m_ctx.editorCamera->m_position.x;
        m_settings.cameraLastPy = m_ctx.editorCamera->m_position.y;
        m_settings.cameraLastPz = m_ctx.editorCamera->m_position.z;
        m_settings.cameraLastRx = m_ctx.editorCamera->m_rotation.x;
        m_settings.cameraLastRy = m_ctx.editorCamera->m_rotation.y;
        m_settings.cameraLastRz = m_ctx.editorCamera->m_rotation.z;
        m_settings.cameraLastRw = m_ctx.editorCamera->m_rotation.w;
        m_settings.cameraOrthographic = m_ctx.editorCamera->m_projection
                                      == renderer::ProjectionMode::Orthographic;
        m_settings.cameraOrthoHeight  = m_ctx.editorCamera->m_orthoHeight;
    }
    m_settings.editorUiScale         = m_ctx.editorUiScale;
    m_settings.language              = loc::Id(loc::GetLanguage());
    /// @note アイコンサイズとツリー幅は AssetBrowserPanel::OnSaveSettings が書く
    /// @note        (ここでも書くと 2 つの書き手ができ、どちらが勝つか呼び順任せになる)。
    m_settings.assetBrowserBookmarks = m_ctx.assetBrowserBookmarks;
    m_settings.assetBrowserFolderColors.assign(m_ctx.assetBrowserFolderColors.begin(),
                                               m_ctx.assetBrowserFolderColors.end());
    m_settings.assetBrowserRecentFolderColors.assign(
        m_ctx.assetBrowserRecentFolderColors.begin(), m_ctx.assetBrowserRecentFolderColors.end());
    m_settings.defaultImportOptions  = m_ctx.defaultImportOptions;
    /// @note ホットキーバインドをオーバーライドとして保存 (デフォルト値でも全件保存して確実に復元)
    m_settings.hotkeyOverrides.clear();
    for (const auto& hk : m_hotkeys.GetHotkeys()) {
        /// @note 説明専用エントリ (マウス操作など) は割り当てを持たないので保存しない。
        if (hk.infoOnly) continue;
        EditorSettings::HotkeyOverride ov;
        /// @note 鍵は operator id を優先する。表示名を鍵にしていると、ラベルを変えた瞬間に
        /// @note        保存済みのリバインドが誰にも一致せず黙って既定へ戻ってしまう。
        ov.name  = hk.operatorId.empty() ? hk.name : hk.operatorId;
        ov.key   = hk.imguiKey;
        ov.ctrl  = hk.ctrl;
        ov.shift = hk.shift;
        ov.alt   = hk.alt;
        m_settings.hotkeyOverrides.push_back(std::move(ov));
    }
    for (std::size_t i = 0; i < 9; ++i) {
        const auto& s = m_ctx.cameraBookmarks[i];
        auto& d = m_settings.cameraBookmarks[i];
        d.valid = s.valid;
        d.px = s.position.x; d.py = s.position.y; d.pz = s.position.z;
        d.rx = s.rotation.x; d.ry = s.rotation.y;
        d.rz = s.rotation.z; d.rw = s.rotation.w;
    }
    m_settings.mapHierarchyFilter = m_ctx.mapHierarchyFilter;
    m_settings.mapInspectorFilter = m_ctx.mapInspectorFilter;
    m_settings.mapActiveTool      = static_cast<int>(m_ctx.mapActiveTool);
    m_settings.showGeneratedObjects   = m_ctx.showGeneratedObjects;
    m_settings.surfaceSnapAlignToNormal = m_ctx.surfaceSnapAlignToNormal;
    if (m_terrainTool) {
        const auto b = m_terrainTool->GetBrush();
        m_settings.terrainBrushRadius   = b.radius;
        m_settings.terrainBrushStrength = b.strength;
        m_settings.terrainBrushFalloff  = static_cast<int>(b.falloff);
        m_settings.terrainSculptMode    = static_cast<int>(m_terrainTool->GetSculptMode());
        m_settings.terrainPaintLayer    = m_terrainTool->GetPaintLayer();
    }

    /// @note パネル固有の設定 (Console のフィルター、Asset Browser の表示モード等) を回収する。
    for (const auto& panel : m_panels)
        panel->OnSaveSettings(m_settings);

    /// @note Inspector 折り畳み状態を ImGui StateStorage から回収して設定に書き戻す。
    /// @note        ImGuiStorage は key → 共用体 (int/float/void*) の平坦な表で型を覚えていないので、
    /// @note        全件を舐めるとカード本文の高さ (SetFloat) まで 0/1 の int へ潰れる。
    /// @note        ComponentHeader が名乗り出た ID だけを保存対象にする。
    /// @note        前回値を土台にするのは、このセッションで一度も表示しなかったカードを落とさないため。
    if (ImGuiWindow* win = ImGui::FindWindowByName("Inspector")) {
        std::unordered_map<ImGuiID, bool> merged;
        for (const auto& [key, open] : m_ctx.inspectorSectionState) merged[key] = open;
        for (const ImGuiID id : widgets::ComponentHeaderStateIds())
            merged[id] = win->StateStorage.GetInt(id, 0) != 0;

        m_settings.inspectorSectionState.assign(merged.begin(), merged.end());
        /// @note TOML の差分を安定させる (毎回並びが変わると保存のたびに全行が変更扱いになる)。
        std::sort(m_settings.inspectorSectionState.begin(),
                  m_settings.inspectorSectionState.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
    }

    /// @note Debug メニュー - レンダリングオーバーレイ
    m_settings.showColliders        = m_ctx.projectSettings.render.showColliders;
    m_settings.showUIRects          = m_ctx.projectSettings.render.showUIRects;
    m_settings.showTerrainCollision = m_ctx.projectSettings.render.showTerrainCollision;
    m_settings.showDecalBounds      = m_ctx.projectSettings.render.showDecalBounds;
    m_settings.showNavMesh          = m_ctx.projectSettings.render.showNavMesh;
    m_settings.showNavSensors       = m_ctx.projectSettings.render.showNavSensors;
    m_settings.navMeshDrawMode      = static_cast<int>(m_ctx.projectSettings.render.navMeshDrawMode);
    m_settings.navMeshDrawDistance  = m_ctx.projectSettings.render.navMeshDrawDistance;
    m_settings.viewMode = static_cast<int>(m_ctx.projectSettings.render.viewMode);
}

void EditorApp::PersistEditorSettings()
{
    if (m_ctx.projectRoot.empty()) return;

    CaptureEditorSettingsFromContext();
    m_settings.Save(m_ctx.projectRoot + "/Assets/EditorConfig/editor_settings.toml",
                    m_ctx.projectRoot);
}

bool EditorApp::OpenProject(const std::string& projectRoot, const std::string& projectSettingsPath, const std::string& scenePath)
{
    if (!m_ctx.activeScene || !m_resources) return false;

    m_projectRoot      = projectRoot;
    m_ctx.projectRoot  = projectRoot;
    m_ctx.editorSceneState.Clear();

    /// @name EditorConfig を Assets/EditorConfig/ からロード
    /// @note Init() では projectRoot が未確定なのでここで読む。lastScenePath を OpenProject 内で
    /// @note        参照するため、他の初期化より前に済ませる。
    {
        const std::string configDir = projectRoot + "/Assets/EditorConfig";
        util::FileSystem::EnsureDirectory(configDir);
        m_settings.Load(configDir + "/editor_settings.toml", projectRoot);
        m_ctx.inspectorSectionState = m_settings.inspectorSectionState;

        /// @note EditorSettings → EditorContext への全フィールド適用。EditorSettings は TOML の
        /// @note        raw 値、EditorContext はライブ値を保持し、OpenProject で一括コピー、Shutdown で
        /// @note        逆方向に書き戻す。
        m_ctx.showGrid           = m_settings.showGrid;
        m_ctx.gridSize           = m_settings.gridSize;
        m_ctx.snapEnabled = m_settings.snapEnabled;
        m_ctx.snapPos     = m_settings.snapPos;
        m_ctx.snapRot     = m_settings.snapRot;
        m_ctx.snapScale   = m_settings.snapScale;
        /// @note TOML は手で書き換えられ、enum の値を減らした版で保存したファイルも残る。範囲外の
        /// @note        int を enum へキャストすると switch が既定へ落ちず原因不明の挙動になるため、
        /// @note        読み口で clamp して潰す。
        m_ctx.gizmoMode          = static_cast<EditorContext::GizmoMode>(std::clamp(m_settings.gizmoMode, 0, 2));
        m_ctx.gizmoSpace         = static_cast<EditorContext::GizmoSpace>(std::clamp(m_settings.gizmoSpace, 0, 1));
        m_ctx.gizmoPivot         = static_cast<EditorContext::GizmoPivot>(std::clamp(m_settings.gizmoPivot, 0, 1));
        m_ctx.showLightRange     = m_settings.showLightRange;
        m_ctx.showVFXGizmos      = m_settings.showVFXGizmos;
        m_ctx.showFlowFields     = m_settings.showFlowFields;
        m_ctx.showFlowSamples    = m_settings.showFlowSamples;
        m_ctx.showPhysicsVolumes = m_settings.showPhysicsVolumes;
        m_ctx.showWaterFlow      = m_settings.showWaterFlow;
        m_ctx.showRagdoll        = m_settings.showRagdoll;
        m_ctx.showSkeleton       = m_settings.showSkeleton;
        m_ctx.skeletonSelectedOnly = m_settings.skeletonSelectedOnly;
        m_ctx.showScriptGizmos   = m_settings.showScriptGizmos;
        m_ctx.showConstraints    = m_settings.showConstraints;
        m_ctx.showRigidBodies    = m_settings.showRigidBodies;
        m_ctx.showIK             = m_settings.showIK;
        m_ctx.showSpringBones    = m_settings.showSpringBones;
        m_ctx.showAttachments    = m_settings.showAttachments;
        m_ctx.showVFXPaths       = m_settings.showVFXPaths;
        m_ctx.showTerrainBounds  = m_settings.showTerrainBounds;
        m_ctx.showLODBounds      = m_settings.showLODBounds;
        m_ctx.showSceneIcons     = m_settings.showSceneIcons;
        m_ctx.hiddenSceneIcons   = m_settings.hiddenSceneIcons;
        m_ctx.showStats          = m_settings.showStats;
        m_ctx.sceneViewOcclusionCulling = m_settings.sceneViewOcclusionCulling;
        m_ctx.hotReloadEnabled   = m_settings.hotReloadEnabled;
        m_ctx.hotReloadSound     = m_settings.hotReloadSound;
        m_ctx.sceneAutoSaveEnabled     = m_settings.autoSaveEnabled;
        m_ctx.sceneAutoSaveIntervalSec = m_settings.autoSaveIntervalSec;
        m_ctx.aiCommandBusEnabled = m_settings.aiCommandBusEnabled;
        core::DeveloperMode::SetPreference(m_settings.developerMode);
        m_ctx.showTerrainTool    = m_settings.showTerrainTool;
        m_ctx.gameViewportAspect = static_cast<EditorContext::GameViewportAspect>(
            std::clamp(m_settings.gameViewportAspect,
                       0, static_cast<int>(EditorContext::GameViewportAspect::iPhoneLandscape)));
        m_ctx.playFocusMode      = static_cast<EditorContext::PlayFocusMode>(
            std::clamp(m_settings.playFocusMode,
                       0, static_cast<int>(EditorContext::PlayFocusMode::Unfocused)));
        m_ctx.surfaceSnapAlignToNormal = m_settings.surfaceSnapAlignToNormal;
        m_ctx.showGeneratedObjects     = m_settings.showGeneratedObjects;
        m_ctx.mapActiveTool      = static_cast<EditorContext::MapTool>(
            std::clamp(m_settings.mapActiveTool,
                       0, static_cast<int>(EditorContext::MapTool::TerrainHole)));
        m_ctx.cameraSpeed        = m_settings.cameraSpeed;
        m_ctx.cameraSensitivity  = m_settings.cameraSensitivity;
        if (m_ctx.editorCamera) {
            m_ctx.editorCamera->m_orthoHeight = m_settings.cameraOrthoHeight;
            m_ctx.editorCamera->m_projection  = m_settings.cameraOrthographic
                ? renderer::ProjectionMode::Orthographic
                : renderer::ProjectionMode::Perspective;
            /// @note Teleport 経由にして DebugCamera の yaw / pitch / pivot まで揃える。
            /// @note        直書きだけだと、復元直後の 1 回目のオービットで視点が飛ぶ。
            m_debugCamera.Teleport(
                { m_settings.cameraLastPx, m_settings.cameraLastPy, m_settings.cameraLastPz },
                { m_settings.cameraLastRx, m_settings.cameraLastRy, m_settings.cameraLastRz,
                  m_settings.cameraLastRw });
        }
        m_ctx.editorUiScale = m_settings.editorUiScale;
        /// @note ロードしたスケールを即適用
        EditorTheme::SetUiScale(m_ctx.editorUiScale);
        /// @note 表示言語。辞書の作り直しだけなので、UI スケールと違ってフォントには触らない
        /// @note        (EditorTheme が日本語グリフを最初から merge している)。
        loc::SetLanguage(loc::FromId(m_settings.language));
        m_ctx.assetBrowserBookmarks = m_settings.assetBrowserBookmarks;
        m_ctx.assetBrowserFolderColors.clear();
        for (const auto& [path, color] : m_settings.assetBrowserFolderColors)
            m_ctx.assetBrowserFolderColors.emplace(path, color);
        m_ctx.assetBrowserRecentFolderColors.assign(
            m_settings.assetBrowserRecentFolderColors.begin(),
            m_settings.assetBrowserRecentFolderColors.end());
        m_ctx.defaultImportOptions  = m_settings.defaultImportOptions;
        for (std::size_t i = 0; i < 9; ++i) {
            const auto& s = m_settings.cameraBookmarks[i];
            auto& d = m_ctx.cameraBookmarks[i];
            d.valid      = s.valid;
            d.position   = { s.px, s.py, s.pz };
            d.rotation   = { s.rx, s.ry, s.rz, s.rw };
        }
        m_ctx.mapHierarchyFilter = m_settings.mapHierarchyFilter;
        m_ctx.mapInspectorFilter = m_settings.mapInspectorFilter;
        if (m_terrainTool) {
            m_terrainTool->SetBrush(
                m_settings.terrainBrushRadius,
                m_settings.terrainBrushStrength,
                static_cast<TerrainTool::FalloffType>(
                    std::clamp(m_settings.terrainBrushFalloff,
                               0, static_cast<int>(TerrainFalloff::Gaussian))));
            m_terrainTool->SetSculptMode(
                static_cast<TerrainTool::SculptMode>(
                    std::clamp(m_settings.terrainSculptMode,
                               0, static_cast<int>(TerrainSculptOp::Terrace))));
            m_terrainTool->SetPaintLayer(m_settings.terrainPaintLayer);
        }

        /// @note 設定を読むのはこの OpenProject なので、適用もここで行う。Init 時点の
        /// @note        m_settings は既定値のままで、上書きしたキーが戻らない。
        for (const auto& ov : m_settings.hotkeyOverrides)
            m_hotkeys.Rebind(ov.name, ov.key, ov.ctrl, ov.shift, ov.alt);

        /// @note ImGui レイアウトファイルも同ディレクトリに配置する。io.IniFilename は const
        /// @note        char* を保持するため、メンバ文字列のアドレスを渡して寿命を保証する。
        m_imguiIniPath = configDir + "/imgui_layout.ini";
        ImGui::GetIO().IniFilename = m_imguiIniPath.c_str();
        if (!util::FileSystem::Exists(m_imguiIniPath)) {
            /// @note LoadIniSettingsFromMemory は SettingsLoaded フラグを立てるため、
            /// @note        その後の NewFrame() でファイルから上書きされることはない。
            ImGui::LoadIniSettingsFromMemory(DEFAULT_IMGUI_LAYOUT);
        }

        SceneIO::SetProjectRoot(projectRoot);
    }

    /// @note 参照を失った import 生成物を片付ける。ルートを配ると Asset Browser が未 import の
    /// @note        走査を始めるため、それより前に孤児を落としておかないと数え直しになる。
    if (m_settings.sweepOrphanedBakedOnOpen) {
        const auto sweep = asset::AssetDatabase::SweepOrphanedBaked(/*dryRun=*/false);
        if (sweep.aborted) {
            FBZZ_LOG_WARN("EditorApp: baked sweep skipped (%s)", sweep.abortReason.c_str());
        } else if (sweep.removed > 0) {
            /// @note 生成物と fingerprint はキーが同じ guid。片方だけ残すと «記録はあるのに
            /// @note        焼き上がりが無い» 状態になり、再インポートの判定が狂う。
            ImportCacheStore::Forget(sweep.removedGuids);
            Toast::Info("Cleaned " + std::to_string(sweep.removed)
                        + " unused import cache folders ("
                        + std::to_string(sweep.bytesFreed / (1024 * 1024)) + " MB)");
        }
    }

    LoadRuntimeBuildMetadata(m_ctx);
    /// @note ルートは開いている枚数ぶん全部に配る。1 枚目だけだと 2 枚目以降が前のプロジェクトの
    /// @note        Assets を指したままになり、消えたパスを一覧しようとする。
    if (!m_projectRoot.empty()) {
        for (AssetBrowserPanel* browser : m_assetBrowserPanels)
            browser->SetRootPath(m_projectRoot + "/Assets");
    }

    /// @note パネル固有の設定を適用する。Asset Browser は前回のフォルダをルート配下かどうかで
    /// @note        検証するため、SetRootPath より後でないと必ず捨てられる。
    for (auto& panel : m_panels)
        panel->OnLoadSettings(m_settings);
    RestorePanelVisibility();

    if (!projectSettingsPath.empty()) {
        m_projectSettingsPath = projectSettingsPath;
        m_ctx.projectSettings.Load(m_projectSettingsPath);
        m_ctx.projectSettingsPath = m_projectSettingsPath;
        Time::targetFps = m_ctx.projectSettings.app.targetFps;
        if (auto* audioManager = core::Application::Get().GetAudioManager()) {
            audioManager->SetVoiceLimit(
                static_cast<size_t>(m_ctx.projectSettings.audio.voiceLimit));
            audioManager->ApplyBusLayout(m_ctx.projectSettings.audio.BuildBusLayout());
        }
    }

    /// @note Debug メニューのレンダリング設定はエディター個人設定であり projectSettings より
    /// @note        優先する。projectSettings.Load() の後に上書きし、プロジェクト共有値に左右されない。
    m_ctx.projectSettings.render.showColliders        = m_settings.showColliders;
    m_ctx.projectSettings.render.showUIRects          = m_settings.showUIRects;
    m_ctx.projectSettings.render.showTerrainCollision = m_settings.showTerrainCollision;
    m_ctx.projectSettings.render.showDecalBounds      = m_settings.showDecalBounds;
    m_ctx.projectSettings.render.showNavMesh          = m_settings.showNavMesh;
    m_ctx.projectSettings.render.showNavSensors       = m_settings.showNavSensors;
    m_ctx.projectSettings.render.navMeshDrawMode =
        static_cast<renderer::NavMeshDrawMode>(std::clamp(m_settings.navMeshDrawMode, 0, 4));
    m_ctx.projectSettings.render.navMeshDrawDistance  = m_settings.navMeshDrawDistance;
    m_ctx.projectSettings.render.viewMode = static_cast<renderer::ViewMode>(m_settings.viewMode);

    /// @note 上書きの後で «保存済み» を採る。ファイルの本文ではなく今の値を基準にしないと、
    /// @note        開いただけで (表記揺れ・個人設定の上書きで) 未保存扱いになり即座に書き直す。
    m_projectSettingsSavedToml = m_ctx.projectSettings.ToToml();
    m_projectSettingsLastToml  = m_projectSettingsSavedToml;
    m_projectSettingsFailedToml.clear();
    m_ctx.projectSettingsSaveState = EditorContext::SettingsSaveState::Saved;
    m_ctx.projectSettingsSavedClock.clear();

    /// @note SceneIO::Load() は ScriptComponent の復元で ScriptFactory を引く。
    /// @note        必ずシーンロードより前に DLL を読むこと (未ロードだとスクリプトが生成されない)。
    InitScriptDll();

    /// @note PrefabSerializer は Editor 側にあり Engine から直接呼べないので、実装を注入する。
    /// @note        PrefabRef::path は "Assets/..." 起点の相対パスなので絶対パスへ直してから渡す。
    const std::string capturedRoot = projectRoot;
    scene::Script::SetPrefabInstantiationCallback([capturedRoot](scene::Scene& s, const std::string& path,
                                                   std::vector<scene::EntityID>& roots) {
        const std::string diskPath = ToProjectAssetDiskPath(capturedRoot, path);
        return PrefabSerializer::Instantiate(s, diskPath, roots);
    });

    std::string sceneToOpen = scenePath;
    const std::string lastScenePath = ResolveScenePathForProject(projectRoot, m_settings.lastScenePath);
    if (IsScenePathInsideProject(projectRoot, lastScenePath)) {
        sceneToOpen = lastScenePath;
    } else if (sceneToOpen.empty() && !m_ctx.projectSettings.game.runtime.startScene.empty()) {
        sceneToOpen = ResolveScenePathForProject(projectRoot, m_ctx.projectSettings.game.runtime.startScene);
    }

    if (!sceneToOpen.empty()) {
        if (!SceneIO::Load(*m_ctx.editScene, sceneToOpen)) {
            FBZZ_LOG_ERROR("Open project scene failed: %s", sceneToOpen.c_str());
            return false;
        }
        /// @note SceneSerializer はローカル position のみ復元し worldPosition はゼロのまま。
        /// @note        OnInit の WarmupRenderResources がスケジューラより前に描画するため、
        /// @note        ここで即時フラッシュしてロード直後の最初のフレームも正しい位置で表示する。
        scene::FlushWorldTransforms(*m_ctx.editScene);
        m_settings.lastScenePath = sceneToOpen;
        m_ctx.currentScenePath   = sceneToOpen;
        ClearEntitySelection(m_ctx);
        ApplyEditorViewStateFromSceneMeta();
        RebuildEditorUIFromScene();
        CaptureCleanScene();
    }

    /// @note 印を書く前に前回の印を見る。書いた後だと毎回 «異常終了» に見える。
    DetectCrashRecovery();
    WriteSessionLock();

    FBZZ_LOG_INFO("Opened project: %s", m_projectRoot.c_str());
    UpdateWindowTitle();

    if (m_ctx.aiCommandBusEnabled && !StartAiCommandBus()) {
        /// @note プロジェクト自体は開けるため失敗を致命扱いにせず、AI Settings から再試行可能にする。
        FBZZ_LOG_ERROR("保存済み設定から AI Command Bus を開始できませんでした");
    } else if (!m_ctx.aiCommandBusEnabled) {
        /// @note プロジェクト切替で自動開始設定が無効になった場合は、前プロジェクトの待受を残さない。
        StopAiCommandBus();
    }

    return true;
}

void EditorApp::RebuildEditorUIFromScene()
{
    /// @note UI Viewport の編集対象は EditorContext の一時状態であり、.fbzz には保存しない。
    /// @note        シーン読込直後に Scene 内の UICanvas から復元しないと、初回表示で UI 編集ガイドや
    /// @note        pick 対象が前シーンの無効 ID のままになり、Canvas をクリックするまで再構築されない。
    m_ctx.activeUICanvas = scene::EntityID::INVALID;
    if (!m_ctx.activeScene) {
        return;
    }

    for (auto& go : m_ctx.activeScene->GameObjects()) {
        auto* canvas = go.GetComponent<scene::UICanvas>();
        if (!canvas || !IsEditorUICanvas(*canvas)) {
            continue;
        }

        m_ctx.activeUICanvas = go.GetID();
        FBZZ_LOG_DEBUG("EditorUI: active Canvas restored from scene data");
        return;
    }
}

/// @note  ウィンドウタイトル

void EditorApp::UpdateWindowTitle()
{
    if (!m_hwnd) return;

    /// @note Prefab 編集モード中は編集対象がシーンではなくアセットなので、タイトルもそちらを出す。
    /// @note        タイトルバーがシーン名のままだと、編集モードに入っていることを見落として
    /// @note        「シーンを壊してしまった」と誤解する。
    const bool inPrefabEdit = m_ctx.InPrefabEditMode();
    const std::string displayName = inPrefabEdit
        ? util::FileSystem::GetFilename(m_ctx.prefabEditPath)
        : (m_ctx.currentScenePath.empty()
               ? "Untitled"
               : util::FileSystem::GetFilename(m_ctx.currentScenePath));
    const bool dirty = inPrefabEdit ? m_ctx.prefabEditDirty : m_ctx.sceneDirty;

    /// @note Play 中は「開いているシーン」と「走っているシーン」が食い違いうる。遷移したなら
    /// @note        走っている方の名前も出す。保存先は常に開いている方であることを見失わせない。
    std::string playTag;
    if (const PlayState state = m_playMode.GetState(); state != PlayState::Editor) {
        playTag = (state == PlayState::Paused) ? " [Paused" : " [Playing";
        if (!m_ctx.playSceneName.empty()) playTag += ": " + m_ctx.playSceneName;
        playTag += "]";
    }

    /// @note 変化検知のキーにはモードを含める (同名でもモードが違えば描き直す)。
    const std::string titleKey =
        (inPrefabEdit ? "prefab:" : "scene:") +
        (inPrefabEdit ? m_ctx.prefabEditPath : m_ctx.currentScenePath) + playTag;

    if (m_titleInitialized &&
        m_lastTitleDirty     == dirty &&
        m_lastTitleScenePath == titleKey)
        return;

    m_titleInitialized   = true;
    m_lastTitleDirty     = dirty;
    m_lastTitleScenePath = titleKey;

    std::string title = inPrefabEdit
        ? "FBZZ Editor - [Prefab] " + displayName
        : "FBZZ Editor - " + displayName;
    if (dirty) title += "*";
    title += playTag;
    /// @note 使用中の描画バックエンド (DirectX 11/12) をタイトルに付す。app 起動時に付けた
    /// @note        タイトルは本メソッドで上書きされるため、ここでも同じタグを付け直す。バックエンド名は
    /// @note        IRenderer 抽象越しに取得しダウンキャストしない。
    if (m_ctx.renderer)
        title += std::string(" [") + m_ctx.renderer->GetBackendName() + "]";
    if (m_window)
        m_window->SetTitle(util::StringUtils::ToWide(title));
}

/// @note  フレーム

void EditorApp::BeginFrame()
{
    /// @note Play/Pause 中のランタイム変化を Editor の Undo 履歴へ混入させない。
    m_undoStack.SetRecordingEnabled(m_playMode.IsInEditor());

    /// @note Viewport パネルサイズが前フレームで変わった場合は RT を再生成する。
    /// @note        main ループの「シーン描画」より前に呼ぶことで、RT のサイズが確定した状態で
    /// @note        シーンをレンダリングでき、リサイズ直後のフレームで古い解像度の画像が表示されるのを防ぐ。
    {
        FBZZ_PROFILE_SCOPE("EditorBegin::ResizeViewportRTs");
        ResizeViewportRTsIfNeeded();
    }

    {
        FBZZ_PROFILE_SCOPE("EditorBegin::ImGuiNewFrame");
        m_imguiRenderer->ImGuiNewFrame();
        ImGui::NewFrame();
    }

    UpdatePlayCursorControls();

    /// @note Play/Pause の識別色もブランドテーマ側へ集約し、通常時に旧配色を復元しない。
    EditorTheme::ApplyWorkspaceTint(
        m_playMode.IsPlaying() ? WorkspaceTint::Playing :
        m_playMode.IsPaused()  ? WorkspaceTint::Paused  :
                                 WorkspaceTint::Editor);

    ImGuizmo::BeginFrame();
    {
        FBZZ_PROFILE_SCOPE("EditorBegin::Hotkeys");
        /// @note ドキュメントを編集する面にフォーカスがある間、Ctrl+S はその面のものにする。
        /// @note        ProcessInput はパネル描画より前に走り SuppressOperatorThisFrame は «次» フレーム
        /// @note        にしか効かないため、前フレームで確定済みのフォーカス状態をここで先に使う。
        if (m_ctx.PanelScopeFocused(HotkeyScope::FluidEditor))
            m_hotkeys.SuppressOperatorThisFrame("scene.save");
        m_hotkeys.ProcessInput();
    }
    {
        FBZZ_PROFILE_SCOPE("EditorBegin::HotReload");
        CheckHotReload();
        CheckScriptDirtyAndRebuild();
        CheckHlslDirty();
    }
    {
        FBZZ_PROFILE_SCOPE("EditorBegin::SceneDirty");
        RefreshSceneDirtyState(false);
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoDocking             |
        ImGuiWindowFlags_NoTitleBar            |
        ImGuiWindowFlags_NoCollapse            |
        ImGuiWindowFlags_NoResize              |
        ImGuiWindowFlags_NoMove                |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus            |
        ImGuiWindowFlags_NoScrollbar           |
        ImGuiWindowFlags_NoScrollWithMouse;

    {
        FBZZ_PROFILE_SCOPE("EditorBegin::DockSpace");
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    { 0.0f, 0.0f });
        ImGui::Begin("##DockSpaceHost", nullptr, hostFlags);
        ImGui::PopStyleVar(3);

        BuildPlayToolbar(m_ctx);
        /// @note Prefab 編集モードの帯はツールバー直下・DockSpace の上に置く。パネルの中に
        /// @note        埋めるとレイアウト次第で見えなくなり、「今アセットを直している」という
        /// @note        一番外してはいけない前提が伝わらない。
        DrawPrefabEditBar(m_ctx);
        DrawSceneReloadBar(m_ctx);
        DrawBuildNotificationBar(m_ctx);
        DrawGuidConflictBar(m_ctx);
        DrawAboutDialog();

        ImGuiID dockId = ImGui::GetID("MainDockSpace");
        ProcessMapEditingModeTransition(static_cast<uint32_t>(dockId));
        ProcessPlayViewportLayoutTransition(static_cast<uint32_t>(dockId));
        /// @note StatusBar一段分を残してDockSpaceを描き、ドロワーボタンを常に画面下端へ置く。
        const float statusBarHeight = ImGui::GetFrameHeight() + 2.0f;
        ImGui::DockSpace(dockId, { 0.0f, -statusBarHeight },
            ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_AutoHideTabBar);
        m_statusBar->Draw(m_ctx,
            m_assetBrowserPanel != nullptr ? &m_assetBrowserPanel->visible : nullptr,
            m_consolePanel != nullptr ? &m_consolePanel->visible : nullptr);

        /// @note ModalDialog::OpenPopup は ImGui ウィンドウ (Begin/End) のスコープ内でしか機能しない。
        /// @note        DockSpaceHost ウィンドウの内側に置くことでその制約を満たす。
        ModalDialog::OnRender();

        ImGui::End();
    }
}

void EditorApp::RenderPanels(EditorContext& ctx)
{
    /// @note Play ボタンは BeginFrame 内で状態を変えるため、同じフレームの Panel 描画前にも同期する。
    m_undoStack.SetRecordingEnabled(m_playMode.IsInEditor());

    /// @note HotkeyManager の scope 判定に使うフォーカス状態を落とし、パネルに立て直させる。
    /// @note        非表示のパネルは OnRender が呼ばれず申告できないので、落とさないと
    /// @note        「閉じたパネルにフォーカスがある」ままキーが効き続ける。
    ctx.focusedPanelScope    = HotkeyScope::None;
    ctx.viewportFocused      = false;
    ctx.sceneViewportHovered = false;
    ctx.gameViewportRectValid = false;
    ctx.gameViewportFocused   = false;

    /// @note Build Output パネルの表示要求を処理する (StatusBar クリック / 失敗通知バーの Show)。
    if (m_buildOutputPanel) {
        if (ctx.requestFocusBuildError) {
            /// @note 表示 ON + 最初のエラーへスクロール
            m_buildOutputPanel->RequestFocusFirstError();
            ctx.requestFocusBuildError = false;
            ctx.requestOpenBuildOutput = false;
            ImGui::SetWindowFocus(m_buildOutputPanel->GetWindowName());
        } else if (ctx.requestOpenBuildOutput) {
            m_buildOutputPanel->visible = true;
            ctx.requestOpenBuildOutput = false;
            ImGui::SetWindowFocus(m_buildOutputPanel->GetWindowName());
        }
    }

    for (auto& panel : m_panels) {
        if (!panel->visible) continue;

        /// @note RenderPanels 全体の計測だけでは、重いパネルを特定できない。パネル名は Panel
        /// @note        の生存中有効なため、そのまま Profiler marker として利用する。
        const profiler::ProfileScope panelScope(
            profiler::ProfilerMarker(panel->GetWindowName(), "Editor Panels"));
        panel->OnRender(ctx);
    }

    if (ctx.requestEditorSettingsSave) {
        ctx.requestEditorSettingsSave = false;
        PersistEditorSettings();
    }
    TickProjectSettingsAutoSave(ImGui::GetIO().DeltaTime);
    TickAutoSave(ImGui::GetIO().DeltaTime);
    DrawAutoSaveNotice();
    ProcessCrashRecovery();

    /// @note アセット参照欄 (widgets::AssetPathField) のクリック → 参照先アセットを辿る。
    /// @note        ・Inspector の表示対象を移す — ここで即座に確定させる
    /// @note        ・Asset Browser を該当フォルダへ移動して ping する — 次フレームのパネルが消費する
    /// @note        widgets 層は EditorContext を知らないので要求は静的チャネルに積まれる。
    /// @note        パネル描画の後に 1 回取り出せば、どのパネルの参照欄から出た要求も同じ経路で届く。
    if (widgets::AssetRevealRequest reveal; widgets::ConsumeAssetRevealRequest(reveal)) {
        /// @note Inspector の表示対象の切り替えはここで完結させる。
        /// @note        Asset Browser へ任せると、非アクティブなタブでは OnRenderContent 自体が
        /// @note        呼ばれず (Inspector と同じドックノードだと常にこの状態)、一覧へ辿れない参照でも
        /// @note        選択を書く前に return するため、参照欄をダブルクリックしても何も起きない。
        /// @note        「参照を辿って中身を見る」は Inspector 単体で成立すべき動線。
        if (reveal.selectInInspector) {
            /// @note Sprite 参照 (`` "<画像>::sprite::<id>" ``) は元画像を Inspector へ出す。
            /// @note        ParseSpriteReference は false のときも logicalPath へ元の文字列を書くため、
            /// @note        戻り値を見る必要はない (AssetBrowserPanel::HandleRevealRequest と同じ扱い)。
            std::string logicalPath;
            std::string spriteToken;
            (void)asset::ParseSpriteReference(reveal.path, logicalPath, spriteToken);
            std::string absolute = util::FileSystem::NormalizePathSeparators(
                asset::AssetManager::ResolveAssetPath(logicalPath));
            if (!absolute.empty() && util::FileSystem::Exists(absolute))
                SelectAsset(ctx, std::move(absolute));
        }

        ctx.requestRevealAssetPath   = std::move(reveal.path);
        ctx.requestRevealAssetSelect = reveal.selectInInspector;
        for (auto& panel : m_panels) {
            if (std::strcmp(panel->GetWindowName(), "Asset Browser") != 0) continue;
            /// @note 閉じている / 非アクティブなタブに埋もれていると「示した」ことにならないため、
            /// @note        表示 ON + タブを手前へ出すところまでを 1 操作で済ませる。
            panel->visible = true;
            ImGui::SetWindowFocus(panel->GetWindowName());
            break;
        }
    }

    /// @name 全パネルの上に重ねるオーバーレイ
    /// @note パネルより後に描くのは、モーダル的なオーバーレイを最前面に出すため。
    DrawCommandPalette(ctx);
    DrawShortcutsOverlay(ctx);
    /// @note 選択の変化を毎フレーム拾って往復ヒストリへ積む (Alt+←/→ の材料)。
    RecordSelectionHistory();

    if (ctx.requestOpenProjectSettings) {
        if (m_projectSettingsPanel) m_projectSettingsPanel->visible = true;
        ctx.requestOpenProjectSettings = false;
    }

    if (ctx.requestOpenBuildSettings) {
        ctx.requestOpenBuildSettings = false;
        if (m_buildSettingsPanel) {
            m_buildSettingsPanel->visible = true;
            ImGui::SetNextWindowFocus();
        }
    }

    if (ctx.requestOpenAnalysis) {
        ctx.requestOpenAnalysis = false;
        if (m_analysisPanel) {
            m_analysisPanel->visible = true;
            ImGui::SetNextWindowFocus();
        }
    }

    /// @note requestOpenAnimationGraph は「窓を出す」だけ。どの .animcontroller を開くかは
    /// @note        ctx.openAnimationGraph(path) で呼び出し元が明示する。ここで兼ねると Inspector が
    /// @note        渡したパスを selectedAssetPath で上書きしてしまう。
    if (ctx.requestOpenAnimationGraph) {
        ctx.requestOpenAnimationGraph = false;
        for (auto& panel : m_panels) {
            if (std::strcmp(panel->GetWindowName(), "Animation Graph") == 0) {
                panel->visible = true;
                ImGui::SetNextWindowFocus();
                break;
            }
        }
    }

    if (ctx.requestOpenBehaviorTree) {
        ctx.requestOpenBehaviorTree = false;
        if (ctx.openBehaviorTree) ctx.openBehaviorTree(ctx.selectedAssetPath);
        for (auto& panel : m_panels) {
            if (std::strcmp(panel->GetWindowName(), "Behavior Tree") == 0) {
                panel->visible = true;
                ImGui::SetNextWindowFocus();
                break;
            }
        }
    }

    /// @note 要求を読んで消すのはパネル自身 (OnBeforeBegin)。ここは閉じていれば開くだけ。
    if (!ctx.requestOpenFluidEditor.empty() && m_fluidEditorPanel != nullptr && !m_fluidEditorPanel->visible)
        InvokePanelFocus(m_fluidEditorPanel);

    if (ctx.requestOpenSfxEditor) {
        ctx.requestOpenSfxEditor = false;
        if (ctx.openSfxEditor) ctx.openSfxEditor(ctx.selectedAssetPath);
        for (auto& panel : m_panels) {
            if (std::strcmp(panel->GetWindowName(), "SFX Editor") == 0) {
                panel->visible = true;
                ImGui::SetNextWindowFocus();
                break;
            }
        }
    }

    /// @note requestOpenAnimationGraph と同じく「窓を出す」だけ。どの .sequence を開くかは
    /// @note        ctx.openSequence(path) で呼び出し元が明示済み。
    if (ctx.requestOpenSequence) {
        ctx.requestOpenSequence = false;
        for (auto& panel : m_panels) {
            if (std::strcmp(panel->GetWindowName(), "Sequence") == 0) {
                panel->visible = true;
                ImGui::SetNextWindowFocus();
                break;
            }
        }
    }


    /// @note すべての通常ウィンドウの後に呼ぶことで、オーバーレイが最前面に描画される。
    /// @note        IsActive() == false のときは何もしないのでパネルのないフレームでも安全。
    {
        FBZZ_PROFILE_SCOPE("EditorPanel::TaskOverlay");
        EditorTaskOverlay::Render();
    }
}

void EditorApp::ProcessMapEditingModeTransition(uint32_t dockId)
{
    if (m_ctx.mapEditingMode && !m_playMode.IsInEditor()) {
        ExitMapEditingMode(dockId);
        m_ctx.requestMapEditingModeToggle = false;
        return;
    }

    if (!m_ctx.requestMapEditingModeToggle)
        return;

    m_ctx.requestMapEditingModeToggle = false;
    if (m_ctx.mapEditingMode)
        ExitMapEditingMode(dockId);
    else if (m_playMode.IsInEditor() && m_ctx.activeScene)
        EnterMapEditingMode(dockId);
}

void EditorApp::ProcessPlayViewportLayoutTransition(uint32_t dockId)
{
    const bool shouldExpandGameView =
        m_ctx.playFocusMode == EditorContext::PlayFocusMode::Maximized
        && !m_playMode.IsInEditor()
        && !m_playMode.HasPendingRestore();
    if (shouldExpandGameView && !m_playViewportLayoutActive) {
        EnterPlayViewportLayout(dockId);
    } else if (!shouldExpandGameView && m_playViewportLayoutActive) {
        ExitPlayViewportLayout(dockId);
    }
}

void EditorApp::EnterMapEditingMode(uint32_t dockId)
{
    if (m_ctx.mapEditingMode)
        return;

    size_t iniSize = 0;
    const char* iniData = ImGui::SaveIniSettingsToMemory(&iniSize);
    m_normalLayoutIni.assign(iniData, iniSize);

    m_normalPanelVisibility.clear();
    m_normalPanelVisibility.reserve(m_panels.size());
    for (const auto& panel : m_panels)
        m_normalPanelVisibility.push_back(panel->visible);

    m_terrainToolWasActive = m_terrainTool && m_terrainTool->IsActive();
    if (m_terrainTool)
        m_terrainToolModeBeforeMap = static_cast<int>(m_terrainTool->GetMode());

    m_ctx.mapEditingMode = true;
    m_normalIniFilename = ImGui::GetIO().IniFilename;
    ImGui::GetIO().IniFilename = nullptr;
    for (auto& panel : m_panels) {
        const char* windowName = panel->GetWindowName();
        const bool keepVisible =
            panel.get() == m_sceneViewportPanel
            || panel.get() == m_assetBrowserPanel
            || panel.get() == m_mapEditorPanel
            || std::strcmp(windowName, "Scene Hierarchy") == 0
            || std::strcmp(windowName, "Inspector") == 0
            || std::strcmp(windowName, "##statusbar") == 0;
        panel->visible = keepVisible;
    }

    BuildMapEditingLayout(dockId);
}

void EditorApp::ExitMapEditingMode(uint32_t dockId)
{
    if (!m_ctx.mapEditingMode)
        return;

    m_ctx.mapEditingMode = false;
    ImGui::GetIO().IniFilename = m_normalIniFilename;
    if (m_terrainTool) {
        m_terrainTool->SetActive(m_terrainToolWasActive);
        m_terrainTool->SetMode(
            static_cast<TerrainTool::Mode>(m_terrainToolModeBeforeMap));
    }

    if (m_normalPanelVisibility.size() == m_panels.size()) {
        for (size_t index = 0; index < m_panels.size(); ++index)
            m_panels[index]->visible = m_normalPanelVisibility[index];
    }

    ImGui::DockBuilderRemoveNode(static_cast<ImGuiID>(dockId));
    if (!m_normalLayoutIni.empty()) {
        ImGui::ClearIniSettings();
        ImGui::LoadIniSettingsFromMemory(
            m_normalLayoutIni.data(), m_normalLayoutIni.size());
    }
}

void EditorApp::EnterPlayViewportLayout(uint32_t dockId)
{
    if (m_playViewportLayoutActive)
        return;

    size_t iniSize = 0;
    const char* iniData = ImGui::SaveIniSettingsToMemory(&iniSize);
    m_playLayoutIni.assign(iniData, iniSize);

    m_playPanelVisibility.clear();
    m_playPanelVisibility.reserve(m_panels.size());
    for (const auto& panel : m_panels)
        m_playPanelVisibility.push_back(panel->visible);

    /// @note Unity の Maximize On Play に近い挙動として、Play 中だけ Game View を中央 Dock
    /// @note        全体へ広げる。Stop 時に保存済みレイアウトを復元し、ユーザーの通常レイアウトは変更しない。
    m_playViewportLayoutActive = true;
    m_playIniFilename = ImGui::GetIO().IniFilename;
    ImGui::GetIO().IniFilename = nullptr;

    for (auto& panel : m_panels)
        panel->visible = (panel.get() == m_gameViewportPanel);

    if (m_gameViewportPanel)
        m_gameViewportPanel->visible = true;
    m_ctx.requestGameViewportFocus = true;

    BuildPlayViewportLayout(dockId);
}

void EditorApp::ExitPlayViewportLayout(uint32_t dockId)
{
    if (!m_playViewportLayoutActive)
        return;

    m_playViewportLayoutActive = false;
    ImGui::GetIO().IniFilename = m_playIniFilename;

    if (m_playPanelVisibility.size() == m_panels.size()) {
        for (size_t index = 0; index < m_panels.size(); ++index)
            m_panels[index]->visible = m_playPanelVisibility[index];
    }

    ImGui::DockBuilderRemoveNode(static_cast<ImGuiID>(dockId));
    if (!m_playLayoutIni.empty()) {
        ImGui::ClearIniSettings();
        ImGui::LoadIniSettingsFromMemory(
            m_playLayoutIni.data(), m_playLayoutIni.size());
    }
}

void EditorApp::BuildMapEditingLayout(uint32_t dockId)
{
    const ImGuiID root = static_cast<ImGuiID>(dockId);
    ImGui::DockBuilderRemoveNode(root);
    ImGui::DockBuilderAddNode(
        root, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(root, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center    = root;
    ImGuiID leftCol   = 0;
    ImGuiID tools     = 0;
    ImGuiID hierarchy = 0;
    ImGuiID inspector = 0;
    ImGuiID assets    = 0;

    /// @note 左カラムを Map Tools(上) と Scene Hierarchy(下) に分割する。
    /// @note        Map 編集はツールパネルが主役なので、Inspector の裏のタブへ隠れない左カラムへ固定する。
    ImGui::DockBuilderSplitNode(
        center, ImGuiDir_Left, 0.18f, &leftCol, &center);
    ImGui::DockBuilderSplitNode(
        leftCol, ImGuiDir_Down, 0.45f, &hierarchy, &tools);
    ImGui::DockBuilderSplitNode(
        center, ImGuiDir_Right, 0.22f, &inspector, &center);
    ImGui::DockBuilderSplitNode(
        center, ImGuiDir_Down, 0.24f, &assets, &center);

    ImGui::DockBuilderDockWindow("Map Tools", tools);
    ImGui::DockBuilderDockWindow("Scene Hierarchy", hierarchy);
    ImGui::DockBuilderDockWindow("Inspector", inspector);
    ImGui::DockBuilderDockWindow("Asset Browser", assets);
    ImGui::DockBuilderDockWindow("Scene", center);
    ImGui::DockBuilderFinish(root);
}

void EditorApp::BuildPlayViewportLayout(uint32_t dockId)
{
    const ImGuiID root = static_cast<ImGuiID>(dockId);
    ImGui::DockBuilderRemoveNode(root);
    ImGui::DockBuilderAddNode(
        root, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(root, ImGui::GetMainViewport()->WorkSize);
    ImGui::DockBuilderDockWindow("Game", root);
    ImGui::DockBuilderFinish(root);
}

void EditorApp::UpdatePlayCursorControls()
{
    const bool playing =
        !m_playMode.IsInEditor() && !m_playMode.HasPendingRestore();

    if (!playing) {
        if (m_playCursorApplied) {
            core::Cursor::ResetForEditor();
            m_playCursorApplied  = false;
            m_playCursorReleased = false;
        }
        m_ctx.requestGameCursorCapture = false;
        /// @note FBZZ_EXECUTE_ALWAYS() の Script は Play を押していなくても OnStart/OnUpdate が
        /// @note        走り、cursor プロキシを触るとシーンを開いただけで OS カーソルが消えたり中央へ
        /// @note        拘束されたりする。要求は Cursor 側に残し、«OS へ流すか» だけをここで抑制する。
        core::Cursor::SetSuppressed(true);
        return;
    }

    /// @note Game View は Dock のドラッグやリサイズで動くため、拘束範囲を «見えているゲーム画面»
    /// @note        に毎フレーム合わせ続けないと縁や中心がずれる。矩形はパネルが描いたときにしか更新
    /// @note        されないため、Play 開始直後の未描画状態では既定値へ閉じ込めず全体を使わせる。
    if (m_ctx.gameViewportRectValid) {
        core::Cursor::SetClipRegion(m_ctx.gameViewportOriginX, m_ctx.gameViewportOriginY,
                                    m_ctx.gameViewportWidth,   m_ctx.gameViewportHeight);
    } else {
        core::Cursor::ClearClipRegion();
    }

    /// @note ここでは拘束も表示も押し込まない。初期化は StartPlayMode が済ませてあり、以降の
    /// @note        正本はスクリプトの要求だけ。«初期値» と «実行中の要求» を同じ値で共有すると Play 中の
    /// @note        設定変更で要求が黙って消えるため、名乗る側 (cursor.Push) だけが持つ。
    if (!m_playCursorApplied) {
        m_playCursorApplied  = true;
        m_playCursorReleased = false;
    }

    /// @note Play Unfocused は «ゲームは回すが編集は続ける» モードなので、無条件拘束はその
    /// @note        選択を壊す。Game View を離れたら OS へは効かせず、戻れば要求どおり張り直す
    /// @note        (要求自体は Cursor が保持)。Escape の «解放» は画面を離れるまでの一時措置で、
    /// @note        Game View を出た時点で役目を終える。Free は «初期値» でなく抑制で表す。初期値
    /// @note        にすると OnStart の cursor.SetLockMode で上書きされ、外したはずのカーソルが
    /// @note        戻らない。解放を «離れたとき» だけで畳むと Escape 後に画面内をクリックしても
    /// @note        捕獲へ戻らないため、クリックでも解放を畳んで «クリックして入り直す» 経路を保つ。
    const bool freeOverride =
        m_ctx.playCursorOverride == EditorContext::PlayCursorOverride::Free;
    const bool gameFocused = m_ctx.gameViewportFocused;
    /// @note 移動中は Game View の矩形が毎フレーム動き拘束範囲も追従する (上の SetClipRegion)。
    /// @note        そこへ Locked の中央戻しが効くと窓とカーソルが互いを追いかけずれが増幅するため、
    /// @note        タイトルバーから掴んだ場合も含め «掴んでいる間はカーソルを人へ返す» を保証する。
    const bool movingWindow = ImGui::GetCurrentContext()->MovingWindow != nullptr;
    if (!gameFocused || m_ctx.requestGameCursorCapture)
        m_playCursorReleased = false;
    m_ctx.requestGameCursorCapture = false;
    core::Cursor::SetSuppressed(
        freeOverride || !gameFocused || m_playCursorReleased || movingWindow);

    /// @note Locked の中央戻しはここが担い、Confined も他アプリに ClipCursor を取られると
    /// @note        黙って外れる。抑制中は ApplyLock 自身が何もしない。
    core::Cursor::ApplyLock();

    /// @note Input::KeyDown は Win32 の生状態で ImGui のフィールド編集中かを知らない。
    /// @note        WantTextInput を見ないと、Inspector で名前を打っている最中の «編集キャンセルの
    /// @note        Escape» がそのまま Play の停止まで巻き込む。
    if (!m_ctx.activeScene
        || ImGui::GetIO().WantTextInput
        || !input::Input::KeyDown(input::KeyCode::ESCAPE))
        return;

    /// @note Escape を横取りしてよいのは «今まさにカーソルを取り上げているとき» と、
    /// @note        従来どおり Focused 実行のとき。自由なカーソルで Maximized / Unfocused を
    /// @note        回しているなら Escape はゲームのもので、終了はツールバー / Ctrl+P が担う。
    const core::CursorPolicy request = core::Cursor::GetEffectivePolicy();
    if (!core::Cursor::IsSuppressed() && request.CapturesCursor()) {
        /// @note 1 回目は解放だけ。ゲームは動かしたまま Inspector を触りに行ける。
        m_playCursorReleased = true;
        core::Cursor::SetSuppressed(true);
        return;
    }

    const bool escapeStops =
        m_playCursorReleased
        || m_ctx.playFocusMode == EditorContext::PlayFocusMode::Focused;
    if (!escapeStops)
        return;

    /// @note 終了時の後始末は StopPlayMode() に一本化されている。m_playMode.Stop() だけを直接
    /// @note        呼ぶと、ループ Voice の一括停止 (AudioSystem は SimOnly で EditMode 後は止められない)
    /// @note        や Play 中にスクリプトが変えた画質/明るさの破棄が抜け、Escape 経由で抜けたときだけ
    /// @note        状態が残る。経路が増えても後始末が漏れないよう必ずここを通す。
    StopPlayMode();
    core::Cursor::ResetForEditor();
    m_playCursorApplied  = false;
    m_playCursorReleased = false;
}

void EditorApp::EndFrame(renderer::IImGuiRenderer& imguiRenderer)
{
    ImGui::Render();
    imguiRenderer.ImGuiRenderDrawData();
    imguiRenderer.ImGuiRenderPlatformWindows();
}

/// @note  Viewport RT リサイズ

void EditorApp::ResizeViewportRTsIfNeeded()
{
    m_sceneViewportRTRecreated = false;
    m_gameViewportRTRecreated  = false;
    if (!m_renderer) return;

    auto resizeRT = [this](renderer::ResourceHandle<renderer::RenderTargetTag>& rt,
                           ViewportPanel* panel,
                           float width,
                           float height) {
        /// @note ここで弾くと、一度でも生成に失敗したビューポートは «作り直しの入口» を失い、
        /// @note        二度と映らなくなる。
        if (!panel) return false;

        const uint32_t vpW = static_cast<uint32_t>(width);
        const uint32_t vpH = static_cast<uint32_t>(height);
        if (vpW == 0 || vpH == 0) return false;
        auto* currentRT = m_resources->Get(rt);
        if (currentRT && vpW == currentRT->GetWidth() && vpH == currentRT->GetHeight()) return false;

        const auto created = m_resources->CreateRenderTarget(vpW, vpH);
        /// @note 生成に失敗したら今の RT を持ち続ける。捨てた上で作れないと絵が消えたまま戻らない。
        if (!created.IsValid()) return false;

        const auto previousRT = rt;
        rt = created;
        /// @note ハンドルの上書きだけでは旧 GPU リソースが ResourceManager に残り、
        /// @note        Dock 操作を繰り返すほど VRAM 使用量と Present 待機が増える。
        if (previousRT.IsValid())
            m_resources->Release(previousRT);
        panel->hdrRT = rt;
        return true;
    };

    m_sceneViewportRTRecreated =
        resizeRT(m_sceneViewportRT, m_sceneViewportPanel, m_ctx.viewportWidth, m_ctx.viewportHeight);
    m_gameViewportRTRecreated =
        resizeRT(m_gameViewportRT, m_gameViewportPanel, m_ctx.gameViewportWidth, m_ctx.gameViewportHeight);
    /// @note UI Viewport は専用 RT を持たず、Game View の完成済み RT を参照する。
    /// @note        リサイズ後もパネル側のハンドルを張り直して、古い RT 参照が残らないようにする。
    if (m_uiViewportPanel)
        m_uiViewportPanel->hdrRT = m_gameViewportRT;

}

/// @note  IModule — app::Run() から呼ばれるライフサイクル

bool EditorApp::OnInit()
{
    /// @note Init() と OpenProject() は app::Run() の前に呼ばれているため、
    /// @note        ここでは Post-project セットアップだけを担う。
    m_runtime.ApplySettings(m_ctx.projectSettings);
    m_runtime.ApplyAdditionalUIContext(m_ctx.projectSettings, m_sceneUICtx);
    m_runtime.BindExternalScene(m_scene.get());
    m_runtime.RegisterScenes(util::FileSystem::PathFromUtf8(m_ctx.projectRoot), *m_resources);

    /// @note OpenProject() 時点では editorCamera が nullptr なので、確定した OnInit() で適用する。
    /// @note        EditorSettings の既定値は従来のハードコード値 {0,2.5,-8} と一致する。
    m_debugCamera.camera.m_position = { m_settings.cameraLastPx, m_settings.cameraLastPy, m_settings.cameraLastPz };
    m_debugCamera.camera.m_rotation = { m_settings.cameraLastRx, m_settings.cameraLastRy, m_settings.cameraLastRz, m_settings.cameraLastRw };
    m_debugCamera.camera.m_aspect   = 1920.0f / 1080.0f;
    /// @note Scene View は広い地形を編集するため、シーン内の CameraComponent の farZ と独立して遠景を映す。
    m_debugCamera.camera.m_far      = 10000.0f;
    /// @note 射影は Teleport の後に直接書く。SetProjection は「見かけの大きさを引き継ぐ」ため
    /// @note        保存した orthoHeight を上書きしてしまう。復元では保存値をそのまま採用したい。
    m_debugCamera.camera.m_orthoHeight = m_settings.cameraOrthoHeight;
    m_debugCamera.camera.m_projection  = m_settings.cameraOrthographic
        ? renderer::ProjectionMode::Orthographic
        : renderer::ProjectionMode::Perspective;
    /// @note 位置・回転を直書きした後なので、pivot / yaw / pitch を実際の姿勢へ合わせ直す。
    m_debugCamera.Teleport(m_debugCamera.camera.m_position, m_debugCamera.camera.m_rotation);
    m_ctx.editorCamera = &m_debugCamera.camera;

    if (auto* rt = m_resources->Get(m_sceneViewportRT))
        m_debugCamera.camera.m_aspect =
            static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());

    WarmupRenderResources();
    return true;
}

void EditorApp::OnInputPolled()
{
    TickPlaytest();
}

void EditorApp::TickPlaytest()
{
    namespace fs = std::filesystem;
    if (m_batch.IsEnabled() && !m_batchStarted) {
        m_batchStarted = true;
        /// @note バッチは人の操作を受けない。保存済み設定で開いた AI のパイプは、起動中の別エディターと名前が衝突するので閉じる。
        StopAiCommandBus();
        const auto failToStart = [this](const std::string& message) {
            FBZZ_LOG_ERROR("[Playtest] %s", message.c_str());
            m_batchExitCode = 2;
            core::Application::Get().Quit();
        };
        std::ifstream stream(m_batch.scenarioPath, std::ios::binary);
        if (!stream) { failToStart("シナリオを開けません: " + m_batch.scenarioPath.generic_string()); return; }
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        std::string error;
        std::optional<ai::JsonValue> scenario = ai::ParseJson(text, &error);
        if (!scenario.has_value()) { failToStart(m_batch.scenarioPath.generic_string() + ": " + error); return; }
        const fs::path output = m_batch.reportPath.empty()
            ? fs::path(m_projectRoot) / "Library" / "Playtests" / m_batch.scenarioPath.stem().stem()
            : m_batch.reportPath.parent_path();
        if (!m_playtest.Start(*scenario, m_batch.scenarioPath, m_projectRoot, output, m_batch.playtest, error)) {
            failToStart(error);
            return;
        }
        FBZZ_LOG_INFO("[Playtest] 開始: %s", m_batch.scenarioPath.generic_string().c_str());
    }

    if (!m_playtest.IsRunning()) return;
    if (!m_playtestDispatcher) m_playtestDispatcher = std::make_unique<ai::EditorBusDispatcher>(m_ctx);
    m_playtestDispatcher->SetSceneViewportRT(m_sceneViewportRT);
    m_playtestDispatcher->SetGameViewportRT(m_gameViewportRT);

    playtest::PlaytestHooks hooks;
    hooks.bus = [this](const std::string& request) { return m_playtestDispatcher->Handle(request); };
    hooks.prepareFluidPlayback = [this](const ai::JsonValue& step, std::string& error) {
        const ai::JsonValue* fluidValue = step.Find("fluidPath");
        const ai::JsonValue* modeValue = step.Find("bakeMode");
        const ai::JsonValue* frameValue = step.Find("frame");
        if (fluidValue == nullptr || !fluidValue->IsString() || modeValue == nullptr || !modeValue->IsString()
            || frameValue == nullptr || !frameValue->IsNumber()) {
            error = "fluidBaked には fluidPath、bakeMode、frame が必要です";
            return false;
        }
        const std::string fluidPath = fluidValue->AsString();
        const std::string mode = modeValue->AsString();
        const double frame = frameValue->AsNumber();
        const ai::JsonValue* strengthValue = step.Find("motionStrength");
        const double strength = strengthValue != nullptr && strengthValue->IsNumber()
            ? strengthValue->AsNumber() : -1.0;
        if ((mode != "2d" && mode != "3d") || !std::isfinite(frame) || frame < 0.0
            || !std::isfinite(strength) || strength > 1.0) {
            error = "fluidBaked の bakeMode、frame、motionStrength が不正です";
            return false;
        }
        const fs::path assetsRoot = util::FileSystem::MakeAbsolute(
            util::FileSystem::PathFromUtf8(m_projectRoot) / "Assets");
        const fs::path relativePath = util::FileSystem::PathFromUtf8(fluidPath);
        const fs::path diskPath = util::FileSystem::MakeAbsolute(
            util::FileSystem::PathFromUtf8(m_projectRoot) / relativePath);
        if (relativePath.is_absolute() || relativePath.extension() != ".fluid"
            || !util::FileSystem::IsChildPathText(util::FileSystem::PathToUtf8(diskPath),
                                                   util::FileSystem::PathToUtf8(assetsRoot))) {
            error = "fluidPath はプロジェクトの Assets 配下の .fluid を指定してください";
            return false;
        }
        return fluideditor::PrepareFluidPlaybackCapture(m_ctx, util::FileSystem::PathToUtf8(diskPath),
                                                        mode == "3d", static_cast<float>(frame),
                                                        static_cast<float>(strength), error);
    };
    hooks.capture = [this](std::string_view view, std::vector<uint8_t>& png) {
        if (view == "fluidBaked") return fluideditor::CaptureFluidPlayback(m_ctx, png);
        const auto target = view == "scene" ? m_sceneViewportRT : m_gameViewportRT;
        uint32_t width = 0;
        uint32_t height = 0;
        return target.IsValid() && m_renderer->CaptureRenderTargetToPng(target, *m_resources, png, width, height);
    };
    hooks.keepViewportsRendering = [this]() {
        m_ctx.aiViewportRenderUntilFrame = std::max<std::uint64_t>(m_ctx.aiViewportRenderUntilFrame, Time::frameCount + 8);
    };
    m_playtest.Tick(hooks);

    if (m_batch.IsEnabled() && !m_playtest.IsRunning()) {
        const bool passed = m_playtest.State() == playtest::PlaytestState::PASSED;
        m_batchExitCode = passed ? 0 : 1;
        std::error_code ec;
        if (!m_batch.reportPath.empty() && fs::weakly_canonical(m_batch.reportPath, ec) != fs::weakly_canonical(m_playtest.ReportPath(), ec))
            fs::copy_file(m_playtest.ReportPath(), m_batch.reportPath, fs::copy_options::overwrite_existing, ec);
        FBZZ_LOG_INFO("[Playtest] %s: %s", passed ? "合格" : "不合格", m_playtest.ReportPath().generic_string().c_str());
        core::Application::Get().Quit();
    }
}

void EditorApp::OnUpdate(float dt)
{
    BeginFrame();

    /// @note Prefab 編集モードの出入りはシーンの中身を丸ごと差し替える。パネル描画の途中で
    /// @note        やると、その後のパネルが破棄済みの GameObject を掴むため、フレーム先頭で処理する。
    ProcessPrefabEditRequests();

    /// @note アセットの中身を差し替えると、その版を読むパネルと既に読み終えたパネルが
    /// @note        同じフレームで食い違うため、パネル描画に入る前に済ませる。
    ProcessAssetDiskReloads();
    /// @note guid → パスの索引をディスクへ追従させる。変化が無いフレームは何もしない。索引は
    /// @note        人と外部ツールが guid を引くための道具で製品ビルドには要らないため、エディターだけ
    /// @note        が書き、読み取り専用の配置先へは書きにいかせない。
    asset::AssetDatabase::FlushIndexFile();
    /// @note シーンを開き直すと、その中で参照されるアセットを新しい版で読み直せる。逆順だと
    /// @note        「開き直した直後だけ古いアセットを掴む」1 フレームができるため、アセットより後に行う。
    ProcessSceneDiskReload();

    if (m_aiPipeServer && m_aiPipeServer->IsRunning() && m_aiDispatcher) {
        m_aiDispatcher->SetSceneViewportRT(m_sceneViewportRT);
        m_aiDispatcher->SetGameViewportRT(m_gameViewportRT);
        m_aiPipeServer->DrainRequests([this](const std::string& request) {
            return HandleAiRequest(request);
        });
    }

    /// @note アクション層の評価後に読むので、このフレームにゲームが見た入力と一致する。
    if (m_inputRecorder.IsRecording() && !m_playMode.IsInEditor()) m_inputRecorder.Capture();

    auto* playMode = m_ctx.playMode;
    if (playMode->ApplyPendingRestore(*m_scene)) {
        /// @note Play 中の LoadScene で m_externalScene が nullptr へ落ちる。Stop 後もそのままだと
        /// @note        TransformEditorPreview がゲームシーン側で動き、m_scene の worldPosition が 0 のまま残る。
        m_runtime.BindExternalScene(m_scene.get());
        /// @note 遷移していたなら、これから捨てるシーンを activeScene と選択が指している。
        /// @note        OnRender の付け替えを待つと、その前に走る RestoreEditorHiding が解放済みの
        /// @note        Scene を触る。
        if (m_ctx.activeScene != m_scene.get()) {
            ClearEntitySelection(m_ctx);
            m_ctx.activeScene = m_scene.get();
        }
        /// @note 遷移先のシーンは Stop の時点で用済み。World を作り直す前に捨てて、
        /// @note        実行中だった Script のデストラクタを «まだ生きている» World の下で走らせる。
        /// @note        保留中の遷移要求もここで消える (残すと次フレームに外部バインドが再び外れる)。
        m_runtime.ReleaseOwnedScene();
        m_ctx.playSceneName.clear();
        /// @note World は m_contactCache/m_prevEvents を保持するため、Stop 復元時に丸ごと
        /// @note        リセットしないと前 Play セッションの Collider* が残る。
        m_runtime.ResetPhysics(m_ctx.projectSettings);
        /// @note Stop 復元後に editor-only 非表示を再適用
        RestoreEditorHiding();

        /// @note SceneSerializer はローカル position のみ復元し worldPosition はゼロになる。
        /// @note        この後の Update で TransformEditorPreview が走るが、同フレーム内の OnRender
        /// @note        より先に worldPosition を正確にしておくため即時フラッシュする。
        scene::FlushWorldTransforms(*m_scene);

        /// @note Serializer が設定する needsBake=true を上書きしてベイク済み NavMesh を復元する。
        /// @note        navMesh はランタイムキャッシュのため TOML 非保存で、Play→Stop のたびに再ベイクが
        /// @note        走らないよう Play 開始前に保存したキャッシュを差し戻す。
        if (!m_navMeshPlayCache.empty()) {
            for (scene::EntityID eid : m_scene->GetEntities<scene::NavMeshSurfaceComponent>()) {
                auto* surf = m_scene->GetComponent<scene::NavMeshSurfaceComponent>(eid);
                auto* go   = m_scene->GetGameObject(eid);
                if (!surf || !go) continue;
                auto it = m_navMeshPlayCache.find(go->instanceId);
                if (it != m_navMeshPlayCache.end()) {
                    surf->navMesh         = std::move(it->second.navMesh);
                    surf->bakeStats       = std::move(it->second.stats);
                    surf->bakeDebug       = std::move(it->second.debug);
                    surf->bakedSourceHash = it->second.sourceHash;
                    surf->bakeState = scene::NavMeshBakeState::Done;
                    surf->needsBake = false;
                }
            }
            m_navMeshPlayCache.clear();
        }

        /// @note 復元したシーンがリソースを作り直し終えるまで数フレーム待ってから数える。
        m_memoryLeakDiff.ScheduleCompare(3, "Play -> Stop");
    }

    if (m_ctx.resources != nullptr)
        static_cast<void>(m_memoryLeakDiff.Tick(*m_ctx.resources));

    if (!playMode->IsPlaying()) {
        m_debugCamera.moveSpeed = m_ctx.cameraSpeed;
        m_debugCamera.mouseSens = m_ctx.cameraSensitivity;
        m_debugCamera.Update(dt, m_ctx.sceneViewportHovered);
        UpdateFocusAnim(dt);
        /// @note ナビゲーションギズモが「ピボット固定で視点だけ回す」ために読む。
        m_ctx.editorCameraPivot         = m_debugCamera.Pivot();
        m_ctx.editorCameraFocusDistance = m_debugCamera.FocusDistance();
        m_ctx.editorCameraViewDistance  = m_debugCamera.ViewDistance();
    }

    const bool stepFrame   = playMode->ConsumeStep();
    m_simulationDt         = stepFrame ? (1.0f / 60.0f) : dt;

    if (playMode->IsPlaying()) {
        /// @note ビューポートリサイズに追従するため毎フレーム更新する。ProjectRuntime が所有
        /// @note        する ScriptRuntime を更新すれば ScriptProxy 全体へ反映される。
        m_runtime.UpdateScriptViewport(
            static_cast<uint32_t>(m_ctx.gameViewportWidth),
            static_cast<uint32_t>(m_ctx.gameViewportHeight));
    }

    /// @note Pause は IsPlaying() が false になるが編集へ戻ったわけではない。同じフラグで渡すと
    /// @note        ScriptSystem が Pause を編集モードと読み、Play 中の Script のライフサイクルを畳む
    /// @note        ため、playing は分けて渡す。
    m_runtime.Update(m_simulationDt, m_ctx.projectSettings,
                     playMode->IsPlaying() || stepFrame, stepFrame,
                     !playMode->IsInEditor());
}

void EditorApp::OnLateUpdate(float dt)
{
    (void)dt;
    m_runtime.LateUpdate(m_simulationDt);
}

void EditorApp::OnRender()
{
    if (m_renderPassViewerPanel) m_renderPassViewerPanel->PrepareFrame();
    if (auto* rt = m_resources->Get(m_sceneViewportRT))
        m_debugCamera.camera.m_aspect =
            static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());

    float gameAspect = m_debugCamera.camera.m_aspect;
    if (auto* rt = m_resources->Get(m_gameViewportRT))
        gameAspect = static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());

    /// @note Play 中に LoadScene が発生すると m_scene は遷移前のシーンのままになる。カメラ解決・
    /// @note        カリングマスク計算は新シーンのコンポーネントを参照する必要があるため、Play 中は
    /// @note        SceneManager::GetActive() を優先する。
    scene::Scene* const resolveScene     = (m_playMode.IsPlaying() && m_runtime.GetActiveScene())
                                              ? m_runtime.GetActiveScene() : m_scene.get();
    const renderer::Camera  gameCamera       = scene::ResolveEditorGameCamera(*resolveScene, m_debugCamera.camera, gameAspect);
    const fbzz::LayerMask   gameCullingMask  = scene::ResolveGameCullingMask(*resolveScene);

    m_renderer->BeginFrame();
    fluideditor::RenderFluidPlaybackCapture(m_ctx);

    /// @note 実際に画面へ出ているビューポートだけを描く。Scene View と Game View はそれぞれ
    /// @note        フル描画 (Shadow / GBuffer / ライティング / ポスト一式) なので、両方回すと素で 2 倍になる。
    /// @note        WasContentRendered() はパネル描画がこの後なので 1 フレーム遅れ。visible と併せて見れば、
    /// @note        閉じた瞬間はその場で止まり、開き直したときは待たずに描き始められる。
    const auto isViewportShowing = [](const ViewportPanel* panel) {
        return panel != nullptr && panel->visible && panel->WasContentRendered();
    };
    /// @note AI が RT を読み出す間は表示状態に関わらず描き続ける (キャプチャが古い絵を掴まないように)。
    const bool aiViewportCaptureActive = m_ctx.aiViewportRenderUntilFrame != 0
        && Time::frameCount <= m_ctx.aiViewportRenderUntilFrame;

    /// @note 新しい RT の中身は未定義で、DX12 では解放待ちの領域を使い回すため «少し前の絵» が
    /// @note        残る。WasContentRendered() は 1 フレーム遅れて寸法変化時に false になりうるため、
    /// @note        そのフレームを飛ばすとパネルが未初期化の RT を貼り、止まった絵と重なって出る。
    const bool fluidPlaybackCapture = m_playtest.IsFluidPlaybackCaptureStep();
    const bool needSceneView = !fluidPlaybackCapture
        && (isViewportShowing(m_sceneViewportPanel) || aiViewportCaptureActive
            || m_sceneViewportRTRecreated
            || (m_renderPassViewerPanel && m_renderPassViewerPanel->CaptureForView(false)));
    /// @note Game View の RT は UI Viewport が背景として共有する (m_uiViewportPanel->hdrRT = m_gameViewportRT)。
    /// @note        どちらか一方でも出ていれば描かないと、UI 編集画面が止まった絵のままになる。
    const bool needGameView = !fluidPlaybackCapture
        && (isViewportShowing(m_gameViewportPanel)
            || isViewportShowing(m_uiViewportPanel)
            || aiViewportCaptureActive
            || m_gameViewportRTRecreated
            || (m_renderPassViewerPanel && m_renderPassViewerPanel->CaptureForView(true)));

    scene::RenderFrameGeometryCache frameGeometry;
    auto* sharedGeometry = needSceneView && needGameView ? &frameGeometry : nullptr;
    if (needSceneView)
        RenderSceneView(gameCamera, gameCullingMask, sharedGeometry);
    if (needGameView)
        RenderGameView(gameCamera, gameCullingMask, sharedGeometry);

    /// @note 3D の焼きは Dispatch と読み戻しを伴うのでフレーム内で回す (DX12 はフレーム外を捨てる)。
    /// @note        パネルの開閉に関わらず毎フレーム進める。Baker が触った RT は直後のバックバッファ設定で戻る。
    if (m_fluidBake) m_fluidBake->Tick(m_ctx);

    m_renderer->SetRenderTarget({}, *m_resources);
    m_renderer->Clear({ 0.02f, 0.02f, 0.02f, 1.0f });

    /// @note Play 中に LoadScene でシーン遷移が発生すると SceneManager が新シーンを所有し、
    /// @note        m_scene は遷移前の古いシーンのままになる。パネル描画は遷移後シーンを参照する
    /// @note        必要があるため GetActive() で解決する。
    {
        scene::Scene* const nextActive = m_playMode.IsPlaying()
            ? m_runtime.GetActiveScene()
            : m_scene.get();
        /// @note シーン遷移を検知したら旧シーンの EntityID を持つ selectedEntities をクリアする。
        /// @note        遷移後シーンで同じ index を持つ別 Entity が選択状態に見えるのを防ぐため。
        if (nextActive != m_ctx.activeScene)
            ClearEntitySelection(m_ctx);
        m_ctx.activeScene = nextActive;
        /// @note 遷移してもドキュメントは開いたままなので、走っているシーン名は別に見せる。
        /// @note        これが出ていない限り、保存先は currentScenePath のまま動いていない。
        m_ctx.playSceneName = m_playMode.IsInEditor()
            ? std::string{}
            : m_runtime.ActiveSceneName();
        /// @note Play / Pause の切り替えと Play 中の遷移はイベントを持たないので、ここで叩く。
        /// @note        UpdateWindowTitle は前回と同じ内容なら何もしない。
        UpdateWindowTitle();
    }
    /// @note Animation Preview はウィンドウが閉じていても選択対象と再生時刻を保持する。Preview
    /// @note        パネルの OnRenderContent だけに任せると、非表示タブや Inspector の初回表示では
    /// @note        選択変化を拾えず、再アタッチするまでプレビューが更新されない。
    TickAnimationPreview(m_ctx);
    RenderPanels(m_ctx);
    EndFrame(*m_imguiRenderer);

    m_renderer->EndFrame();
}

void EditorApp::OnShutdown()
{
    /// @note 窓を閉じて中断されたシナリオも «不合格» のレポートを残し、ロックステップを解く。
    m_playtest.Cancel("エディターが終了した");
    m_playtestDispatcher.reset();
    fluideditor::ShutdownFluidPlaybackCapture(m_ctx);
    /// @note FreeLibrary より前に全スクリプトの OnDestroy と destructor を DLL コードが
    /// @note        有効なうちに実行する。ProjectRuntime::Shutdown は Editor 外部 Scene と、Play 中の
    /// @note        シーン遷移で残った Manager 所有 Scene の両方を破棄する。
    m_runtime.Shutdown();
    /// @note Unload(nullptr) で DestroyAllScripts をスキップする (Clear() 済みのため)
    m_ctx.activeScene = nullptr;
    m_ctx.editScene   = nullptr;
    Shutdown();
}

/// @note  IModule — プライベートヘルパー

void EditorApp::WarmupRenderResources()
{
    /// @note RenderSystem は初回呼び出しで shader/PSO/shadow map/GBuffer などを lazy initialize
    /// @note        する。その負荷を最初の可視フレームに乗せると起動直後だけ FPS が大きく落ちるため、
    /// @note        メインループ開始前に 1 回描画してリソースを先行生成する。
    m_renderer->BeginFrame();

    const auto sceneRT = m_sceneViewportRT;
    if (sceneRT.IsValid()) {
        m_renderer->SetRenderTarget(sceneRT, *m_resources);
        m_renderer->Clear({ 0.05f, 0.05f, 0.08f, 1.0f });

        float w = 1920.0f, h = 1080.0f;
        if (auto* rt = m_resources->Get(sceneRT)) {
            w = static_cast<float>(rt->GetWidth());
            h = static_cast<float>(rt->GetHeight());
        }
        scene::RenderSystemUIOptions uiOptions{};
        uiOptions.enabled       = true;
        uiOptions.viewportWidth  = w;
        uiOptions.viewportHeight = h;
        uiOptions.targetView    = scene::UIRenderTargetView::SceneViewport;
        uiOptions.context       = &m_sceneUICtx;
        const scene::CameraCullingSettings warmupSceneViewCulling{};
        renderer::Camera warmupCamera = m_debugCamera.camera;
        warmupCamera.m_backgroundColor = scene::ResolveGameBackgroundColor(*m_scene);
        /// @note 温めたいのは «本番で使うシェーダーと PSO»。nullptr を渡すと既定 (Forward) の
        /// @note        組み合わせが作られ、Deferred+ のプロジェクトでは 1 つも当たらず、最初の
        /// @note        可視フレームで結局作り直す。
        scene::RenderSystem(*m_scene, *m_renderer, *m_resources,
                            warmupCamera, sceneRT, &m_ctx.projectSettings.render,
                            fbzz::Layer::Everything, &uiOptions, nullptr,
                            &warmupSceneViewCulling);
    }

    const auto gameRT = m_gameViewportRT;
    if (gameRT.IsValid()) {
        m_renderer->SetRenderTarget(gameRT, *m_resources);
        m_renderer->Clear({ 0.02f, 0.02f, 0.05f, 1.0f });

        float w = 1920.0f, h = 1080.0f;
        if (auto* rt = m_resources->Get(gameRT)) {
            w = static_cast<float>(rt->GetWidth());
            h = static_cast<float>(rt->GetHeight());
        }
        const float warmupAspect = (h > 0.0f) ? (w / h) : 1.0f;
        const renderer::Camera warmupCamera =
            scene::ResolveEditorGameCamera(*m_scene, m_debugCamera.camera, warmupAspect);
        scene::RenderSystemUIOptions uiOptions{};
        uiOptions.enabled       = true;
        uiOptions.viewportWidth  = w;
        uiOptions.viewportHeight = h;
        uiOptions.targetView    = scene::UIRenderTargetView::GameViewport;
        uiOptions.context       = &m_runtime.GetGameUIContext();
        /// @note 本番の Game View と同じ設定で温める (RenderGameView と同じく診断表示を外す)。
        renderer::RenderSettings gameRenderSettings = m_ctx.projectSettings.render;
        gameRenderSettings.StripDebugVisualization();
        scene::RenderSystem(*m_scene, *m_renderer, *m_resources,
                            warmupCamera, gameRT,
                            &gameRenderSettings,
                            scene::ResolveGameCullingMask(*m_scene), &uiOptions);
    }

    m_renderer->SetRenderTarget({}, *m_resources);
    m_renderer->Clear({ 0.02f, 0.02f, 0.02f, 1.0f });
    m_renderer->EndFrame();
}

void EditorApp::UpdateFocusAnim(float dt)
{
    constexpr float kFocusAnimDuration = 0.30f;
    constexpr float kFocusDist         = 5.0f;

    /// @note 射影の切り替えはカメラ位置も動かすため、Teleport や補間より先に確定させる。
    if (m_ctx.requestCameraProjection) {
        m_ctx.requestCameraProjection = false;
        m_debugCamera.SetProjection(m_ctx.cameraProjection);
    }

    if (m_ctx.requestTeleportCamera) {
        m_ctx.requestTeleportCamera = false;
        m_focusAnim.active = false;
        m_debugCamera.Teleport(m_ctx.teleportPosition, m_ctx.teleportRotation);
    }

    if (m_ctx.requestFocusOnSelected) {
        m_ctx.requestFocusOnSelected = false;
        const math::Vector3 target = m_ctx.focusTargetPosition;
        const math::Vector3 dir    = m_debugCamera.camera.m_position - target;
        const float         dist   = dir.Length();
        const math::Vector3 camDir = (dist > 0.01f)
            ? dir * (1.0f / dist)
            : math::Vector3{ 0.0f, 0.5f, -1.0f }.Normalized();
        /// @note Unity の Frame Selected と同様、対象バウンズの大きさに応じてカメラ距離を
        /// @note        変える。半径 0 (バウンズ不明) は従来の固定距離。
        const float focusDist = (std::max)(kFocusDist, m_ctx.focusTargetRadius * 2.2f);
        m_ctx.focusTargetRadius = 0.0f;
        m_focusAnim.active   = true;
        m_focusAnim.startPos = m_debugCamera.camera.m_position;
        m_focusAnim.endPos   = target + camDir * focusDist;
        m_focusAnim.target   = target;
        m_focusAnim.t        = 0.0f;
    }

    if (m_focusAnim.active) {
        m_focusAnim.t += dt / kFocusAnimDuration;
        if (m_focusAnim.t >= 1.0f) {
            m_focusAnim.t      = 1.0f;
            m_focusAnim.active = false;
        }
        const float s = m_focusAnim.t * m_focusAnim.t * (3.0f - 2.0f * m_focusAnim.t);
        m_debugCamera.camera.m_position =
            m_focusAnim.startPos + (m_focusAnim.endPos - m_focusAnim.startPos) * s;
        m_debugCamera.LookAt(m_focusAnim.target);
    }
}

void EditorApp::RenderSceneView(const renderer::Camera& /*gameCamera*/, fbzz::LayerMask /*gameCullingMask*/,
                                scene::RenderFrameGeometryCache* frameGeometry)
{
    FBZZ_PROFILE_SCOPE("EditorApp::RenderSceneView");
    const auto sceneRT = m_sceneViewportRT;
    m_renderer->SetRenderTarget(sceneRT, *m_resources);
    m_renderer->Clear({ 0.05f, 0.05f, 0.08f, 1.0f });

    auto sceneRenderSettings = m_ctx.projectSettings.render;
    sceneRenderSettings.selectedObjects.clear();
    /// @note 親を選んだら子孫も選択へ展開する。輪郭と «選択中だけ» の診断表示が同じ集合を見る。
    const auto appendHierarchy = [&](auto&& self, scene::GameObject& object) -> void {
        const scene::EntityID id = object.GetID();
        sceneRenderSettings.selectedObjects.push_back({ id.index, id.generation });
        for (int childIndex = 0; childIndex < object.GetChildCount(); ++childIndex)
            if (auto* child = object.GetChild(childIndex)) self(self, *child);
    };
    if (m_ctx.activeScene != nullptr) {
        for (const scene::EntityID id : m_ctx.selectedEntities)
            if (auto* selected = m_ctx.activeScene->GetGameObject(id))
                appendHierarchy(appendHierarchy, *selected);
    }

    {
        float w = 1920.0f, h = 1080.0f;
        if (auto* rt = m_resources->Get(sceneRT)) {
            w = static_cast<float>(rt->GetWidth());
            h = static_cast<float>(rt->GetHeight());
        }
        scene::RenderSystemUIOptions uiOptions{};
        uiOptions.enabled       = true;
        uiOptions.viewportWidth  = w;
        uiOptions.viewportHeight = h;
        uiOptions.targetView    = scene::UIRenderTargetView::SceneViewport;
        uiOptions.context       = &m_sceneUICtx;
        sceneRenderSettings.showSkeleton    = m_ctx.showSkeleton;
        sceneRenderSettings.showGrid        = m_ctx.showGrid;
        sceneRenderSettings.showLightRange  = m_ctx.showLightRange;
        sceneRenderSettings.showVFXGizmos   = m_ctx.showVFXGizmos;
        sceneRenderSettings.showFlowFields     = m_ctx.showFlowFields;
        sceneRenderSettings.showFlowSamples    = m_ctx.showFlowSamples;
        sceneRenderSettings.showPhysicsVolumes = m_ctx.showPhysicsVolumes;
        sceneRenderSettings.showWaterFlow      = m_ctx.showWaterFlow;
        sceneRenderSettings.showRagdoll     = m_ctx.showRagdoll;
        sceneRenderSettings.skeletonSelectedOnly = m_ctx.skeletonSelectedOnly;
        sceneRenderSettings.showScriptGizmos  = m_ctx.showScriptGizmos;
        sceneRenderSettings.showConstraints   = m_ctx.showConstraints;
        sceneRenderSettings.showRigidBodies   = m_ctx.showRigidBodies;
        sceneRenderSettings.showIK            = m_ctx.showIK;
        sceneRenderSettings.showSpringBones   = m_ctx.showSpringBones;
        sceneRenderSettings.showAttachments   = m_ctx.showAttachments;
        sceneRenderSettings.showVFXPaths      = m_ctx.showVFXPaths;
        sceneRenderSettings.showTerrainBounds = m_ctx.showTerrainBounds;
        sceneRenderSettings.showLODBounds     = m_ctx.showLODBounds;
        /// @note NavMesh は Play 中も Scene View で出せる (経路の穴を実行中に確かめるため)。Game View は Strip で消える。
        /// @note Play 中の LoadScene 後は m_scene が遷移前のままなので、実行中シーンを描く。
        scene::Scene* const sceneViewScene = (m_playMode.IsPlaying() && m_runtime.GetActiveScene())
            ? m_runtime.GetActiveScene() : m_scene.get();
        /// @note ゲームカメラのカリング設定は持ち込まない。オクルージョンだけ Overlays から切り替える。
        scene::CameraCullingSettings sceneViewCulling{};
        sceneViewCulling.occlusionCulling = m_ctx.sceneViewOcclusionCulling;
        /// @note 背景色だけはゲームカメラから借りる。発光や UI の色決めが本番と同じ背景で成立するように。
        renderer::Camera sceneViewCamera = m_debugCamera.camera;
        sceneViewCamera.m_backgroundColor = scene::ResolveGameBackgroundColor(*sceneViewScene);
        scene::RenderSystem(*sceneViewScene, *m_renderer, *m_resources,
                            sceneViewCamera, sceneRT, &sceneRenderSettings,
                        fbzz::Layer::Everything, &uiOptions, &m_runtime.GetPhysicsWorld(),
                        &sceneViewCulling,
                         m_renderPassViewerPanel ? m_renderPassViewerPanel->CaptureForView(false) : nullptr,
                         frameGeometry);
    }
}

void EditorApp::RenderGameView(const renderer::Camera& gameCamera, fbzz::LayerMask gameCullingMask,
                               scene::RenderFrameGeometryCache* frameGeometry)
{
    FBZZ_PROFILE_SCOPE("EditorApp::RenderGameView");
    const auto gameRT = m_gameViewportRT;
    if (!gameRT.IsValid()) return;

    /// @note Play 中に LoadScene でシーン遷移すると SceneManager が新シーンを所有するため、
    /// @note        m_scene (遷移前) ではなく GetActive() を参照してゲームビューに正しいシーンを描く。
    scene::Scene* renderScene = m_playMode.IsPlaying()
        ? m_runtime.GetActiveScene()
        : m_scene.get();
    if (!renderScene) return;

    m_renderer->SetRenderTarget(gameRT, *m_resources);
    m_renderer->Clear({ 0.02f, 0.02f, 0.05f, 1.0f });

    float w = 1920.0f, h = 1080.0f;
    if (auto* rt = m_resources->Get(gameRT)) {
        w = static_cast<float>(rt->GetWidth());
        h = static_cast<float>(rt->GetHeight());
    }
    /// @note ゲームビューポート外のクリック (Inspector 等) が UIButton に届かないよう、
    /// @note        マウスがビューポート矩形内にあるときだけ mousePressed を渡す。
    const ImVec2 mousePos = ImGui::GetIO().MousePos;
    const float relX = mousePos.x - m_ctx.gameViewportOriginX;
    const float relY = mousePos.y - m_ctx.gameViewportOriginY;
    const bool mouseOverViewport = relX >= 0.0f && relX <= w && relY >= 0.0f && relY <= h;

    scene::RenderSystemUIOptions uiOptions{};
    uiOptions.enabled            = true;
    uiOptions.viewportWidth      = w;
    uiOptions.viewportHeight     = h;
    uiOptions.mouseInCanvasSpace = { relX, relY };
    uiOptions.mousePressed       = mouseOverViewport && ImGui::GetIO().MouseDown[0];
    uiOptions.targetView         = scene::UIRenderTargetView::GameViewport;
    uiOptions.context            = &m_runtime.GetGameUIContext();
    /// @note Debug メニューの診断表示は Scene View 専用。Game View はゲームの見た目だけを描く。
    renderer::RenderSettings gameRenderSettings = m_ctx.projectSettings.render;
    gameRenderSettings.StripDebugVisualization();
    scene::RenderSystem(*renderScene, *m_renderer, *m_resources,
                        gameCamera, gameRT,
                        &gameRenderSettings,
                        gameCullingMask, &uiOptions, nullptr, nullptr,
                         m_renderPassViewerPanel ? m_renderPassViewerPanel->CaptureForView(true) : nullptr,
                         frameGeometry);
}

} /// @note namespace fbzz::editor
