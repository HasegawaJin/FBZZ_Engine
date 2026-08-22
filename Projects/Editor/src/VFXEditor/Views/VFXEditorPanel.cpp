// FBZZ Engine
// VFXEditorPanel.cpp | fbzz::editor
// VFX Editor の合成ルート View 実装 (DockSpace・メニュー・ツールバー)
#include <Editor/VFXEditor/Views/VFXEditorPanel.hpp>

#include <Editor/VFXEditor/Application/VFXEditorSession.hpp>
#include <Editor/VFXEditor/Document/VFXGraphOps.hpp>
#include <Editor/VFXEditor/Views/VFXEditorUiCommon.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Panels/StatusBar.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/ParticleEditWidgets.hpp>
#include <Editor/Util/ParticleEmitterModules.hpp>
#include <Editor/Util/SchemaInspector.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/ProceduralVFXTextures.hpp>
#include <Engine/Asset/VFXAuthoringSchema.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/ShaderCompileDiagnostics.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/ParticleOverdrawStats.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <imnodes.h>
#include <toml++/toml.hpp>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <span>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbzz::editor {

// 共有ヘルパー (ノード色・アイコン・パラメーター編集など) は fbzz::editor::vfx に
// 隔離してある。Editor 全体の名前空間を汚さないための境界。
using namespace vfx;

void VFXEditorPanel::OnBeforeBegin(EditorContext&)
{
    if (m_standaloneApplicationMode) {
        // 独立AppではルートWindowをOSクライアント領域全面へ追従させる。
        // WHY: 子パネルとAsset Browserの配置・寸法は、内部DockSpaceが一元管理するため。
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
        return;
    }
    // WHY: NoAutoMerge を VFX Editor だけに付け、他の通常パネルは従来どおり DockSpace で管理する。
    //      OS ウィンドウのライフサイクル自体は ImGui backend に任せ、Win32/DX11 型を漏らさない。
    ImGuiWindowClass windowClass{};
    // 固定 ClassId は ImGui::Begin 前でも安全で、ini のレイアウト識別もフレーム間で安定する。
    windowClass.ClassId = static_cast<ImGuiID>(0x56465845u); // "VFXE"
    windowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&windowClass);

    // WHAT: メインEditorと完全に重ねず、右側の空き領域・別モニター・カスケード配置の順に選ぶ。
    // WHY: 同じ位置と大きさでは背面へ回ったVFX Editorの端すら見えず、閉じたように見えるため。
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    if (mainViewport != nullptr) {
        ImVec2 targetPos = mainViewport->Pos;
        ImVec2 targetSize = mainViewport->Size;
        const ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
        const ImVec2 mainCenter{ mainViewport->Pos.x + mainViewport->Size.x * 0.5f,
                                 mainViewport->Pos.y + mainViewport->Size.y * 0.5f };
        const ImGuiPlatformMonitor* mainMonitor = nullptr;
        for (const ImGuiPlatformMonitor& monitor : platform.Monitors) {
            if (mainCenter.x >= monitor.WorkPos.x && mainCenter.x < monitor.WorkPos.x + monitor.WorkSize.x
                && mainCenter.y >= monitor.WorkPos.y && mainCenter.y < monitor.WorkPos.y + monitor.WorkSize.y) {
                mainMonitor = &monitor;
                break;
            }
        }
        // 別モニターがあれば同じ大きさを保ち、右側のモニターを優先する。
        const ImGuiPlatformMonitor* secondary = nullptr;
        for (const ImGuiPlatformMonitor& monitor : platform.Monitors) {
            if (&monitor == mainMonitor) continue;
            if (secondary == nullptr
                || (monitor.WorkPos.x >= mainViewport->Pos.x + mainViewport->Size.x
                    && secondary->WorkPos.x < mainViewport->Pos.x + mainViewport->Size.x))
                secondary = &monitor;
        }
        if (secondary != nullptr) {
            targetPos = secondary->WorkPos;
            targetSize = { (std::min)(mainViewport->Size.x, secondary->WorkSize.x),
                           (std::min)(mainViewport->Size.y, secondary->WorkSize.y) };
        } else if (mainMonitor != nullptr) {
            constexpr float GAP = 10.0f;
            // 最小ウィンドウ制約と同じ幅を要求し、配置直後に右端が画面外へ押し出されるのを防ぐ。
            constexpr float MIN_USEFUL_WIDTH = 960.0f;
            const float rightX = mainViewport->Pos.x + mainViewport->Size.x + GAP;
            const float rightSpace = mainMonitor->WorkPos.x + mainMonitor->WorkSize.x - rightX;
            if (rightSpace >= MIN_USEFUL_WIDTH) {
                // Editor右側に実用幅がある場合は、その空きを余白のまま残さずVFX制作領域へ使う。
                targetPos = { rightX, mainViewport->Pos.y };
                targetSize = { rightSpace,
                    (std::min)(mainViewport->Size.y,
                               mainMonitor->WorkPos.y + mainMonitor->WorkSize.y - mainViewport->Pos.y) };
            } else {
                // 同一画面で横並びにできない場合も、右下へずらしてクリック可能な縁を必ず残す。
                constexpr float CASCADE = 36.0f;
                targetSize = { (std::min)(mainViewport->Size.x, mainMonitor->WorkSize.x - CASCADE),
                               (std::min)(mainViewport->Size.y, mainMonitor->WorkSize.y - CASCADE) };
                targetPos = {
                    std::clamp(mainViewport->Pos.x + CASCADE, mainMonitor->WorkPos.x,
                        mainMonitor->WorkPos.x + mainMonitor->WorkSize.x - targetSize.x),
                    std::clamp(mainViewport->Pos.y + CASCADE, mainMonitor->WorkPos.y,
                        mainMonitor->WorkPos.y + mainMonitor->WorkSize.y - targetSize.y)
                };
            }
        }
        const ImGuiCond placementCondition = m_resetWindowPlacementRequested
            ? ImGuiCond_Always : ImGuiCond_Appearing;
        ImGui::SetNextWindowPos(targetPos, placementCondition);
        ImGui::SetNextWindowSize(targetSize, placementCondition);
    }
    ImGui::SetNextWindowSizeConstraints({ 960.0f, 640.0f }, { FLT_MAX, FLT_MAX });
    if (m_focusWindowRequested) {
        ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
        ImGui::SetNextWindowFocus();
    }
    m_focusWindowRequested = false;
    m_resetWindowPlacementRequested = false;
}


void VFXEditorPanel::RequestFocusAndReveal(bool resetPlacement)
{
    visible = true;
    m_focusWindowRequested = true;
    m_resetWindowPlacementRequested = m_resetWindowPlacementRequested || resetPlacement;
}


void VFXEditorPanel::OpenDroppedAsset(EditorContext& ctx, const std::string& path)
{
    if (IsVFXAssetPath(path)) {
        m_session.requestedAssetPath = path;
        return;
    }
    const bool actorAsset = EndsWithInsensitive(path, ".fbx")
        || EndsWithInsensitive(path, ".gltf") || EndsWithInsensitive(path, ".glb")
        || EndsWithInsensitive(path, ".anim")
        || EndsWithInsensitive(path, ".animcontroller")
        || EndsWithInsensitive(path, ".animctrl")
        || EndsWithInsensitive(path, ".mat");
    if (actorAsset) {
        std::string error;
        if (!m_session.preview.LoadPreviewAsset(ctx, path, &error))
            m_session.document.error = std::move(error);
        else
            m_session.SaveEditorPreferences();
        return;
    }
    if (m_session.graphMode) m_canvas.AddNodeFromAsset(path);
    else m_session.CreatePreviewEmitterFromAsset(ctx, path);
}


bool VFXEditorPanel::SaveCurrentGraph()
{
    const bool saved = m_session.SaveGraph();
    if (saved) m_session.preview.restartRequested = true;
    return saved;
}


bool VFXEditorPanel::ReloadCurrentGraphFromDisk(const std::string& changedPath)
{
    (void)changedPath;
    if (m_session.document.path.empty()) return true;
    if (m_session.document.dirty || !m_session.LoadGraph(m_session.document.path)) return false;
    m_session.preview.restartRequested = true;
    return true;
}


void VFXEditorPanel::OnInit(EditorContext& ctx)
{
    // ImNodes のコンテキスト所有権は Canvas View に閉じる。Panel は生成・破棄の
    // タイミング (Panel ライフサイクル) だけを与える。
    m_canvas.CreateContexts();
    // Session は View を知らないので、Graph 差し替え時の後始末はフックで受け取る。
    m_session.onGraphReplaced = [this](bool resetPanning) { m_canvas.OnGraphReplaced(resetPanning); };
    m_session.onSelectNodes = [this](std::vector<int> ids) { m_canvas.RequestSelection(std::move(ids)); };
    // キャンバスの右クリックメニューへ Template 一覧を差し込む (実体は Panel が持つ)。
    m_canvas.onDrawTemplateMenu = [this](EditorContext& context) { DrawGraphTemplateMenu(context); };
    m_session.LoadEditorPreferences(ctx);
}


void VFXEditorPanel::OnShutdown()
{
    m_session.SaveEditorPreferences();
    m_canvas.DestroyContexts();
}

