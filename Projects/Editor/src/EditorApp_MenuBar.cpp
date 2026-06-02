// FBZZ Engine
// EditorApp_MenuBar.cpp | fbzz::editor
// メインメニューバーの構築とホットキー登録
//
// WHY: メニューバーは ImGui の MenuItem 呼び出しが大量に並ぶ UI 記述コードであり、
//      ライフサイクル管理やシーン I/O とは関心が異なる。
//      独立ファイルに分離することで、メニュー項目の追加・変更を局所化できる。
#include <Editor/EditorApp.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/StandaloneLauncher.hpp>
#include <imgui.h>
#include <Windows.h>

namespace fbzz::editor {

namespace {

enum class PlayToolbarIcon {
    Play,
    Stop,
    Pause,
    Step
};

void DrawPlayToolbarIcon(PlayToolbarIcon icon, const ImVec2& min, const ImVec2& max, ImU32 color)
{
    // WHY: PlayMode のアイコンは短い記号テキストだと幅やフォントに左右され、エディターの工具感が弱くなる。
    // WHAT: ImGui の DrawList で単純な幾何形状を描き、フォント非依存の固定アイコンとして表示する。
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImDrawListFlags oldFlags = drawList->Flags;
    drawList->Flags |= ImDrawListFlags_AntiAliasedFill | ImDrawListFlags_AntiAliasedLines;

    auto drawTriangle = [drawList, color](const ImVec2& a, const ImVec2& b, const ImVec2& c) {
        // WHY: 塗りつぶし三角形だけだと斜辺のジャギーが目立つため、
        //      同色のアンチエイリアス線を重ねて輪郭をなじませる。
        drawList->AddTriangleFilled(a, b, c, color);
        drawList->AddTriangle(a, b, c, color, 1.35f);
    };

    const ImVec2 center { (min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f };

    switch (icon) {
    case PlayToolbarIcon::Play:
        drawTriangle(
            { center.x - 4.0f, center.y - 7.0f },
            { center.x - 4.0f, center.y + 7.0f },
            { center.x + 7.0f, center.y });
        break;
    case PlayToolbarIcon::Stop:
        drawList->AddRectFilled(
            { center.x - 5.5f, center.y - 5.5f },
            { center.x + 5.5f, center.y + 5.5f },
            color,
            1.5f);
        break;
    case PlayToolbarIcon::Pause:
        drawList->AddRectFilled(
            { center.x - 6.0f, center.y - 7.0f },
            { center.x - 2.0f, center.y + 7.0f },
            color,
            1.0f);
        drawList->AddRectFilled(
            { center.x + 2.0f, center.y - 7.0f },
            { center.x + 6.0f, center.y + 7.0f },
            color,
            1.0f);
        break;
    case PlayToolbarIcon::Step:
        drawTriangle(
            { center.x - 7.0f, center.y - 6.5f },
            { center.x - 7.0f, center.y + 6.5f },
            { center.x + 2.0f, center.y });
        drawList->AddRectFilled(
            { center.x + 5.0f, center.y - 7.0f },
            { center.x + 7.0f, center.y + 7.0f },
            color,
            1.0f);
        break;
    }

    drawList->Flags = oldFlags;
}

ImVec4 WithAlpha(ImVec4 color, float alpha)
{
    color.w = alpha;
    return color;
}

bool PlayToolbarButton(
    const char* id,
    const char* tooltip,
    PlayToolbarIcon icon,
    bool enabled,
    bool active,
    const ImVec4& activeColor,
    const ImVec2& size)
{
    // WHY: PlayMode 操作は Godot のように常に同じ位置へ置き、状態確認と操作を視線移動なしで行えるようにする。
    // WHAT: active 時だけ操作種別の色を背景へ乗せ、無効時は ImGui の Disabled スタイルで入力も止める。
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, activeColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(activeColor, 0.92f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, WithAlpha(activeColor, 1.0f));
    }

    if (!enabled)
        ImGui::BeginDisabled();

    const bool pressed = ImGui::Button(id, size);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImU32 iconColor = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    DrawPlayToolbarIcon(icon, min, max, iconColor);

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", tooltip);

    if (!enabled)
        ImGui::EndDisabled();

    if (active)
        ImGui::PopStyleColor(3);

    return enabled && pressed;
}

} // namespace

void EditorApp::BuildMenuBar(EditorContext& ctx)
{
    if (!ImGui::BeginMenuBar()) return;

    // --- File ------------------------------------------------------------
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Scene"))
            RequestNewScene();
        if (ImGui::MenuItem("Open...", "Ctrl+O", false, ctx.activeScene != nullptr))
            RequestOpenSceneFromDialog();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, ctx.activeScene != nullptr))
            SaveScene();
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, ctx.activeScene != nullptr))
            SaveSceneAsDialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) RequestExit();
        ImGui::EndMenu();
    }

    // --- Edit ------------------------------------------------------------
    if (ImGui::BeginMenu("Edit")) {
        const bool canUndo = ctx.undoStack && ctx.undoStack->CanUndo();
        const bool canRedo = ctx.undoStack && ctx.undoStack->CanRedo();
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo)) ctx.undoStack->Undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo)) ctx.undoStack->Redo();
        ImGui::EndMenu();
    }

    // --- View ------------------------------------------------------------
    if (ImGui::BeginMenu("View")) {
        if (ImGui::BeginMenu("Panels")) {
            for (auto& panel : m_panels) {
                if (panel->ShowInViewMenu())
                    ImGui::MenuItem(panel->GetViewMenuName(), nullptr, &panel->visible);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    // --- Debug -----------------------------------------------------------
    if (ImGui::BeginMenu("Debug")) {
        // WHY: Godot は表示パネル操作とデバッグ描画切替を別メニューに分けている。
        //      FBZZ でも View はレイアウト・パネル、Debug は実行/描画診断に寄せることで項目の意味を読み取りやすくする。
        if (ImGui::MenuItem("Analysis")) {
            ctx.requestOpenAnalysis = true;
        }
        ImGui::Separator();
        ImGui::MenuItem("Grid",         nullptr, &ctx.showGrid);
        ImGui::MenuItem("Light Range",  nullptr, &ctx.showLightRange);
        ImGui::MenuItem("Colliders",    nullptr, &ctx.projectSettings.render.showColliders);
        ImGui::MenuItem("Decal Bounds", nullptr, &ctx.projectSettings.render.showDecalBounds);
        ImGui::MenuItem("Skeleton",     nullptr, &ctx.showSkeleton);
        ImGui::MenuItem("Stats",        nullptr, &ctx.showStats);
        ImGui::MenuItem("Hot Reload",   nullptr, &ctx.hotReloadEnabled);
        ImGui::Separator();
        if (ImGui::BeginMenu("View Mode")) {
            auto& vm = ctx.projectSettings.render.viewMode;
            if (ImGui::MenuItem("Lit",            nullptr, vm == renderer::ViewMode::Lit))           vm = renderer::ViewMode::Lit;
            if (ImGui::MenuItem("Unlit",          nullptr, vm == renderer::ViewMode::Unlit))         vm = renderer::ViewMode::Unlit;
            if (ImGui::MenuItem("Wireframe Lit",  nullptr, vm == renderer::ViewMode::WireframeLit))  vm = renderer::ViewMode::WireframeLit;
            if (ImGui::MenuItem("Wireframe Unlit",nullptr, vm == renderer::ViewMode::WireframeUnlit))vm = renderer::ViewMode::WireframeUnlit;
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Post Process")) {
            ImGui::MenuItem("Shadow",              nullptr, &ctx.projectSettings.render.shadowEnabled);
            auto& pp = ctx.projectSettings.render.postProcess;
            ImGui::MenuItem("Bloom",               nullptr, &pp.bloom.enabled);
            ImGui::MenuItem("Fog",                 nullptr, &pp.fog.enabled);
            ImGui::MenuItem("FXAA",                nullptr, &pp.fxaaEnabled);
            ImGui::MenuItem("Color Grading",       nullptr, &pp.colorGrading.enabled);
            ImGui::MenuItem("Vignette",            nullptr, &pp.vignette.enabled);
            ImGui::MenuItem("Film Grain",          nullptr, &pp.filmGrain.enabled);
            ImGui::MenuItem("Sharpen",             nullptr, &pp.sharpen.enabled);
            ImGui::MenuItem("Depth of Field",      nullptr, &pp.depthOfField.enabled);
            ImGui::MenuItem("Chromatic Aberration",nullptr, &pp.lens.chromaticAberrationEnabled);
            ImGui::MenuItem("Lens Distortion",     nullptr, &pp.lens.distortionEnabled);
            ImGui::MenuItem("Sepia",               nullptr, &pp.stylized.sepiaEnabled);
            ImGui::MenuItem("Invert",              nullptr, &pp.stylized.invertEnabled);
            ImGui::MenuItem("Posterize",           nullptr, &pp.stylized.posterizeEnabled);
            ImGui::MenuItem("Pixelate",            nullptr, &pp.stylized.pixelateEnabled);
            ImGui::MenuItem("Ambient Occlusion",   nullptr, &pp.ambientOcclusion.enabled);
            ImGui::Separator();
            ImGui::SliderFloat("Exposure",        &pp.exposure,                0.1f, 4.0f);
            ImGui::SliderFloat("Bloom Intensity", &pp.bloom.intensity,         0.0f, 3.0f);
            ImGui::SliderFloat("Contrast",        &pp.colorGrading.contrast,  -1.0f, 1.0f);
            ImGui::SliderFloat("Saturation",      &pp.colorGrading.saturation, 0.0f, 2.0f);
            ImGui::SliderFloat("Hue Shift",       &pp.colorGrading.hueShift, -180.0f, 180.0f);
            ImGui::SliderFloat("Sharpen Strength", &pp.sharpen.strength,        0.0f, 2.0f);
            ImGui::SliderFloat("DOF Focus",        &pp.depthOfField.focusDistance, 0.1f, 100.0f);
            ImGui::SliderFloat("DOF Blur",         &pp.depthOfField.blurRadius, 0.0f, 12.0f);
            ImGui::SliderFloat("Posterize Levels", &pp.stylized.posterizeLevels, 2.0f, 32.0f);
            ImGui::SliderFloat("Pixel Size",       &pp.stylized.pixelSize,      1.0f, 32.0f);
            ImGui::SliderFloat("Fog Density",     &pp.fog.density,             0.0f, 1.0f);
            ImGui::SliderFloat("Fog Far",         &pp.fog.farDistance,         1.0f, 100.0f);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Tools")) {
        ImGui::MenuItem("Terrain Tool", nullptr, &ctx.showTerrainTool);
        ImGui::Separator();

        // WHY: Standalone ボタンは Play と独立した位置に置き、
        //      「配布版と同じ状態を手軽に確認できる」という意図を明示する。
        const bool hasProject = !ctx.projectRoot.empty();
        if (ImGui::MenuItem("Standalone", nullptr, false, hasProject)) {
            // GetModuleFileNameW で自身のパスを取得して子プロセスとして起動する
            wchar_t exePathBuf[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
            std::wstring exePathW(exePathBuf);
            const int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0,
                exePathW.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string exePath(static_cast<size_t>(sizeNeeded - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, exePathW.c_str(), -1,
                exePath.data(), sizeNeeded, nullptr, nullptr);

            StandaloneLauncher::Launch(exePath, ctx.projectRoot);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Launch game without editor UI");
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Build Settings...", "Ctrl+Shift+B")) {
            ctx.requestOpenBuildSettings = true;
        }
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

void EditorApp::BuildPlayToolbar(EditorContext& ctx)
{
    // WHY: Godot は Play 系操作を上部中央へ独立配置しており、メニュー項目より実行状態が読み取りやすい。
    //      FBZZ でも DockSpace の直上に固定ツールバーを置くことで、各パネルのドッキングを崩さず同じ操作感に寄せる。
    constexpr float TOOLBAR_HEIGHT = 34.0f;
    const ImVec2 BUTTON_SIZE { 34.0f, 24.0f };
    constexpr float BUTTON_SPACING = 4.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 8.0f, 5.0f });
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { BUTTON_SPACING, 0.0f });
    ImGui::BeginChild("##MainPlayToolbar", { 0.0f, TOOLBAR_HEIGHT }, false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const float groupWidth = BUTTON_SIZE.x * 4.0f + BUTTON_SPACING * 3.0f;
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float centerOffset = (availableWidth > groupWidth) ? (availableWidth - groupWidth) * 0.5f : 0.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + centerOffset);

    PlayModeController* pm = ctx.playMode;
    const bool hasScene = ctx.activeScene != nullptr;
    const bool isEditor = pm && pm->IsInEditor();
    const bool isPlaying = pm && pm->IsPlaying();
    const bool isPaused = pm && pm->IsPaused();

    const ImVec4 playColor  { 0.18f, 0.58f, 0.33f, 1.0f };
    const ImVec4 stopColor  { 0.62f, 0.20f, 0.20f, 1.0f };
    const ImVec4 pauseColor { 0.72f, 0.52f, 0.18f, 1.0f };

    if (PlayToolbarButton(
        "##PlayModePlay",
        isPaused ? "Resume from Play Mode" : "Play",
        PlayToolbarIcon::Play,
        pm && hasScene && isEditor,
        isPlaying,
        playColor,
        BUTTON_SIZE)) {
        pm->Play(*ctx.activeScene);
        if (pm->IsPlaying())
            ctx.requestGameViewportFocus = true;
    }

    ImGui::SameLine();
    if (PlayToolbarButton(
        "##PlayModeStop",
        "Stop",
        PlayToolbarIcon::Stop,
        pm && hasScene && !isEditor,
        isEditor,
        stopColor,
        BUTTON_SIZE))
        pm->Stop(*ctx.activeScene);

    ImGui::SameLine();
    if (PlayToolbarButton(
        "##PlayModePause",
        isPaused ? "Resume" : "Pause",
        PlayToolbarIcon::Pause,
        pm && !isEditor,
        isPaused,
        pauseColor,
        BUTTON_SIZE))
        pm->Pause();

    ImGui::SameLine();
    if (PlayToolbarButton(
        "##PlayModeStep",
        "Step",
        PlayToolbarIcon::Step,
        pm && isPaused,
        false,
        pauseColor,
        BUTTON_SIZE))
        pm->RequestStep();

    // WHY: ボタンの色変化だけでは Play / Pause / Editor の状態が曖昧。
    //      Unity 同様、ツールバー右端に現在の状態をテキストで表示することで
    //      視線を動かさず一目で把握できるようにする。
    if (!isEditor) {
        const char* label     = isPlaying ? "PLAYING" : "PAUSED";
        const ImVec4 labelCol = isPlaying
            ? ImVec4{ 0.28f, 0.88f, 0.53f, 1.0f } // 緑: Play
            : ImVec4{ 0.92f, 0.72f, 0.28f, 1.0f }; // 黄: Pause
        const float textWidth = ImGui::CalcTextSize(label).x;
        const float posX = availableWidth - textWidth - 12.0f;
        const float posY = (TOOLBAR_HEIGHT - ImGui::GetTextLineHeight()) * 0.5f;
        if (posX > 0.0f) {
            ImGui::SetCursorPos({ posX, posY });
            ImGui::TextColored(labelCol, "%s", label);
        }
    }

    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 max = { min.x + ImGui::GetWindowWidth(), min.y + ImGui::GetWindowHeight() };
    ImGui::GetWindowDrawList()->AddLine({ min.x, max.y - 1.0f }, { max.x, max.y - 1.0f },
        ImGui::GetColorU32(ImGuiCol_Separator));

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}

// =============================================================================
// ホットキー登録
// =============================================================================

void EditorApp::RegisterDefaultHotkeys()
{
    m_hotkeys.Register({ "Undo",           ImGuiKey_Z, true,  false, false,
        [this]() { m_undoStack.Undo(); } });
    m_hotkeys.Register({ "Redo",           ImGuiKey_Y, true,  false, false,
        [this]() { m_undoStack.Redo(); } });
    m_hotkeys.Register({ "Open Scene",     ImGuiKey_O, true,  false, false,
        [this]() { RequestOpenSceneFromDialog(); } });
    m_hotkeys.Register({ "Save Scene",     ImGuiKey_S, true,  false, false,
        [this]() { SaveScene(); } });
    m_hotkeys.Register({ "Save Scene As",  ImGuiKey_S, true,  true,  false,
        [this]() { SaveSceneAsDialog(); } });
}

} // namespace fbzz::editor
