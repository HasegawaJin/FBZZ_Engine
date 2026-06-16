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
#include <Editor/GraphLayoutSerializer.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/ModalDialog.hpp>
#include <Editor/Util/FileDialog.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Panels/SceneHierarchyPanel.hpp>
#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/Panels/ViewportPanel.hpp>
#include <Editor/Panels/ConsolePanel.hpp>
#include <Editor/Panels/AssetBrowserPanel.hpp>
#include <Editor/Panels/StatusBar.hpp>
#include <Editor/Panels/ProjectSettingsPanel.hpp>
#include <Editor/Panels/BuildSettingsPanel.hpp>
#include <Editor/Panels/AnalysisPanel.hpp>
#include <Editor/Panels/AnimationGraphPanel.hpp>
#include <Editor/Panels/MapEditorPanel.hpp>
#include "Tools/TerrainTool.hpp"
#include "Tools/WaterTool.hpp"
#include "Tools/DetailTool.hpp"
#include "Tools/FoliageTool.hpp"
#include <Engine/Core/Application.hpp>
#include <Engine/Renderer/DebugCamera.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/RenderDebugOverlay.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/SceneUtils.hpp>
#include <Engine/Scene/Systems/DebugDrawSystem.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Core/Window.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <imgui_impl_win32.h>
#include <toml++/toml.hpp>
#include <Windows.h>
#include <filesystem>
#include <utility>

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
    if (util::StringUtils::ToLower(util::FileSystem::GetExtension(scenePath)) != ".fbzz") return false;
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
        return;
    }

    const std::string targetName = table["project"]["target_name"].value_or(std::string{});
    if (!targetName.empty() && targetName.rfind("{{", 0) != 0) {
        ctx.standaloneTargetName = targetName + "Standalone";
        ctx.projectTargetName    = targetName;
    }

    const std::string engineRoot = table["engine"]["root"].value_or(std::string{});
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
EditorApp::EditorApp()  = default;
EditorApp::~EditorApp() = default;

bool EditorApp::Init(renderer::IRenderer& renderer, renderer::IImGuiRenderer& imguiRenderer, renderer::ResourceManager& resources, core::Window& window)
{
    m_hwnd          = window.GetHandle();
    m_renderer      = &renderer;
    m_imguiRenderer = &imguiRenderer;
    m_resources     = &resources;

    window.SetWndProcHook([this](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> bool {
        if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp) != 0)
            return true;
        if (msg == WM_CLOSE) {
            RequestExit();
            return true;
        }
        return false;
    });

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuizmo::SetImGuiContext(ImGui::GetCurrentContext());
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // WHY: IniFilename は OpenProject() で projectRoot が確定してから設定する。
    //      Init() 時点では projectRoot が空なので nullptr にしておき、
    //      最初の NewFrame() で自動ロードされないようにする。
    io.IniFilename = nullptr;

    EditorTheme::Apply();

    imguiRenderer.ImGuiInit(m_hwnd);

    m_ctx.undoStack   = &m_undoStack;
    m_ctx.playMode    = &m_playMode;
    m_ctx.renderer    = &renderer;
    m_ctx.imguiRenderer = &imguiRenderer;
    m_ctx.resources   = &resources;
    m_ctx.memorySystem = &core::Application::Get().GetMemorySystem();
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

    m_panels.push_back(std::make_unique<SceneHierarchyPanel>());
    m_panels.push_back(std::make_unique<InspectorPanel>());
    {
        auto graph = std::make_unique<AnimationGraphPanel>();
        graph->visible = false;
        m_panels.push_back(std::move(graph));
    }
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
    m_panels.push_back(std::make_unique<ConsolePanel>(m_consoleSink));
    {
        auto assets = std::make_unique<AssetBrowserPanel>("Assets");
        m_assetBrowserPanel = assets.get();
        m_panels.push_back(std::move(assets));
    }
    m_panels.push_back(std::make_unique<StatusBar>());
    {
        auto ps = std::make_unique<ProjectSettingsPanel>();
        ps->visible = false;
        m_projectSettingsPanel = ps.get();
        m_panels.push_back(std::move(ps));
    }
    {
        auto bs = std::make_unique<BuildSettingsPanel>();
        bs->visible = false;
        m_buildSettingsPanel = bs.get();
        m_panels.push_back(std::move(bs));
    }
    {
        auto analysis = std::make_unique<AnalysisPanel>();
        analysis->visible = false;
        m_analysisPanel = analysis.get();
        m_panels.push_back(std::move(analysis));
    }
    {
        auto mapEditor = std::make_unique<MapEditorPanel>();
        mapEditor->visible = false;
        m_mapEditorPanel = mapEditor.get();
        m_panels.push_back(std::move(mapEditor));
    }

    for (auto& panel : m_panels)
        panel->OnInit(m_ctx);

    RegisterDefaultHotkeys();

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

    // シーンはここで生成し activeScene にバインドする。
    // OpenProject() が activeScene を参照するため Init() で確立しておく必要がある。
    m_scene = std::make_unique<scene::Scene>();
    m_ctx.activeScene = m_scene.get();

    FBZZ_LOG_INFO("EditorApp init done");
    UpdateWindowTitle();
    return true;
}