void VFXEditorPanel::DrawShaderCompileErrorBanner()
{
    const auto diagnostics = renderer::GetShaderCompileDiagnostics();
    const auto firstError = std::find_if(diagnostics.begin(), diagnostics.end(),
        [](const renderer::ShaderCompileDiagnostic& item) { return item.isError; });
    if (firstError == diagnostics.end()) return;

    const int errorCount = static_cast<int>(std::count_if(diagnostics.begin(), diagnostics.end(),
        [](const renderer::ShaderCompileDiagnostic& item) { return item.isError; }));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{ 0.24f, 0.055f, 0.055f, 1.0f });
    ImGui::BeginChild("##VFXShaderCompileErrorBanner", { 0.0f, 66.0f }, true);
    ImGui::TextColored({ 1.0f, 0.36f, 0.30f, 1.0f }, "SHADER COMPILE ERROR (%d)", errorCount);
    ImGui::SameLine();
    if (ImGui::SmallButton("Show Details")) m_shaderDiagnosticsOpen = true;
    ImGui::TextDisabled("%s%s%s",
        firstError->path.c_str(),
        firstError->entryPoint.empty() ? "" : "  |  ",
        firstError->entryPoint.c_str());
    // コンパイラ本文の先頭行だけを常設し、全ログは専用Windowへ逃がして制作領域を潰さない。
    const std::size_t lineEnd = firstError->message.find_first_of("\r\n");
    const std::string firstLine = firstError->message.substr(0, lineEnd);
    ImGui::TextWrapped("%s", firstLine.c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void VFXEditorPanel::DrawShaderCompileDebugMenuItems()
{
    const auto diagnostics = renderer::GetShaderCompileDiagnostics();
    const int errorCount = static_cast<int>(std::count_if(diagnostics.begin(), diagnostics.end(),
        [](const renderer::ShaderCompileDiagnostic& item) { return item.isError; }));
    const int warningCount = static_cast<int>(diagnostics.size()) - errorCount;
    ImGui::Separator();
    ImGui::TextDisabled("Shader Compiler");
    char label[96];
    std::snprintf(label, sizeof(label), "Compile Diagnostics (%d errors, %d warnings)",
                  errorCount, warningCount);
    if (ImGui::MenuItem(label, nullptr, false, !diagnostics.empty()))
        m_shaderDiagnosticsOpen = true;
    if (ImGui::MenuItem("Clear Shader Diagnostics", nullptr, false, !diagnostics.empty()))
        renderer::ClearShaderCompileDiagnostics();
}

void VFXEditorPanel::DrawShaderCompileDiagnosticsWindow()
{
    if (!m_shaderDiagnosticsOpen) return;
    if (!ImGui::Begin("Shader Compile Errors###VFXShaderCompileErrors",
                      &m_shaderDiagnosticsOpen, ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
        ImGui::End();
        return;
    }

    const auto diagnostics = renderer::GetShaderCompileDiagnostics();
    if (diagnostics.empty()) {
        ImGui::TextDisabled("No shader compile diagnostics.");
        ImGui::End();
        return;
    }
    if (ImGui::Button("Clear")) {
        renderer::ClearShaderCompileDiagnostics();
        ImGui::End();
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d records", static_cast<int>(diagnostics.size()));
    ImGui::Separator();
    for (const auto& diagnostic : diagnostics) {
        ImGui::PushID(static_cast<int>(diagnostic.sequence));
        const ImVec4 color = diagnostic.isError
            ? ImVec4{ 1.0f, 0.38f, 0.32f, 1.0f }
            : ImVec4{ 1.0f, 0.76f, 0.28f, 1.0f };
        ImGui::TextColored(color, "%s", diagnostic.isError ? "ERROR" : "WARNING");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", diagnostic.path.c_str());
        if (!diagnostic.entryPoint.empty() || !diagnostic.target.empty()) {
            ImGui::TextDisabled("Entry: %s    Target: %s",
                                diagnostic.entryPoint.c_str(), diagnostic.target.c_str());
        }
        if (ImGui::SmallButton("Copy Message"))
            ImGui::SetClipboardText(diagnostic.message.c_str());
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(diagnostic.message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::End();
}


void VFXEditorPanel::OnRenderContent(EditorContext& ctx)
{
    // 過去フレームやiniに残ったホストのスクロール量を破棄する。
    // DockingWindow自身のスクロールは各Windowが個別に保持するため、編集領域の操作性には影響しない。
    ImGui::SetScrollX(0.0f);
    ImGui::SetScrollY(0.0f);
    m_session.preview.renderRequested = true;
    m_session.preview.hovered = false;
    if (m_session.preview.restorePending && ctx.vfxPreviewScene != nullptr) {
        m_session.preview.restorePending = false;
        const std::string model = m_session.preview.actorModelPath;
        const std::string controller = m_session.preview.actorControllerPath;
        const std::string clip = m_session.preview.actorClipPath;
        const std::string material = m_session.preview.actorMaterialPath;
        std::string ignored;
        if (m_session.preview.LoadPreviewAsset(ctx, model, &ignored)) {
            if (!controller.empty())
                (void)m_session.preview.LoadPreviewAsset(ctx, controller, &ignored);
            if (!clip.empty())
                (void)m_session.preview.LoadPreviewAsset(ctx, clip, &ignored);
            if (!material.empty())
                (void)m_session.preview.LoadPreviewAsset(ctx, material, &ignored);
        }
    }
    if (!m_session.requestedAssetPath.empty()) {
        ctx.selectedAssetPath = std::move(m_session.requestedAssetPath);
        m_session.requestedAssetPath.clear();
        // ドリルダウン以外 (Asset Browser や D&D) で別の .vfx を開いたら、
        // 前の階層のパン屑は無関係になる。残すと存在しない親へ「戻る」ことになる。
        if (!m_session.subGraphNavigationPending) m_session.subGraphBreadcrumb.clear();
        m_session.subGraphNavigationPending = false;
    }
    m_session.graphMode = IsVFXAssetPath(ctx.selectedAssetPath);
    if (m_standaloneApplicationMode && !m_session.graphMode && ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Graph...", "Ctrl+O", false,
                                static_cast<bool>(ctx.requestOpenVFXAssetDialog)))
                ctx.requestOpenVFXAssetDialog();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (m_session.assetBrowserVisible != nullptr)
                ImGui::MenuItem("Asset Browser", nullptr, m_session.assetBrowserVisible);
            if (ImGui::MenuItem("Reset Dock Layout"))
                m_resetDockLayoutRequested = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Debug")) {
            bool preferencesChanged = false;
            if (ImGui::MenuItem("Particle Overdraw", nullptr, &m_session.preview.overdrawView)) {
                m_session.preview.restartRequested = true;
                m_session.preview.overdrawReadbackRequested = true;
            }
            preferencesChanged |= ImGui::MenuItem("VFX Gizmos", nullptr, &m_session.preview.showGizmos);
            preferencesChanged |= ImGui::MenuItem("Floor Grid", nullptr, &m_session.preview.showFloorGrid);
            ImGui::MenuItem("Pause", "Space", &m_session.preview.paused);
            if (ImGui::MenuItem("Restart")) m_session.preview.restartRequested = true;
            if (ImGui::MenuItem("Measure Overdraw Now"))
                m_session.preview.overdrawReadbackRequested = true;
            const scene::ParticleOverdrawStats& overdraw = scene::GetLastParticleOverdrawStats();
            ImGui::Separator();
            if (overdraw.valid) {
                ImGui::Text("Overdraw %.2fx  Mean %.2f  Max %.1f",
                            overdraw.overdrawFactor, overdraw.meanLayers, overdraw.maxLayers);
            } else {
                ImGui::TextDisabled("Overdraw: enable Particle Overdraw to measure");
            }
            DrawShaderCompileDebugMenuItems();
            if (preferencesChanged) m_session.SaveEditorPreferences();
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    if (!m_session.graphMode) DrawShaderCompileErrorBanner();
    if (m_standaloneApplicationMode && !m_session.graphMode && !ImGui::GetIO().WantTextInput
        && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O) && ctx.requestOpenVFXAssetDialog)
        ctx.requestOpenVFXAssetDialog();
    if (m_session.graphMode != m_session.previousGraphMode && ctx.vfxPreviewScene != nullptr) {
        // Graph実行物と単体Emitterを同じPreview Worldへ残さず、モード切替時に所有物を明確に分ける。
        ctx.vfxPreviewScene->Clear();
        m_session.preview.graphEntity = scene::EntityID::INVALID;
        m_session.preview.selectedEntity = scene::EntityID::INVALID;
        m_session.previousGraphMode = m_session.graphMode;
    }
    if (m_session.graphMode) {
        DrawGraphEditor(ctx);
        DrawShaderCompileDiagnosticsWindow();
        return;
    }

    if (ctx.vfxPreviewScene == nullptr) {
        ImGui::TextDisabled("VFX Preview World is unavailable.");
        return;
    }

    // WHY: EditorContextを複製してSceneと選択だけをPreview Worldへ差し替えることで、既存の
    //      Particle Inspectorを再利用しながらメインSceneのHierarchy・Undo・Dirtyを一切変更しない。
    EditorContext previewCtx = ctx;
    previewCtx.activeScene = ctx.vfxPreviewScene;
    previewCtx.selectedEntities.clear();
    if (previewCtx.activeScene->IsValid(m_session.preview.selectedEntity))
        previewCtx.selectedEntities.push_back(m_session.preview.selectedEntity);
    previewCtx.markSceneDirty = []() {};

    const std::vector<scene::EntityID> group = m_session.preview.BuildEffectGroup(previewCtx);
    scene::ParticleEmitter* root = nullptr;
    if (!group.empty())
        root = previewCtx.activeScene->GetComponent<scene::ParticleEmitter>(group.front());

    // プレビュー速度の注入。毎フレーム書き込み、書き込みが止まると Engine 側が自動で 1.0 へ戻す。
    // Play Mode 中はゲーム本来の再生を邪魔しないよう注入しない。
    const bool inPlayMode = previewCtx.playMode && !previewCtx.playMode->IsInEditor();
    if (!inPlayMode) {
        for (const scene::EntityID id : group) {
            if (auto* emitter = previewCtx.activeScene->GetComponent<scene::ParticleEmitter>(id)) {
                emitter->editorTimeScale      = m_session.preview.paused ? 0.0f : (std::max)(m_session.preview.speed, 0.0f);
                emitter->editorTimeScaleFrame = Time::frameCount;
            }
        }
    }

    // Space で再生 / 一時停止 (パネルフォーカス中のみ。テキスト入力中は無視)
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space, false) && root)
        m_session.preview.paused = !m_session.preview.paused;

    m_timeline.DrawTransport(previewCtx, group, root);
    ImGui::Separator();
    // Graph モードの StatusBar と同じ位置・同じボタンで Asset Browser を開閉できるようにする。
    // WHY: モードによって開閉口が変わると「さっきまであったボタンが無い」状態になる。
    DrawDrawerToggle("Asset Browser", m_session.assetBrowserVisible, "Asset Browser");
    ImGui::NewLine();
    DrawDockWorkspace();

    if (ImGui::Begin(kWorkspaceWindow))
        DrawHierarchy(previewCtx);
    ImGui::End();

    if (ImGui::Begin(kPreviewWindow, nullptr,
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        m_previewView.DrawViewport(previewCtx, true);
    ImGui::End();

    if (ImGui::Begin(kTimelineWindow)) {
        if (root) {
            m_timeline.DrawEmitterTimeline(previewCtx, group, root);
            m_timeline.DrawStats(previewCtx, group, root);
        } else {
            ImGui::TextDisabled("Select an emitter to edit its timeline.");
        }
    }
    ImGui::End();

    if (ImGui::Begin(kInspectorWindow)) {
        if (root) {
            if (DrawParticleEmitterModules(*root, previewCtx)) {
                // Preview Worldは保存対象外。変更結果は即時プレビューだけへ反映する。
            }
        } else {
            ImGui::Spacing();
            ImGui::TextDisabled("No emitter selected in VFX Preview World");
            ImGui::TextWrapped("Create one in the VFX Hierarchy, or drop a texture, material or mesh asset into this window.");
        }
    }
    ImGui::End();
    if (!previewCtx.selectedEntities.empty())
        m_session.preview.selectedEntity = previewCtx.PrimarySelected();
    DrawShaderCompileDiagnosticsWindow();
}


void VFXEditorPanel::DrawDockWorkspace()
{
    const ImGuiID dockId = ImGui::GetID("##VFXEditorDockSpace");
    if (m_resetDockLayoutRequested || ImGui::DockBuilderGetNode(dockId) == nullptr) {
        BuildDefaultDockLayout(dockId);
        m_resetDockLayoutRequested = false;
    }
    // タブバーを残し、ユーザーが各領域を掴んで分離・再配置できることを明示する。
    ImGui::DockSpace(dockId, { 0.0f, 0.0f }, ImGuiDockNodeFlags_None);
}


void VFXEditorPanel::BuildDefaultDockLayout(ImGuiID dockId)
{
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    const ImVec2 available = ImGui::GetContentRegionAvail();
    ImGui::DockBuilderSetNodeSize(
        dockId, { (std::max)(available.x, 1.0f), (std::max)(available.y, 1.0f) });

    ImGuiID upper = dockId;
    ImGuiID bottom = 0;
    ImGui::DockBuilderSplitNode(upper, ImGuiDir_Down, 0.27f, &bottom, &upper);

    ImGuiID inspector = 0;
    ImGui::DockBuilderSplitNode(upper, ImGuiDir_Right, 0.24f, &inspector, &upper);
    ImGuiID preview = 0;
    ImGuiID workspace = 0;
    ImGui::DockBuilderSplitNode(upper, ImGuiDir_Right, 0.38f, &preview, &workspace);
    // Hierarchy は Canvas の左へ細く置く。Canvas で図を追いながら木で構造を確認する並びにする。
    ImGuiID hierarchy = 0;
    ImGui::DockBuilderSplitNode(workspace, ImGuiDir_Left, 0.26f, &hierarchy, &workspace);

    ImGuiID timeline = bottom;
    ImGuiID assets = 0;
    if (m_standaloneApplicationMode && m_session.assetBrowserVisible != nullptr)
        ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.36f, &assets, &timeline);

    ImGui::DockBuilderDockWindow(kWorkspaceWindow, workspace);
    ImGui::DockBuilderDockWindow(kHierarchyWindow, hierarchy);
    ImGui::DockBuilderDockWindow(kPreviewWindow, preview);
    ImGui::DockBuilderDockWindow(kInspectorWindow, inspector);
    ImGui::DockBuilderDockWindow(kTimelineWindow, timeline);
    if (assets != 0)
        ImGui::DockBuilderDockWindow("Asset Browser", assets);
    ImGui::DockBuilderFinish(dockId);
}


void VFXEditorPanel::DrawHierarchy(EditorContext& ctx)
{
    ImGui::TextUnformatted("Effects");
    ImGui::Separator();
    ImGui::SameLine();
    ImGui::TextDisabled("VFX World");
    if (!ctx.activeScene) {
        ImGui::TextDisabled("No preview world");
        return;
    }

    // 新規エミッター作成。エミッター選択中ならその子として作り、エフェクトの部品追加を1クリックにする。
    if (ImGui::Button("+ New Emitter", { -1.0f, 0.0f })) {
        scene::EntityID parentId = scene::EntityID::INVALID;
        if (auto* selGo = ctx.GetSelectedGO(); selGo && selGo->GetComponent<scene::ParticleEmitter>())
            parentId = selGo->GetID();
        auto& go = ctx.activeScene->CreateGameObject("VFX Emitter");
        // CreateGameObject で内部ストレージが再配置され得るため、親は EntityID から引き直す
        if (ctx.activeScene->IsValid(parentId))
            if (auto* parentGo = ctx.activeScene->GetGameObject(parentId))
                go.SetParent(*parentGo);
        go.AddComponent<scene::ParticleEmitter>();
        ctx.selectedEntities = { go.GetID() };
    }
    if (ImGui::BeginDragDropTarget()) {
        std::string path;
        if (ReadAssetPayload(ImGui::AcceptDragDropPayload("ASSET_PATH"), path))
            m_session.CreatePreviewEmitterFromAsset(ctx, path);
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Create an empty emitter, or drop an asset onto this button");
    ImGui::Spacing();

    // 他エミッターから SubEmitter として名前参照されているものはルートに並べない
    std::vector<std::string> referencedNames;
    for (const scene::EntityID id : ctx.activeScene->GetEntities<scene::ParticleEmitter>()) {
        const auto* emitter = ctx.activeScene->GetComponent<scene::ParticleEmitter>(id);
        if (!emitter) continue;
        if (!emitter->birthSubEmitter.empty())     referencedNames.push_back(emitter->birthSubEmitter);
        if (!emitter->deathSubEmitter.empty())     referencedNames.push_back(emitter->deathSubEmitter);
        if (!emitter->collisionSubEmitter.empty()) referencedNames.push_back(emitter->collisionSubEmitter);
    }

    bool anyRoot = false;
    std::vector<scene::EntityID> visited;
    for (const scene::EntityID id : ctx.activeScene->GetEntities<scene::ParticleEmitter>()) {
        auto* go = ctx.activeScene->GetGameObject(id);
        if (!go) continue;
        // Graph 再生中に VFXGraphSystem が作ったノード実体は、この一覧の編集対象ではない。
        // WHY: ここは「手で組む単体 Emitter」の階層で、行を選べば Inspector が編集させる。
        //      生成物は .vfx が唯一の編集元なので、触れる形で並べると
        //      「直したのに保存されない」編集を誘発する。Graph 側の Canvas で扱う。
        if (go->runtimeGenerated) continue;
        const bool isReferenced =
            std::find(referencedNames.begin(), referencedNames.end(), go->name) != referencedNames.end();
        if (isReferenced || AncestorHasEmitter(go))
            continue; // ルートではない → 親ノードの下に表示される
        anyRoot = true;
        visited.clear();
        DrawEmitterNode(ctx, id, 0, visited);
    }
    if (!anyRoot) {
        ImGui::Spacing();
        ImGui::TextDisabled("No emitters in VFX Preview World");
        ImGui::TextWrapped("Drop a texture, material or mesh here to create one.");
    }
    if (!ImGui::GetIO().WantTextInput
        && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && ImGui::IsKeyPressed(ImGuiKey_Delete, false)
        && ctx.activeScene->IsValid(ctx.PrimarySelected())) {
        ctx.activeScene->DestroyGameObject(ctx.PrimarySelected());
        ctx.selectedEntities.clear();
        m_session.preview.selectedEntity = scene::EntityID::INVALID;
    }

    // Hierarchyの空白へ落とした場合はルートへ移動、AssetならルートEmitterとして生成する。
    ImGui::InvisibleButton("##VFXHierarchyDropZone",
                           { -1.0f, (std::max)(ImGui::GetContentRegionAvail().y, 36.0f) });
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* entityPayload =
            ImGui::AcceptDragDropPayload("FBZZ_VFX_PREVIEW_ENTITY");
        if (entityPayload != nullptr && entityPayload->DataSize == sizeof(scene::EntityID)) {
            const scene::EntityID draggedId =
                *static_cast<const scene::EntityID*>(entityPayload->Data);
            if (auto* dragged = ctx.activeScene->GetGameObject(draggedId)) dragged->ClearParent();
        }
        std::string path;
        if (ReadAssetPayload(ImGui::AcceptDragDropPayload("ASSET_PATH"), path))
            m_session.CreatePreviewEmitterFromAsset(ctx, path);
        ImGui::EndDragDropTarget();
    }
}


void VFXEditorPanel::DrawEmitterNode(EditorContext& ctx, scene::EntityID id, int depth,
                                     std::vector<scene::EntityID>& visited)
{
    // SubEmitter の名前参照は循環し得るため、visited と深さ上限の両方で打ち切る
    if (depth > 8 || Contains(visited, id) || !ctx.activeScene)
        return;
    visited.push_back(id);

    auto* go = ctx.activeScene->GetGameObject(id);
    auto* emitter = go ? go->GetComponent<scene::ParticleEmitter>() : nullptr;
    if (!go || !emitter)
        return;

    // 子ノード = 子 GameObject のエミッター + SubEmitter 名前参照
    std::vector<scene::EntityID> childEmitters;
    for (int i = 0; i < go->GetChildCount(); ++i)
        if (auto* child = go->GetChild(i))
            if (child->GetComponent<scene::ParticleEmitter>())
                childEmitters.push_back(child->GetID());
    struct SubRef { const char* eventName; scene::EntityID id; };
    std::vector<SubRef> subRefs;
    const std::pair<const char*, const std::string*> refs[] = {
        { "Birth", &emitter->birthSubEmitter },
        { "Death", &emitter->deathSubEmitter },
        { "Collision", &emitter->collisionSubEmitter },
    };
    for (const auto& [eventName, name] : refs) {
        if (name->empty()) continue;
        if (auto* target = ctx.activeScene->Find(*name))
            if (target->GetComponent<scene::ParticleEmitter>())
                subRefs.push_back({ eventName, target->GetID() });
    }

    const bool selected = ctx.PrimarySelected() == id;
    const bool leaf = childEmitters.empty() && subRefs.empty();
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_SpanAvailWidth
                             | ImGuiTreeNodeFlags_DefaultOpen;
    if (leaf)     flags |= ImGuiTreeNodeFlags_Leaf;
    if (selected) flags |= ImGuiTreeNodeFlags_Selected;

    char label[160];
    std::snprintf(label, sizeof(label), "%s  (%d)%s",
                  go->name.c_str(),
                  static_cast<int>(emitter->particles.size()),
                  emitter->playing ? "" : "  [stopped]");
    ImGui::PushID(static_cast<int>(id.index));
    const bool open = ImGui::TreeNodeEx(label, flags);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
        ctx.selectedEntities = { id };
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("FBZZ_VFX_PREVIEW_ENTITY", &id, sizeof(id));
        ImGui::TextUnformatted(go->name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FBZZ_VFX_PREVIEW_ENTITY");
        if (payload != nullptr && payload->DataSize == sizeof(scene::EntityID)) {
            const scene::EntityID draggedId = *static_cast<const scene::EntityID*>(payload->Data);
            if (draggedId != id) {
                if (auto* dragged = ctx.activeScene->GetGameObject(draggedId))
                    dragged->SetParent(go);
            }
        }
        std::string path;
        if (ReadAssetPayload(ImGui::AcceptDragDropPayload("ASSET_PATH"), path)) {
            ctx.selectedEntities = { id };
            m_session.CreatePreviewEmitterFromAsset(ctx, path);
        }
        ImGui::EndDragDropTarget();
    }
    bool deleteRequested = false;
    if (ImGui::BeginPopupContextItem("##VFXEmitterContext")) {
        ImGui::TextDisabled("VFX Preview World");
        if (ImGui::MenuItem("Add Child Emitter")) {
            ctx.selectedEntities = { id };
            m_session.CreatePreviewEmitterFromAsset(ctx, "");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete", "Del")) deleteRequested = true;
        ImGui::EndPopup();
    }
    if (open) {
        for (const scene::EntityID child : childEmitters)
            DrawEmitterNode(ctx, child, depth + 1, visited);
        for (const SubRef& ref : subRefs) {
            // イベント名バッジ + 参照先ノード。参照先は独立した GO なので同じ描画を使う
            ImGui::TextDisabled("  [%s]", ref.eventName);
            ImGui::SameLine();
            DrawEmitterNode(ctx, ref.id, depth + 1, visited);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
    if (deleteRequested) {
        ctx.activeScene->DestroyGameObject(id);
        if (ctx.PrimarySelected() == id) ctx.selectedEntities.clear();
        if (m_session.preview.selectedEntity == id) m_session.preview.selectedEntity = scene::EntityID::INVALID;
    }
}



void VFXEditorPanel::DrawGraphEditor(EditorContext& ctx)
{
    // アセット欄はこの値を使ってプロジェクト相対パスへ正規化する。
    g_assetFieldProjectRoot = ctx.projectRoot;
    // ノードサムネイルの解決先。埋め込み版・独立版のどちらでもここを通る。
    g_thumbnailResources = ctx.resources;
    g_thumbnailImGuiRenderer = ctx.imguiRenderer;
    DrawGraphLayout(ctx);
    // "..." ピッカーの実体。呼ばないとボタンを押しても何も出ない。
    widgets::DrawAssetPickerModal(ctx.resources, ctx.imguiRenderer);
    // 保存待ちレジストリへの登録はグラフ全体の複製を伴う。dirty の間ずっと毎フレーム
    // 作り直していたので、内容が変わった (版数が進んだ) ときだけに絞る。
    if (m_session.document.dirty && m_session.lastRegisteredDirtyRevision != m_session.document.dirty.ContentRevision()) {
        m_session.lastRegisteredDirtyRevision = m_session.document.dirty.ContentRevision();
        const std::string capturedPath = m_session.document.path;
        const asset::VFXGraphAsset capturedGraph = m_session.document.graph;
        AssetDirtyRegistry::Register(
            capturedPath, NormalizeAssetPath(capturedPath), "VFX",
            [capturedPath, capturedGraph]() {
                return asset::SaveVFXGraphAsset(capturedPath, capturedGraph);
            });
    }
}


// 親子の付け替えが循環を作らないか調べる。target が child の子孫なら不可。
// WHY: 循環は ValidateVFXGraphAsset が保存時に弾くが、そこまで放置すると
//      「掴んで落とせたのに保存できない」状態が生まれる。落とせない形で先に伝える。
static bool WouldCreateParentCycle(const asset::VFXGraphAsset& graph, int child, int target)
{
    if (child == target) return true;
    for (int cursor = target, guard = 0;
         cursor != -1 && guard <= static_cast<int>(graph.nodes.size()); ++guard) {
        if (cursor == child) return true;
        const auto node = std::find_if(graph.nodes.begin(), graph.nodes.end(),
            [&](const asset::VFXGraphNode& candidate) { return candidate.id == cursor; });
        cursor = (node != graph.nodes.end()) ? node->parentNodeId : -1;
    }
    return false;
}

void VFXEditorPanel::DrawGraphHierarchyNode(EditorContext& ctx, int nodeId,
                                            const std::unordered_set<int>& activeNodeIds,
                                            std::vector<int>& visited,
                                            int& pendingParentChild, int& pendingParentTarget)
{
    auto& graph = m_session.document.graph;
    // 破損データ (検証前の一時状態) でも無限再帰しないよう、訪問済みは必ず打ち切る。
    if (std::find(visited.begin(), visited.end(), nodeId) != visited.end()) return;
    visited.push_back(nodeId);

    auto* node = FindGraphNode(graph, nodeId);
    if (node == nullptr) return;

    std::vector<int> children;
    for (const auto& candidate : graph.nodes)
        if (candidate.parentNodeId == nodeId) children.push_back(candidate.id);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth
                             | ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_DefaultOpen
                             | ImGuiTreeNodeFlags_FramePadding;
    if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (m_session.selectedNodeId == nodeId) flags |= ImGuiTreeNodeFlags_Selected;

    ImGui::PushID(nodeId);
    // 無効ノードは淡く。Canvas 側の "DISABLED" 表示と読みを揃える。
    if (!node->enabled) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(130, 132, 138, 255));
    char label[192];
    std::snprintf(label, sizeof(label), "[%s] %s", VFXNodeIcon(node->type), node->name.c_str());
    const bool opened = ImGui::TreeNodeEx(label, flags);
    if (!node->enabled) ImGui::PopStyleColor();

    // クリックで選択。Canvas / Inspector / Preview が同じノードを見るよう Session 経由で伝える。
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        m_session.selectedNodeId = nodeId;
        m_session.selectedLinkIndex = -1;
        m_session.selectedGroupId = -1;
        if (m_session.onSelectNodes) m_session.onSelectNodes({ nodeId });
        // ダブルクリックは Canvas 上の該当ノードへ視点を送る (木で見つけて図で確認する導線)。
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            m_session.focusSelectionRequested = true;
    }

    // D&D: 掴んだノードを別ノードへ落として親子付けする。
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
        ImGui::SetDragDropPayload("FBZZ_VFX_GRAPH_NODE", &nodeId, sizeof(int));
        ImGui::Text("Reparent %s", node->name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FBZZ_VFX_GRAPH_NODE");
            payload != nullptr && payload->DataSize == sizeof(int)) {
            const int dragged = *static_cast<const int*>(payload->Data);
            // 実体を持たないノードは親になれない (ランタイムで owner 直下へ落ちるだけ)。
            const bool parentable = node->type != asset::VFXNodeType::Entry
                                 && node->type != asset::VFXNodeType::Delay;
            if (parentable && !WouldCreateParentCycle(graph, dragged, nodeId)) {
                // 反映はツリー走査が終わってから。走査中に parentNodeId を書き換えると、
                // 同じフレームの残りの再帰が組み替え後の木を歩いて表示が乱れる。
                pendingParentChild = dragged;
                pendingParentTarget = nodeId;
            }
        }
        ImGui::EndDragDropTarget();
    }

    // 行末の状態表示。実行中は Canvas と同じ緑、無効は文字で。
    if (activeNodeIds.contains(nodeId)) {
        ImGui::SameLine();
        ImGui::TextColored({ 0.42f, 0.94f, 0.62f, 1.0f }, "*");
    }
    if (!node->enabled) {
        ImGui::SameLine();
        ImGui::TextDisabled("off");
    }

    if (!children.empty()) {
        if (opened) {
            for (const int child : children)
                DrawGraphHierarchyNode(ctx, child, activeNodeIds, visited,
                                       pendingParentChild, pendingParentTarget);
            ImGui::TreePop();
        }
    }
    ImGui::PopID();
}

void VFXEditorPanel::DrawGraphHierarchy(EditorContext& ctx)
{
    auto& graph = m_session.document.graph;
    if (graph.nodes.empty()) {
        ImGui::TextDisabled("No nodes");
        return;
    }

    ImGui::TextDisabled("Transform parenting");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Drag a node onto another to parent it (position only).\n"
                          "Graph links control timing and are edited in the Graph canvas.");
    ImGui::Separator();

    // Canvas と同じ実行中ノード集合。木の側でも「今動いているのはどれか」を出す。
    std::unordered_set<int> activeNodeIds;
    if (ctx.vfxPreviewScene != nullptr) {
        if (auto* owner = ctx.vfxPreviewScene->GetGameObject(m_session.preview.graphEntity)) {
            if (const auto* component = owner->GetComponent<scene::VFXGraphComponent>()) {
                for (const auto& state : component->runtimeNodes)
                    if (state.active) activeNodeIds.insert(state.nodeId);
            }
        }
    }

    // 付け替え要求は走査後にまとめて適用する (理由は DrawGraphHierarchyNode 内のコメント)。
    int pendingParentChild = -1;
    int pendingParentTarget = -1;
    std::vector<int> visited;
    visited.reserve(graph.nodes.size());
    for (const auto& node : graph.nodes) {
        // 親が無い / 親が消えたノードを根として並べる。後者を拾わないと木から消えてしまう。
        const bool parentExists = node.parentNodeId != -1
            && std::any_of(graph.nodes.begin(), graph.nodes.end(),
                [&](const asset::VFXGraphNode& candidate) { return candidate.id == node.parentNodeId; });
        if (parentExists) continue;
        DrawGraphHierarchyNode(ctx, node.id, activeNodeIds, visited,
                               pendingParentChild, pendingParentTarget);
    }

    // 余白へ落としたら親を外してルートへ戻す (Unity の Hierarchy と同じ操作)。
    ImGui::InvisibleButton("##VFXHierarchyRootDrop",
                           { -1.0f, (std::max)(ImGui::GetContentRegionAvail().y, 24.0f) });
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FBZZ_VFX_GRAPH_NODE");
            payload != nullptr && payload->DataSize == sizeof(int)) {
            pendingParentChild = *static_cast<const int*>(payload->Data);
            pendingParentTarget = -1;
        }
        ImGui::EndDragDropTarget();
    }

    if (pendingParentChild != -1) {
        if (auto* child = FindGraphNode(graph, pendingParentChild);
            child != nullptr && child->parentNodeId != pendingParentTarget) {
            m_session.PushUndo();
            child->parentNodeId = pendingParentTarget;
            m_session.document.dirty = true;
            // 親子はグループ GameObject の有無を変えるため、値反映では追従できない。
            // 構造から作り直させる。
            m_session.document.liveDirtyNodeId = -1;
            m_session.preview.restartRequested = true;
        }
    }
}

