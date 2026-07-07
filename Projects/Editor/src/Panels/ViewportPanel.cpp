// FBZZ Engine
// ViewportPanel.cpp | fbzz::editor
// Scene / Game / UI Viewport のレイアウトと入力ルーティング
#include "Viewport/ViewportCommon.hpp"
#include <Editor/Util/SceneEditUtils.hpp>

namespace fbzz::editor {

float GetGameViewportAspectRatio(EditorContext::GameViewportAspect aspect)
{
    switch (aspect) {
    case EditorContext::GameViewportAspect::Ratio16x9:  return 16.0f / 9.0f;
    case EditorContext::GameViewportAspect::Ratio4x3:   return 4.0f / 3.0f;
    case EditorContext::GameViewportAspect::Ratio1x1:   return 1.0f;
    case EditorContext::GameViewportAspect::Ratio9x16:  return 9.0f / 16.0f;
    case EditorContext::GameViewportAspect::HD:          return 1280.0f / 720.0f;
    case EditorContext::GameViewportAspect::FullHD:      return 1920.0f / 1080.0f;
    case EditorContext::GameViewportAspect::QHD:         return 2560.0f / 1440.0f;
    case EditorContext::GameViewportAspect::UHD4K:       return 3840.0f / 2160.0f;
    case EditorContext::GameViewportAspect::WXGA:        return 1280.0f / 800.0f;
    case EditorContext::GameViewportAspect::WUXGA:       return 1920.0f / 1200.0f;
    case EditorContext::GameViewportAspect::iPhonePortrait:  return 1080.0f / 1920.0f;
    case EditorContext::GameViewportAspect::iPhoneLandscape: return 1920.0f / 1080.0f;
    case EditorContext::GameViewportAspect::Free:
    default:                                            return 0.0f;
    }
}

const char* GetGameViewportAspectLabel(EditorContext::GameViewportAspect aspect)
{
    switch (aspect) {
    case EditorContext::GameViewportAspect::Ratio16x9:  return "16:9";
    case EditorContext::GameViewportAspect::Ratio4x3:   return "4:3";
    case EditorContext::GameViewportAspect::Ratio1x1:   return "1:1";
    case EditorContext::GameViewportAspect::Ratio9x16:  return "9:16";
    case EditorContext::GameViewportAspect::HD:          return "HD (1280x720)";
    case EditorContext::GameViewportAspect::FullHD:      return "Full HD (1920x1080)";
    case EditorContext::GameViewportAspect::QHD:         return "QHD (2560x1440)";
    case EditorContext::GameViewportAspect::UHD4K:       return "4K UHD (3840x2160)";
    case EditorContext::GameViewportAspect::WXGA:        return "WXGA (1280x800)";
    case EditorContext::GameViewportAspect::WUXGA:       return "WUXGA (1920x1200)";
    case EditorContext::GameViewportAspect::iPhonePortrait:  return "iPhone Portrait";
    case EditorContext::GameViewportAspect::iPhoneLandscape: return "iPhone Landscape";
    case EditorContext::GameViewportAspect::Free:
    default:                                            return "Free";
    }
}

void DrawGameViewportAspectControl(EditorContext& ctx)
{
    if (ImGui::BeginCombo("##game_aspect", GetGameViewportAspectLabel(ctx.gameViewportAspect))) {
        constexpr EditorContext::GameViewportAspect kAspects[] = {
            EditorContext::GameViewportAspect::Free,
            EditorContext::GameViewportAspect::Ratio16x9,
            EditorContext::GameViewportAspect::Ratio4x3,
            EditorContext::GameViewportAspect::Ratio1x1,
            EditorContext::GameViewportAspect::Ratio9x16,
            EditorContext::GameViewportAspect::HD,
            EditorContext::GameViewportAspect::FullHD,
            EditorContext::GameViewportAspect::QHD,
            EditorContext::GameViewportAspect::UHD4K,
            EditorContext::GameViewportAspect::WXGA,
            EditorContext::GameViewportAspect::WUXGA,
            EditorContext::GameViewportAspect::iPhonePortrait,
            EditorContext::GameViewportAspect::iPhoneLandscape
        };
        for (EditorContext::GameViewportAspect aspect : kAspects) {
            const bool selected = ctx.gameViewportAspect == aspect;
            if (ImGui::Selectable(GetGameViewportAspectLabel(aspect), selected))
                ctx.gameViewportAspect = aspect;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

const char* GetPlayFocusModeLabel(EditorContext::PlayFocusMode mode)
{
    switch (mode) {
    case EditorContext::PlayFocusMode::Focused:   return "Play Focused";
    case EditorContext::PlayFocusMode::Maximized: return "Play Maximized";
    case EditorContext::PlayFocusMode::Unfocused: return "Play Unfocused";
    default:                                      return "Play Maximized";
    }
}

void DrawGameViewportToolbar(EditorContext& ctx)
{
    // WHY: Game View は Play 確認の中心なので、フォーカス操作を Viewport 直上へ置く。
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 4.0f, 0.0f });

    ImGui::SetNextItemWidth(96.0f);
    DrawGameViewportAspectControl(ctx);

    ImGui::SameLine();
    if (ImGui::SmallButton("Focus")) {
        ctx.requestGameViewportFocus = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Focus Game View");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(138.0f);
    if (ImGui::BeginCombo("##play_focus_mode", GetPlayFocusModeLabel(ctx.playFocusMode))) {
        constexpr EditorContext::PlayFocusMode kModes[] = {
            EditorContext::PlayFocusMode::Focused,
            EditorContext::PlayFocusMode::Maximized,
            EditorContext::PlayFocusMode::Unfocused
        };
        for (EditorContext::PlayFocusMode mode : kModes) {
            const bool selected = ctx.playFocusMode == mode;
            if (ImGui::Selectable(GetPlayFocusModeLabel(mode), selected))
                ctx.playFocusMode = mode;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Controls Game View behavior when Play starts");

    ImGui::PopStyleVar(2);
}

ImVec2 FitSizeToAspect(ImVec2 size, float aspect)
{
    if (aspect <= 0.0f) return size;
    const float availableAspect = size.x / size.y;
    if (availableAspect > aspect)
        size.x = size.y * aspect;
    else
        size.y = size.x / aspect;
    return size;
}

void DrawViewModeToolbar(EditorContext& ctx, const ImVec2& viewportMin)
{
    struct ModeEntry {
        const char*        label;
        const char*        tooltip;
        renderer::ViewMode mode;
    };
    static constexpr ModeEntry kModes[] = {
        { "Lit",    "Lit \xe2\x80\x94 full lighting",                    renderer::ViewMode::Lit            },
        { "Unlit",  "Unlit \xe2\x80\x94 no lighting",                   renderer::ViewMode::Unlit          },
        { "Wf Lit", "Wireframe Lit \xe2\x80\x94 wireframe + lighting",  renderer::ViewMode::WireframeLit   },
        { "Wf",     "Wireframe Unlit \xe2\x80\x94 wireframe only",      renderer::ViewMode::WireframeUnlit },
    };

    renderer::ViewMode& current = ctx.projectSettings.render.viewMode;

    ImGui::SetCursorScreenPos({ viewportMin.x + 6.0f, viewportMin.y + 6.0f });
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 3.0f, 0.0f });

    for (const auto& entry : kModes) {
        const bool active = current == entry.mode;
        ImGui::PushStyleColor(ImGuiCol_Button,
            active ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
                   : ImVec4(0.15f, 0.15f, 0.15f, 0.75f));

        if (ImGui::SmallButton(entry.label))
            current = entry.mode;

        ImGui::PopStyleColor();

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", entry.tooltip);

        ImGui::SameLine();
    }

    // Overlays ▼ ドロップダウン
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 0.75f));
    if (ImGui::SmallButton("Overlays \xe2\x96\xbc"))
        ImGui::OpenPopup("##overlays_popup");
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Toggle scene overlay visibility");

    if (ImGui::BeginPopup("##overlays_popup")) {
        ImGui::Checkbox("Grid",        &ctx.showGrid);
        ImGui::Checkbox("Light Range", &ctx.showLightRange);
        ImGui::Checkbox("Colliders",   &ctx.projectSettings.render.showColliders);
        ImGui::Checkbox("NavMesh",     &ctx.projectSettings.render.showNavMesh);
        ImGui::Checkbox("AI Sensors",  &ctx.projectSettings.render.showNavSensors);
        ImGui::Checkbox("Skeleton",    &ctx.showSkeleton);
        ImGui::Checkbox("Stats",       &ctx.showStats);
        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(2);
}


ViewportPanel::ViewportPanel(Kind kind)
    : m_kind(kind)
    , m_windowName(kind == Kind::Scene ? "Scene" : (kind == Kind::Game ? "Game" : "UI"))
{
}

void ViewportPanel::OnBeforeBegin(EditorContext& ctx)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.0f, 0.0f });
    if (m_kind == Kind::Game && ctx.requestGameViewportFocus)
        ImGui::SetNextWindowFocus();
}

