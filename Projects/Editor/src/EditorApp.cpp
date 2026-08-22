// FBZZ Engine
// EditorApp.cpp | fbzz::editor
// エディター全体のライフサイクル管理 + DockSpace
//
// ファイル構成:
//   EditorApp.cpp         - Init / Shutdown / OpenProject / BeginFrame / EndFrame / RenderPanels
//   EditorApp_Scene.cpp   - シーン I/O・ダーティ追跡・ホットリロード
//   EditorApp_MenuBar.cpp - メインメニューバーの構築・ホットキー登録
#include <Editor/EditorApp.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/EditorTaskOverlay.hpp>
#include <Editor/Ai/EditorBusDispatcher.hpp>
#include <Editor/Ai/NamedPipeServer.hpp>
#include <Editor/Ai/VFXPreviewCamera.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/ModalDialog.hpp>
#include <Editor/Util/FileDialog.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/VFXEditorLauncher.hpp>
#include <Editor/Panels/SceneHierarchyPanel.hpp>
#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/Panels/PreviewPanel.hpp>
#include <Editor/Panels/AnimationPreview.hpp>
#include <Editor/Panels/AnimationMaskPreviewPanel.hpp>
#include <Editor/Panels/ViewportPanel.hpp>
#include <Editor/Panels/ConsolePanel.hpp>
#include <Editor/Panels/BuildOutputPanel.hpp>
#include <Editor/Panels/AssetBrowserPanel.hpp>
#include <Editor/Panels/DependencyViewPanel.hpp>
#include <Editor/Panels/StatusBar.hpp>
#include <Editor/Panels/UndoHistoryPanel.hpp>
#include <Editor/Panels/HotkeyEditorPanel.hpp>
#include <Editor/Panels/ProjectSettingsPanel.hpp>
#include <Editor/Panels/BuildSettingsPanel.hpp>
#include <Editor/Panels/AnalysisPanel.hpp>
#include <Editor/Panels/AnimationGraphPanel.hpp>
#include <Editor/Panels/BehaviorTreePanel.hpp>
#include <Editor/Panels/SpriteEditorPanel.hpp>
#include <Editor/VFXEditor/Views/VFXEditorPanel.hpp>
#include <Editor/Panels/MapEditorPanel.hpp>
#include <Editor/Panels/IblBakePanel.hpp>
#include <Editor/Panels/AiSettingsPanel.hpp>
#include "Tools/TerrainTool.hpp"
#include "Tools/WaterTool.hpp"
#include "Tools/DetailTool.hpp"
#include "Tools/FoliageTool.hpp"
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Cursor.hpp>
#include <Engine/Renderer/DebugCamera.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/RenderDebugOverlay.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
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
#include <filesystem>
#include <unordered_map>
#include <utility>
#include <vector>

// imgui_impl_win32.h では #if 0 で隠されているため手動で前方宣言する
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fbzz::editor {

// WHY: デフォルトレイアウトをファイルスコープで定義し、OpenProject() から参照する。
//      io.IniFilename は projectRoot 確定後にセットするため Init() では設定しない。
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

    // SDKの実パスはマシン固有なのでGameHubが環境変数で渡す。
    // .fbzz_projにはportableなsdk_idだけを保存し、旧sdk_root/rootは移行用に限って読む。
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

} // namespace

// =============================================================================
// 初期化 / 終了
// =============================================================================

// コンストラクタ・デストラクタをここで定義する。
// WHY: EditorApp.hpp は TerrainTool を前方宣言のみにとどめているため、
//      ヘッダーのインクルード先 (main.cpp 等) では TerrainTool の定義が見えない。
//      std::unique_ptr のデストラクタは完全型を要求するので、
//      TerrainTool.hpp をインクルードしているこの .cpp で定義する必要がある。
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