void VFXEditorPanel::DrawGraphLayout(EditorContext& context)
{
    if (m_session.document.path != context.selectedAssetPath) {
        // Graph切替時に破棄するのは専用Preview Worldだけで、ゲームSceneへは触れない。
        if (context.vfxPreviewScene != nullptr) context.vfxPreviewScene->Clear();
        m_session.preview.graphEntity = scene::EntityID::INVALID;
        m_session.LoadGraph(context.selectedAssetPath);
    }

    // Inspector編集直後にも検証し、保存時まで破損理由が見えない状態を作らない。
    if (!m_session.document.graph.nodes.empty()) {
        std::string validationError;
        if (asset::ValidateVFXGraphAsset(m_session.document.graph, &validationError)) m_session.document.error.clear();
        else m_session.document.error = std::move(validationError);
    }

    DrawGraphMenuBar(context);
    DrawGraphToolbar(context);
    ImGui::Separator();
    DrawShaderCompileErrorBanner();
    // Graph自体の検証エラーより、直前の操作が拒否された理由を優先して見せる。
    // WHY: 後者は上の検証で毎フレーム消されるため、専用の一時メッセージとして保持している。
    const bool hasTransient = !m_canvas.TransientError().empty();
    if (hasTransient || !m_session.document.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Color(ThemeColor::SurfaceRaised));
        ImGui::BeginChild("##VFXErrorBanner", { 0.0f, 42.0f }, true);
        ImGui::TextColored({ 1.0f, 0.55f, 0.48f, 1.0f },
                           hasTransient ? "Rejected" : "Validation failed");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", hasTransient ? m_canvas.TransientError().c_str()
                                              : m_session.document.error.c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    // 保存は通るが見た目が壊れる設定。エラーと同じ赤で出すと本物のエラーが埋もれるため、
    // 折りたたみ可能な琥珀色の帯として別枠にしている。
    // 収集はグラフ内容が変わったときだけ (全ノード + 全 binding の走査になるため)。
    if (m_session.document.warningRevision != m_session.document.dirty.ContentRevision()) {
        m_session.document.warningRevision = m_session.document.dirty.ContentRevision();
        m_session.document.warnings = asset::CollectVFXGraphWarnings(m_session.document.graph);
    }
    if (!m_session.document.warnings.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Color(ThemeColor::SurfaceRaised));
        const float height = m_graphWarningsExpanded
            ? (std::min)(28.0f + 20.0f * static_cast<float>(m_session.document.warnings.size()), 160.0f)
            : 28.0f;
        ImGui::BeginChild("##VFXWarningBanner", { 0.0f, height }, true);
        const std::string label = (m_graphWarningsExpanded ? "v " : "> ")
            + std::to_string(m_session.document.warnings.size()) + " warning(s)";
        if (ImGui::SmallButton(label.c_str()))
            m_graphWarningsExpanded = !m_graphWarningsExpanded;
        if (!m_graphWarningsExpanded) {
            ImGui::SameLine();
            ImGui::TextColored({ 1.0f, 0.84f, 0.45f, 1.0f }, "%s",
                               m_session.document.warnings.front().message.c_str());
        } else {
            for (const auto& warning : m_session.document.warnings) {
                ImGui::PushID(&warning);
                // クリックで原因ノードへ飛ぶ。文言だけ出しても、どのノードか探す手間が残る。
                if (warning.nodeId != 0 && ImGui::SmallButton("Go")) {
                    m_session.selectedNodeId = warning.nodeId;
                    m_session.focusSelectionRequested = true;
                }
                if (warning.nodeId != 0) ImGui::SameLine();
                ImGui::TextColored({ 1.0f, 0.84f, 0.45f, 1.0f }, "%s", warning.message.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    DrawGraphStatusBar();
    DrawDockWorkspace();

    // WHAT: 制作領域を独立DockingWindowとして送出し、リサイズだけでなくタブ化・分離も許可する。
    // WHY: 固定Tableでは作業内容に応じてGraphとPreviewの優先度を切り替えられないため。
    if (ImGui::Begin(kWorkspaceWindow, nullptr,
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        m_canvas.Draw(context);
    ImGui::End();

    // 同じグラフの 2 つ目のビュー。Canvas が因果 (link)、こちらが空間 (parentNodeId)。
    if (ImGui::Begin(kHierarchyWindow))
        DrawGraphHierarchy(context);
    // 既存ユーザーの vfx_editor.ini には Hierarchy の配置が無いため、既定レイアウトの構築が
    // 走らず単独ウィンドウとして宙に浮く。ドックされていなければ 1 度だけ組み直す。
    // WHY: 「新機能を足したらウィンドウが画面外に出た」は、ユーザー側からは
    //      不具合と見分けが付かない。初回だけ自動で正しい位置へ収める。
    if (!m_hierarchyDockChecked) {
        m_hierarchyDockChecked = true;
        if (!ImGui::IsWindowDocked()) m_resetDockLayoutRequested = true;
    }
    ImGui::End();

    if (ImGui::Begin("Preview###VFXPreview", nullptr,
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        m_previewView.DrawViewport(context, true);
    ImGui::End();

    if (ImGui::Begin("Inspector###VFXInspector"))
        m_inspector.Draw(context);
    ImGui::End();

    if (m_session.showTimeline) {
        if (ImGui::Begin("Timeline###VFXTimeline"))
            m_timeline.DrawGraphTimeline(context);
        ImGui::End();
    }
    // Template の確認・保存ダイアログはメニュー階層の外でしか開けないため、ここで描画する。
    DrawGraphTemplateDialogs(context);
}

void VFXEditorPanel::DrawGraphMenuBar(EditorContext& ctx)
{
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open Graph...", "Ctrl+O", false,
                            static_cast<bool>(ctx.requestOpenVFXAssetDialog))) {
            ctx.requestOpenVFXAssetDialog();
        }
        if (ImGui::MenuItem("Save", "Ctrl+S", false, m_session.document.dirty.IsDirty())) {
            if (m_session.SaveGraph()) m_session.preview.restartRequested = true;
        }
        if (ImGui::MenuItem("Reload from Disk", "Ctrl+R")) {
            m_session.LoadGraph(m_session.document.path);
            m_session.preview.restartRequested = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo VFX Edit", "Ctrl+Z", false, m_session.document.history.CanUndo())) m_session.Undo();
        if (ImGui::MenuItem("Redo VFX Edit", "Ctrl+Y", false, m_session.document.history.CanRedo())) m_session.Redo();
        if (ImGui::BeginMenu("History")) {
            ImGui::TextDisabled("Undo %d  |  Redo %d",
                static_cast<int>(m_session.document.history.undoStack.size()),
                static_cast<int>(m_session.document.history.redoStack.size()));
            if (m_session.document.history.savedStateId == ~std::uint64_t{ 0 }) {
                ImGui::TextDisabled("State %llu  |  Saved state is outside history",
                    static_cast<unsigned long long>(m_session.document.history.currentStateId));
            } else {
                ImGui::TextDisabled("State %llu  |  Saved %llu",
                    static_cast<unsigned long long>(m_session.document.history.currentStateId),
                    static_cast<unsigned long long>(m_session.document.history.savedStateId));
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear History", nullptr, false,
                                m_session.document.history.CanUndo() || m_session.document.history.CanRedo())) {
                const bool isSaved = !m_session.document.dirty.IsDirty();
                m_session.document.history.Clear();
                if (!isSaved) {
                    m_session.document.history.currentStateId = 1;
                    m_session.document.history.nextStateId = 2;
                }
                m_session.document.history.savedStateId = isSaved ? 0 : ~std::uint64_t{ 0 };
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        const bool hasNodeSelection = !m_canvas.SelectedNodeIds().empty();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasNodeSelection)) m_canvas.DuplicateSelectedNodes();
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, hasNodeSelection)) m_canvas.CopySelectedNodes(false);
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, hasNodeSelection)) m_canvas.CopySelectedNodes(true);
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, !m_session.document.clipboard.nodes.empty())) m_canvas.PasteNodes(false);
        if (ImGui::MenuItem("Delete", "Del", false, hasNodeSelection)) m_canvas.DeleteSelectedNodes();
        ImGui::Separator();
        // 整列は座標だけを動かすので実行版数を進めない (プレビューは作り直さない)。
        if (ImGui::BeginMenu("Layout")) {
            const int selectionCount = static_cast<int>(m_canvas.SelectedNodeIds().size());
            const bool canAlign = selectionCount >= 2;
            const bool canDistribute = selectionCount >= 3;
            if (ImGui::MenuItem("Align Left", nullptr, false, canAlign))
                m_canvas.AlignSelectedNodes(GraphAlign::Left);
            if (ImGui::MenuItem("Align Center (X)", nullptr, false, canAlign))
                m_canvas.AlignSelectedNodes(GraphAlign::CenterX);
            if (ImGui::MenuItem("Align Right", nullptr, false, canAlign))
                m_canvas.AlignSelectedNodes(GraphAlign::Right);
            ImGui::Separator();
            if (ImGui::MenuItem("Align Top", nullptr, false, canAlign))
                m_canvas.AlignSelectedNodes(GraphAlign::Top);
            if (ImGui::MenuItem("Align Middle (Y)", nullptr, false, canAlign))
                m_canvas.AlignSelectedNodes(GraphAlign::CenterY);
            if (ImGui::MenuItem("Align Bottom", nullptr, false, canAlign))
                m_canvas.AlignSelectedNodes(GraphAlign::Bottom);
            ImGui::Separator();
            if (ImGui::MenuItem("Distribute Horizontally", nullptr, false, canDistribute))
                m_canvas.DistributeSelectedNodes(true);
            if (ImGui::MenuItem("Distribute Vertically", nullptr, false, canDistribute))
                m_canvas.DistributeSelectedNodes(false);
            ImGui::Separator();
            if (ImGui::MenuItem("Auto Layout (whole graph)", nullptr, false, !m_session.document.graph.nodes.empty()))
                m_canvas.AutoLayoutGraph();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Entry からのリンク深さを列にして並べ直します。\n"
                                  "列内の上下は今の並び順を保つため、意図した層構成は崩れません。");
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Add")) {
        m_canvas.DrawAddNodeMenu();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Templates")) {
        DrawGraphTemplateMenu(ctx);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Viewport (Always Visible)", nullptr, true, false);
        bool preferencesChanged = false;
        preferencesChanged |= ImGui::MenuItem("Graph Grid", nullptr, &m_session.showGraphGrid);
        // 常設ボタンは StatusBar 側にある。ここはメニューから探す人向けの二重の導線。
        if (m_session.assetBrowserVisible != nullptr) {
            ImGui::MenuItem("Asset Browser", nullptr, m_session.assetBrowserVisible);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("テクスチャ/マテリアルを一覧し、Inspector のアセット欄へ\n"
                                  "ドラッグ&ドロップで割り当てられます。\n"
                                  "下端の StatusBar からも開閉できます。");
        }
        preferencesChanged |= ImGui::MenuItem("Mini Map", nullptr, &m_session.showMiniMap);
        const float previousZoom = m_session.graphZoom;
        preferencesChanged |= ImGui::SliderFloat(
            "Graph Zoom", &m_session.graphZoom, 0.45f, 1.80f, "%.2fx",
            ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Canvas上のマウスホイールでも拡大・縮小できます");
        if (std::fabs(previousZoom - m_session.graphZoom) > 0.0001f)
            m_session.positionsPending = true;
        preferencesChanged |= ImGui::MenuItem("Timeline", nullptr, &m_session.showTimeline);
        if (m_session.showTimeline) {
            preferencesChanged |= ImGui::MenuItem("Timeline Snap", nullptr, &m_session.timelineSnap);
            preferencesChanged |= ImGui::SliderFloat("Snap Step", &m_session.timelineSnapStep,
                                                     0.01f, 0.5f, "%.2fs");
        }
        preferencesChanged |= ImGui::MenuItem("Preview Floor Grid", nullptr, &m_session.preview.showFloorGrid);
        preferencesChanged |= ImGui::MenuItem("Preview VFX Gizmos", nullptr, &m_session.preview.showGizmos);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("力場の影響半径・向きと、エミッターの発生形状/初速をワイヤー表示します。\n"
                              "見えない体積を当て推量で調整しなくて済みます。\n"
                              "AI capture には写り込みません。");
        if (ImGui::MenuItem("Frame Selection", "F", false, m_session.selectedNodeId > 0))
            m_session.focusSelectionRequested = true;
        if (ImGui::MenuItem("Frame All", "A", false, !m_session.document.graph.nodes.empty()))
            m_session.frameAllRequested = true;
        if (ImGui::MenuItem("Find Node...", "Ctrl+F", &m_canvas.nodeSearchOpen))
            m_canvas.nodeSearchFocusRequested = m_canvas.nodeSearchOpen;
        // Solo は「この層だけ出す」ための一時状態。アセットへは保存しない。
        if (ImGui::MenuItem("Solo Selected Node", "S", m_session.document.soloNodeId > 0,
                            m_session.selectedNodeId > 0 || m_session.document.soloNodeId > 0))
            m_session.ToggleSolo(m_session.document.soloNodeId > 0 ? m_session.document.soloNodeId : m_session.selectedNodeId);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("選択ノードだけを生成したプレビューにします。\n"
                              "アセットの enabled は変えないため保存内容に残りません。");
        ImGui::Separator();
        preferencesChanged |= ImGui::MenuItem("Live Edit (apply without saving)", nullptr, &m_session.liveEditEnabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("編集を保存せずプレビューへ即反映します。\n"
                              "値だけの変更なら走っている粒子を止めずに差し替わり、\n"
                              "ノードや配線を変えたときだけ作り直します (再生位置は維持)。");
        preferencesChanged |= ImGui::MenuItem("Auto-Connect New Nodes", nullptr, &m_session.autoConnectNewNodes);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Link newly added nodes to the selected node (or Entry)");
        if (ImGui::MenuItem("Add Group / Note")) m_canvas.AddGroup();
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Dock Layout"))
            m_resetDockLayoutRequested = true;
        if (preferencesChanged) m_session.SaveEditorPreferences();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Debug")) {
        bool preferencesChanged = false;
        ImGui::TextDisabled("Visualization");
        if (ImGui::MenuItem("Particle Overdraw", nullptr, &m_session.preview.overdrawView)) {
            m_session.preview.restartRequested = true;
            m_session.preview.overdrawReadbackRequested = true;
        }
        preferencesChanged |= ImGui::MenuItem("VFX Gizmos", nullptr, &m_session.preview.showGizmos);
        preferencesChanged |= ImGui::MenuItem("Floor Grid", nullptr, &m_session.preview.showFloorGrid);
        ImGui::MenuItem("Loop Seam Preview", nullptr, &m_session.preview.showLoopSeam);
        ImGui::Separator();
        ImGui::TextDisabled("Simulation");
        ImGui::MenuItem("Pause", "Space", &m_session.preview.paused);
        if (ImGui::MenuItem("Restart")) m_session.preview.restartRequested = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Validate Graph Now")) {
            m_session.document.error.clear();
            (void)asset::ValidateVFXGraphAsset(m_session.document.graph, &m_session.document.error);
            m_session.document.warnings = asset::CollectVFXGraphWarnings(m_session.document.graph);
            m_session.document.warningRevision = m_session.document.dirty.ContentRevision();
            m_graphWarningsExpanded = !m_session.document.warnings.empty();
        }
        if (ImGui::MenuItem("Measure Overdraw Now"))
            m_session.preview.overdrawReadbackRequested = true;

        const asset::VFXGraphBudgetStats budget = asset::CalculateVFXGraphBudget(m_session.document.graph);
        ImGui::Separator();
        ImGui::TextDisabled("Graph Diagnostics");
        ImGui::Text("Nodes %d  Links %d", static_cast<int>(m_session.document.graph.nodes.size()),
                    static_cast<int>(m_session.document.graph.links.size()));
        ImGui::Text("Particles %d / %d", budget.particles, m_session.document.graph.maxParticles);
        ImGui::Text("Lights %d / %d", budget.lights, m_session.document.graph.maxLights);
        ImGui::Text("Audio %d / %d", budget.audioVoices, m_session.document.graph.maxAudioVoices);
        const scene::ParticleOverdrawStats& overdraw = scene::GetLastParticleOverdrawStats();
        if (overdraw.valid) {
            ImGui::Text("Overdraw %.2fx  Mean %.2f  Max %.1f",
                        overdraw.overdrawFactor, overdraw.meanLayers, overdraw.maxLayers);
            ImGui::Text("Coverage %.1f%%  Heavy %.1f%%",
                        overdraw.coveredRatio * 100.0f, overdraw.heavyRatio * 100.0f);
        } else {
            ImGui::TextDisabled("Overdraw: enable Particle Overdraw to measure");
        }
        DrawShaderCompileDebugMenuItems();
        if (preferencesChanged) m_session.SaveEditorPreferences();
        ImGui::EndMenu();
    }
    if (!m_standaloneApplicationMode && ImGui::BeginMenu("Window")) {
        if (ImGui::MenuItem("Move to Available Screen Area"))
            RequestFocusAndReveal(true);
        ImGui::TextDisabled("Editor: View > Bring VFX Editor to Front");
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();

    if (!ImGui::GetIO().WantTextInput && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S)) {
        if (m_session.SaveGraph()) m_session.preview.restartRequested = true;
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O)
        && ctx.requestOpenVFXAssetDialog) {
        ctx.requestOpenVFXAssetDialog();
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_R)) {
        m_session.LoadGraph(m_session.document.path);
        m_session.preview.restartRequested = true;
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z)) m_session.Undo();
    if (!ImGui::GetIO().WantTextInput && (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y)
        || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))) m_session.Redo();
}