void ViewportPanel::OnAfterBegin(EditorContext& ctx)
{
    if (m_kind == Kind::Game)
        ctx.requestGameViewportFocus = false;
    ImGui::PopStyleVar();
}

void ViewportPanel::OnRenderContent(EditorContext& ctx)
{
    const bool isSceneView = m_kind == Kind::Scene;
    const bool isGameView = m_kind == Kind::Game;
    const bool isUIView = m_kind == Kind::UI;
    if (isGameView) {
        DrawGameViewportToolbar(ctx);
    }

    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 1.0f) size.x = 1.0f;
    if (size.y < 1.0f) size.y = 1.0f;

    if (isGameView || isUIView) {
        // WHY: UI Viewport は Game View の完成済み RT を共有するため、表示枠も Game View と同じ比率にする。
        //      Canvas 比率で引き伸ばすと、背景とクリック座標が Game 出力からずれてしまう。
        const float viewportAspect = GetGameViewportAspectRatio(ctx.gameViewportAspect);
        size = FitSizeToAspect(size, viewportAspect);
        if (size.x < 1.0f) size.x = 1.0f;
        if (size.y < 1.0f) size.y = 1.0f;
    }

    if (isSceneView) {
        ctx.viewportWidth = size.x;
        ctx.viewportHeight = size.y;
        ctx.viewportFocused = ImGui::IsWindowFocused();
        ctx.sceneViewportHovered = ImGui::IsWindowHovered();
    } else if (isGameView) {
        ctx.gameViewportWidth = size.x;
        ctx.gameViewportHeight = size.y;
        ctx.gameViewportFocused = ImGui::IsWindowFocused();
    } else if (isUIView) {
        ctx.uiViewportWidth = size.x;
        ctx.uiViewportHeight = size.y;
        ctx.uiViewportFocused = ImGui::IsWindowFocused();
    }

    ImVec2 viewportMin = ImGui::GetCursorScreenPos();
    if (isGameView || isUIView) {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        viewportMin.x += (std::max)(0.0f, (available.x - size.x) * 0.5f);
        viewportMin.y += (std::max)(0.0f, (available.y - size.y) * 0.5f);
        if (isGameView) {
            ctx.gameViewportOriginX = viewportMin.x;
            ctx.gameViewportOriginY = viewportMin.y;
        } else {
            ctx.uiViewportOriginX = viewportMin.x;
            ctx.uiViewportOriginY = viewportMin.y;
        }
    }
    ImVec2 viewportMax = { viewportMin.x + size.x, viewportMin.y + size.y };
    bool viewportHovered = ImGui::IsMouseHoveringRect(viewportMin, viewportMax);
    if (hdrRT.IsValid() && ctx.imguiRenderer && resources) {
        ImTextureID texID = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(ctx.imguiRenderer->GetImTextureID(hdrRT, *resources, 0)));
        ImGui::GetWindowDrawList()->AddImage(texID, viewportMin, viewportMax);
    } else {
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            cursor, { cursor.x + size.x, cursor.y + size.y }, IM_COL32(30, 30, 30, 255));
        ImGui::SetCursorScreenPos({ cursor.x + size.x * 0.5f - 60.0f, cursor.y + size.y * 0.5f - 7.0f });
        ImGui::TextDisabled("No Render Target");
    }

    const bool inPlayOrPause = ctx.playMode && !ctx.playMode->IsInEditor();

    if (isUIView && !inPlayOrPause)
        DrawCanvasEditorGuides(ctx, viewportMin, size);

    if (isSceneView && !inPlayOrPause) {
        const ImGuiID viewportDropId = ImGui::GetID("##scene_view_prefab_drop_target");
        if (ImGui::BeginDragDropTargetCustom(ImRect(viewportMin, viewportMax), viewportDropId)) {
            std::string assetPath;
            if (ReadAssetPayload(ImGui::AcceptDragDropPayload("ASSET_PATH"), assetPath) &&
                InstantiateAssetAtViewport(ctx, assetPath, viewportMin)) {
                if (ctx.markSceneDirty) ctx.markSceneDirty();
            }
            ImGui::EndDragDropTarget();
        }
    }

    const bool gizmoWantsMouse = ImGuizmo::IsUsing() || ImGuizmo::IsOver()
                              || ImGuizmo::IsUsingViewManipulate() || ImGuizmo::IsViewManipulateHovered();
    const bool anyToolActive =
        (ctx.terrainTool && ctx.terrainTool->IsActive()) ||
        (ctx.waterTool   && ctx.waterTool->IsActive())   ||
        (ctx.detailTool  && ctx.detailTool->IsActive())  ||
        (ctx.foliageTool && ctx.foliageTool->IsActive());
    // Alt+左ドラッグはカメラオービットに割り当てられているため、選択操作から除外する
    const bool altHeld = ImGui::GetIO().KeyAlt;
    if (isSceneView && !inPlayOrPause && viewportHovered && !altHeld &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !gizmoWantsMouse &&
        !m_prevOverlayHovered)
        PickEntity(ctx, viewportMin);

    // --- 矩形 (ドラッグ) 選択 ---
    // WHY: 複数オブジェクトをまとめて動かす作業は Unity の箱選択が前提。
    //      クリック位置からしきい値以上ドラッグしたら矩形選択モードへ移行し、
    //      離した時点で矩形内の GO を選択する (クリック選択の結果は上書きされる)。
    if (isSceneView && !inPlayOrPause) {
        constexpr float kDragThreshold = 5.0f;  // px: クリックと区別するしきい値

        if (viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !gizmoWantsMouse && !anyToolActive && !altHeld && !m_prevOverlayHovered) {
            m_rectSelecting = true;
            m_rectStart = ImGui::GetMousePos();
        }

        if (m_rectSelecting && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 cur = ImGui::GetMousePos();
            const float dx = cur.x - m_rectStart.x;
            const float dy = cur.y - m_rectStart.y;
            if (dx * dx + dy * dy > kDragThreshold * kDragThreshold) {
                const ImVec2 rMin = { (std::min)(m_rectStart.x, cur.x), (std::min)(m_rectStart.y, cur.y) };
                const ImVec2 rMax = { (std::max)(m_rectStart.x, cur.x), (std::max)(m_rectStart.y, cur.y) };
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(rMin, rMax, IM_COL32(100, 180, 255, 30));
                dl->AddRect(rMin, rMax, IM_COL32(100, 180, 255, 200), 0.0f, 0, 1.5f);
            }
        }

        if (m_rectSelecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            const ImVec2 cur = ImGui::GetMousePos();
            const float dx = cur.x - m_rectStart.x;
            const float dy = cur.y - m_rectStart.y;
            if (dx * dx + dy * dy > kDragThreshold * kDragThreshold)
                RectSelectEntities(ctx, viewportMin, size, m_rectStart, cur);
            m_rectSelecting = false;
        }

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            m_rectSelecting = false;
    }

    if (isSceneView && !inPlayOrPause)
        DrawViewModeToolbar(ctx, viewportMin);

    if (isSceneView && !inPlayOrPause) {
        DrawSceneIcons(ctx, viewportMin, size);
        DrawGizmo(ctx, viewportMin, size, m_lastGizmoOp, m_lastGizmoMode, m_prevGizmoOver, m_prevGizmoUsing);
        DrawOrientationGizmo(ctx, viewportMin, size);

        // ── Snap インジケーター (ツールバー右隣、ON 時のみ) ─────────────────
        if (ctx.snapEnabled) {
            char snapBuf[32];
            if (ctx.gizmoMode == EditorContext::GizmoMode::Rotate)
                std::snprintf(snapBuf, sizeof(snapBuf), " ROT %.1f\xc2\xb0 ", ctx.snapRot);
            else if (ctx.gizmoMode == EditorContext::GizmoMode::Scale)
                std::snprintf(snapBuf, sizeof(snapBuf), " SCL %.2f ", ctx.snapScale);
            else
                std::snprintf(snapBuf, sizeof(snapBuf), " POS %.2f ", ctx.snapPos);
            const ImVec2 tsz = ImGui::CalcTextSize(snapBuf);
            const ImVec2 p   = { viewportMin.x + 6.0f, viewportMin.y + 6.0f +
                                  ImGui::GetFrameHeight() + 4.0f };
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled({ p.x - 2, p.y - 1 },
                              { p.x + tsz.x + 2, p.y + tsz.y + 1 },
                              IM_COL32(20, 80, 120, 200), 3.0f);
            dl->AddText(p, IM_COL32(100, 220, 255, 255), snapBuf);
        }

        // ── カメラブックマーク HUD (オリエンテーションギズモ下) ─────────────
        bool bookmarkRowHovered = false;
        {
            constexpr float kSlotSz  = 18.0f;
            constexpr float kSlotGap = 2.0f;
            constexpr float kGizmoBottom = 8.0f + 120.0f + 6.0f; // margin + gizmo + gap
            const float rowW = 9.0f * (kSlotSz + kSlotGap) - kSlotGap;
            const ImVec2 rowStart = {
                viewportMin.x + size.x - rowW - 8.0f,
                viewportMin.y + kGizmoBottom
            };
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const bool shiftHeld = ImGui::IsKeyDown(ImGuiKey_LeftShift)
                                || ImGui::IsKeyDown(ImGuiKey_RightShift);
            for (int i = 0; i < 9; ++i) {
                const bool valid = ctx.cameraBookmarks[i].valid;
                const ImVec2 p = { rowStart.x + i * (kSlotSz + kSlotGap), rowStart.y };
                const ImVec2 pMax = { p.x + kSlotSz, p.y + kSlotSz };

                // WHY: 以前は DrawList の飾りだけでクリックできず、
                //      「押せそうで押せない」UI になっていた。マウスでも保存/呼び出しできるようにする。
                const bool hovered = ImGui::IsMouseHoveringRect(p, pMax);
                bookmarkRowHovered |= hovered;
                if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    auto& bm = ctx.cameraBookmarks[i];
                    if (shiftHeld && ctx.editorCamera) {
                        bm.position = ctx.editorCamera->m_position;
                        bm.rotation = ctx.editorCamera->m_rotation;
                        bm.valid    = true;
                    } else if (bm.valid) {
                        ctx.requestTeleportCamera = true;
                        ctx.teleportPosition      = bm.position;
                        ctx.teleportRotation      = bm.rotation;
                    }
                }
                if (hovered)
                    ImGui::SetTooltip(shiftHeld
                        ? "Save camera bookmark %d (Shift+%d)"
                        : (valid ? "Go to camera bookmark %d (key %d)"
                                 : "Empty slot %d \xe2\x80\x94 Shift+click or Shift+%d to save"),
                        i + 1, i + 1);

                const ImU32 bg  = valid
                    ? (shiftHeld ? IM_COL32(200, 120, 30,  200) : IM_COL32(60, 160, 60, 200))
                    : (shiftHeld ? IM_COL32(120, 60,  10,  140) : IM_COL32(25, 25,  25, 140));
                const ImU32 txt = valid ? IM_COL32(220, 255, 220, 255) : IM_COL32(120, 120, 120, 200);
                dl->AddRectFilled(p, pMax, bg, 3.0f);
                dl->AddRect(p, pMax,
                            hovered ? IM_COL32(200, 200, 200, 220) : IM_COL32(80, 80, 80, 160), 3.0f);
                char label[2] = { static_cast<char>('1' + i), '\0' };
                const ImVec2 tsz = ImGui::CalcTextSize(label);
                dl->AddText({ p.x + (kSlotSz - tsz.x) * 0.5f,
                               p.y + (kSlotSz - tsz.y) * 0.5f }, txt, label);
            }
        }

        // F: focus the editor camera on the selected object when the Scene viewport has keyboard focus.
        // WHY: マルチ選択時は全選択を包含するバウンディング球を注視し、
        //      対象の大きさに応じてカメラ距離が決まる (Unity の Frame Selected 互換)。
        if (ctx.viewportFocused && !ImGui::GetIO().WantTextInput &&
            input::Input::KeyDown(input::KeyCode::F)) {
            math::Vector3 center{};
            float radius = 0.0f;
            if (ComputeSelectionBounds(ctx, center, radius)) {
                ctx.focusTargetPosition    = center;
                ctx.focusTargetRadius      = radius;
                ctx.requestFocusOnSelected = true;
            }
        }

        // WHY: オーバーレイ UI (表示モードボタン等の ImGui アイテム、ブックマークスロット) を
        //      クリックした瞬間に背後の 3D ピッキングが同時に走ると選択が意図せず変わる。
        //      このフレームのホバー状態を記録し、次フレームの PickEntity / 矩形選択開始を抑制する。
        m_prevOverlayHovered = bookmarkRowHovered || ImGui::IsAnyItemHovered();

        // Delete / Ctrl+D: Scene View フォーカス中でも Hierarchy と同じ編集操作を受け付ける。
        // WHY: Unity では Scene View で選択したまま Delete / Ctrl+D が効く。
        //      Hierarchy へフォーカスを移さないと消せないのは操作動線として遠回り。
        if (ctx.viewportFocused && !ImGui::GetIO().WantTextInput &&
            !ctx.selectedEntities.empty())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
                DeleteSelectedWithUndo(ctx);
            else if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
                DuplicateSelectedWithUndo(ctx);
            else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                ctx.selectedEntities.clear();  // Esc: 選択解除 (Unity 互換)
        }

        // Ctrl+A: シーン内の全 GO を選択 (ロック中は除外)
        if (ctx.viewportFocused && !ImGui::GetIO().WantTextInput && ctx.activeScene &&
            ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
            ctx.selectedEntities.clear();
            for (auto& go : ctx.activeScene->GameObjects())
                if (!ctx.IsLocked(go.GetID()))
                    ctx.selectedEntities.push_back(go.GetID());
        }

        // Camera bookmarks: Shift+1~9 to save, 1~9 to recall.
        if (ctx.viewportFocused && ctx.editorCamera) {
            static const ImGuiKey kNumKeys[9] = {
                ImGuiKey_1, ImGuiKey_2, ImGuiKey_3,
                ImGuiKey_4, ImGuiKey_5, ImGuiKey_6,
                ImGuiKey_7, ImGuiKey_8, ImGuiKey_9
            };
            const bool shiftHeld = ImGui::IsKeyDown(ImGuiKey_LeftShift)
                                || ImGui::IsKeyDown(ImGuiKey_RightShift);
            for (int i = 0; i < 9; ++i) {
                if (!ImGui::IsKeyPressed(kNumKeys[i])) continue;
                auto& bm = ctx.cameraBookmarks[i];
                if (shiftHeld) {
                    bm.position = ctx.editorCamera->m_position;
                    bm.rotation = ctx.editorCamera->m_rotation;
                    bm.valid    = true;
                } else if (bm.valid) {
                    ctx.requestTeleportCamera = true;
                    ctx.teleportPosition      = bm.position;
                    ctx.teleportRotation      = bm.rotation;
                }
            }
        }
    }
    bool uiGizmoActive = false;
    if (isUIView && !inPlayOrPause)
        uiGizmoActive = DrawUIGizmo(ctx, viewportMin, size, m_uiGizmoDrag, m_uiGizmoDragStart, m_uiGizmoStartX, m_uiGizmoStartY, m_uiGizmoStartWidth, m_uiGizmoStartHeight, m_uiGizmoStartAngle, m_uiGizmoStartZ);
    if (isUIView && !inPlayOrPause && viewportHovered
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !uiGizmoActive)
        PickUIEntity(ctx, viewportMin, size);

    // 矢印キーで選択 UI 要素を微移動する（ビューポートにキーボードフォーカスがあるときのみ）。
    if (isUIView && !inPlayOrPause)
        HandleUINudge(ctx, ImGui::IsWindowFocused());

    // ── TerrainTool: Sculpt / Paint 操作 ───────────────────────────────────
    if (isSceneView && ctx.terrainTool && ctx.activeScene
        && !(ctx.playMode && !ctx.playMode->IsInEditor()))
    {
        const bool vpHovered = ImGui::IsWindowHovered()
            && !(ctx.detailTool && ctx.detailTool->IsActive())
            && !(ctx.foliageTool && ctx.foliageTool->IsActive());
        ctx.terrainTool->Update(
            *ctx.activeScene,
            *ctx.editorCamera,
            ImGui::GetIO().DeltaTime,
            vpHovered,
            viewportMin,
            size,
            ctx.markSceneDirty,
            ctx.undoStack);
        if (ctx.showTerrainTool && !ctx.mapEditingMode)
            ctx.terrainTool->OnEditorGUI(*ctx.activeScene, ctx.undoStack, ctx.markSceneDirty);
    }

    // ── WaterTool: 水面の範囲・波向き可視化とツールウィンドウ ──────────────
    if (isSceneView && ctx.waterTool && ctx.activeScene
        && !(ctx.playMode && !ctx.playMode->IsInEditor()))
    {
        ctx.waterTool->Update(
            *ctx.activeScene,
            *ctx.editorCamera,
            viewportMin,
            size,
            ctx.markSceneDirty);
        if (ctx.showWaterTool && !ctx.mapEditingMode)
            ctx.waterTool->OnEditorGUI(
                *ctx.activeScene,
                ctx.projectRoot,
                ctx.markSceneDirty,
                ctx.undoStack);
    }

    // ── DetailTool: 密度マップペイント + チャンク可視化 ────────────────────
    if (isSceneView && ctx.detailTool && ctx.activeScene
        && !(ctx.playMode && !ctx.playMode->IsInEditor()))
    {
        const bool vpHovered = ImGui::IsWindowHovered()
            && !(ctx.foliageTool && ctx.foliageTool->IsActive());
        ctx.detailTool->Update(
            *ctx.activeScene,
            *ctx.editorCamera,
            vpHovered,
            viewportMin,
            size,
            ctx.markSceneDirty);
        if (ctx.showDetailTool && !ctx.mapEditingMode)
            ctx.detailTool->OnEditorGUI(*ctx.activeScene, ctx.markSceneDirty);
    }

    if (isSceneView && ctx.foliageTool && ctx.activeScene
        && !(ctx.playMode && !ctx.playMode->IsInEditor()))
    {
        const bool vpHovered = ImGui::IsWindowHovered();
        ctx.foliageTool->Update(
            *ctx.activeScene,
            *ctx.editorCamera,
            vpHovered,
            viewportMin,
            size,
            ctx.markSceneDirty,
            ctx.undoStack);
        if (ctx.showFoliageTool && !ctx.mapEditingMode)
            ctx.foliageTool->OnEditorGUI(*ctx.activeScene, ctx.markSceneDirty);
    }

    // Show play/pause state with a viewport border.
    if (isGameView && ctx.playMode && ctx.playMode->IsPlaying())
        ImGui::GetWindowDrawList()->AddRect(viewportMin, viewportMax, IM_COL32(80, 200, 80, 220), 0.0f, 0, 3.0f);
    else if (isGameView && ctx.playMode && ctx.playMode->IsPaused())
        ImGui::GetWindowDrawList()->AddRect(viewportMin, viewportMax, IM_COL32(255, 180, 50, 220), 0.0f, 0, 3.0f);

    if (isGameView && ctx.showStats) {
        int entityCount = 0;
        int meshCount = 0;
        if (ctx.activeScene) {
            for ([[maybe_unused]] auto& go : ctx.activeScene->GameObjects()) ++entityCount;
            meshCount = static_cast<int>(ctx.activeScene->GetEntities<scene::MeshRenderer>().size());
        }
        const auto& rs = renderer::RenderDebugOverlay::GetLastSnapshot().renderStats;

        // 左下に配置 (タブバー・ツールバーと重ならないよう上マージンを考慮)
        // WHY: 右上は ImGuizmo のビューキューブと重なりやすく、
        //      左下はほぼ空きスペースになるため視認性が高い。
        ImVec2 winPos  = ImGui::GetWindowPos();
        ImVec2 winSize = ImGui::GetWindowSize();
        constexpr float kMargin = 10.0f;
        ImGui::SetNextWindowPos(
            { winPos.x + kMargin, winPos.y + winSize.y - kMargin },
            ImGuiCond_Always,
            { 0.0f, 1.0f }); // pivot: 左下
        ImGui::SetNextWindowBgAlpha(0.60f);
        constexpr ImGuiWindowFlags kOverlayFlags =
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoFocusOnAppearing;
        if (ImGui::Begin("##vp_stats", nullptr, kOverlayFlags)) {
            // ── 基本情報 ───────────────────────────────────────────────────────
            ImGui::TextColored({ 0.9f, 0.9f, 0.5f, 1.0f }, "--- Game ---");
            ImGui::Text("FPS        %.1f (%.2f ms)",
                        ImGui::GetIO().Framerate,
                        1000.0f / ImGui::GetIO().Framerate);
            ImGui::Text("Entities   %d", entityCount);
            ImGui::Text("Meshes     %d", meshCount);

            // ── 描画統計 ───────────────────────────────────────────────────────
            ImGui::Spacing();
            ImGui::TextColored({ 0.9f, 0.9f, 0.5f, 1.0f }, "--- Render ---");
            ImGui::Text("Draw Calls %d", rs.drawCalls);

            // 頂点数・三角形数をカンマ区切りで読みやすく表示する
            // (snprintf で手動フォーマット。printf の %'d はクロスプラットフォームで動作しないため)
            char vtxBuf[32], triBuf[32];
            auto fmtK = [](char* buf, int n) {
                if (n >= 1000000)      std::snprintf(buf, 32, "%.1fM", n / 1000000.0f);
                else if (n >= 1000)    std::snprintf(buf, 32, "%.1fK", n / 1000.0f);
                else                   std::snprintf(buf, 32, "%d", n);
            };
            fmtK(vtxBuf, rs.vertexCount);
            fmtK(triBuf, rs.triangleCount);
            ImGui::Text("Vertices   %s", vtxBuf);
            ImGui::Text("Triangles  %s", triBuf);

            // ── カリング統計 ───────────────────────────────────────────────────
            ImGui::Spacing();
            ImGui::TextColored({ 0.9f, 0.9f, 0.5f, 1.0f }, "--- Culling ---");
            ImGui::Text("Total      %d", rs.totalObjects);

            // カリング済み数を割合付きで表示する
            const float total = static_cast<float>(rs.totalObjects > 0 ? rs.totalObjects : 1);
            ImGui::Text("Frustum    %d (%.0f%%)",
                        rs.frustumCulled,
                        rs.frustumCulled / total * 100.0f);
            ImGui::Text("Occlusion  %d (%.0f%%)",
                        rs.occlusionCulled,
                        rs.occlusionCulled / total * 100.0f);

            // 合計カリング率を色付きで表示 (50% 以上は緑、30% 未満は赤)
            const int totalCulled = rs.frustumCulled + rs.occlusionCulled;
            const float cullRate  = totalCulled / total * 100.0f;
            ImVec4 rateColor = cullRate >= 50.0f
                ? ImVec4{ 0.4f, 1.0f, 0.4f, 1.0f }
                : (cullRate >= 30.0f ? ImVec4{ 1.0f, 1.0f, 0.4f, 1.0f }
                                     : ImVec4{ 1.0f, 0.5f, 0.4f, 1.0f });
            ImGui::TextColored(rateColor, "Rate       %.0f%%", cullRate);
        }
        ImGui::End();
    }

}

} // namespace fbzz::editor