// AI 要求1行の宛先を決める。
//
// WHY: VFX Preview World は FBZZVFXEditor.exe が所有し、Editor 本体は持たない
//      (Editor::Init が m_vfxPreviewScene を明示的に空へ落としている)。
//      にもかかわらず要求を全てここで処理していたため、vfx.preview / vfx.previewMetrics /
//      vfx.runtime / viewport.capture(view=vfx) は必ず「Preview World がありません」で
//      失敗していた。VFXEditorLauncher には転送の仕組みが揃っていたが、
//      drain ループから呼ばれていなかったのが原因。ここが唯一の分岐点になる。
//
// 転送に失敗した場合はローカルへ落とす。ローカル側は Preview World を持たないので
// 詳細付きの NO_PREVIEW_WORLD を返し、AI は次に何をすればよいかを応答から読める。
std::string EditorApp::HandleAiRequest(const std::string& request)
{
    if (!m_aiDispatcher) return {};

    if (VFXEditorLauncher::ShouldRouteRequest(request)) {
        std::string response;
        if (VFXEditorLauncher::EnsureAndForward(m_ctx.projectRoot, request, response))
            return response;
        // ここに来るのは exe が無い / 起動しても Pipe を開けない場合。
        // 理由は VFXEditorLauncher 側がログへ残しているので、AI は console.logs で追える。
    }

    // .vfx を書き換えるコマンドは、独立VFXEditorが開いている文書と衝突しうる。
    // 編集前に相手の未保存変更を保存させ、編集後に再読み込みさせて表示を一致させる。
    const bool authoring = VFXEditorLauncher::IsVFXAuthoringCommand(request);
    if (authoring) VFXEditorLauncher::PrepareForVFXAuthoringCommand();
    std::string response = m_aiDispatcher->Handle(request);
    if (authoring) VFXEditorLauncher::NotifyVFXAuthoringCommand(request);
    return response;
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

    // ゲーム入力のアクション層はエディット中は止めておき、Play 開始時に
    // PlayModeController が有効化する。
    // WHY: 既定は有効 (Standalone 起動でそのまま遊べる状態) にしてあるため、
    //      エディタ側で明示的に落とす必要がある。これが無いとシーン編集中に
    //      W/A/S/D がゲーム入力としても解釈される。
    input::InputActionMap::SetEnabled(false);

    window.SetWndProcHook([this](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> bool {
        if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp) != 0)
            return true;
        if (msg == WM_CLOSE) {
            RequestExit();
            return true;
        }
        return false;
    });

    // ── エクスプローラーからの外部ファイル D&D ────────────────────────────
    // WHY: AssetBrowser 側 (AcceptExternalDrop / ConsiderExternalDropTarget /
    //      FinalizeExternalDrop) は EditorContext を読むだけの消費者で、
    //      それを埋める producer がどこにも居なかった。そのため OS からのドロップは
    //      Window の OLE ドロップターゲットまでは届いていたのに、Editor 側では
    //      何も起きなかった。ここでプラットフォーム層と EditorContext を接続する。
    //
    // 座標: Window は「クライアント座標」で通知する。一方 ImGui は Multi-Viewport
    //       有効時に OS デスクトップ座標で当たり判定するため、そのまま渡すと
    //       ドロップ先フォルダの判定がウィンドウ位置分ずれる。ここで screen 空間へ揃える。
    const auto toScreenSpace = [this](int clientX, int clientY, float& outX, float& outY) {
        POINT p{ static_cast<LONG>(clientX), static_cast<LONG>(clientY) };
        ClientToScreen(m_hwnd, &p);
        outX = static_cast<float>(p.x);
        outY = static_cast<float>(p.y);
    };

    window.SetFileDropCallback(
        [this, toScreenSpace](const std::vector<std::string>& paths, int x, int y) {
            if (paths.empty()) return;
            // 同一フレームで複数回ドロップされることはないが、未消費分は失わず連結する。
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
    // 通常パネルをメイン HWND の外や別モニターへドラッグできるよう、OS Multi-Viewport を有効化する。
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // WHY: IniFilename は OpenProject() で projectRoot が確定してから設定する。
    //      Init() 時点では projectRoot が空なので nullptr にしておき、
    //      最初の NewFrame() で自動ロードされないようにする。
    io.IniFilename = nullptr;

    EditorTheme::Apply();
    // 追加 OS Window とメイン Window の見た目を連続させ、境界移動時の角丸差をなくす。
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
    m_ctx.renderer    = &renderer;
    m_ctx.imguiRenderer = &imguiRenderer;
    m_ctx.resources   = &resources;
    auto& application = core::Application::Get();
    m_ctx.memorySystem = &application.GetMemorySystem();
    // EditorはApplication所有とは別のProjectRuntimeを更新するため、音響を明示的に接続する。
    m_runtime.GetSceneManager().SetAudioManager(application.GetAudioManager());
    m_terrainTool     = std::make_unique<TerrainTool>();
    m_ctx.terrainTool = m_terrainTool.get();
    m_waterTool       = std::make_unique<WaterTool>();
    m_ctx.waterTool   = m_waterTool.get();
    m_detailTool      = std::make_unique<DetailTool>();
    m_ctx.detailTool  = m_detailTool.get();
    m_foliageTool     = std::make_unique<FoliageTool>();
    m_ctx.foliageTool = m_foliageTool.get();
    m_ctx.markSceneDirty  = [this]() { MarkSceneDirty(); };
    m_ctx.requestOpenScene = [this](const std::string& path) { RequestOpenScenePath(path); };
    // AI (Command Bus) からの入出力はモーダル確認を挟まない実体を直接呼ぶ。
    // 未保存変更の扱いは EditorBusDispatcher 側が discardUnsaved 引数で判定する。
    m_ctx.openScenePathImmediate = [this](const std::string& path) { return OpenScenePath(path); };
    m_ctx.saveScenePathImmediate = [this](const std::string& path) {
        return path.empty() ? SaveScene() : SaveScenePath(path);
    };

    m_panels.push_back(std::make_unique<SceneHierarchyPanel>());
    m_panels.push_back(std::make_unique<InspectorPanel>());
    // Animation / Material / VFX を同じ選択導線で確認できる共通プレビュー。
    m_panels.push_back(std::make_unique<PreviewPanel>());
    m_panels.push_back(std::make_unique<AnimationMaskPreviewPanel>());
    {
        // .animcontroller は「開く」操作でこのパネルへ渡す (BehaviorTree と同じ方式)。
        auto animationGraph = std::make_unique<AnimationGraphPanel>();
        AnimationGraphPanel* animationGraphPtr = animationGraph.get();
        m_ctx.openAnimationGraph = [animationGraphPtr](const std::string& path) {
            animationGraphPtr->RequestOpen(path);
        };
        m_panels.push_back(std::move(animationGraph));
    }
    {
        // .behaviortree はダブルクリックでこのパネルへ渡す。以前は作れるのに
        // 開く手段が無く、TOML を手書きするしかなかった。
        auto behaviorTree = std::make_unique<BehaviorTreePanel>();
        BehaviorTreePanel* behaviorTreePtr = behaviorTree.get();
        m_ctx.openBehaviorTree = [behaviorTreePtr](const std::string& path) {
            behaviorTreePtr->RequestOpen(path);
        };
        m_panels.push_back(std::move(behaviorTree));
    }
    {
        auto spriteEditor = std::make_unique<SpriteEditorPanel>();
        SpriteEditorPanel* spriteEditorPtr = spriteEditor.get();
        m_ctx.openSpriteEditor = [spriteEditorPtr](const std::string& metaPath) {
            spriteEditorPtr->Open(metaPath);
        };
        m_panels.push_back(std::move(spriteEditor));
    }
    // VFX Editorは別プロセスで専用Preview Worldを所有する。
    // WHY: Editor SceneへPreview Entityが混入する経路をプロセス境界で完全に断つため。
    m_vfxEditorPanel = nullptr;
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
    {
        auto buildOutput = std::make_unique<BuildOutputPanel>();
        m_buildOutputPanel = buildOutput.get();
        m_panels.push_back(std::move(buildOutput));
    }
    {
        auto assets = std::make_unique<AssetBrowserPanel>("Assets");
        m_assetBrowserPanel = assets.get();
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
        auto aiSettings = std::make_unique<AiSettingsPanel>();
        m_aiSettingsPanel = aiSettings.get();
        m_panels.push_back(std::move(aiSettings));
    }

    for (auto& panel : m_panels) {
        panel->visible = panel->GetDefaultVisibility();
        panel->OnInit(m_ctx);
    }

    // 操作の登録が先。ホットキーは operator id へキーを割り当てるだけなので、
    // レジストリが空だと 1 つも解決できない。
    RegisterBuiltinOperators();
    RegisterDefaultHotkeys();
    // 保存済みのオーバーライドを適用する (鍵は operator id、旧形式は表示名)
    for (const auto& ov : m_settings.hotkeyOverrides)
        m_hotkeys.Rebind(ov.name, ov.key, ov.ctrl, ov.shift, ov.alt);

    // 初回 RT をウィンドウサイズで生成する
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
        // WHY: UI Viewport は Game View の完成フレームを背景として共有する。
        //      UI 専用 RT を別描画すると、Clear 順や RenderGraph 経路の差で青い空 RT が表示される。
        //      編集用ガイドとギズモだけを ImGui 側で重ねることで、Game View と同じ出力を見ながら UI を編集できる。
        m_uiViewportPanel->hdrRT     = m_gameViewportRT;
        m_uiViewportPanel->renderer  = &renderer;
        m_uiViewportPanel->resources = &resources;
    }
    if (m_vfxEditorPanel)
        m_vfxEditorPanel->Session().preview.renderTarget = m_vfxPreviewRT;

    // シーンはここで生成し activeScene にバインドする。
    // OpenProject() が activeScene を参照するため Init() で確立しておく必要がある。
    m_scene = std::make_unique<scene::Scene>();
    m_ctx.activeScene = m_scene.get();
    // AI(EditorBusDispatcher)のphysicsクエリが参照するProjectRuntimeを共有する。EditorAppが所有。
    m_ctx.runtime = &m_runtime;
    // AIのconsole.logsクエリが読むログシンクを共有する。EditorAppが所有。
    m_ctx.consoleSink = &m_consoleSink;
    // VFX Preview は編集 Scene と別の SceneManager で駆動し、生成物を Scene 保存・Undo から隔離する。
    // VFX Preview WorldはFBZZVFXEditor.exeが所有する。Editorプロセスには生成しない。
    m_vfxPreviewScene.reset();
    m_ctx.vfxPreviewScene = nullptr;

    FBZZ_LOG_INFO("EditorApp init done");
    UpdateWindowTitle();
    InstallNativeMenuBar();
    return true;
}