void VFXEditorPanel::DrawGraphStatusBar()
{
    std::string validationError;
    const bool valid = asset::ValidateVFXGraphAsset(m_session.document.graph, &validationError);
    const asset::VFXGraphBudgetStats budget = asset::CalculateVFXGraphBudget(m_session.document.graph);
    const bool overBudget = budget.particles > m_session.document.graph.maxParticles
        || budget.lights > m_session.document.graph.maxLights || budget.audioVoices > m_session.document.graph.maxAudioVoices;

    ImGui::Separator();
    // Asset Browser は DockSpace 参加型のウィンドウで、閉じるとタブごと消える。
    // WHY: 開閉口が View メニューの中だけだと、一度閉じたときに戻し方が分からなくなる。
    //      メイン Editor の StatusBar と同じボタンを同じ位置 (下端の左) に置いて、
    //      どちらのエディターでも同じ操作で開き直せるようにする。
    DrawDrawerToggle("Asset Browser", m_session.assetBrowserVisible, "Asset Browser");
    ImGui::TextColored(valid && !overBudget ? ImVec4{ 0.38f, 0.82f, 0.52f, 1.0f }
                                             : ImVec4{ 1.0f, 0.42f, 0.30f, 1.0f },
                       valid && !overBudget ? "READY" : "NEEDS ATTENTION");
    ImGui::SameLine();
    ImGui::TextDisabled("%d nodes  |  %d links  |  P %d/%d  L %d/%d  A %d/%d",
                        static_cast<int>(m_session.document.graph.nodes.size()), static_cast<int>(m_session.document.graph.links.size()),
                        budget.particles, m_session.document.graph.maxParticles, budget.lights, m_session.document.graph.maxLights,
                        budget.audioVoices, m_session.document.graph.maxAudioVoices);
    if (!valid && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", validationError.c_str());
    // 未接続ノードは検証を通ってしまうため、警告として別枠で出す。押せば最初の1つへ飛ぶ。
    if (!m_session.document.unreachableNodes.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Warning));
        char label[64];
        std::snprintf(label, sizeof(label), "(!) %d unlinked",
                      static_cast<int>(m_session.document.unreachableNodes.size()));
        if (ImGui::SmallButton(label)) {
            m_session.selectedNodeId = m_session.document.unreachableNodes.front();
            m_session.focusSelectionRequested = true;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Entry から辿り着けないノードがあります (実行時に起動しません)\n"
                              "クリックで最初の1つへジャンプ");
    }
    const float shortcutX = (std::max)(ImGui::GetCursorPosX() + 16.0f,
                                       ImGui::GetContentRegionMax().x - 245.0f);
    ImGui::SameLine(shortcutX);
    ImGui::TextDisabled("Ctrl+S Save   F/A Frame   Ctrl+C/V Copy/Paste");
}