void EditorApp::Shutdown()
{
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

    // --- EditorContext → EditorSettings への書き戻し ----------------------
    // WHY: パネルやメインループは EditorContext のライブ値を直接変更する。
    //      Shutdown 時にここで一括書き戻さないと、起動時に読んだ初期値が
    //      そのまま保存されてしまい、ユーザーの変更が永続化されない。
    m_settings.showGrid           = m_ctx.showGrid;
    m_settings.gridSize           = m_ctx.gridSize;
    m_settings.snapEnabled        = m_ctx.snapEnabled;
    m_settings.snapDistance       = m_ctx.snapDistance;
    m_settings.gizmoMode          = static_cast<int>(m_ctx.gizmoMode);
    m_settings.gizmoSpace         = static_cast<int>(m_ctx.gizmoSpace);
    m_settings.showLightRange     = m_ctx.showLightRange;
    m_settings.showSkeleton       = m_ctx.showSkeleton;
    m_settings.showStats          = m_ctx.showStats;
    m_settings.hotReloadEnabled   = m_ctx.hotReloadEnabled;
    m_settings.showTerrainTool    = m_ctx.showTerrainTool;
    m_settings.showWaterTool      = m_ctx.showWaterTool;
    m_settings.showDetailTool     = m_ctx.showDetailTool;
    m_settings.showFoliageTool    = m_ctx.showFoliageTool;
    m_settings.gameViewportAspect = static_cast<int>(m_ctx.gameViewportAspect);
    m_settings.cameraSpeed           = m_ctx.cameraSpeed;
    m_settings.cameraSensitivity     = m_ctx.cameraSensitivity;
    m_settings.assetBrowserIconSize  = m_ctx.assetBrowserIconSize;
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

    // Inspector 折り畳み状態を ImGui StateStorage から回収して設定に書き戻す
    if (ImGuiWindow* win = ImGui::FindWindowByName("Inspector")) {
        m_settings.inspectorSectionState.clear();
        for (const auto& entry : win->StateStorage.Data)
            m_settings.inspectorSectionState.emplace_back(entry.key, entry.val_i != 0);
    }

    // Debug メニュー - レンダリングオーバーレイ
    m_settings.showColliders        = m_ctx.projectSettings.render.showColliders;
    m_settings.showTerrainCollision = m_ctx.projectSettings.render.showTerrainCollision;
    m_settings.showDecalBounds      = m_ctx.projectSettings.render.showDecalBounds;
    m_settings.viewMode        = static_cast<int>(m_ctx.projectSettings.render.viewMode);
    m_settings.shadowEnabled   = m_ctx.projectSettings.render.shadowEnabled;
    // Debug メニュー - Post Process
    {
        const auto& pp             = m_ctx.projectSettings.render.postProcess;
        m_settings.ppFxaaEnabled              = pp.fxaaEnabled;
        m_settings.ppExposure                 = pp.exposure;
        m_settings.ppBloomEnabled             = pp.bloom.enabled;
        m_settings.ppBloomIntensity           = pp.bloom.intensity;
        m_settings.ppAoEnabled                = pp.ambientOcclusion.enabled;
        m_settings.ppFogEnabled               = pp.fog.enabled;
        m_settings.ppFogDensity               = pp.fog.density;
        m_settings.ppFogFar                   = pp.fog.farDistance;
        m_settings.ppColorGradingEnabled      = pp.colorGrading.enabled;
        m_settings.ppContrast                 = pp.colorGrading.contrast;
        m_settings.ppSaturation               = pp.colorGrading.saturation;
        m_settings.ppHueShift                 = pp.colorGrading.hueShift;
        m_settings.ppVignetteEnabled          = pp.vignette.enabled;
        m_settings.ppFilmGrainEnabled         = pp.filmGrain.enabled;
        m_settings.ppSharpenEnabled           = pp.sharpen.enabled;
        m_settings.ppSharpenStrength          = pp.sharpen.strength;
        m_settings.ppDofEnabled               = pp.depthOfField.enabled;
        m_settings.ppDofFocus                 = pp.depthOfField.focusDistance;
        m_settings.ppDofBlur                  = pp.depthOfField.blurRadius;
        m_settings.ppChromaticAberrationEnabled = pp.lens.chromaticAberrationEnabled;
        m_settings.ppLensDistortionEnabled    = pp.lens.distortionEnabled;
        m_settings.ppSepiaEnabled             = pp.stylized.sepiaEnabled;
        m_settings.ppInvertEnabled            = pp.stylized.invertEnabled;
        m_settings.ppPosterizeEnabled         = pp.stylized.posterizeEnabled;
        m_settings.ppPixelateEnabled          = pp.stylized.pixelateEnabled;
        m_settings.ppPosterizeLevels          = pp.stylized.posterizeLevels;
        m_settings.ppPixelSize                = pp.stylized.pixelSize;
    }

    m_settings.Save(m_ctx.projectRoot + "/Assets/EditorConfig/editor_settings.toml", m_ctx.projectRoot);
    m_ctx.projectSettings.Save(m_projectSettingsPath);
    m_sceneViewportRT = {};
    m_gameViewportRT  = {};
    m_imguiRenderer->ImGuiShutdown();
    ImGui::DestroyContext();
}