void EditorApp::Shutdown()
{
    // 接続中の AI ワーカーを Scene / Panel より先に停止し、破棄済み状態への要求を防ぐ。
    StopAiCommandBus();

    // WHY: Map Mode の Dock を imgui_layout.ini へ保存すると次回起動も専用配置になる。
    //      終了経路でも通常 Workspace をメモリから戻してから ImGui を破棄する。
    if (m_ctx.mapEditingMode && !m_normalLayoutIni.empty()) {
        ImGui::GetIO().IniFilename = m_normalIniFilename;
        ImGui::ClearIniSettings();
        ImGui::LoadIniSettingsFromMemory(
            m_normalLayoutIni.data(), m_normalLayoutIni.size());
        m_ctx.mapEditingMode = false;
    }

    // DLL 仮想デストラクタが DLL コードを参照するため、パネル・シーンより先にアンロードする。
    m_scriptDll.Unload(m_ctx.activeScene);

    for (auto& panel : m_panels)
        panel->OnShutdown();
    ShutdownAnimationPreview();

    // --- EditorContext → EditorSettings への書き戻し ----------------------
    // WHY: パネルやメインループは EditorContext のライブ値を直接変更する。
    //      Shutdown 時にここで一括書き戻さないと、起動時に読んだ初期値が
    //      そのまま保存されてしまい、ユーザーの変更が永続化されない。
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
    m_settings.showSkeleton       = m_ctx.showSkeleton;
    m_settings.showStats          = m_ctx.showStats;
    m_settings.sceneViewOcclusionCulling = m_ctx.sceneViewOcclusionCulling;
    m_settings.hotReloadEnabled   = m_ctx.hotReloadEnabled;
    m_settings.aiCommandBusEnabled = m_ctx.aiCommandBusEnabled;
    m_settings.showTerrainTool    = m_ctx.showTerrainTool;
    m_settings.showWaterTool      = m_ctx.showWaterTool;
    m_settings.showDetailTool     = m_ctx.showDetailTool;
    m_settings.showFoliageTool    = m_ctx.showFoliageTool;
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
    }
    m_settings.editorUiScale         = m_ctx.editorUiScale;
    m_settings.assetBrowserIconSize  = m_ctx.assetBrowserIconSize;
    m_settings.assetBrowserTreeWidth = m_ctx.assetBrowserTreeWidth;
    m_settings.assetBrowserBookmarks = m_ctx.assetBrowserBookmarks;
    m_settings.defaultImportOptions  = m_ctx.defaultImportOptions;
    // ホットキーバインドをオーバーライドとして保存 (デフォルト値でも全件保存して確実に復元)
    m_settings.hotkeyOverrides.clear();
    for (const auto& hk : m_hotkeys.GetHotkeys()) {
        // 説明専用エントリ (マウス操作など) は割り当てを持たないので保存しない。
        if (hk.infoOnly) continue;
        EditorSettings::HotkeyOverride ov;
        // 鍵は operator id を優先する。表示名を鍵にしていると、ラベルを変えた瞬間に
        // 保存済みのリバインドが誰にも一致せず黙って既定へ戻ってしまう。
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
    if (m_terrainTool) {
        const auto b = m_terrainTool->GetBrush();
        m_settings.terrainBrushRadius   = b.radius;
        m_settings.terrainBrushStrength = b.strength;
        m_settings.terrainBrushFalloff  = static_cast<int>(b.falloff);
        m_settings.terrainSculptMode    = static_cast<int>(m_terrainTool->GetSculptMode());
        m_settings.terrainPaintLayer    = m_terrainTool->GetPaintLayer();
    }
    if (m_detailTool) {
        m_settings.detailBrushRadius    = m_detailTool->GetBrushRadius();
        m_settings.detailBrushStrength  = m_detailTool->GetBrushStrength();
        m_settings.detailMode           = m_detailTool->GetMode();
        m_settings.detailLayerIndex     = m_detailTool->GetLayerIndex();
        m_settings.detailShowChunkBounds = m_detailTool->GetShowChunkBounds();
        m_settings.detailShowCounts      = m_detailTool->GetShowCounts();
    }

    // Inspector 折り畳み状態を ImGui StateStorage から回収して設定に書き戻す。
    //
    // WHY 全エントリを舐めないか (不具合修正):
    //   ImGuiStorage は key → 共用体 (int / float / void*) の平坦な表で、型を覚えていない。
    //   以前はここで Data 全件を "val_i != 0" として保存し、起動時に SetInt で書き戻して
    //   いたため、折り畳み以外の値まで 0/1 の int へ潰していた。実害として、
    //   カード本文の高さ (SetFloat) が壊れた状態で復元される、参照欄 (AssetPathField) が
    //   パス直接編集モードのまま固定される、といった「終了時の状態が焼き付く」挙動が出る。
    //   ComponentHeader が名乗り出た ID だけを保存対象にする。
    // WHY 前回値を土台にするか: 今回のセッションで一度も表示しなかったカードの状態を
    //   落とさないため。ctx.inspectorSectionState は起動時に読んだ内容のまま保持している。
    if (ImGuiWindow* win = ImGui::FindWindowByName("Inspector")) {
        std::unordered_map<ImGuiID, bool> merged;
        for (const auto& [key, open] : m_ctx.inspectorSectionState) merged[key] = open;
        for (const ImGuiID id : widgets::ComponentHeaderStateIds())
            merged[id] = win->StateStorage.GetInt(id, 0) != 0;

        m_settings.inspectorSectionState.assign(merged.begin(), merged.end());
        // TOML の差分を安定させる (毎回並びが変わると保存のたびに全行が変更扱いになる)。
        std::sort(m_settings.inspectorSectionState.begin(),
                  m_settings.inspectorSectionState.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
    }

    // Debug メニュー - レンダリングオーバーレイ
    m_settings.showColliders        = m_ctx.projectSettings.render.showColliders;
    m_settings.showUIRects          = m_ctx.projectSettings.render.showUIRects;
    m_settings.showTerrainCollision = m_ctx.projectSettings.render.showTerrainCollision;
    m_settings.showDecalBounds      = m_ctx.projectSettings.render.showDecalBounds;
    m_settings.showNavMesh          = m_ctx.projectSettings.render.showNavMesh;
    m_settings.showNavSensors       = m_ctx.projectSettings.render.showNavSensors;
    m_settings.viewMode = static_cast<int>(m_ctx.projectSettings.render.viewMode);

    m_settings.Save(m_ctx.projectRoot + "/Assets/EditorConfig/editor_settings.toml", m_ctx.projectRoot);
    m_ctx.projectSettings.Save(m_projectSettingsPath);
    m_sceneViewportRT = {};
    m_gameViewportRT  = {};
    m_vfxPreviewRT    = {};
    m_imguiRenderer->ImGuiShutdown();
    ImGui::DestroyContext();
}

bool EditorApp::OpenProject(const std::string& projectRoot, const std::string& projectSettingsPath, const std::string& scenePath)
{
    if (!m_ctx.activeScene || !m_resources) return false;

    m_projectRoot      = projectRoot;
    m_ctx.projectRoot  = projectRoot;
    m_ctx.editorSceneState.Clear();

    // --- EditorConfig を Assets/EditorConfig/ からロード --------------------
    // WHY: Init() 時点では projectRoot が未確定なので、ここで遅延ロードする。
    //      Settings は OpenProject 内で lastScenePath を参照するため、
    //      他の初期化より前に完了させる必要がある。
    {
        const std::string configDir = projectRoot + "/Assets/EditorConfig";
        util::FileSystem::EnsureDirectory(configDir);
        m_settings.Load(configDir + "/editor_settings.toml", projectRoot);
        m_ctx.inspectorSectionState = m_settings.inspectorSectionState;

        // EditorSettings → EditorContext への全フィールド適用
        // WHY: EditorSettings は TOML の raw 値を保持し、EditorContext がライブ値を保持する。
        //      OpenProject で一括コピーし、Shutdown で逆方向に書き戻す。
        m_ctx.showGrid           = m_settings.showGrid;
        m_ctx.gridSize           = m_settings.gridSize;
        m_ctx.snapEnabled = m_settings.snapEnabled;
        m_ctx.snapPos     = m_settings.snapPos;
        m_ctx.snapRot     = m_settings.snapRot;
        m_ctx.snapScale   = m_settings.snapScale;
        m_ctx.gizmoMode          = static_cast<EditorContext::GizmoMode>(m_settings.gizmoMode);
        m_ctx.gizmoSpace         = static_cast<EditorContext::GizmoSpace>(m_settings.gizmoSpace);
    m_ctx.gizmoPivot         = static_cast<EditorContext::GizmoPivot>(m_settings.gizmoPivot);
        m_ctx.showLightRange     = m_settings.showLightRange;
        m_ctx.showVFXGizmos      = m_settings.showVFXGizmos;
        m_ctx.showSkeleton       = m_settings.showSkeleton;
        m_ctx.showStats          = m_settings.showStats;
        m_ctx.sceneViewOcclusionCulling = m_settings.sceneViewOcclusionCulling;
        m_ctx.hotReloadEnabled   = m_settings.hotReloadEnabled;
        m_ctx.aiCommandBusEnabled = m_settings.aiCommandBusEnabled;
        m_ctx.showTerrainTool    = m_settings.showTerrainTool;
        m_ctx.showWaterTool      = m_settings.showWaterTool;
        m_ctx.showDetailTool     = m_settings.showDetailTool;
        m_ctx.showFoliageTool    = m_settings.showFoliageTool;
        m_ctx.gameViewportAspect = static_cast<EditorContext::GameViewportAspect>(m_settings.gameViewportAspect);
        m_ctx.playFocusMode      = static_cast<EditorContext::PlayFocusMode>(m_settings.playFocusMode);
        m_ctx.cameraSpeed        = m_settings.cameraSpeed;
        m_ctx.cameraSensitivity  = m_settings.cameraSensitivity;
        if (m_ctx.editorCamera) {
            m_ctx.editorCamera->m_position = { m_settings.cameraLastPx, m_settings.cameraLastPy, m_settings.cameraLastPz };
            m_ctx.editorCamera->m_rotation = { m_settings.cameraLastRx, m_settings.cameraLastRy, m_settings.cameraLastRz, m_settings.cameraLastRw };
        }
        m_ctx.editorUiScale = m_settings.editorUiScale;
        EditorTheme::SetUiScale(m_ctx.editorUiScale); // ロードしたスケールを即適用
        m_ctx.assetBrowserIconSize = m_settings.assetBrowserIconSize;
        m_ctx.assetBrowserTreeWidth = m_settings.assetBrowserTreeWidth;
        m_ctx.assetBrowserBookmarks = m_settings.assetBrowserBookmarks;
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
                static_cast<TerrainTool::FalloffType>(m_settings.terrainBrushFalloff));
            m_terrainTool->SetSculptMode(
                static_cast<TerrainTool::SculptMode>(m_settings.terrainSculptMode));
            m_terrainTool->SetPaintLayer(m_settings.terrainPaintLayer);
        }
        if (m_detailTool) {
            m_detailTool->SetBrush(m_settings.detailBrushRadius, m_settings.detailBrushStrength);
            m_detailTool->SetMode(m_settings.detailMode);
            m_detailTool->SetLayerIndex(m_settings.detailLayerIndex);
            m_detailTool->SetShowChunkBounds(m_settings.detailShowChunkBounds);
            m_detailTool->SetShowCounts(m_settings.detailShowCounts);
        }

        // ImGui レイアウトファイルも同ディレクトリに配置する。
        // WHY: io.IniFilename は const char* を保持するため、メンバ文字列のアドレスを渡して寿命を保証する。
        m_imguiIniPath = configDir + "/imgui_layout.ini";
        ImGui::GetIO().IniFilename = m_imguiIniPath.c_str();
        if (!util::FileSystem::Exists(m_imguiIniPath)) {
            // LoadIniSettingsFromMemory は SettingsLoaded フラグを立てるため、
            // その後の NewFrame() でファイルから上書きされることはない。
            ImGui::LoadIniSettingsFromMemory(DEFAULT_IMGUI_LAYOUT);
        }

        SceneIO::SetProjectRoot(projectRoot);
    }

    LoadRuntimeBuildMetadata(m_ctx);
    if (m_assetBrowserPanel && !m_projectRoot.empty())
        m_assetBrowserPanel->SetRootPath(m_projectRoot + "/Assets");

    if (!projectSettingsPath.empty()) {
        m_projectSettingsPath = projectSettingsPath;
        m_ctx.projectSettings.Load(m_projectSettingsPath);
        Time::targetFps = m_ctx.projectSettings.app.targetFps;
    }

    // WHY: Debug メニューのレンダリング設定はエディター個人設定であり projectSettings より優先する。
    //      projectSettings.Load() の後に上書きすることでプロジェクト共有値に左右されない。
    m_ctx.projectSettings.render.showColliders        = m_settings.showColliders;
    m_ctx.projectSettings.render.showUIRects          = m_settings.showUIRects;
    m_ctx.projectSettings.render.showTerrainCollision = m_settings.showTerrainCollision;
    m_ctx.projectSettings.render.showDecalBounds      = m_settings.showDecalBounds;
    m_ctx.projectSettings.render.showNavMesh          = m_settings.showNavMesh;
    m_ctx.projectSettings.render.showNavSensors       = m_settings.showNavSensors;
    m_ctx.projectSettings.render.viewMode = static_cast<renderer::ViewMode>(m_settings.viewMode);

    // WHY: SceneIO::Load() がシーン内の ScriptComponent を復元する際に
    //      ScriptFactory からファクトリ関数を引く。DLL が未ロードだとスクリプトインスタンスが
    //      生成されず Play 中も OnUpdate() が呼ばれない。
    //      必ずシーンロードより前に DLL をロードして ScriptFactory を準備する。
    InitScriptDll();

    // WHY: PrefabSerializer は Editor プロジェクトにあり Engine から直接呼べないため、
    //      Script::SetPrefabInstantiationCallback で実装を注入する。ScriptSceneProxy::Instantiate が
    //      ここを経由して PrefabSerializer::Instantiate を呼ぶ。
    //      PrefabRef::path は Assets 起点の相対パス ("Assets/Foo.prefab") で保存されるため、
    //      ToProjectAssetDiskPath でプロジェクトルートを補完して絶対パスへ変換してから渡す。
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
        if (!SceneIO::Load(*m_ctx.activeScene, sceneToOpen)) {
            FBZZ_LOG_ERROR("Open project scene failed: %s", sceneToOpen.c_str());
            return false;
        }
        // WHY: SceneSerializer はローカル position のみ復元し worldPosition はゼロのまま。
        //      OnInit の WarmupRenderResources がスケジューラより前に描画するため、
        //      ここで即時フラッシュしてロード直後の最初のフレームも正しい位置で表示する。
        scene::FlushWorldTransforms(*m_ctx.activeScene);
        m_settings.lastScenePath = sceneToOpen;
        m_ctx.currentScenePath   = sceneToOpen;
        m_ctx.selectedEntities.clear();
        RebuildEditorUIFromScene();
        CaptureCleanScene();
    }

    FBZZ_LOG_INFO("Opened project: %s", m_projectRoot.c_str());
    UpdateWindowTitle();

    if (m_ctx.aiCommandBusEnabled && !StartAiCommandBus()) {
        // プロジェクト自体は開けるため失敗を致命扱いにせず、AI Settings から再試行可能にする。
        FBZZ_LOG_ERROR("保存済み設定から AI Command Bus を開始できませんでした");
    } else if (!m_ctx.aiCommandBusEnabled) {
        // プロジェクト切替で自動開始設定が無効になった場合は、前プロジェクトの待受を残さない。
        StopAiCommandBus();
    }

    return true;
}