// カタログ 1 件の中身。メニューとダイアログの両方から呼ぶので、
// 「Template の何を見せるか」の正本はここ 1 か所だけにする。
void VFXEditorPanel::DrawTemplateEntryPreview(EditorContext& ctx,
                                              const GraphTemplateEntry& entry, bool compact)
{
    if (!entry.valid) {
        ImGui::TextColored({ 1.0f, 0.45f, 0.4f, 1.0f }, "%s", entry.summary.c_str());
        return;
    }
    // サムネイル。文字の内訳だけでは爆発と魔法の区別が名前でしかつかない。
    if (!entry.thumbnailPath.empty()) {
        if (void* textureId = widgets::ResolveAssetThumbnail(entry.thumbnailPath, ctx.resources,
                                                             ctx.imguiRenderer)) {
            const float size = compact ? 96.0f : 160.0f;
            ImGui::Image(widgets::ToImTextureID(textureId), ImVec2{ size, size * 0.5625f });
        }
    } else {
        ImGui::TextDisabled("(サムネイル未生成)");
    }
    ImGui::TextDisabled("%d nodes  %d links  %.2fs  [%s]", entry.nodeCount, entry.linkCount,
                        entry.duration, TemplateOriginName(entry.origin));
    ImGui::TextDisabled("%s", entry.summary.c_str());
    // budget は取り込み後に必要になる量。ここを出さないと「入れたのに粒子が出ない」になる。
    ImGui::TextDisabled("budget: particles %d / lights %d / audio %d",
                        entry.particleBudget, entry.lightBudget, entry.audioBudget);
    if (!entry.description.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (compact ? 260.0f : 380.0f));
        ImGui::TextUnformatted(entry.description.c_str());
        ImGui::PopTextWrapPos();
    }
    if (!entry.tags.empty()) {
        std::string tags;
        for (const auto& tag : entry.tags) tags += (tags.empty() ? "" : ", ") + tag;
        ImGui::TextDisabled("tags: %s", tags.c_str());
    }
    if (!entry.variants.empty()) {
        std::string variants;
        for (const auto& variant : entry.variants)
            variants += (variants.empty() ? "" : ", ") + variant.name;
        ImGui::TextDisabled("variants: %s", variants.c_str());
    }
    // 素材が無い Template は「置いても何も出ない」形でしか失敗しない。適用前に出す。
    if (!entry.missingAssets.empty()) {
        ImGui::TextColored({ 1.0f, 0.55f, 0.35f, 1.0f }, "素材が %d 件見つかりません",
                           static_cast<int>(entry.missingAssets.size()));
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("このまま適用すると、該当ノードは配置されても描画されません:");
            for (std::size_t index = 0; index < entry.missingAssets.size() && index < 8; ++index)
                ImGui::BulletText("%s", entry.missingAssets[index].c_str());
            if (entry.missingAssets.size() > 8)
                ImGui::TextDisabled("... 他 %d 件",
                                    static_cast<int>(entry.missingAssets.size() - 8));
            ImGui::EndTooltip();
        }
    }
}


// Template を適用する唯一の経路。メニューの即実行もダイアログの実行もここを通す。
void VFXEditorPanel::ApplyTemplateWithOptions(EditorContext& ctx, const GraphTemplateEntry& entry,
                                              TemplateApplyMode mode, bool saveImmediately)
{
    vfx::TemplateMergeOptions options = m_mergeOptions;
    // 層のチェック状態を groupFilter へ落とす。全選択は「全体取り込み」と同義なので空にする。
    options.groupFilter.clear();
    const bool allSelected = std::all_of(m_layerSelection.begin(), m_layerSelection.end(),
        [](const auto& item) { return item.second; });
    if (!m_layerSelection.empty() && !allSelected)
        for (const auto& [groupId, selected] : m_layerSelection)
            if (selected) options.groupFilter.push_back(groupId);
    options.variantName.clear();
    if (m_pendingVariantIndex > 0
        && m_pendingVariantIndex <= static_cast<int>(entry.variants.size()))
        options.variantName = entry.variants[m_pendingVariantIndex - 1].name;

    if (m_session.ApplyTemplate(ctx, entry, mode, options, saveImmediately)
        && m_session.hasMergeReport) {
        const auto& report = m_session.lastMergeReport;
        // 黙って起きると困ることが 1 つでもあれば報告を開く。
        // 何も無かったときにダイアログを出すと、ただの邪魔になる。
        m_openMergeReport = !report.renamedParameters.empty() || !report.missingAssets.empty()
            || report.budgetBefore[0] != report.budgetAfter[0]
            || report.budgetBefore[1] != report.budgetAfter[1]
            || report.budgetBefore[2] != report.budgetAfter[2];
    }
}