bool EditorApp::OpenProject(const std::string& projectRoot, const std::string& projectSettingsPath, const std::string& scenePath)
{
    if (!m_ctx.activeScene || !m_resources) return false;

    m_projectRoot      = projectRoot;
    m_ctx.projectRoot  = projectRoot;

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
        m_ctx.snapEnabled        = m_settings.snapEnabled;
        m_ctx.snapDistance       = m_settings.snapDistance;
        m_ctx.gizmoMode          = static_cast<EditorContext::GizmoMode>(m_settings.gizmoMode);
        m_ctx.gizmoSpace         = static_cast<EditorContext::GizmoSpace>(m_settings.gizmoSpace);
        m_ctx.showLightRange     = m_settings.showLightRange;
        m_ctx.showSkeleton       = m_settings.showSkeleton;
        m_ctx.showStats          = m_settings.showStats;
        m_ctx.hotReloadEnabled   = m_settings.hotReloadEnabled;
        m_ctx.showTerrainTool    = m_settings.showTerrainTool;
        m_ctx.showWaterTool      = m_settings.showWaterTool;
        m_ctx.showDetailTool     = m_settings.showDetailTool;
        m_ctx.showFoliageTool    = m_settings.showFoliageTool;
        m_ctx.gameViewportAspect = static_cast<EditorContext::GameViewportAspect>(m_settings.gameViewportAspect);
        m_ctx.cameraSpeed        = m_settings.cameraSpeed;
        m_ctx.cameraSensitivity  = m_settings.cameraSensitivity;
        m_ctx.assetBrowserIconSize = m_settings.assetBrowserIconSize;
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
    m_ctx.projectSettings.render.showTerrainCollision = m_settings.showTerrainCollision;
    m_ctx.projectSettings.render.showDecalBounds      = m_settings.showDecalBounds;
    m_ctx.projectSettings.render.viewMode        = static_cast<renderer::ViewMode>(m_settings.viewMode);
    m_ctx.projectSettings.render.shadowEnabled   = m_settings.shadowEnabled;
    {
        auto& pp                          = m_ctx.projectSettings.render.postProcess;
        pp.fxaaEnabled                    = m_settings.ppFxaaEnabled;
        pp.exposure                       = m_settings.ppExposure;
        pp.bloom.enabled                  = m_settings.ppBloomEnabled;
        pp.bloom.intensity                = m_settings.ppBloomIntensity;
        pp.ambientOcclusion.enabled       = m_settings.ppAoEnabled;
        pp.fog.enabled                    = m_settings.ppFogEnabled;
        pp.fog.density                    = m_settings.ppFogDensity;
        pp.fog.farDistance                = m_settings.ppFogFar;
        pp.colorGrading.enabled           = m_settings.ppColorGradingEnabled;
        pp.colorGrading.contrast          = m_settings.ppContrast;
        pp.colorGrading.saturation        = m_settings.ppSaturation;
        pp.colorGrading.hueShift          = m_settings.ppHueShift;
        pp.vignette.enabled               = m_settings.ppVignetteEnabled;
        pp.filmGrain.enabled              = m_settings.ppFilmGrainEnabled;
        pp.sharpen.enabled                = m_settings.ppSharpenEnabled;
        pp.sharpen.strength               = m_settings.ppSharpenStrength;
        pp.depthOfField.enabled           = m_settings.ppDofEnabled;
        pp.depthOfField.focusDistance     = m_settings.ppDofFocus;
        pp.depthOfField.blurRadius        = m_settings.ppDofBlur;
        pp.lens.chromaticAberrationEnabled = m_settings.ppChromaticAberrationEnabled;
        pp.lens.distortionEnabled         = m_settings.ppLensDistortionEnabled;
        pp.stylized.sepiaEnabled          = m_settings.ppSepiaEnabled;
        pp.stylized.invertEnabled         = m_settings.ppInvertEnabled;
        pp.stylized.posterizeEnabled      = m_settings.ppPosterizeEnabled;
        pp.stylized.pixelateEnabled       = m_settings.ppPixelateEnabled;
        pp.stylized.posterizeLevels       = m_settings.ppPosterizeLevels;
        pp.stylized.pixelSize             = m_settings.ppPixelSize;
    }

    // WHY: SceneIO::Load() がシーン内の ScriptComponent を復元する際に
    //      ScriptFactory からファクトリ関数を引く。DLL が未ロードだとスクリプトインスタンスが
    //      生成されず Play 中も OnUpdate() が呼ばれない。
    //      必ずシーンロードより前に DLL をロードして ScriptFactory を準備する。
    InitScriptDll();

    // WHY: PrefabSerializer は Editor プロジェクトにあり Engine から直接呼べないため、
    //      Script::SetInstantiateFn で実装を注入する。ScriptSceneProxy::Instantiate が
    //      ここを経由して PrefabSerializer::Instantiate を呼ぶ。
    //      PrefabRef::path は Assets 起点の相対パス ("Assets/Foo.fbzzprefab") で保存されるため、
    //      ToProjectAssetDiskPath でプロジェクトルートを補完して絶対パスへ変換してから渡す。
    const std::string capturedRoot = projectRoot;
    scene::Script::SetInstantiateFn([capturedRoot](scene::Scene& s, const std::string& path,
                                                   std::vector<scene::EntityID>& roots) {
        const std::string diskPath = ToProjectAssetDiskPath(capturedRoot, path);
        return PrefabSerializer::Instantiate(s, diskPath, roots);
    });

    std::string sceneToOpen = scenePath;
    const std::string lastScenePath = ResolveScenePathForProject(projectRoot, m_settings.lastScenePath);
    if (IsScenePathInsideProject(projectRoot, lastScenePath)) {
        sceneToOpen = lastScenePath;
    } else if (sceneToOpen.empty() && !m_ctx.projectSettings.runtime.startScene.empty()) {
        sceneToOpen = ResolveScenePathForProject(projectRoot, m_ctx.projectSettings.runtime.startScene);
    }

    if (!sceneToOpen.empty()) {
        if (!SceneIO::Load(*m_ctx.activeScene, sceneToOpen)) {
            FBZZ_LOG_ERROR("Open project scene failed: %s", sceneToOpen.c_str());
            return false;
        }
        m_settings.lastScenePath = sceneToOpen;
        m_ctx.currentScenePath   = sceneToOpen;
        GraphLayoutSerializer::Load(m_ctx.graphLayouts, sceneToOpen);
        m_ctx.selectedEntities.clear();
        RebuildEditorUIFromScene();
        CaptureCleanScene();
    }

    FBZZ_LOG_INFO("Opened project: %s", m_projectRoot.c_str());
    UpdateWindowTitle();

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

    const std::string sceneName = m_ctx.currentScenePath.empty()
        ? "Untitled"
        : util::FileSystem::GetFilename(m_ctx.currentScenePath);

    if (m_titleInitialized &&
        m_lastTitleDirty     == m_ctx.sceneDirty &&
        m_lastTitleScenePath == m_ctx.currentScenePath)
        return;

    m_titleInitialized   = true;
    m_lastTitleDirty     = m_ctx.sceneDirty;
    m_lastTitleScenePath = m_ctx.currentScenePath;

    std::string title = "FBZZ Editor - " + sceneName;
    if (m_ctx.sceneDirty) title += "*";
    SetWindowTextW(m_hwnd, util::StringUtils::ToWide(title).c_str());
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

    // WHY: Unity 同様、Play 中・Pause 中はエディターとの区別を一目で把握できるようにする。
    //      ImGui のスタイルカラーをフレームごとに上書きすることで
    //      全ウィンドウ背景にティントを掛けられる。
    //      Push/Pop ではなくフレームごと直接書き換えることで、
    //      ウィンドウ単位でなく全体へ適用できる。
    {
        ImGuiStyle& style = ImGui::GetStyle();
        if (m_playMode.IsPlaying()) {
            // Play 中: 青系ティント (#1A2433)
            style.Colors[ImGuiCol_WindowBg]  = { 0.10f, 0.14f, 0.20f, 1.0f };
            style.Colors[ImGuiCol_ChildBg]   = { 0.08f, 0.12f, 0.18f, 1.0f };
            style.Colors[ImGuiCol_MenuBarBg] = { 0.07f, 0.10f, 0.16f, 1.0f };
        } else if (m_playMode.IsPaused()) {
            // Pause 中: 黄系ティント (#2E2614)
            style.Colors[ImGuiCol_WindowBg]  = { 0.18f, 0.16f, 0.10f, 1.0f };
            style.Colors[ImGuiCol_ChildBg]   = { 0.15f, 0.13f, 0.08f, 1.0f };
            style.Colors[ImGuiCol_MenuBarBg] = { 0.13f, 0.11f, 0.07f, 1.0f };
        } else {
            // Editor モードのデフォルト色を毎フレーム復元する
            style.Colors[ImGuiCol_WindowBg]  = { 0.173f, 0.173f, 0.173f, 1.0f }; // BG_BASE
            style.Colors[ImGuiCol_ChildBg]   = { 0.141f, 0.141f, 0.141f, 1.0f }; // BG_DARK
            style.Colors[ImGuiCol_MenuBarBg] = { 0.102f, 0.102f, 0.102f, 1.0f }; // BG_DARKEST
        }
    }

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
        ImGuiWindowFlags_MenuBar;

    {
        FBZZ_PROFILE_SCOPE("EditorBegin::DockSpace");
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    { 0.0f, 0.0f });
        ImGui::Begin("##DockSpaceHost", nullptr, hostFlags);
        ImGui::PopStyleVar(3);

        BuildMenuBar(m_ctx);
        BuildPlayToolbar(m_ctx);

        ImGuiID dockId = ImGui::GetID("MainDockSpace");
        ProcessMapEditingModeTransition(static_cast<uint32_t>(dockId));
        ImGui::DockSpace(dockId, { 0, 0 }, ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_AutoHideTabBar);

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

    for (auto& panel : m_panels) {
        if (!panel->visible) continue;

        // WHY: RenderPanels 全体の計測だけでは、重いパネルを特定できない。
        //      パネル名は Panel の生存中有効なため、そのまま Profiler marker として利用する。
        const profiler::ProfileScope panelScope(
            profiler::ProfilerMarker(panel->GetWindowName(), "Editor Panels"));
        panel->OnRender(ctx);
    }

    // GPU レンダリング完了後・ImGui フレーム内のここで描画する。
    // RenderSystem は GPU 実行中のため直接 ImGui を呼べず、スナップショットだけ保存している。
    if (m_imguiRenderer && m_resources) {
        FBZZ_PROFILE_SCOPE("EditorPanel::RenderDebugOverlay");
        renderer::RenderDebugOverlay::DrawIfEnabled(*m_imguiRenderer, *m_resources);
    }

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

void EditorApp::BuildMapEditingLayout(uint32_t dockId)
{
    const ImGuiID root = static_cast<ImGuiID>(dockId);
    ImGui::DockBuilderRemoveNode(root);
    ImGui::DockBuilderAddNode(
        root, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(root, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = root;
    ImGuiID hierarchy = 0;
    ImGuiID inspector = 0;
    ImGuiID tools = 0;
    ImGuiID assets = 0;
    ImGui::DockBuilderSplitNode(
        center, ImGuiDir_Left, 0.16f, &hierarchy, &center);
    ImGui::DockBuilderSplitNode(
        center, ImGuiDir_Right, 0.24f, &inspector, &center);
    ImGui::DockBuilderSplitNode(
        center, ImGuiDir_Down, 0.24f, &assets, &center);

    tools = inspector;
    ImGui::DockBuilderDockWindow("Scene Hierarchy", hierarchy);
    ImGui::DockBuilderDockWindow("Inspector", inspector);
    ImGui::DockBuilderDockWindow("Map Tools", tools);
    ImGui::DockBuilderDockWindow("Asset Browser", assets);
    ImGui::DockBuilderDockWindow("Scene", center);
    ImGui::DockBuilderFinish(root);
}

void EditorApp::EndFrame(renderer::IImGuiRenderer& imguiRenderer)
{
    ImGui::Render();
    imguiRenderer.ImGuiRenderDrawData();
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

        rt         = m_resources->CreateRenderTarget(vpW, vpH);
        panel->hdrRT = rt;
    };

    resizeRT(m_sceneViewportRT, m_sceneViewportPanel, m_ctx.viewportWidth,     m_ctx.viewportHeight);
    resizeRT(m_gameViewportRT,  m_gameViewportPanel,  m_ctx.gameViewportWidth,  m_ctx.gameViewportHeight);
    // WHY: UI Viewport は専用 RT を持たず、Game View の完成済み RT を参照する。
    //      リサイズ後もパネル側のハンドルを張り直して、古い RT 参照が残らないようにする。
    if (m_uiViewportPanel)
        m_uiViewportPanel->hdrRT = m_gameViewportRT;
}

// =============================================================================
// IModule — app::Run() から呼ばれるライフサイクル
// =============================================================================

bool EditorApp::OnInit()
{
    // Init() と OpenProject() は app::Run() の前に呼ばれているため、
    // ここでは Post-project セットアップだけを担う。
    scene::ApplyPhysicsSettings(m_physicsWorld, m_ctx.projectSettings);
    scene::ApplyUISettings(m_ctx.projectSettings, &m_gameUICtx);
    scene::ApplyUISettings(m_ctx.projectSettings, &m_sceneUICtx);

    m_sceneManager.SetScene(m_scene.get());
    m_sceneManager.SetPhysicsHz(m_ctx.projectSettings.physics.hz);

    m_debugCamera.camera.m_position = { 0.0f, 2.5f, -8.0f };
    m_debugCamera.camera.m_aspect   = 1920.0f / 1080.0f;
    m_ctx.editorCamera = &m_debugCamera.camera;

    if (auto* rt = m_resources->Get(m_sceneViewportRT))
        m_debugCamera.camera.m_aspect =
            static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());

    WarmupRenderResources();
    return true;
}

void EditorApp::OnUpdate(float dt)
{
    BeginFrame();

    auto* playMode = m_ctx.playMode;
    if (playMode->ApplyPendingRestore(*m_scene)) {
        // WHY: World は m_contactCache / m_prevEvents を保持するため、
        //      Stop 復元時に丸ごとリセットしないと前 Play セッションの Collider* が残る。
        m_physicsWorld = physics::World{};
        scene::ApplyPhysicsSettings(m_physicsWorld, m_ctx.projectSettings);
    }

    if (!playMode->IsPlaying()) {
        m_debugCamera.moveSpeed = m_ctx.cameraSpeed;
        m_debugCamera.mouseSens = m_ctx.cameraSensitivity;
        m_debugCamera.Update(dt, m_ctx.sceneViewportHovered);
        UpdateFocusAnim(dt);
    }

    const bool stepFrame   = playMode->ConsumeStep();
    m_simulationDt         = stepFrame ? (1.0f / 60.0f) : dt;

    m_sceneManager.SetSimulating(playMode->IsPlaying() || stepFrame);
    m_sceneManager.SetSingleStep(stepFrame);

    if (playMode->IsPlaying()) {
        scene::ApplyPhysicsSettings(m_physicsWorld, m_ctx.projectSettings);
        scene::Script::SetPhysicsWorld(&m_physicsWorld);
    }

    m_sceneManager.Update(m_simulationDt, m_physicsWorld);
}

void EditorApp::OnLateUpdate(float dt)
{
    (void)dt;
    m_sceneManager.LateUpdate(m_simulationDt, m_physicsWorld);
}

void EditorApp::OnRender()
{
    if (auto* rt = m_resources->Get(m_sceneViewportRT))
        m_debugCamera.camera.m_aspect =
            static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());

    float gameAspect = m_debugCamera.camera.m_aspect;
    if (auto* rt = m_resources->Get(m_gameViewportRT))
        gameAspect = static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());

    const renderer::Camera  gameCamera       = scene::ResolveEditorGameCamera(*m_scene, m_debugCamera.camera, gameAspect);
    const fbzz::LayerMask   gameCullingMask  = scene::ResolveGameCullingMask(*m_scene);

    m_renderer->BeginFrame();

    RenderSceneView(gameCamera, gameCullingMask);
    RenderGameView(gameCamera, gameCullingMask);

    m_renderer->SetRenderTarget({}, *m_resources);
    m_renderer->Clear({ 0.02f, 0.02f, 0.02f, 1.0f });

    m_ctx.activeScene = m_scene.get();
    RenderPanels(m_ctx);
    EndFrame(*m_imguiRenderer);

    m_renderer->EndFrame();
}