void EditorApp::RebuildEditorUIFromScene()
{
    // WHY: UI Viewport の編集対象は EditorContext の一時状態であり、.fbzz には保存しない。
    //      シーンを読み込んだ直後に Scene 内の UICanvas から復元しないと、初回表示で UI 編集ガイドや
    //      pick 対象が前シーンの無効 ID のままになり、Canvas をクリックするまで再構築されない。
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

// =============================================================================
// ウィンドウタイトル
// =============================================================================

void EditorApp::UpdateWindowTitle()
{
    if (!m_hwnd) return;

    // Prefab 編集モード中は編集対象がシーンではなくアセットなので、タイトルもそちらを出す。
    // WHY: タイトルバーだけシーン名のままだと、編集モードに入っていることを見落として
    //      「シーンを壊してしまった」と誤解する。
    const bool inPrefabEdit = m_ctx.InPrefabEditMode();
    const std::string displayName = inPrefabEdit
        ? util::FileSystem::GetFilename(m_ctx.prefabEditPath)
        : (m_ctx.currentScenePath.empty()
               ? "Untitled"
               : util::FileSystem::GetFilename(m_ctx.currentScenePath));
    const bool dirty = inPrefabEdit ? m_ctx.prefabEditDirty : m_ctx.sceneDirty;
    // 変化検知のキーにはモードを含める (同名でもモードが違えば描き直す)。
    const std::string titleKey =
        (inPrefabEdit ? "prefab:" : "scene:") +
        (inPrefabEdit ? m_ctx.prefabEditPath : m_ctx.currentScenePath);

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
    // 使用中の描画バックエンド (DirectX 11 / 12) をタイトルに付す。
    // WHY: app 起動時に付けたタイトルは本メソッドで上書きされるため、ここでも同じタグを付け直す。
    //      バックエンド名は IRenderer 抽象越しに取得しダウンキャストしない。
    if (m_ctx.renderer)
        title += std::string(" [") + m_ctx.renderer->GetBackendName() + "]";
    if (m_window)
        m_window->SetTitle(util::StringUtils::ToWide(title));
}

// =============================================================================
// フレーム
// =============================================================================

void EditorApp::BeginFrame()
{
    // WHY: Play/Pause 中のランタイム変化を Editor の Undo 履歴へ混入させない。
    m_undoStack.SetRecordingEnabled(m_playMode.IsInEditor());

    // Viewport パネルサイズが前フレームで変わった場合は RT を再生成する。
    // main ループの「シーン描画」より前に呼ぶことで、RT のサイズが確定した状態で
    // シーンをレンダリングでき、リサイズ直後のフレームで古い解像度の画像が表示されるのを防ぐ。
    {
        FBZZ_PROFILE_SCOPE("EditorBegin::ResizeViewportRTs");
        ResizeViewportRTsIfNeeded();
    }

    {
        FBZZ_PROFILE_SCOPE("EditorBegin::ImGuiNewFrame");
        m_imguiRenderer->ImGuiNewFrame();
        ImGui::NewFrame();
    }

    UpdatePlayFocusModeControls();

    // WHY: Play/Pause の識別色もブランドテーマ側へ集約し、通常時に旧配色を復元しない。
    EditorTheme::ApplyWorkspaceTint(
        m_playMode.IsPlaying() ? WorkspaceTint::Playing :
        m_playMode.IsPaused()  ? WorkspaceTint::Paused  :
                                 WorkspaceTint::Editor);

    ImGuizmo::BeginFrame();
    {
        FBZZ_PROFILE_SCOPE("EditorBegin::Hotkeys");
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
        // WHY: Prefab 編集モードの帯はツールバー直下・DockSpace の上に置く。
        //      パネルの中に埋めるとレイアウト次第で見えなくなり、
        //      「今アセットを直している」という一番外さしてはいけない前提が伝わらない。
        DrawPrefabEditBar(m_ctx);
        DrawSceneReloadBar(m_ctx);
        DrawBuildNotificationBar(m_ctx);

        ImGuiID dockId = ImGui::GetID("MainDockSpace");
        ProcessMapEditingModeTransition(static_cast<uint32_t>(dockId));
        ProcessPlayViewportLayoutTransition(static_cast<uint32_t>(dockId));
        // StatusBar一段分を残してDockSpaceを描き、ドロワーボタンを常に画面下端へ置く。
        const float statusBarHeight = ImGui::GetFrameHeight() + 2.0f;
        ImGui::DockSpace(dockId, { 0.0f, -statusBarHeight },
            ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_AutoHideTabBar);
        m_statusBar->Draw(m_ctx,
            m_assetBrowserPanel != nullptr ? &m_assetBrowserPanel->visible : nullptr,
            m_consolePanel != nullptr ? &m_consolePanel->visible : nullptr);

        // ModalDialog::OpenPopup は ImGui ウィンドウ (Begin/End) のスコープ内でしか機能しない。
        // DockSpaceHost ウィンドウの内側に置くことでその制約を満たす。
        ModalDialog::OnRender();

        ImGui::End();
    }
}

void EditorApp::RenderPanels(EditorContext& ctx)
{
    // Play ボタンは BeginFrame 内で状態を変えるため、同じフレームの Panel 描画前にも同期する。
    m_undoStack.SetRecordingEnabled(m_playMode.IsInEditor());

    // HotkeyManager の scope 判定に使うフォーカス状態を落とし、これから描くパネルに
    // 立て直させる。
    // WHY: パネルは非表示だと OnRenderContent が呼ばれずフラグを更新できない。
    //      落とさないと「閉じたパネルにフォーカスがある」ままキーが効き続ける。
    //      ここで落とすのは、この直前までの処理 (デバッグカメラ等) が
    //      前フレームの値を読み終わっているため。
    ctx.viewportFocused      = false;
    ctx.sceneViewportHovered = false;
    ctx.hierarchyFocused     = false;
    ctx.assetBrowserFocused  = false;

    // Build Output パネルの表示要求を処理する (StatusBar クリック / 失敗通知バーの Show)。
    if (m_buildOutputPanel) {
        if (ctx.requestFocusBuildError) {
            m_buildOutputPanel->RequestFocusFirstError();  // 表示 ON + 最初のエラーへスクロール
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

        // WHY: RenderPanels 全体の計測だけでは、重いパネルを特定できない。
        //      パネル名は Panel の生存中有効なため、そのまま Profiler marker として利用する。
        const profiler::ProfileScope panelScope(
            profiler::ProfilerMarker(panel->GetWindowName(), "Editor Panels"));
        panel->OnRender(ctx);
    }

    // アセット参照欄 (widgets::AssetPathField) のクリック → 参照先アセットを辿る。
    // 要求は 2 つの独立した仕事に分かれる:
    //   ・Inspector の表示対象をそのアセットへ移す (ダブルクリック) — ここで即座に確定させる
    //   ・Asset Browser の一覧を該当フォルダへ移動して ping する — 次フレームのパネルが消費する
    // WHY ここで中継するか: widgets 層は EditorContext を知らないため、要求は静的チャネルに
    //     積まれる。パネル描画の後に 1 回だけ取り出せば、どのパネルの参照欄から出た要求でも
    //     同じ経路で届く。
    if (widgets::AssetRevealRequest reveal; widgets::ConsumeAssetRevealRequest(reveal)) {
        // Inspector の表示対象の切り替えはここで完結させる。
        //
        // WHY Asset Browser へ任せないか (重要):
        //   以前は ctx.selectedAssetPath を AssetBrowserPanel::HandleRevealRequest だけが
        //   書いていた。あれは OnRenderContent の中にあるため、次の 2 つの条件で
        //   1 度も走らず、参照欄をダブルクリックしても Inspector が沈黙していた。
        //     1. Asset Browser が非アクティブなドッキングタブだと ImGui::Begin が false を
        //        返し OnRenderContent 自体が呼ばれない (IPanel::OnRender / WasContentRendered)。
        //        Inspector と同じドックノードに同居していると常にこの状態になる。
        //     2. 一覧へナビゲートできない参照 (ファイル欠落・ブラウズ可能ルート外 =
        //        エンジン内蔵マテリアル等) では、あちらが選択を書く前に return する。
        //   「参照を辿って中身を見る」は Inspector 単体で成立すべき動線で、
        //   一覧上で位置を示す (ping) のはその副作用にすぎない。責務を分離する。
        if (reveal.selectInInspector) {
            // Sprite 参照 ("<画像>::sprite::<id>") は元画像を Inspector へ出す。
            // ParseSpriteReference は false のときも logicalPath へ元の文字列を書くため、
            // 戻り値を見る必要はない (AssetBrowserPanel::HandleRevealRequest と同じ扱い)。
            std::string logicalPath;
            std::string spriteToken;
            (void)asset::ParseSpriteReference(reveal.path, logicalPath, spriteToken);
            std::string absolute = util::FileSystem::NormalizePathSeparators(
                asset::AssetManager::ResolveAssetPath(logicalPath));
            if (!absolute.empty() && util::FileSystem::Exists(absolute)) {
                ctx.selectedAssetPath = std::move(absolute);
                ctx.selectedEntities.clear();
            }
        }

        ctx.requestRevealAssetPath   = std::move(reveal.path);
        ctx.requestRevealAssetSelect = reveal.selectInInspector;
        for (auto& panel : m_panels) {
            if (std::strcmp(panel->GetWindowName(), "Asset Browser") != 0) continue;
            // 閉じている / 非アクティブなタブに埋もれていると「示した」ことにならないため、
            // 表示 ON + タブを手前へ出すところまでを 1 操作で済ませる。
            panel->visible = true;
            ImGui::SetWindowFocus(panel->GetWindowName());
            break;
        }
    }

    // GPU レンダリング完了後・ImGui フレーム内のここで描画する。
    // RenderSystem は GPU 実行中のため直接 ImGui を呼べず、スナップショットだけ保存している。
    if (m_imguiRenderer && m_resources) {
        FBZZ_PROFILE_SCOPE("EditorPanel::RenderDebugOverlay");
        renderer::RenderDebugOverlay::DrawIfEnabled(*m_imguiRenderer, *m_resources);
    }

    // ── 全パネルの上に重ねるオーバーレイ ─────────────────────────────────────
    // WHY: コマンドパレット (Ctrl+K)・ショートカット一覧 (F1)・選択ヒストリ
    //      (Alt+←/→) は実装だけあってどこからも呼ばれておらず、機能として
    //      存在していなかった。ホットキーの集約に合わせてここで配線する。
    //      パネルより後に描くのは、モーダル的なオーバーレイを最前面に出すため。
    DrawCommandPalette(ctx);
    DrawShortcutsOverlay(ctx);
    // 選択の変化を毎フレーム拾って往復ヒストリへ積む (Alt+←/→ の材料)。
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

    // WHY requestOpenAnimationGraph が「窓を出す」だけか:
    //   どの .animcontroller を開くかは ctx.openAnimationGraph(path) で呼び出し元が
    //   明示する。両方をここで兼ねると、Inspector が渡したパスを直後に
    //   selectedAssetPath で上書きしてしまう (Inspector の選択は GameObject なので、
    //   そこで selectedAssetPath を見ると無関係なアセットを開くことになる)。
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

    if (ctx.requestOpenVFXEditor) {
        ctx.requestOpenVFXEditor = false;
        if (!VFXEditorLauncher::Launch(ctx.projectRoot, ctx.selectedAssetPath))
            FBZZ_LOG_WARN("FBZZVFXEditor.exeを起動できません。VFX Editorターゲットをビルドしてください。");
    }

    // WHY: すべての通常ウィンドウの後に呼ぶことで、オーバーレイが最前面に描画される。
    //      IsActive() == false のときは何もしないのでパネルのないフレームでも安全。
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
    m_waterToolWasActive = m_waterTool && m_waterTool->IsActive();
    m_detailToolWasActive = m_detailTool && m_detailTool->IsActive();
    m_foliageToolWasActive = m_foliageTool && m_foliageTool->IsActive();

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
    if (m_waterTool) m_waterTool->SetActive(m_waterToolWasActive);
    if (m_detailTool) m_detailTool->SetActive(m_detailToolWasActive);
    if (m_foliageTool) m_foliageTool->SetActive(m_foliageToolWasActive);

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

    // WHY: Unity の Maximize On Play に近い挙動として、Play 中だけ Game View を中央 Dock 全体へ広げる。
    //      Stop 時に保存済みレイアウトを復元するため、ユーザーの通常レイアウトは変更しない。
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

    // 左カラムを Map Tools(上) と Scene Hierarchy(下) に分割し、ツールパネルへ専用領域を与える。
    // WHY: 以前は Map Tools を Inspector と同じノードへドックしていたため、オブジェクト選択中は
    //      Inspector の裏のタブへ隠れ、ツール操作のたびにタブ切替が必要で使いづらかった。
    //      Map 編集はツールパネルが主役なので常時見える左カラムへ固定し、Viewport を広く保つ。
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

void EditorApp::UpdatePlayFocusModeControls()
{
    const bool focusedPlay =
        m_ctx.playFocusMode == EditorContext::PlayFocusMode::Focused
        && !m_playMode.IsInEditor()
        && !m_playMode.HasPendingRestore();

    if (focusedPlay && !m_playFocusedCursorHidden) {
        // WHY: 非表示だけでは OS カーソルが画面端に到達して入力が止まる。
        //      Unity の Focused 実行と同様にカーソルを隠し、ウィンドウ中央へロックする。
        core::Cursor::SetLockMode(core::CursorLockMode::Locked);
        core::Cursor::SetVisible(false);
        m_playFocusedCursorHidden = true;
    } else if (!focusedPlay && m_playFocusedCursorHidden) {
        core::Cursor::ResetForEditor();
        m_playFocusedCursorHidden = false;
    }

    if (focusedPlay)
        core::Cursor::ApplyLock();

    if (focusedPlay && m_ctx.activeScene && input::Input::KeyDown(input::KeyCode::ESCAPE)) {
        // WHAT: Focused 実行中の Escape はゲーム入力の解放と PlayMode 終了を兼ねる。
        scene::ScriptRuntime::Override(nullptr);
        m_playMode.Stop(*m_ctx.activeScene);
        m_undoStack.Clear();
        core::Cursor::ResetForEditor();
        m_playFocusedCursorHidden = false;
    }
}

void EditorApp::EndFrame(renderer::IImGuiRenderer& imguiRenderer)
{
    ImGui::Render();
    imguiRenderer.ImGuiRenderDrawData();
    imguiRenderer.ImGuiRenderPlatformWindows();
}

// =============================================================================
// Viewport RT リサイズ
// =============================================================================

void EditorApp::ResizeViewportRTsIfNeeded()
{
    if (!m_renderer) return;

    auto resizeRT = [this](renderer::ResourceHandle<renderer::RenderTargetTag>& rt,
                           ViewportPanel* panel,
                           float width,
                           float height) {
        if (!rt.IsValid() || !panel) return;

        const uint32_t vpW = static_cast<uint32_t>(width);
        const uint32_t vpH = static_cast<uint32_t>(height);
        if (vpW == 0 || vpH == 0) return;
        auto* currentRT = m_resources->Get(rt);
        if (currentRT && vpW == currentRT->GetWidth() && vpH == currentRT->GetHeight()) return;

        const auto previousRT = rt;
        rt = m_resources->CreateRenderTarget(vpW, vpH);
        // WHY: ハンドルの上書きだけでは旧DX11リソースがResourceManagerに残り、
        //      Dock操作を繰り返すほどVRAM使用量とPresent待機が増える。
        if (previousRT.IsValid())
            m_resources->Release(previousRT);
        panel->hdrRT = rt;
    };

    resizeRT(m_sceneViewportRT, m_sceneViewportPanel, m_ctx.viewportWidth,     m_ctx.viewportHeight);
    resizeRT(m_gameViewportRT,  m_gameViewportPanel,  m_ctx.gameViewportWidth,  m_ctx.gameViewportHeight);
    // WHY: UI Viewport は専用 RT を持たず、Game View の完成済み RT を参照する。
    //      リサイズ後もパネル側のハンドルを張り直して、古い RT 参照が残らないようにする。
    if (m_uiViewportPanel)
        m_uiViewportPanel->hdrRT = m_gameViewportRT;

    // VFX Preview は独立ウィンドウ内の利用可能領域にだけ追従する。
    // 1px 単位の揺れで毎フレーム RT を作り直さないよう Panel 側で整数化した寸法を受ける。
    if (m_vfxEditorPanel && m_vfxPreviewRT.IsValid()) {
        const uint32_t width = static_cast<uint32_t>(m_vfxEditorPanel->Session().preview.width);
        const uint32_t height = static_cast<uint32_t>(m_vfxEditorPanel->Session().preview.height);
        if (width > 0 && height > 0) {
            auto* currentRT = m_resources->Get(m_vfxPreviewRT);
            if (currentRT && (currentRT->GetWidth() != width || currentRT->GetHeight() != height)) {
                const auto previousRT = m_vfxPreviewRT;
                m_vfxPreviewRT = m_resources->CreateRenderTarget(width, height);
                m_resources->Release(previousRT);
                m_vfxEditorPanel->Session().preview.renderTarget = m_vfxPreviewRT;
            }
        }
    }
}

// =============================================================================
// IModule — app::Run() から呼ばれるライフサイクル
// =============================================================================

bool EditorApp::OnInit()
{
    // Init() と OpenProject() は app::Run() の前に呼ばれているため、
    // ここでは Post-project セットアップだけを担う。
    m_runtime.ApplySettings(m_ctx.projectSettings);
    m_runtime.ApplyAdditionalUIContext(m_ctx.projectSettings, m_sceneUICtx);
    m_runtime.BindExternalScene(m_scene.get());
    m_runtime.RegisterScenes(util::FileSystem::PathFromUtf8(m_ctx.projectRoot), *m_resources);

    // WHY: OpenProject() 時点では editorCamera が nullptr のため設定を直接適用できない。
    //      OnInit() で editorCamera を確定させた後に保存値を適用する。
    //      EditorSettings のデフォルト値が従来のハードコード値 {0,2.5,-8} と一致するため
    //      初回起動時も同じ初期位置になる。
    m_debugCamera.camera.m_position = { m_settings.cameraLastPx, m_settings.cameraLastPy, m_settings.cameraLastPz };
    m_debugCamera.camera.m_rotation = { m_settings.cameraLastRx, m_settings.cameraLastRy, m_settings.cameraLastRz, m_settings.cameraLastRw };
    m_debugCamera.camera.m_aspect   = 1920.0f / 1080.0f;
    m_ctx.editorCamera = &m_debugCamera.camera;

    // VFX Preview は原点に生成されるため、Scene View Camera と独立した再現可能な初期構図を持つ。
    m_vfxPreviewCamera.camera.m_position = { 0.0f, 1.0f, -5.0f };
    m_vfxPreviewCamera.LookAt({ 0.0f, 0.5f, 0.0f });

    if (auto* rt = m_resources->Get(m_sceneViewportRT))
        m_debugCamera.camera.m_aspect =
            static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());

    WarmupRenderResources();
    return true;
}

void EditorApp::OnUpdate(float dt)
{
    BeginFrame();

    // WHY: Prefab 編集モードの出入りはシーンの中身を丸ごと差し替える。パネル描画の
    //      途中でやると、その後のパネルが破棄済みの GameObject を掴むため、
    //      フレーム先頭のこの位置でだけ処理する。
    ProcessPrefabEditRequests();

    // WHY 同じ位置で処理するか: アセットの中身を差し替えると、その版を読むパネルと
    //      既に読み終えたパネルが同じフレームで食い違う。パネル描画に入る前に済ませる。
    ProcessAssetDiskReloads();
    // guid → パスの索引をディスクへ追従させる。変化が無いフレームは何もしない。
    // WHY エディターだけで書くか: 索引は人と外部ツールが guid を引くための道具で、
    //     製品ビルドには要らない。読み取り専用の配置先へ書きにいかせない。
    asset::AssetDatabase::FlushIndexFile();
    // WHY アセットより後か: シーンを開き直すと、その中で参照されるアセットを
    //      新しい版で読み直せる。逆順だと「開き直した直後だけ古いアセットを掴む」
    //      1 フレームができる。
    ProcessSceneDiskReload();

    if (m_aiPipeServer && m_aiPipeServer->IsRunning() && m_aiDispatcher) {
        m_aiDispatcher->SetSceneViewportRT(m_sceneViewportRT);
        m_aiDispatcher->SetGameViewportRT(m_gameViewportRT);
        m_aiPipeServer->DrainRequests([this](const std::string& request) {
            return HandleAiRequest(request);
        });
    }

    auto* playMode = m_ctx.playMode;
    if (playMode->ApplyPendingRestore(*m_scene)) {
        // WHY: World は m_contactCache / m_prevEvents を保持するため、
        //      Stop 復元時に丸ごとリセットしないと前 Play セッションの Collider* が残る。
        m_runtime.ResetPhysics(m_ctx.projectSettings);
        RestoreEditorHiding();  // Stop 復元後に editor-only 非表示を再適用

        // WHY: Play 中に ScriptProxy 経由で LoadScene が呼ばれると SceneManager 内の
        //      m_externalScene が nullptr にリセットされる (SceneManager.cpp LoadScene 処理)。
        //      Stop 後もその状態が残ると TransformEditorPreview が m_active (ゲームシーン) に
        //      対して動作し、m_scene の worldPosition が永遠に 0 のままになる。
        //      再設定することで CurrentScene() が m_scene を返すよう回復する。
        m_runtime.BindExternalScene(m_scene.get());
        // WHY: SceneSerializer はローカル position のみ復元し worldPosition はゼロになる。
        //      この後の Update で TransformEditorPreview が走るが、同フレーム内の
        //      OnRender より先に worldPosition を正確にしておくため即時フラッシュする。
        scene::FlushWorldTransforms(*m_scene);

        // Serializer が設定する needsBake=true を上書きしてベイク済み NavMesh を復元する。
        // WHY: navMesh はランタイムキャッシュのため TOML 非保存。Play→Stop のたびに再ベイクが
        //      走らないよう、Play 開始前に保存したキャッシュを差し戻す。
        if (!m_navMeshPlayCache.empty()) {
            for (scene::EntityID eid : m_scene->GetEntities<scene::NavMeshSurfaceComponent>()) {
                auto* surf = m_scene->GetComponent<scene::NavMeshSurfaceComponent>(eid);
                auto* go   = m_scene->GetGameObject(eid);
                if (!surf || !go) continue;
                auto it = m_navMeshPlayCache.find(go->instanceId);
                if (it != m_navMeshPlayCache.end()) {
                    surf->navMesh   = std::move(it->second);
                    surf->bakeState = scene::NavMeshBakeState::Done;
                    surf->needsBake = false;
                }
            }
            m_navMeshPlayCache.clear();
        }
    }

    if (!playMode->IsPlaying()) {
        m_debugCamera.moveSpeed = m_ctx.cameraSpeed;
        m_debugCamera.mouseSens = m_ctx.cameraSensitivity;
        const bool vfxViewportHovered = m_vfxEditorPanel != nullptr
            && m_vfxEditorPanel->IsPreviewHovered();
        if (!vfxViewportHovered)
            m_debugCamera.Update(dt, m_ctx.sceneViewportHovered);
        UpdateFocusAnim(dt);
        // ナビゲーションギズモが「ピボット固定で視点だけ回す」ために読む。
        m_ctx.editorCameraPivot         = m_debugCamera.Pivot();
        m_ctx.editorCameraFocusDistance = m_debugCamera.FocusDistance();
    }

    const bool stepFrame   = playMode->ConsumeStep();
    m_simulationDt         = stepFrame ? (1.0f / 60.0f) : dt;

    if (playMode->IsPlaying()) {
        // WHY: ビューポートリサイズに追従するため毎フレーム更新する。
        //      ProjectRuntimeが所有するScriptRuntimeを更新すればScriptProxy全体へ反映される。
        m_runtime.UpdateScriptViewport(
            static_cast<uint32_t>(m_ctx.gameViewportWidth),
            static_cast<uint32_t>(m_ctx.gameViewportHeight));
    }

    m_runtime.Update(m_simulationDt, m_ctx.projectSettings,
                     playMode->IsPlaying() || stepFrame, stepFrame);

    const bool aiVfxPreviewActive = m_ctx.vfxAiPreviewUntilFrame != 0
        && Time::frameCount <= m_ctx.vfxAiPreviewUntilFrame;
    if (m_vfxEditorPanel
        && (m_vfxEditorPanel->WantsPreviewRender() || aiVfxPreviewActive)
        && m_vfxEditorPanel->UsesIsolatedPreviewWorld() && m_vfxPreviewScene) {
        m_vfxPreviewSceneManager.SetPhysicsHz(m_ctx.projectSettings.physics.hz);
        m_vfxPreviewSceneManager.Update(m_simulationDt, m_vfxPreviewPhysicsWorld);
    }
    // VFX専用Viewportだけがホバーされている間、専用Cameraへ入力を渡す。
    // WHY: Scene View Cameraを共有すると、VFXの構図調整がメインEditorの視点を壊すため。
    if (m_vfxEditorPanel && m_vfxEditorPanel->WantsPreviewRender()
        && m_vfxEditorPanel->IsPreviewHovered())
        m_vfxPreviewCamera.Update(dt, true);
}

void EditorApp::OnLateUpdate(float dt)
{
    (void)dt;
    m_runtime.LateUpdate(m_simulationDt);
    const bool aiVfxPreviewActive = m_ctx.vfxAiPreviewUntilFrame != 0
        && Time::frameCount <= m_ctx.vfxAiPreviewUntilFrame;
    if (m_vfxEditorPanel
        && (m_vfxEditorPanel->WantsPreviewRender() || aiVfxPreviewActive)
        && m_vfxEditorPanel->UsesIsolatedPreviewWorld() && m_vfxPreviewScene)
        m_vfxPreviewSceneManager.LateUpdate(m_simulationDt, m_vfxPreviewPhysicsWorld);
}

void EditorApp::OnRender()
{
    if (auto* rt = m_resources->Get(m_sceneViewportRT))
        m_debugCamera.camera.m_aspect =
            static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());

    float gameAspect = m_debugCamera.camera.m_aspect;
    if (auto* rt = m_resources->Get(m_gameViewportRT))
        gameAspect = static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());

    // WHY: Play 中に LoadScene が発生すると m_scene は遷移前のシーンのままになる。
    //      カメラ解決・カリングマスク計算は新シーンのコンポーネントを参照する必要があるため、
    //      Play 中は SceneManager::GetActive() を優先する。
    scene::Scene* const resolveScene     = (m_playMode.IsPlaying() && m_runtime.GetActiveScene())
                                              ? m_runtime.GetActiveScene() : m_scene.get();
    const renderer::Camera  gameCamera       = scene::ResolveEditorGameCamera(*resolveScene, m_debugCamera.camera, gameAspect);
    const fbzz::LayerMask   gameCullingMask  = scene::ResolveGameCullingMask(*resolveScene);

    m_renderer->BeginFrame();

    // 実際に画面へ出ているビューポートだけを描く。
    // WHY: Scene View と Game View は同じシーンをそれぞれフル描画する (Shadow / GBuffer /
    //      ライティング / ポストプロセス一式) ため、両方を毎フレーム回すと描画コストが素で 2 倍になる。
    //      しかも Play 中は Game View だけ、Map 編集中は Scene View だけを残してもう片方を
    //      visible = false にする構成なので、隠れている側の 1 周ぶんは完全な捨て仕事だった。
    // NOTE: WasContentRendered() はパネル描画が本関数より後にあるため 1 フレーム遅れの情報。
    //       visible と併せて見ることで、閉じられた瞬間はその場で止まり、
    //       開き直したときは (隠す前の値が残っているため) 待たずに描き始められる。
    const auto isViewportShowing = [](const ViewportPanel* panel) {
        return panel != nullptr && panel->visible && panel->WasContentRendered();
    };
    // AI が RT を読み出す間は表示状態に関わらず描き続ける (キャプチャが古い絵を掴まないように)。
    const bool aiViewportCaptureActive = m_ctx.aiViewportRenderUntilFrame != 0
        && Time::frameCount <= m_ctx.aiViewportRenderUntilFrame;

    const bool needSceneView = isViewportShowing(m_sceneViewportPanel) || aiViewportCaptureActive;
    // Game View の RT は UI Viewport が背景として共有する (m_uiViewportPanel->hdrRT = m_gameViewportRT)。
    // どちらか一方でも出ていれば描かないと、UI 編集画面が止まった絵のままになる。
    const bool needGameView = isViewportShowing(m_gameViewportPanel)
        || isViewportShowing(m_uiViewportPanel)
        || aiViewportCaptureActive;

    if (needSceneView)
        RenderSceneView(gameCamera, gameCullingMask);
    if (needGameView)
        RenderGameView(gameCamera, gameCullingMask);
    RenderVFXPreview();

    m_renderer->SetRenderTarget({}, *m_resources);
    m_renderer->Clear({ 0.02f, 0.02f, 0.02f, 1.0f });

    // WHY: Play 中に LoadScene でシーン遷移が発生すると SceneManager が新シーンを所有し、
    //      m_scene は遷移前の古いシーンのままになる。
    //      パネル描画は遷移後シーンを参照する必要があるため GetActive() で解決する。
    {
        scene::Scene* const nextActive = m_playMode.IsPlaying()
            ? m_runtime.GetActiveScene()
            : m_scene.get();
        // シーン遷移を検知したら旧シーンの EntityID を持つ selectedEntities をクリアする。
        // WHY: 遷移後シーンで同じ index を持つ別 Entity が選択状態に見えるのを防ぐ。
        if (nextActive != m_ctx.activeScene)
            m_ctx.selectedEntities.clear();
        m_ctx.activeScene = nextActive;
    }
    // Animation Preview はウィンドウが閉じていても選択対象と再生時刻を保持する。
    // WHY: Preview パネルの OnRenderContent だけに任せると、非表示タブや Inspector の
    //      初回表示では選択変化を拾えず、再アタッチするまでプレビューが更新されない。
    TickAnimationPreview(m_ctx);
    RenderPanels(m_ctx);
    // AssetBrowserから独立VFXEditorウィンドウ上でreleaseされたdragをIPC dropへ変換する。
    VFXEditorLauncher::UpdateTrackedAssetDrag();
    EndFrame(*m_imguiRenderer);

    m_renderer->EndFrame();
}