void VFXEditorPanel::DrawGraphTemplateMenu(EditorContext& ctx)
{
    m_session.templates.Scan(ctx, false);
    if (m_session.templates.entries.empty()) {
        ImGui::TextDisabled("Assets/VFX/Templates に .vfx がありません");
    }
    // 検索。件数が増えるとカテゴリのメニューを開いて回るより打った方が速い。
    ImGui::SetNextItemWidth(220.0f);
    InputString("##TemplateFilter", m_templateFilter, 64);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("名前・カテゴリ・タグ・説明・ノード内訳を大小無視で部分一致");
    ImGui::Separator();

    const std::vector<const GraphTemplateEntry*> matches =
        m_session.templates.Filter(m_templateFilter);
    // 検索中はカテゴリを畳まず平坦に出す (絞った結果をもう一段掘らせない)。
    const bool flat = !m_templateFilter.empty();
    const auto drawEntry = [&](const GraphTemplateEntry& entry) {
        // 名前だけでは分からないので、hover でカタログの中身をそのまま出す。
        if (!ImGui::BeginMenu(entry.name.c_str())) return;
        DrawTemplateEntryPreview(ctx, entry, true);
        ImGui::Separator();
        if (ImGui::MenuItem("Apply...", nullptr, false, entry.valid)) {
            m_pendingTemplate = entry;
            m_pendingMode = TemplateApplyMode::Merge;
            m_openTemplateApply = true;
            m_mergeOptions = vfx::TemplateMergeOptions{};
            m_mergeOptions.anchorNodeId = m_session.selectedNodeId;
            m_layerSelection.clear();
            for (const auto& layer : entry.layers) m_layerSelection.emplace_back(layer.groupId, true);
            m_pendingVariantIndex = 0;
            m_session.templates.status.clear();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("取り込む層・接続先・Variant を選んでから適用します");
        // 全体を Entry へ足すだけなら確認は要らない (非破壊で Ctrl+Z で戻せる)。
        if (ImGui::MenuItem("Quick Merge", nullptr, false, entry.valid)) {
            m_mergeOptions = vfx::TemplateMergeOptions{};
            m_layerSelection.clear();
            m_pendingVariantIndex = 0;
            ApplyTemplateWithOptions(ctx, entry, TemplateApplyMode::Merge, false);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Template 全体を現在のグラフへ追記します (Ctrl+Z で戻せます)");
        if (ImGui::MenuItem("Link as Sub Graph", nullptr, false, entry.valid)) {
            m_mergeOptions = vfx::TemplateMergeOptions{};
            m_layerSelection.clear();
            m_pendingVariantIndex = 0;
            ApplyTemplateWithOptions(ctx, entry, TemplateApplyMode::SubGraph, false);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("複製せず参照として置きます。\n"
                              "Template を直すと、参照している全てのグラフへ伝播します。\n"
                              "Merge (コピー) との違いはそこだけです。");
        if (ImGui::MenuItem("Replace Graph...", nullptr, false, entry.valid)) {
            m_pendingTemplate = entry;
            m_openTemplateConfirm = true;
            m_templateSaveImmediately = false;
            m_pendingVariantIndex = 0;
            m_session.templates.status.clear();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Discard the current graph and load this template instead");
        ImGui::EndMenu();
    };

    if (flat) {
        for (const GraphTemplateEntry* entry : matches) drawEntry(*entry);
    } else {
        // カテゴリ (Templates ルートからのサブフォルダ) ごとに畳む。ルート直下は素で並べる。
        // entries はカテゴリ優先で整列済みだが、ここは順序に依存しない形で組む。
        for (const GraphTemplateEntry* entry : matches)
            if (entry->category.empty()) drawEntry(*entry);
        for (const std::string& category : m_session.templates.Categories()) {
            if (category.empty()) continue;
            if (!ImGui::BeginMenu(category.c_str())) continue;
            for (const GraphTemplateEntry* entry : matches)
                if (entry->category == category) drawEntry(*entry);
            ImGui::EndMenu();
        }
    }
    if (matches.empty() && !m_session.templates.entries.empty())
        ImGui::TextDisabled("一致する Template がありません");
    ImGui::Separator();
    // ゼロから積むより骨格に沿わせた方が確実に「らしく」なる。空 Entry からの脱出口。
    if (ImGui::MenuItem("New from Recipe...")) {
        m_openRecipeWizard = true;
        m_recipeStatus.clear();
        m_recipeRoleTextures.clear();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("目的 (Fire / Explosion / Impact / Smoke / Beam / Aura) と規模を選ぶと\n"
                          "層構成・ブレンド・描画順が揃った骨格を生成します。\n"
                          "AI の vfx.guide と同じ recipe を使うため、指示が食い違いません。");
    ImGui::Separator();
    // 連番書き出しは Template と同じ「グラフ単位の成果物を出す」操作なのでここへ置く。
    if (ImGui::MenuItem("Export Frame Sequence...", nullptr, false, !m_session.document.graph.nodes.empty())) {
        m_previewView.sequenceDialogOpen = true;
        m_session.sequenceExport.message.clear();
        // 既定は再生長そのまま・プロジェクト直下の Captures/<グラフ名>/。
        if (m_session.sequenceExport.duration <= 0.0f) m_session.sequenceExport.duration = 2.0f;
        if (m_session.sequenceExport.directory.empty()) {
            const std::string name = m_session.document.graph.name.empty() ? std::string("VFX") : m_session.document.graph.name;
            m_session.sequenceExport.directory =
                (std::filesystem::path(ctx.projectRoot) / "Captures" / name).string();
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("決定論スクラブで全区間を連番 PNG にします。\n"
                          "同じ .vfx なら何度実行しても同じ絵になります。");
    if (ImGui::MenuItem("Save Current Graph as Template...")) {
        m_saveTemplateName = m_session.document.graph.name;
        m_saveTemplateCategory.clear();
        m_saveTemplateDescription = m_session.document.graph.description;
        m_saveTemplateTags.clear();
        for (const auto& tag : m_session.document.graph.tags)
            m_saveTemplateTags += (m_saveTemplateTags.empty() ? "" : ", ") + tag;
        m_session.templates.status.clear();
        m_openSaveTemplateDialog = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Bake Missing Thumbnails", nullptr, false,
                        !m_thumbnailBaker.IsRunning() && !m_session.templates.entries.empty())) {
        std::vector<std::string> paths;
        for (const auto& entry : m_session.templates.entries)
            if (entry.valid) paths.push_back(entry.path);
        m_thumbnailBaker.Begin(std::move(paths), false);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("未生成の Template だけを決定論スクラブで 1 枚ずつ焼きます。\n"
                          "実行中はプレビューが一時的に Template の絵へ切り替わります。");
    if (ImGui::MenuItem("Rebake All Thumbnails", nullptr, false,
                        !m_thumbnailBaker.IsRunning() && !m_session.templates.entries.empty())) {
        std::vector<std::string> paths;
        for (const auto& entry : m_session.templates.entries)
            if (entry.valid) paths.push_back(entry.path);
        m_thumbnailBaker.Begin(std::move(paths), true);
    }
    if (m_thumbnailBaker.IsRunning() && ImGui::MenuItem("Cancel Thumbnail Bake"))
        m_thumbnailBaker.Cancel();
    if (!m_thumbnailBaker.message.empty())
        ImGui::TextDisabled("%s", m_thumbnailBaker.message.c_str());
    if (ImGui::MenuItem("Rescan Templates")) m_session.templates.Scan(ctx, true);
    ImGui::TextDisabled("Merge は追記(コピー) / Sub Graph は参照 / Replace は全置換");
    ImGui::TextDisabled("いずれも Ctrl+Z で戻せます");
}


void VFXEditorPanel::DrawGraphTemplateDialogs(EditorContext& ctx)
{
    // 連番書き出しも同じ「メニュー階層の外で開く」制約に従う。
    // 実行中の 1 フレーム進行はここで回す (毎フレーム必ず通る場所であるため)。
    m_previewView.DrawSequenceExportDialog(ctx);
    m_session.sequenceExport.Tick(ctx, m_session.preview);
    // サムネイル焼きも同じ理由でここで進める。
    m_thumbnailBaker.Tick(ctx, m_session.preview);
    if (m_thumbnailBaker.restoreRequested) {
        m_thumbnailBaker.restoreRequested = false;
        // Preview へ差し込んだ Template のグラフを捨て、編集中のグラフを押し直させる。
        // 版数を 0 に戻すのが「次のフレームで再送出する」合図 (通常のライブ編集と同じ経路)。
        m_session.lastPushedGraphRevision = 0;
        if (ctx.vfxPreviewScene != nullptr) {
            if (auto* owner = ctx.vfxPreviewScene->GetGameObject(m_session.preview.graphEntity)) {
                if (auto* instance = owner->GetComponent<scene::VFXGraphComponent>()) {
                    instance->authoringGraph.reset();
                    instance->editorScrubTime = -1.0f;
                    instance->reloadRequested = true;
                }
            }
        }
        m_session.preview.restartRequested = true;
        m_session.templates.Scan(ctx, true); // 焼いた PNG をカタログへ反映する
    }
    // メニュー階層の内側では BeginPopupModal を開始できないため、要求フラグを見て
    // ウィンドウ直下のここで開く。
    if (m_openTemplateConfirm) {
        ImGui::OpenPopup("Replace Graph with Template##VFX");
        m_openTemplateConfirm = false;
    }
    if (m_openTemplateApply) {
        ImGui::OpenPopup("Apply Template##VFX");
        m_openTemplateApply = false;
    }
    if (m_openMergeReport) {
        ImGui::OpenPopup("Template Applied##VFX");
        m_openMergeReport = false;
    }
    if (m_openSaveTemplateDialog) {
        ImGui::OpenPopup("Save as Template##VFX");
        m_openSaveTemplateDialog = false;
    }
    if (m_openRecipeWizard) {
        ImGui::OpenPopup("New from Recipe##VFX");
        m_openRecipeWizard = false;
    }
    DrawTemplateApplyDialog(ctx);
    DrawTemplateMergeReport();
    DrawRecipeWizardDialog(ctx);

    ImGui::SetNextWindowSize({ 460.0f, 0.0f }, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Replace Graph with Template##VFX", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Template \"%s\" で現在のGraphを置き換えます。",
                           m_pendingTemplate.name.c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("Current : %d nodes  %d links%s",
                            static_cast<int>(m_session.document.graph.nodes.size()),
                            static_cast<int>(m_session.document.graph.links.size()),
                            m_session.document.dirty ? "  (未保存の変更あり)" : "");
        ImGui::TextDisabled("Template: %d nodes  %d links  %.2fs",
                            m_pendingTemplate.nodeCount, m_pendingTemplate.linkCount,
                            m_pendingTemplate.duration);
        ImGui::Spacing();
        ImGui::TextColored({ 1.0f, 0.78f, 0.35f, 1.0f },
                           "現在のノード・リンク・グループは全て破棄されます。");
        // Replace でも Variant を選んで焼き込める (Small / Medium / Large の作り分け)。
        if (!m_pendingTemplate.variants.empty()) {
            std::vector<const char*> labels{ "(default)" };
            for (const auto& variant : m_pendingTemplate.variants)
                labels.push_back(variant.name.c_str());
            ImGui::Combo("Variant", &m_pendingVariantIndex, labels.data(),
                         static_cast<int>(labels.size()));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("選んだ Variant の値を公開パラメーターの既定値へ焼き込みます。");
        }
        ImGui::Checkbox("すぐ保存してPreviewへ反映する", &m_templateSaveImmediately);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Preview はディスク上の .vfx を読むため、反映には保存が必要です。\n"
                              "オフのままなら .vfx は上書きされず、Ctrl+Z で完全に元へ戻せます。");
        ImGui::Separator();
        if (ImGui::Button("Replace", { 140.0f, 0.0f })) {
            m_mergeOptions = vfx::TemplateMergeOptions{};
            m_layerSelection.clear();
            ApplyTemplateWithOptions(ctx, m_pendingTemplate, TemplateApplyMode::Replace,
                                     m_templateSaveImmediately);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Merge Instead", { 140.0f, 0.0f })) {
            m_mergeOptions = vfx::TemplateMergeOptions{};
            m_layerSelection.clear();
            ApplyTemplateWithOptions(ctx, m_pendingTemplate, TemplateApplyMode::Merge, false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", { 120.0f, 0.0f })) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowSize({ 520.0f, 0.0f }, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Save as Template##VFX", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("現在のGraphを Assets/VFX/Templates へ保存します。");
        InputString("Template Name", m_saveTemplateName, 128);
        InputString("Category", m_saveTemplateCategory, 96);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Templates 直下のサブフォルダ名。空ならルート直下。\n"
                              "メニューではカテゴリごとに畳んで表示されます。");
        InputString("Description", m_saveTemplateDescription, 256);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("用途・演出意図・成功条件。カタログにも AI の検索にも出ます。\n"
                              "ここを空のままにすると、名前だけで中身を判断することになります。");
        InputString("Tags (comma separated)", m_saveTemplateTags, 192);
        const bool overwrites = std::any_of(m_session.templates.entries.begin(), m_session.templates.entries.end(),
            [this](const GraphTemplateEntry& item) {
                return item.name == m_saveTemplateName && item.category == m_saveTemplateCategory;
            });
        if (overwrites)
            ImGui::TextColored({ 1.0f, 0.78f, 0.35f, 1.0f }, "同名のTemplateを上書きします");
        // 公開パラメーターの無い Template は、複製しても値だけ変える使い方ができない。
        if (m_session.document.graph.parameters.empty())
            ImGui::TextColored({ 1.0f, 0.78f, 0.35f, 1.0f },
                               "公開パラメーターがありません (複製後に raw field を触ることになります)");
        if (!m_session.templates.status.empty()) ImGui::TextWrapped("%s", m_session.templates.status.c_str());
        ImGui::Separator();
        if (ImGui::Button("Save", { 140.0f, 0.0f })) {
            std::vector<std::string> tags;
            std::string current;
            for (const char character : m_saveTemplateTags + ",") {
                if (character == ',') {
                    // 前後の空白を落とす。", 爆発 , 屋外" のような打ち方を許すため。
                    const std::size_t begin = current.find_first_not_of(" \t");
                    const std::size_t end = current.find_last_not_of(" \t");
                    if (begin != std::string::npos) tags.push_back(current.substr(begin, end - begin + 1));
                    current.clear();
                } else current += character;
            }
            if (m_session.templates.SaveAsTemplate(ctx, m_session.document.graph, m_saveTemplateName,
                                                   m_saveTemplateCategory, m_saveTemplateDescription,
                                                   tags))
                ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", { 120.0f, 0.0f })) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}


// 取り込む層・接続先・Variant を決めてから適用する。Merge と Sub Graph の共通入口。
void VFXEditorPanel::DrawTemplateApplyDialog(EditorContext& ctx)
{
    ImGui::SetNextWindowSize({ 560.0f, 0.0f }, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Apply Template##VFX", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::TextWrapped("Template \"%s\" を現在のグラフへ取り込みます。",
                       m_pendingTemplate.name.c_str());
    ImGui::Separator();
    DrawTemplateEntryPreview(ctx, m_pendingTemplate, false);
    ImGui::Separator();

    // ── 取り込み方 ──
    int mode = static_cast<int>(m_pendingMode);
    ImGui::RadioButton("Merge (コピー)", &mode, static_cast<int>(TemplateApplyMode::Merge));
    ImGui::SameLine();
    ImGui::RadioButton("Sub Graph (参照)", &mode, static_cast<int>(TemplateApplyMode::SubGraph));
    m_pendingMode = static_cast<TemplateApplyMode>(mode);
    ImGui::TextDisabled("%s", m_pendingMode == TemplateApplyMode::Merge
        ? "取り込んだ後は独立して編集できます。Template を直しても伝播しません。"
        : "Template を直すと参照している全グラフへ伝播します。中身はここでは編集できません。");

    const bool merging = m_pendingMode == TemplateApplyMode::Merge;

    // ── 取り込む層 ──
    if (merging && !m_layerSelection.empty()) {
        ImGui::Spacing();
        widgets::SectionHeader("取り込む層");
        ImGui::TextDisabled("Template のグループ枠が層の単位です。必要な層だけ選べます。");
        for (auto& [groupId, selected] : m_layerSelection) {
            const auto layer = std::find_if(m_pendingTemplate.layers.begin(),
                m_pendingTemplate.layers.end(),
                [groupId](const GraphTemplateLayer& item) { return item.groupId == groupId; });
            if (layer == m_pendingTemplate.layers.end()) continue;
            const std::string label = layer->title + "  (" + std::to_string(layer->nodeCount) + " nodes)";
            ImGui::Checkbox(label.c_str(), &selected);
            if (!layer->note.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", layer->note.c_str());
        }
        if (ImGui::SmallButton("All")) for (auto& item : m_layerSelection) item.second = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("None")) for (auto& item : m_layerSelection) item.second = false;
    } else if (merging) {
        ImGui::TextDisabled("この Template はグループ枠を持たないため、全体を取り込みます。");
    }

    // ── 接続先 ──
    ImGui::Spacing();
    widgets::SectionHeader("接続先");
    // 「Entry から」だけだと、ヒットの後段へ煙を足すのに取り込んでから配線し直す手間が要る。
    std::vector<const asset::VFXGraphNode*> anchors;
    for (const auto& node : m_session.document.graph.nodes)
        if (node.type != asset::VFXNodeType::Entry) anchors.push_back(&node);
    std::vector<std::string> anchorLabels{ "Entry (グラフ開始時)" };
    int anchorIndex = 0;
    for (std::size_t index = 0; index < anchors.size(); ++index) {
        anchorLabels.push_back(anchors[index]->name + " ["
                               + asset::VFXNodeTypeName(anchors[index]->type) + "]");
        if (anchors[index]->id == m_mergeOptions.anchorNodeId)
            anchorIndex = static_cast<int>(index) + 1;
    }
    std::vector<const char*> anchorItems;
    for (const auto& label : anchorLabels) anchorItems.push_back(label.c_str());
    if (ImGui::Combo("From", &anchorIndex, anchorItems.data(), static_cast<int>(anchorItems.size())))
        m_mergeOptions.anchorNodeId =
            anchorIndex == 0 ? -1 : anchors[static_cast<std::size_t>(anchorIndex - 1)]->id;

    if (m_mergeOptions.anchorNodeId >= 0) {
        int trigger = static_cast<int>(m_mergeOptions.anchorTrigger);
        const char* triggers[] = { "On Complete", "On Start", "On Collision", "On Death" };
        if (ImGui::Combo("Trigger", &trigger, triggers, IM_ARRAYSIZE(triggers)))
            m_mergeOptions.anchorTrigger = static_cast<asset::VFXLinkTrigger>(trigger);
        // On Collision / On Death は Particle しか発火源にできない。押してから
        // 保存時に落ちるより、ここで理由ごと出す。
        const asset::VFXGraphNode* anchor = FindGraphNode(m_session.document.graph,
                                                          m_mergeOptions.anchorNodeId);
        const bool particleOnly = m_mergeOptions.anchorTrigger == asset::VFXLinkTrigger::OnCollision
                               || m_mergeOptions.anchorTrigger == asset::VFXLinkTrigger::OnDeath;
        if (anchor != nullptr && particleOnly && anchor->type != asset::VFXNodeType::Particle)
            ImGui::TextColored({ 1.0f, 0.45f, 0.4f, 1.0f },
                               "%s は Particle ノードからしか発火できません",
                               asset::VFXLinkTriggerName(m_mergeOptions.anchorTrigger));
    }
    ImGui::DragFloat("Delay", &m_mergeOptions.anchorDelay, 0.01f, 0.0f, 10.0f, "%.2f s");

    if (merging) {
        // 空間の親。link (発火順) とは別軸なので、混同しないよう説明を添える。
        std::vector<std::string> parentLabels{ "(なし)" };
        int parentIndex = 0;
        for (std::size_t index = 0; index < anchors.size(); ++index) {
            parentLabels.push_back(anchors[index]->name);
            if (anchors[index]->id == m_mergeOptions.parentNodeId)
                parentIndex = static_cast<int>(index) + 1;
        }
        std::vector<const char*> parentItems;
        for (const auto& label : parentLabels) parentItems.push_back(label.c_str());
        if (ImGui::Combo("Parent (空間)", &parentIndex, parentItems.data(),
                         static_cast<int>(parentItems.size())))
            m_mergeOptions.parentNodeId =
                parentIndex == 0 ? -1 : anchors[static_cast<std::size_t>(parentIndex - 1)]->id;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Transform の親子であって、発火順ではありません。\n"
                              "「まとめて傾けたい」ときに設定します。");
    }

    // ── Variant ──
    if (!m_pendingTemplate.variants.empty()) {
        ImGui::Spacing();
        std::vector<const char*> labels{ "(default)" };
        for (const auto& variant : m_pendingTemplate.variants) labels.push_back(variant.name.c_str());
        ImGui::Combo("Variant", &m_pendingVariantIndex, labels.data(),
                     static_cast<int>(labels.size()));
    }

    // ── budget ──
    if (merging) {
        ImGui::Spacing();
        ImGui::Checkbox("budget を合算する", &m_mergeOptions.raiseBudget);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("取り込むとノードは増えますが、上限は現在の値のままです。\n"
                              "合算しないと DAG 検証は通るのに実行時だけ粒子が出ません。");
        const asset::VFXGraphBudgetStats usage =
            asset::CalculateVFXGraphBudget(m_session.document.graph);
        ImGui::TextDisabled("現在 %d / 上限 %d  →  取り込み後 およそ %d",
                            usage.particles, m_session.document.graph.maxParticles,
                            usage.particles + m_pendingTemplate.particleBudget);
    }

    ImGui::Separator();
    if (ImGui::Button("Apply", { 140.0f, 0.0f })) {
        ApplyTemplateWithOptions(ctx, m_pendingTemplate, m_pendingMode, false);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", { 120.0f, 0.0f })) ImGui::CloseCurrentPopup();
    if (!m_session.document.error.empty())
        ImGui::TextColored({ 1.0f, 0.45f, 0.4f, 1.0f }, "%s", m_session.document.error.c_str());
    ImGui::EndPopup();
}


// 取り込みで「黙って起きた」ことの報告。何も起きなければ開かない。
void VFXEditorPanel::DrawTemplateMergeReport()
{
    ImGui::SetNextWindowSize({ 520.0f, 0.0f }, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Template Applied##VFX", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;
    const auto& report = m_session.lastMergeReport;
    ImGui::Text("%d ノードを取り込みました。", static_cast<int>(report.addedNodes.size()));

    if (!report.renamedParameters.empty()) {
        ImGui::Spacing();
        widgets::SectionHeader("公開パラメーターの改名");
        ImGui::TextWrapped("同名でも型・レンジ・駆動先が違うため、既存へ相乗りさせず別物として"
                           "取り込みました。既存パラメーターの意味は変わっていません。");
        for (const auto& [oldName, newName] : report.renamedParameters)
            ImGui::BulletText("%s  ->  %s", oldName.c_str(), newName.c_str());
    }
    if (!report.reusedParameters.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("既存へ相乗りしたパラメーター:");
        for (const auto& name : report.reusedParameters) ImGui::BulletText("%s", name.c_str());
    }
    const bool budgetRaised = report.budgetBefore[0] != report.budgetAfter[0]
        || report.budgetBefore[1] != report.budgetAfter[1]
        || report.budgetBefore[2] != report.budgetAfter[2];
    if (budgetRaised) {
        ImGui::Spacing();
        widgets::SectionHeader("budget の引き上げ");
        ImGui::BulletText("particles  %d -> %d", report.budgetBefore[0], report.budgetAfter[0]);
        ImGui::BulletText("lights     %d -> %d", report.budgetBefore[1], report.budgetAfter[1]);
        ImGui::BulletText("audio      %d -> %d", report.budgetBefore[2], report.budgetAfter[2]);
        ImGui::TextDisabled("実使用量: particles %d / lights %d / audio %d",
                            report.usageAfter.particles, report.usageAfter.lights,
                            report.usageAfter.audioVoices);
    }
    if (!report.missingAssets.empty()) {
        ImGui::Spacing();
        ImGui::TextColored({ 1.0f, 0.55f, 0.35f, 1.0f }, "見つからない素材 (該当ノードは描画されません)");
        for (const auto& path : report.missingAssets) ImGui::BulletText("%s", path.c_str());
    }
    if (!report.addedVariants.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("取り込んだ Variant:");
        for (const auto& name : report.addedVariants) ImGui::BulletText("%s", name.c_str());
    }
    if (report.addedSignalNodes > 0 || report.addedSubGraphForwards > 0)
        ImGui::TextDisabled("Signal ノード %d 件 / Sub Graph 転送 %d 件を引き継ぎました",
                            report.addedSignalNodes, report.addedSubGraphForwards);

    ImGui::Separator();
    if (ImGui::Button("OK", { 120.0f, 0.0f })) ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    ImGui::TextDisabled("Ctrl+Z で取り込み前へ戻せます");
    ImGui::EndPopup();
}


// 目的と規模から骨格を生成する。空 Entry 1 個から積み始めなくて済むようにするための入口。
// WHY: VFX で最も難しいのは「どの層を、どの順で、どのブレンドで重ねるか」であって
//      ノードを置く作業ではない。層構成の知識は vfx.guide が AI へ渡していたが、
//      人間側には同じものが無く、Editor では毎回ゼロから積み直していた。
void VFXEditorPanel::DrawRecipeWizardDialog(EditorContext& ctx)
{
    ImGui::SetNextWindowSize({ 560.0f, 0.0f }, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("New from Recipe##VFX", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const std::span<const VFXRecipe> recipes = GetVFXRecipes();
    std::vector<const char*> names;
    for (const VFXRecipe& recipe : recipes) names.push_back(recipe.name);
    m_recipeIndex = std::clamp(m_recipeIndex, 0, static_cast<int>(recipes.size()) - 1);
    if (ImGui::Combo("Recipe", &m_recipeIndex, names.data(), static_cast<int>(names.size())))
        m_recipeRoleTextures.clear(); // 必要ロールが変わるので選び直させる
    const VFXRecipe& recipe = recipes[static_cast<std::size_t>(m_recipeIndex)];
    ImGui::TextDisabled("%s", recipe.summary);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 500.0f);
    ImGui::TextUnformatted(recipe.layers);
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    widgets::SectionHeader("層構成");
    for (const VFXRecipeLayer& layer : recipe.layerSpecs) {
        ImGui::BulletText("%s  [%s]%s%s", layer.name, asset::VFXNodeTypeName(layer.nodeType),
                          layer.assetRole[0] == '\0' ? "" : "  素材: ",
                          layer.assetRole);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", layer.note);
    }

    ImGui::Spacing();
    widgets::SectionHeader("規模と構成");
    const char* scales[] = { "S", "M", "L" };
    ImGui::Combo("Scale", &m_recipeScaleIndex, scales, IM_ARRAYSIZE(scales));
    ImGui::Checkbox("Loop", &m_recipeLoop);
    if (recipe.wantsLight) { ImGui::SameLine(); ImGui::Checkbox("点光源", &m_recipeLight); }
    if (recipe.wantsCameraShake) { ImGui::SameLine(); ImGui::Checkbox("カメラシェイク", &m_recipeShake); }

    // ── 素材 ──
    // 素材の割り当てが空でも生成はできるが、その場合はテクスチャ未設定の粒子になる。
    // 「置いても何も出ない」を作らないよう、何が要るかを明示して受け口を出す。
    ImGui::Spacing();
    widgets::SectionHeader("素材");
    ImGui::TextDisabled("Asset Browser からドロップするか、パスを直接入力してください。");
    // 何が起きるかを先に言う。.mat が勝手に増えると「知らないアセットがある」になる。
    ImGui::TextDisabled("落としたテクスチャは層のブレンドに合わせた .mat へ変換され、");
    ImGui::TextDisabled("Assets/Materials/Particles/ に置かれます (同名があれば再利用)。");
    for (const std::string& role : CollectRecipeRoles(recipe)) {
        if (std::none_of(m_recipeRoleTextures.begin(), m_recipeRoleTextures.end(),
                [&role](const std::pair<std::string, std::string>& item) { return item.first == role; }))
            m_recipeRoleTextures.emplace_back(role, std::string{});
        std::string& texturePath = std::find_if(m_recipeRoleTextures.begin(),
            m_recipeRoleTextures.end(),
            [&role](const std::pair<std::string, std::string>& item) { return item.first == role; })->second;
        // 役割の説明を出す。無いと何をドロップすればよいか判らない。
        const char* purpose = "";
        for (const VFXAssetRoleInfo& info : GetVFXAssetRoles())
            if (role == info.role) purpose = info.purpose;
        widgets::AssetPathField(role.c_str(), texturePath, ".png,.tga,.dds,.jpg,.jpeg",
                                ctx.projectRoot);
        if (ImGui::IsItemHovered() && purpose[0] != '\0') ImGui::SetTooltip("%s", purpose);
    }

    ImGui::Spacing();
    ImGui::Checkbox("現在のグラフを置き換える", &m_recipeReplaceCurrent);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("オフにすると、生成した骨格を現在のグラフへ追記します。\n"
                          "どちらも Ctrl+Z で戻せます。");
    if (!m_recipeStatus.empty()) ImGui::TextWrapped("%s", m_recipeStatus.c_str());

    ImGui::Separator();
    if (ImGui::Button("Generate", { 140.0f, 0.0f })) {
        VFXRecipeBuildOptions options;
        options.scale = m_recipeScaleIndex == 0 ? 0.6f : (m_recipeScaleIndex == 2 ? 1.6f : 1.0f);
        options.loop = m_recipeLoop;
        options.includeLight = m_recipeLight;
        options.includeCameraShake = m_recipeShake;
        for (const auto& [role, path] : m_recipeRoleTextures)
            if (!path.empty()) options.roleTextures[role] = path;
        options.projectRoot = ctx.projectRoot;
        const std::string stem =
            std::filesystem::path(m_session.document.path).stem().generic_string();
        options.graphName = stem.empty() ? std::string(recipe.name) : stem;

        asset::VFXGraphAsset generated = BuildGraphFromRecipe(recipe, options);
        m_session.PushUndo();
        m_session.document.liveDirtyNodeId = -1;
        if (m_recipeReplaceCurrent) {
            m_session.document.graph = std::move(generated);
            m_session.selectedNodeId = -1;
            m_session.selectedLinkIndex = -1;
            m_session.selectedGroupId = -1;
        } else {
            vfx::TemplateMergeOptions mergeOptions;
            vfx::TemplateMergeReport report;
            std::string error;
            if (MergeGraphTemplateInto(m_session.document.graph, generated, recipe.name,
                                       mergeOptions, report, &error)) {
                if (m_session.onSelectNodes) m_session.onSelectNodes(report.addedNodes);
            } else {
                m_session.document.history.undoStack.pop_back();
                m_recipeStatus = error;
                ImGui::EndPopup();
                return;
            }
        }
        m_session.document.dirty = true;
        m_session.positionsPending = true;
        if (m_session.onGraphReplaced) m_session.onGraphReplaced(m_recipeReplaceCurrent);
        m_session.preview.restartRequested = true;
        // 生成直後に検証を回す。素材未割当・到達不能はここで一度に見せる
        // (作った直後に気づける方が、プレビューを睨んでから戻るより速い)。
        m_session.document.warnings = asset::CollectVFXGraphWarnings(m_session.document.graph);
        m_session.document.RefreshUnreachableNodes();
        m_recipeStatus.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", { 120.0f, 0.0f })) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}


void VFXEditorPanel::DrawGraphToolbar(EditorContext& ctx)
{
    ImGui::Text("%s%s", m_session.document.graph.name.c_str(), m_session.document.dirty ? "  *" : "");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m_session.document.path.c_str());
    ImGui::SameLine();
    // 「保存しないと反映されない」誤解を生まないよう、状態を常時見せる。
    if (m_session.liveEditEnabled) {
        ImGui::TextColored({ 0.42f, 0.85f, 0.58f, 1.0f }, "LIVE");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("編集はプレビューへ即反映されています (保存不要)。\n"
                              "View > Live Edit で切り替え。");
    } else {
        ImGui::TextDisabled("SAVE TO APPLY");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ライブ編集が無効です。反映には保存が必要です。");
    }
    ImGui::SameLine();
    const bool graphSaved = ImGui::Button("Save") && m_session.SaveGraph();
    if (graphSaved) m_session.preview.restartRequested = true;
    ImGui::SameLine();
    const bool graphReloaded = ImGui::Button("Reload");
    if (graphReloaded) {
        m_session.LoadGraph(m_session.document.path);
        m_session.preview.restartRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Add Effect")) {
        m_canvas.addNodeFilter.clear();
        m_canvas.addNodeFilterFocus = true;
        ImGui::OpenPopup("##VFXAddNode");
    }
    if (ImGui::BeginPopup("##VFXAddNode")) {
        ImGui::TextDisabled("Effect Nodes");
        ImGui::Separator();
        m_canvas.DrawAddNodeMenu();
        ImGui::EndPopup();
    }

    const std::string assetPath = NormalizeAssetPath(m_session.document.path);
    scene::GameObject* previewObject = ctx.vfxPreviewScene != nullptr
        ? ctx.vfxPreviewScene->GetGameObject(m_session.preview.graphEntity) : nullptr;
    scene::VFXGraphComponent* instance = previewObject != nullptr
        ? previewObject->GetComponent<scene::VFXGraphComponent>() : nullptr;

    // Preview Instance は専用 Scene に自動生成する。Hierarchy選択やScene Dirtyを一切変更しない。
    if (instance == nullptr && ctx.vfxPreviewScene != nullptr && !assetPath.empty()) {
        auto& gameObject = ctx.vfxPreviewScene->CreateGameObject("__VFX_PREVIEW_ROOT");
        scene::VFXGraphComponent component;
        component.graphPath = assetPath;
        component.playOnAwake = false;
        component.playing = true;
        component.syncParentAnimator = m_session.preview.syncActorToVFX;
        component.editorPreviewFrame = Time::frameCount;
        gameObject.AddComponent<scene::VFXGraphComponent>(std::move(component));
        m_session.preview.graphEntity = gameObject.GetID();
        instance = gameObject.GetComponent<scene::VFXGraphComponent>();
        m_session.preview.ApplyActorAttachment(ctx);
    }
    m_session.preview.SyncInstanceCopies(ctx, assetPath);
    // ライブ編集中は保存だけでプレビューを作り直さない。
    // WHY: 保存のたびに頭出しへ戻ると、長い煙や炎の「後半の見え方」を詰められない。
    //      ディスクを正に戻す Reload と、明示的な Restart のときだけ作り直す。
    const bool needsHardReload = graphReloaded
        || (m_session.preview.restartRequested && !m_session.liveEditEnabled)
        || (graphSaved && !m_session.liveEditEnabled);
    if (needsHardReload && instance != nullptr) {
        instance->reloadRequested = true;
        instance->Restart();
    }
    if (graphSaved || graphReloaded || m_session.preview.restartRequested) m_session.preview.restartRequested = false;
    ImGui::SameLine();
    if (instance != nullptr) {
        instance->editorPreviewFrame = Time::frameCount;
        // 未保存グラフをプレビューへ流し込む。VFXGraphSystem 側は「値だけの変更」なら
        // 生成済みノードを壊さずに反映するため、粒子も再生位置も途切れない。
        if (m_session.liveEditEnabled) {
            const std::uint64_t revision = m_session.document.dirty.Revision();
            // Solo は値の版数に現れない表示状態なので、切り替えも再送出の契機に含める。
            // これを忘れると Solo を押しても次の編集まで画面が変わらない。
            if (instance->authoringGraph == nullptr
                || instance->authoringRevision != revision
                || m_session.lastPushedGraphRevision != revision
                || m_session.document.lastPushedSoloNodeId != m_session.document.soloNodeId) {
                // Solo 中は非対象ノードを落としたコピーを渡す。m_session.document.graph 自体は触らないので
                // 保存内容にも Undo 履歴にも Solo は残らない。
                instance->authoringGraph =
                    std::make_shared<const asset::VFXGraphAsset>(m_session.document.MakeSoloFilteredGraph());
                instance->authoringRevision = revision;
                // Solo 切替はノード集合が変わるため、部分更新ではなく作り直させる。
                instance->authoringDirtyNodeId =
                    m_session.document.lastPushedSoloNodeId != m_session.document.soloNodeId ? -1 : m_session.document.liveDirtyNodeId;
                m_session.lastPushedGraphRevision = revision;
                m_session.document.lastPushedSoloNodeId = m_session.document.soloNodeId;
            }
        } else if (instance->authoringGraph != nullptr) {
            // ライブ編集を切ったらディスクの .vfx を正とする状態へ戻す。
            instance->authoringGraph.reset();
            instance->reloadRequested = true;
            m_session.lastPushedGraphRevision = 0;
        }
        ImGui::TextDisabled("  %.2fs / %.2fs", instance->playTime, instance->graphDuration);
        ImGui::SameLine();
        if (ImGui::Button(instance->playing ? "Pause Preview" : "Play Preview")) {
            if (instance->playing) instance->Pause();
            else instance->Resume();
        }
        ImGui::SameLine();
        if (ImGui::Button("Restart Preview")) {
            instance->reloadRequested = true;
            instance->Restart();
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop Preview")) instance->Stop();
        ImGui::SameLine();
        ImGui::Checkbox("Loop", &instance->loop);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragFloat("Speed", &instance->speed, 0.01f, 0.0f, 8.0f);
        // 見え方の条件 (環境・密度・比較) は再生操作と並べる。
        // どちらも「作った物を確かめる」ための操作で、行き来が多いため。
        m_previewView.DrawEnvironmentControls();
        if (!ImGui::GetIO().WantTextInput && ImGui::Shortcut(ImGuiKey_Space)) {
            if (instance->playing) instance->Pause();
            else instance->Resume();
        }
        if (instance->graphDuration > 0.0f) {
            float scrubTime = instance->playTime;
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("Timeline", &scrubTime, 0.0f, instance->graphDuration, "%.3fs")) {
                instance->Pause();
                instance->editorScrubTime = scrubTime;
                instance->editorPreviewFrame = Time::frameCount;
            }
            if (ImGui::Button("Step -")) {
                instance->Pause();
                instance->editorScrubTime = (std::max)(instance->playTime - kScrubStep, 0.0f);
            }
            ImGui::SameLine();
            if (ImGui::Button("Step +")) {
                instance->Pause();
                instance->editorScrubTime = (std::min)(instance->playTime + kScrubStep,
                                                       instance->graphDuration);
            }
        }
        // 構造変更は次のループ先頭で入れ替わる。待たされていることを黙って隠さない。
        if (instance->pendingStructuralRebuild) {
            ImGui::SameLine();
            ImGui::TextColored({ 1.0f, 0.78f, 0.35f, 1.0f }, "REBUILD AT LOOP");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("ノード構成の変更は次のループ先頭でまとめて反映されます。\n"
                                  "(再生中に作り直すと絵が飛ぶため)");
            ImGui::SameLine();
            if (ImGui::SmallButton("Apply Now")) {
                instance->pendingStructuralRebuild = false;
                instance->reloadRequested = true;
            }
        }
        if (ctx.vfxPreviewScene != nullptr) {
            int activeNodes = 0;
            int liveParticles = 0;
            int visibleParticles = 0;
            for (const auto& runtimeNode : instance->runtimeNodes) {
                if (runtimeNode.active) ++activeNodes;
                if (!runtimeNode.entity.IsValid()) continue;
                const auto* emitter = ctx.vfxPreviewScene->GetComponent<scene::ParticleEmitter>(runtimeNode.entity);
                if (emitter == nullptr) continue;
                liveParticles += emitter->simulationMode == scene::ParticleSimulationMode::Gpu
                    ? emitter->visibleParticleCount : static_cast<int>(emitter->particles.size());
                visibleParticles += emitter->visibleParticleCount;
            }
            ImGui::TextDisabled("Runtime  active %d/%d   particles %d   visible %d",
                                activeNodes, static_cast<int>(instance->runtimeNodes.size()),
                                liveParticles, visibleParticles);
        }
        if (m_session.document.dirty && ImGui::IsItemHovered())
            ImGui::SetTooltip("Save the graph before reloading the preview instance");
    }
}

} // namespace fbzz::editor