void EditorApp::OnShutdown()
{
    // WHY: FreeLibrary より前に全スクリプトの OnDestroy と destructor を
    //      DLL コードが有効なうちに実行する。
    if (m_scene)
        m_scene->Clear();
    scene::Script::SetPhysicsWorld(nullptr);
    // Unload(nullptr) で DestroyAllScripts をスキップする (Clear() 済みのため)
    m_ctx.activeScene = nullptr;
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
        scene::RenderSystem(*m_scene, *m_renderer, *m_resources,
                            m_debugCamera.camera, sceneRT, nullptr,
                            fbzz::Layer::Everything, &uiOptions);
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
        uiOptions.context       = &m_gameUICtx;
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

    if (m_ctx.requestFocusOnSelected) {
        m_ctx.requestFocusOnSelected = false;
        const math::Vector3 target = m_ctx.focusTargetPosition;
        const math::Vector3 dir    = m_debugCamera.camera.m_position - target;
        const float         dist   = dir.Length();
        const math::Vector3 camDir = (dist > 0.01f)
            ? dir * (1.0f / dist)
            : math::Vector3{ 0.0f, 0.5f, -1.0f }.Normalized();
        m_focusAnim.active   = true;
        m_focusAnim.startPos = m_debugCamera.camera.m_position;
        m_focusAnim.endPos   = target + camDir * kFocusDist;
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
    const auto sceneRT = m_sceneViewportRT;
    m_renderer->SetRenderTarget(sceneRT, *m_resources);
    m_renderer->Clear({ 0.05f, 0.05f, 0.08f, 1.0f });

    auto sceneRenderSettings = m_ctx.projectSettings.render;
    sceneRenderSettings.selectedObjects.clear();
    sceneRenderSettings.selectedObjects.reserve(m_ctx.selectedEntities.size());
    for (scene::EntityID id : m_ctx.selectedEntities)
        sceneRenderSettings.selectedObjects.push_back({ id.index, id.generation });

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
        scene::RenderSystem(*m_scene, *m_renderer, *m_resources,
                            m_debugCamera.camera, sceneRT, &sceneRenderSettings,
                            fbzz::Layer::Everything, &uiOptions);
    }

    const auto& render = m_ctx.projectSettings.render;
    if (render.showColliders || render.showTerrainCollision) {
        renderer::DebugDraw::BeginFrame(*m_renderer, *m_resources,
                                        m_debugCamera.camera.GetViewProjection());
        if (render.showColliders) {
            scene::ColliderDebugDrawSystem(*m_scene, *m_renderer);
            scene::ConstraintDebugDrawSystem(m_physicsWorld, *m_renderer);
        }
        if (render.showTerrainCollision)
            scene::TerrainCollisionDebugDrawSystem(*m_scene, *m_renderer,
                                                    m_debugCamera.camera.m_position);
        renderer::DebugDraw::Flush();
    }

    if (m_ctx.showSkeleton)
        scene::AnimatorDebugDrawSystem(*m_scene, *m_renderer, *m_resources,
                                        m_debugCamera.camera.GetViewProjection());
    if (m_ctx.showGrid)
        scene::GridDebugDrawSystem(*m_renderer, *m_resources,
                                    m_debugCamera.camera.GetViewProjection());
    if (m_ctx.showLightRange)
        scene::LightRangeDebugDrawSystem(*m_scene, *m_renderer, *m_resources,
                                          m_debugCamera.camera.GetViewProjection());
}

void EditorApp::RenderGameView(const renderer::Camera& gameCamera, fbzz::LayerMask gameCullingMask)
{
    const auto gameRT = m_gameViewportRT;
    if (!gameRT.IsValid()) return;

    m_renderer->SetRenderTarget(gameRT, *m_resources);
    m_renderer->Clear({ 0.02f, 0.02f, 0.05f, 1.0f });

    float w = 1920.0f, h = 1080.0f;
    if (auto* rt = m_resources->Get(gameRT)) {
        w = static_cast<float>(rt->GetWidth());
        h = static_cast<float>(rt->GetHeight());
    }
    scene::RenderSystemUIOptions uiOptions{};
    uiOptions.enabled       = true;
    uiOptions.viewportWidth  = w;
    uiOptions.viewportHeight = h;
    uiOptions.targetView    = scene::UIRenderTargetView::GameViewport;
    uiOptions.context       = &m_gameUICtx;
    scene::RenderSystem(*m_scene, *m_renderer, *m_resources,
                        gameCamera, gameRT,
                        &m_ctx.projectSettings.render,
                        gameCullingMask, &uiOptions);
}

} // namespace fbzz::editor