void EditorApp::OnShutdown()
{
    // WHY: FreeLibrary より前に全スクリプトの OnDestroy と destructor を
    //      DLL コードが有効なうちに実行する。ProjectRuntime::Shutdown はEditor外部Sceneと
    //      Play中のシーン遷移で残ったManager所有Sceneの両方を破棄する。
    m_runtime.Shutdown();
    m_vfxPreviewSceneManager.ClearScenes();
    // Unload(nullptr) で DestroyAllScripts をスキップする (Clear() 済みのため)
    m_ctx.activeScene = nullptr;
    m_ctx.vfxPreviewScene = nullptr;
    Shutdown();
}

// =============================================================================
// IModule — プライベートヘルパー
// =============================================================================

void EditorApp::WarmupRenderResources()
{
    // WHY: RenderSystem は初回呼び出しで shader / PSO / shadow map / GBuffer などを lazy initialize する。
    //      その負荷を最初の可視フレームに乗せると起動直後だけ FPS 表示が大きく落ちるため、
    //      メインループ開始前に 1 回描画してリソースを先行生成する。
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
        scene::RenderSystem(*m_scene, *m_renderer, *m_resources,
                            m_debugCamera.camera, sceneRT, nullptr,
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
        scene::RenderSystem(*m_scene, *m_renderer, *m_resources,
                            warmupCamera, gameRT,
                            &m_ctx.projectSettings.render,
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
        // WHY: Unity の Frame Selected と同様、対象バウンズの大きさに応じて
        //      カメラ距離を変える。半径 0 (バウンズ不明) は従来の固定距離。
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

void EditorApp::RenderSceneView(const renderer::Camera& /*gameCamera*/, fbzz::LayerMask /*gameCullingMask*/)
{
    FBZZ_PROFILE_SCOPE("EditorApp::RenderSceneView");
    const auto sceneRT = m_sceneViewportRT;
    m_renderer->SetRenderTarget(sceneRT, *m_resources);
    m_renderer->Clear({ 0.05f, 0.05f, 0.08f, 1.0f });

    auto sceneRenderSettings = m_ctx.projectSettings.render;
    sceneRenderSettings.selectedObjects.clear();
    // Unity同様、親GameObjectを選択した場合は描画可能な子孫も同じSelection Maskへ合成する。
    // VFXGraphComponentのランタイム生成ノードもowner配下なので、エフェクト全体が一つの輪郭になる。
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
        sceneRenderSettings.showConstraints = sceneRenderSettings.showColliders;
        if (m_playMode.IsPlaying())
            sceneRenderSettings.showNavMesh = false;
        // WHY: Play 中に LoadScene が発生すると m_scene は遷移前のシーンのまま。
        //      SceneView も新シーンをエディタカメラで描画する。
    scene::Scene* const sceneViewScene = (m_playMode.IsPlaying() && m_runtime.GetActiveScene())
        ? m_runtime.GetActiveScene() : m_scene.get();
        // Scene View はデバッグカメラの視点なので、ゲームカメラのカリング設定は持ち込まない。
        // WHY: Unity と同様、Occlusion Culling を切ったゲームカメラの都合で
        //      編集用ビューの見え方が変わると、何を編集しているのか分からなくなる。
        //      cullingMask を Everything にしているのと同じ理由。
        // オクルージョンだけは編集側から切り替えられるようにしてある。既定は無効で、
        // 「見えているのに消える」を疑ったときに Debug メニューから入れて比較する。
        scene::CameraCullingSettings sceneViewCulling{};
        sceneViewCulling.occlusionCulling = m_ctx.sceneViewOcclusionCulling;
        scene::RenderSystem(*sceneViewScene, *m_renderer, *m_resources,
                            m_debugCamera.camera, sceneRT, &sceneRenderSettings,
                        fbzz::Layer::Everything, &uiOptions, &m_runtime.GetPhysicsWorld(),
                        &sceneViewCulling);
    }
}

void EditorApp::RenderVFXPreview()
{
    const bool aiVfxPreviewActive = m_ctx.vfxAiPreviewUntilFrame != 0
        && Time::frameCount <= m_ctx.vfxAiPreviewUntilFrame;
    if (!m_vfxEditorPanel
        || (!m_vfxEditorPanel->WantsPreviewRender() && !aiVfxPreviewActive)
        || !m_vfxPreviewRT.IsValid()) return;

    scene::Scene* renderScene = m_vfxPreviewScene.get();
    if (!renderScene) return;

    float previewAspect = 0.0f;
    if (auto* rt = m_resources->Get(m_vfxPreviewRT)) {
        if (rt->GetHeight() > 0)
            previewAspect = static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());
    }
    // AI capture 中に camera 引数が来ていれば、その視点で描く。
    // WHY: 同じ RT を UI プレビューと共有しているため、AI 要求中だけ差し替える。
    //      常時差し替えると担当者のカメラ操作が奪われる。
    const renderer::Camera previewCamera = ai::MakeVFXPreviewCamera(
        aiVfxPreviewActive ? m_ctx.vfxAiPreviewCamera : EditorContext::VFXAiPreviewCamera{},
        m_vfxPreviewCamera.camera, previewAspect);
    // Preview上のTransformギズモが、描画に使ったのと同じ行列で投影できるようにする。
    m_vfxEditorPanel->Session().preview.camera = previewCamera;
    m_vfxEditorPanel->Session().preview.cameraValid = true;

    m_renderer->SetRenderTarget(m_vfxPreviewRT, *m_resources);
    m_renderer->Clear({ 0.018f, 0.021f, 0.028f, 1.0f });

    // WHY: 専用 Preview は見た目の評価画像なので、Scene View の選択輪郭・Grid・Gizmo・UI を混ぜない。
    //      AI capture も同じ RT を読むことで、操作 UI と評価対象の画を分離できる。
    auto settings = m_ctx.projectSettings.render;
    settings.selectedObjects.clear();
    // 床グリッドはUI操作時のみ任意表示。AI capture(aiVfxPreviewActive)ではクリーンな画を保つ。
    settings.showGrid = m_vfxEditorPanel->Session().preview.showFloorGrid && !aiVfxPreviewActive;
    // ギズモと Overdraw は通常 AI capture へ持ち込まないが、AI が vfx.preview の
    // view で明示要求したときだけは診断用に許可する (既定の評価画は変えない)。
    settings.showVFXGizmos = aiVfxPreviewActive
        ? m_ctx.vfxAiPreviewGizmos
        : m_vfxEditorPanel->Session().preview.showGizmos;
    if (aiVfxPreviewActive) {
        settings.particleOverdrawView = m_ctx.vfxAiPreviewOverdraw;
        // AI capture は実時間性能を要求されないので、overdraw 要求中は数値も併せて取る。
        // UI側はDebugメニューで要求した1フレームだけ読み戻し、常時同期は避ける。
        settings.particleOverdrawReadback = m_ctx.vfxAiPreviewOverdraw;
    } else {
        settings.particleOverdrawView = m_vfxEditorPanel->Session().preview.overdrawView;
        settings.particleOverdrawIncludeModels =
            m_vfxEditorPanel->Session().preview.includeModelsInOverdraw;
        settings.particleOverdrawReadback =
            m_vfxEditorPanel->Session().preview.overdrawReadbackRequested;
    }
    settings.showSkeleton = false;
    settings.showLightRange = false;
    settings.showConstraints = false;
    settings.showColliders = false;
    settings.showNavMesh = false;
    if (!aiVfxPreviewActive) {
        const auto& session = m_vfxEditorPanel->Session();
        const auto appendSelection = [&](scene::EntityID id) {
            if (id.IsValid()) settings.selectedObjects.push_back({ id.index, id.generation });
        };
        if (session.graphMode) {
            bool wholeEffect = session.selectedNodeId <= 0;
            for (const auto& node : session.document.graph.nodes)
                if (node.id == session.selectedNodeId && node.type == asset::VFXNodeType::Entry)
                    wholeEffect = true;
            std::vector<scene::EntityID> graphRoots{ session.preview.graphEntity };
            graphRoots.insert(graphRoots.end(), session.preview.copyEntities.begin(),
                              session.preview.copyEntities.end());
            for (const scene::EntityID rootId : graphRoots) {
                auto* root = renderScene->GetGameObject(rootId);
                auto* graph = root != nullptr
                    ? root->GetComponent<scene::VFXGraphComponent>() : nullptr;
                if (graph == nullptr) continue;
                for (const auto& state : graph->runtimeNodes)
                    if (wholeEffect || state.nodeId == session.selectedNodeId)
                        appendSelection(state.entity);
            }
        } else {
            appendSelection(session.preview.selectedEntity);
        }
        settings.showSelectionOutline = !settings.selectedObjects.empty();
        settings.outlineWidth = 0.025f;
        settings.outlineColor[0] = 1.0f;
        settings.outlineColor[1] = 0.58f;
        settings.outlineColor[2] = 0.12f;
        settings.outlineColor[3] = 1.0f;
    } else {
        settings.showSelectionOutline = false;
    }
    scene::RenderSystemUIOptions uiOptions{};
    uiOptions.enabled = false;
    // Scene View と同じ理由でカリング設定を明示する。
    // WHY: 省略すると ResolveGameCullingSettings がプレビュー World の
    //      メインカメラを探しに行く。今は見つからず既定へ落ちているだけで、
    //      プレビューへカメラを 1 個置いた瞬間にゲーム側の maxDrawDistance などが
    //      効き始め、「エフェクトが途中から消える」理由が辿れなくなる。
    const scene::CameraCullingSettings previewCulling{};
    scene::RenderSystem(*renderScene, *m_renderer, *m_resources,
                        previewCamera, m_vfxPreviewRT, &settings,
                        fbzz::Layer::Everything, &uiOptions,
                        &m_vfxPreviewPhysicsWorld, &previewCulling);
    if (!aiVfxPreviewActive)
        m_vfxEditorPanel->Session().preview.overdrawReadbackRequested = false;
}

void EditorApp::RenderGameView(const renderer::Camera& gameCamera, fbzz::LayerMask gameCullingMask)
{
    FBZZ_PROFILE_SCOPE("EditorApp::RenderGameView");
    const auto gameRT = m_gameViewportRT;
    if (!gameRT.IsValid()) return;

    // WHY: Play 中に LoadScene でシーン遷移すると SceneManager が新シーンを所有するため、
    //      m_scene（遷移前）ではなく GetActive() を参照してゲームビューに正しいシーンを描く。
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
    // WHY: ゲームビューポート外のクリック (Inspector 等) が UIButton に届かないよう、
    //      マウスがビューポート矩形内にあるときだけ mousePressed を渡す。
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
    scene::RenderSystem(*renderScene, *m_renderer, *m_resources,
                        gameCamera, gameRT,
                        &m_ctx.projectSettings.render,
                        gameCullingMask, &uiOptions);
}

} // namespace fbzz::editor
