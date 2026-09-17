/// @file    ViewportPanel.cpp
/// @brief   Scene / Game / UI Viewport のレイアウトと入力ルーティング。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include "Viewport/ViewportCommon.hpp"
#include "MapToolCommon.hpp"
#include <Editor/Util/DragDropSet.hpp>
#include <Editor/Util/EditorIcons.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/SceneEditUtils.hpp>
#include <Editor/Util/ViewportCamera.hpp>
#include <Engine/Core/Cursor.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <cstdio>
#include <iterator>

namespace fbzz::editor {

/// @brief Map Editing Mode 中に Scene ビューポート上端へ重ねる半透明ツールバー。
/// @note ツール切替のたびに MapEditorPanel まで視線とマウスを往復しないよう、編集対象の上で
///       持ち替えられるようにする (Unreal Landscape / Unity Terrain 同様)。状態確認もここで完結させる。
void DrawMapToolOverlay(EditorContext& ctx, const ImVec2& viewportMin)
{
    if (!ctx.mapEditingMode) return;

    /// @note 表示モードツールバー (上段 y+6) と重ならないよう 1 段下げる。
    const float rowY = viewportMin.y + 6.0f + ImGui::GetFrameHeight() + 6.0f;
    ImGui::SetCursorScreenPos({ viewportMin.x + 6.0f, rowY });

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 3.0f, 0.0f });
    ImVec4 overlaySurface = EditorTheme::Color(ThemeColor::SurfaceRaised);
    overlaySurface.w = 0.90f;

    for (const MapToolDef& def : kMapToolDefs) {
        const bool active = ctx.mapActiveTool == def.tool;
        ImGui::PushStyleColor(ImGuiCol_Button,
            active ? EditorTheme::Color(ThemeColor::Secondary)
                   : overlaySurface);
        char label[24];
        std::snprintf(label, sizeof(label), "%s %s", def.shortcut, def.label);
        if (ImGui::SmallButton(label))
            ActivateMapTool(ctx, def.tool);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", def.tooltip);
        ImGui::SameLine();
    }

    /// @note 右側: 現在のサブモード / ブラシ径を常時表示する。
    if (ctx.terrainTool && IsTerrainMapTool(ctx.mapActiveTool)) {
        char info[64];
        std::snprintf(info, sizeof(info), "  %s  |  Brush %.1f",
                      ctx.terrainTool->StatusLabel().c_str(),
                      ctx.terrainTool->GetBrush().radius);
        ImGui::TextColored({ 0.95f, 0.85f, 0.4f, 1.0f }, "%s", info);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", ctx.mapActiveTool == EditorContext::MapTool::TerrainHole
                ? "Ctrl+drag: invert Cut / Fill   [ ]: brush size"
                : "Shift+drag: Smooth   Ctrl+drag: Lower   [ ]: brush size");
        }
    }

    ImGui::PopStyleVar(2);
}

/// @brief Scene View フォーカス中の数字キー (1 からツールの数まで) で Map ツールを切り替える。
/// @note カメラブックマークと同じ 1-9 キーを使うため、Map Editing Mode 中だけツール切替を優先する。
/// @return true if a key consumed the input (呼び出し側はブックマーク処理をスキップする)
bool HandleMapToolHotkeys(EditorContext& ctx)
{
    if (!ctx.mapEditingMode) return false;
    if (ImGui::GetIO().WantTextInput) return false;
    /// @note 定数 6 を持たない: ツールの増減で数字キーの本数だけ取り残されると kMapToolDefs の
    ///       範囲外を読むため、割り当ては定義表から導出する。
    bool consumed = false;
    for (size_t i = 0; i < std::size(kMapToolDefs); ++i) {
        const ImGuiKey key = static_cast<ImGuiKey>(ImGuiKey_1 + static_cast<int>(i));
        if (ImGui::IsKeyPressed(key, false)) {
            ActivateMapTool(ctx, kMapToolDefs[i].tool);
            consumed = true;
        }
    }
    return consumed;
}

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

const char* GetPlayCursorOverrideLabel(EditorContext::PlayCursorOverride mode)
{
    return mode == EditorContext::PlayCursorOverride::Free ? "Cursor: Free"
                                                           : "Cursor: Game";
}

/// @brief Play 中のカーソルを取り上げさせない口。スクリプトの要求そのものは書き換えず、
/// @brief «OS へ効かせるか» だけを止める。エディター再起動で Game へ戻るので、デバッグのために
/// @brief 外したまま忘れても配布ビルドには影響しない。
void DrawPlayCursorOverrideControl(EditorContext& ctx)
{
    if (ImGui::BeginCombo("##play_cursor_override",
                          GetPlayCursorOverrideLabel(ctx.playCursorOverride))) {
        constexpr EditorContext::PlayCursorOverride kModes[] = {
            EditorContext::PlayCursorOverride::Game,
            EditorContext::PlayCursorOverride::Free
        };
        for (EditorContext::PlayCursorOverride mode : kModes) {
            const bool selected = ctx.playCursorOverride == mode;
            if (ImGui::Selectable(GetPlayCursorOverrideLabel(mode), selected))
                ctx.playCursorOverride = mode;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Game: ゲームの要求どおりにカーソルを拘束/非表示にする\n"
                          "Free: スクリプトが拘束を要求しても OS へ効かせない\n"
                          "      (デバッグ用・保存されない)");
}

/// @brief Game View の隅に出すカーソル状態のオーバーレイ。
/// @note 拘束と非表示は «画面から消える» 形でしか現れず、思ったとおりに効いていないときに
///       «誰が何を要求しているのか» を見る場所が無かった。要求はスタックで上から順に並べる。
void DrawCursorOverlay(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& size)
{
    const core::CursorPolicy policy = core::Cursor::GetEffectivePolicy();
    const bool suppressed = core::Cursor::IsSuppressed();
    const bool capturing  = policy.CapturesCursor() && !suppressed;
    /// @note 取り上げるはずの要求が «Editor の都合» で止まっている状態。ここだけ強く見せる。
    const bool released   = policy.CapturesCursor() && suppressed;

    char label[96];
    if (capturing) {
        std::snprintf(label, sizeof(label), "%s%s  |  Esc",
                      core::ToString(policy.lockMode),
                      policy.visible ? "" : " + Hidden");
    } else if (released) {
        std::snprintf(label, sizeof(label), "Cursor released  |  Click to capture");
    } else {
        std::snprintf(label, sizeof(label), "Cursor free");
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImVec4 surface = EditorTheme::Color(ThemeColor::SurfaceRaised);
    surface.w = 0.90f;
    ImGui::PushStyleColor(ImGuiCol_Button, released ? ImVec4{ 0.65f, 0.45f, 0.10f, 0.92f }
                                                    : surface);

    ImGui::SetCursorScreenPos({ viewportMin.x + 6.0f, viewportMin.y + size.y - 28.0f });
    if (ImGui::SmallButton(label))
        ImGui::OpenPopup("##cursor_overlay_popup");
    const bool badgeHovered = ImGui::IsItemHovered();
    ImGui::PopStyleColor();

    if (badgeHovered)
        ImGui::SetTooltip("クリックで «今カーソルを要求しているのは誰か» を開く");

    if (ImGui::BeginPopup("##cursor_overlay_popup")) {
        ImGui::TextUnformatted("Cursor requests");
        ImGui::Separator();

        const std::size_t count = core::Cursor::GetRequestCount();
        if (count == 0) {
            ImGui::TextDisabled("要求なし (基底: %s%s)",
                                core::ToString(core::Cursor::GetBasePolicy().lockMode),
                                core::Cursor::GetBasePolicy().visible ? "" : " + Hidden");
        }
        for (std::size_t i = 0; i < count; ++i) {
            core::CursorRequestInfo info{};
            if (!core::Cursor::GetRequest(i, info)) break;
            if (info.active) ImGui::Bullet();
            else             ImGui::Indent(ImGui::GetStyle().IndentSpacing * 0.5f);
            ImGui::Text("%-3d %-16s %s%s", info.priority,
                        info.label[0] ? info.label : "(unnamed)",
                        core::ToString(info.policy.lockMode),
                        info.policy.visible ? "" : " + Hidden");
            if (!info.active) ImGui::Unindent(ImGui::GetStyle().IndentSpacing * 0.5f);
        }

        ImGui::Separator();
        ImGui::SetNextItemWidth(140.0f);
        DrawPlayCursorOverrideControl(ctx);
        if (released && ImGui::MenuItem("Capture now"))
            ctx.requestGameCursorCapture = true;
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    /// @note 解放中は «ここを押せば戻る» を絵でも示す。枠はボタンの当たり判定を邪魔しない。
    if (released) {
        ImGui::GetWindowDrawList()->AddRect(
            viewportMin, { viewportMin.x + size.x, viewportMin.y + size.y },
            IM_COL32(210, 150, 40, 180), 0.0f, 0, 2.0f);
    }
}

void DrawGameViewportToolbar(EditorContext& ctx)
{
    /// @note Game View は Play 確認の中心なので、フォーカス操作を Viewport 直上へ置く。
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
        ImGui::SetTooltip("Play 開始時の Game View のレイアウトだけを決める。\n"
                          "カーソルの拘束/表示はスクリプトの要求 (cursor.Push) が持ち、\n"
                          "状態と一時解除は画面左下のオーバーレイから触る");

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
    ImVec4 overlaySurface = EditorTheme::Color(ThemeColor::SurfaceRaised);
    overlaySurface.w = 0.90f;

    for (const auto& entry : kModes) {
        const bool active = current == entry.mode;
        ImGui::PushStyleColor(ImGuiCol_Button,
            active ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
                   : overlaySurface);

        if (ImGui::SmallButton(entry.label))
            current = entry.mode;

        ImGui::PopStyleColor();

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", entry.tooltip);

        ImGui::SameLine();
    }

    /// @note Overlays ▼ ドロップダウン
    ImGui::PushStyleColor(ImGuiCol_Button, overlaySurface);
    if (ImGui::SmallButton("Overlays \xe2\x96\xbc"))
        ImGui::OpenPopup("##overlays_popup");
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Toggle scene overlay visibility");

    if (ImGui::BeginPopup("##overlays_popup")) {
        auto& render = ctx.projectSettings.render;
        const auto overlayCheckbox = [](const char* label, bool& value, const char* tooltip) {
            ImGui::Checkbox(label, &value);
            if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        };

        ImGui::TextDisabled("General");
        overlayCheckbox("Grid",          ctx.showGrid, nullptr);
        overlayCheckbox("Script Gizmos", ctx.showScriptGizmos,
                        "スクリプトの OnDrawGizmos / OnDrawGizmosSelected と debug.Draw* を描きます。\n"
                        "Game View と配布ビルドには出ません。");
        overlayCheckbox("Light Range",   ctx.showLightRange, nullptr);
        overlayCheckbox("Decal Bounds",  render.showDecalBounds, "デカールの投影ボックスと投影方向を描きます。");
        overlayCheckbox("Terrain Bounds", ctx.showTerrainBounds, "Terrain の範囲 (±maxHeight) を箱で描きます。");
        overlayCheckbox("LOD Bounds",    ctx.showLODBounds, "LODGroup の判定球を描きます。色は現在の LOD 段です。");
        overlayCheckbox("UI Rects",      render.showUIRects, "UI 要素の当たり判定矩形とピボットを Canvas 上へ重ねます。");

        ImGui::Separator();
        ImGui::TextDisabled("Physics");
        overlayCheckbox("Colliders",     render.showColliders,
                        "緑 = 静的 / 黄 = 動く剛体 / 暗い黄 = 眠っている剛体 / 紫 = トリガー");
        overlayCheckbox("Terrain Collision", render.showTerrainCollision, "地形の当たり判定 (HeightField) を描きます。");
        overlayCheckbox("Constraints",   ctx.showConstraints,
                        "物理拘束を描きます。編集中は JointComponent の設定値 (アンカー・軸・可動域) から描きます。");
        overlayCheckbox("Rigid Bodies",  ctx.showRigidBodies, "選択中の剛体の速度 (1 秒後の到達点)・角速度・重心を描きます。");
        overlayCheckbox("Volumes",       ctx.showPhysicsVolumes,
                        "VolumeComponent のトリガー形状と効果の向き、duration の残り (天面の輪) を描きます。\n"
                        "青 = 重力 / 紫 = 渦 / 橙 = 爆風 / 黄緑 = 時間 (破線が等速) / 桃 = 磁場。\n"
                        "赤い × はトリガーが無い、赤い対角線は判定できない形状 (Box / Mesh / 凸包) です。");
        overlayCheckbox("Water Flow",    ctx.showWaterFlow,
                        "すべての水面の範囲・水流 (波面上の矢印)・渦 (紫)・浮力の届く深さ (破線) を描きます。\n"
                        "範囲は浮力と同じく回転を無視した矩形です。");
        overlayCheckbox("Ragdoll",       ctx.showRagdoll,
                        "剛体・関節の可動域・接触点を重ねます。赤い関節は力負けしています。");

        ImGui::Separator();
        ImGui::TextDisabled("Animation");
        overlayCheckbox("Skeleton",      ctx.showSkeleton, nullptr);
        if (ctx.showSkeleton) {
            ImGui::Indent();
            overlayCheckbox("Selected Only##skeleton", ctx.skeletonSelectedOnly, "選択中のキャラクターの骨だけを描きます。");
            ImGui::Unindent();
        }
        overlayCheckbox("IK Chains",     ctx.showIK, "IKSolver のチェーン・ターゲット (赤)・ポール (黄) を描きます。");
        overlayCheckbox("Spring Bones",  ctx.showSpringBones, "SpringBone の揺れ骨とコライダーを描きます。");
        overlayCheckbox("Attachments",   ctx.showAttachments, "SocketAttachment / TransformConstraint の追従先への線を描きます。");

        ImGui::Separator();
        ImGui::TextDisabled("VFX");
        overlayCheckbox("VFX Gizmos",    ctx.showVFXGizmos, "エミッターの発生形状と初速を描きます。");
        overlayCheckbox("Flow Fields",   ctx.showFlowFields,
                        "FlowField の効く範囲と流れの向きを描きます。破線の内球は影響度 50%。\n"
                        "channels を絞った場は破線、速度場 PNG が読めない Baked は赤い対角線です。");
        overlayCheckbox("Flow Samples",  ctx.showFlowSamples,
                        "格子点で実際の流速 (環境流と重ねた場の合計) を矢印で描きます。色は速さ (青→赤 = 0〜10 m/s)。\n"
                        "範囲は選択中の FlowField、無ければカメラ前方です。");
        overlayCheckbox("VFX Paths",     ctx.showVFXPaths,
                        "選択中の Trail / MeshTrail / LineRenderer / VFXLine / VFXBeam の経路とサンプル点を描きます。");
        overlayCheckbox("Particle Overdraw", render.particleOverdrawView, "パーティクルの重なり枚数をヒートマップで上書きします。");

        ImGui::Separator();
        ImGui::TextDisabled("Navigation");
        overlayCheckbox("NavMesh",       render.showNavMesh, nullptr);
        /// @note 描き方を出す/出さないの隣に置く。点けた流れのまま Areas / Voxels へ移れる。
        if (ctx.projectSettings.render.showNavMesh) {
            static constexpr const char* kNavModes[] = {
                "Solid", "Transparent", "Areas", "Portals", "Voxels" };
            int navMode = static_cast<int>(ctx.projectSettings.render.navMeshDrawMode);
            ImGui::Indent();
            ImGui::SetNextItemWidth(130.0f);
            if (ImGui::Combo("##navmesh_draw_mode", &navMode, kNavModes, 5))
                ctx.projectSettings.render.navMeshDrawMode =
                    static_cast<renderer::NavMeshDrawMode>(navMode);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Solid: 面と外周 / Areas: areaType 塗り分け /\n"
                                  "Portals: ポリゴンの接続 / Voxels: ベイクのセル判定");
            ImGui::Unindent();
        }
        overlayCheckbox("AI Sensors",    render.showNavSensors, nullptr);

        ImGui::Separator();
        ImGui::TextDisabled("Lighting");
        overlayCheckbox("Shadow Cascades", render.shadow.debugVisualizeCascades, "カスケード番号を色で塗ります。");
        overlayCheckbox("Light Clusters",  render.clustered.debugHeatmap,
                        "クラスタあたりのライト数をヒートマップで出します (Forward+ / Deferred+)。");

        ImGui::Separator();
        overlayCheckbox("Stats",         ctx.showStats, nullptr);
        ImGui::Separator();
        ImGui::Checkbox("Scene Icons", &ctx.showSceneIcons);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ライト・カメラ・音源などメッシュを持たないコンポーネントのアイコン。\n"
                              "消すとアイコンのクリック選択も止まります。");
        if (ctx.showSceneIcons && ImGui::TreeNode("Icon Types##scene_icon_types")) {
            for (int type = 0; type < SceneIconTypeCount(); ++type) {
                bool visible = IsSceneIconTypeVisible(ctx, type);
                ImGui::PushID(type);
                if (ImGui::Checkbox(SceneIconTypeLabel(type), &visible))
                    SetSceneIconTypeVisible(ctx, type, visible);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::Separator();
        ImGui::Checkbox("Occlusion Culling", &ctx.sceneViewOcclusionCulling);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scene View で CPU オクルージョンカリングを効かせます。\n"
                              "既定は無効です。遮蔽者はメッシュ実体ではなくバウンディング球の\n"
                              "近似なので、有効にすると見えているものが消える場合があります。\n"
                              "「消えた原因がカリングか」を切り分けるときに入れ切りしてください。");
        ImGui::Separator();
        ImGui::Checkbox("Surface snap aligns to normal", &ctx.surfaceSnapAlignToNormal);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Ctrl+Shift drag: also rotate the object so its up axis "
                              "matches the surface normal");
        ImGui::EndPopup();
    }

    /// @name Gizmo / Snap クイックトグル
    /// @note Gizmo Mode/Space・Snap の切替は従来ホットキーかメニュー依存だった。シーンビュー上に
    ///       置くことで視線移動ゼロでモード切替できる (Unity/Godot のツールバー相当)。
    ImGui::SameLine(0.0f, 10.0f);
    const auto gizmoBtn = [&](const char* label, EditorContext::GizmoMode mode, const char* tip) {
        const bool active = ctx.gizmoMode == mode;
        ImGui::PushStyleColor(ImGuiCol_Button,
            active ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
                   : overlaySurface);
        if (ImGui::SmallButton(label)) ctx.gizmoMode = mode;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        ImGui::SameLine();
    };
    /// @note 記号が使えるときは絵にする。3 つ並ぶ切替は形の違いの方が速く読め、
    ///       幅も詰まって «絵を見る» 面積が残る。読めない環境では従来の短い語へ落ちる。
    gizmoBtn(icons::Or(icons::kMove,   "Move"),  EditorContext::GizmoMode::Translate, "Translate gizmo");
    gizmoBtn(icons::Or(icons::kRotate, "Rot"),   EditorContext::GizmoMode::Rotate,    "Rotate gizmo");
    gizmoBtn(icons::Or(icons::kScale,  "Scale"), EditorContext::GizmoMode::Scale,     "Scale gizmo");

    {
        const bool world = ctx.gizmoSpace == EditorContext::GizmoSpace::World;
        ImGui::PushStyleColor(ImGuiCol_Button, overlaySurface);
        if (ImGui::SmallButton(world ? "World" : "Local"))
            ctx.gizmoSpace = world ? EditorContext::GizmoSpace::Local
                                   : EditorContext::GizmoSpace::World;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Gizmo space: %s (Q to toggle)", world ? "World" : "Local");
        ImGui::SameLine();
    }
    {
        /// @note Pivot / Center トグル (Unity 互換, Z キー)。
        const bool pivot = ctx.gizmoPivot == EditorContext::GizmoPivot::Pivot;
        ImGui::PushStyleColor(ImGuiCol_Button, overlaySurface);
        if (ImGui::SmallButton(pivot ? "Pivot" : "Center"))
            ctx.gizmoPivot = pivot ? EditorContext::GizmoPivot::Center
                                   : EditorContext::GizmoPivot::Pivot;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(pivot
                ? "Pivot: gizmo sits on the active object's origin (Z to toggle)"
                : "Center: gizmo sits at the center of the whole selection (Z to toggle)");
        ImGui::SameLine();
    }
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ctx.snapEnabled
            ? EditorTheme::Color(ThemeColor::AccentActive)
            : overlaySurface);
        if (ImGui::SmallButton(icons::Or(icons::kSnapGrid, "Snap"))) ctx.snapEnabled = !ctx.snapEnabled;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Toggle gizmo snapping (also in the status bar)");
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
    /// @note ゲームが Locked でカーソルを握っている間は拘束範囲が窓を追う (EditorApp::
    ///       UpdatePlayCursorControls) ため、この間に窓を動かす/広げると両者が互いを追いかけて
    ///       増幅し画面外まで飛ぶ。握られている間だけ矩形を固定する (Escape で解放すれば掴める)。
    m_pinWindowRect = m_kind == Kind::Game
        && ctx.playMode != nullptr && !ctx.playMode->IsInEditor()
        && !core::Cursor::IsSuppressed()
        && core::Cursor::GetEffectivePolicy().CapturesCursor();
    if (m_kind == Kind::Game && ctx.requestGameViewportFocus)
        ImGui::SetNextWindowFocus();
}

ImGuiWindowFlags ViewportPanel::GetWindowFlags() const
{
    return m_pinWindowRect ? (ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)
                           : ImGuiWindowFlags_None;
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
        /// @note UI Viewport は Game View の完成済み RT を共有するため表示枠も同じ比率にする。
        ///       Canvas 比率で引き伸ばすと背景とクリック座標が Game 出力からずれる。
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
            ctx.gameViewportRectValid = true;
        } else {
            ctx.uiViewportOriginX = viewportMin.x;
            ctx.uiViewportOriginY = viewportMin.y;
        }
    }
    ImVec2 viewportMax = { viewportMin.x + size.x, viewportMin.y + size.y };
    bool viewportHovered = ImGui::IsMouseHoveringRect(viewportMin, viewportMax);
    /// @note GetImTextureID は RT が生きていても «描画側の枠が尽きた» ときに nullptr を返す。
    ///       そのまま AddImage へ渡すと DX12 では無効なディスクリプタテーブルを束縛してしまう。
    void* rawTexID = (hdrRT.IsValid() && ctx.imguiRenderer && resources)
        ? ctx.imguiRenderer->GetImTextureID(hdrRT, *resources, 0)
        : nullptr;
    if (rawTexID) {
        ImGui::GetWindowDrawList()->AddImage(
            static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(rawTexID)), viewportMin, viewportMax);
    } else {
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            cursor, { cursor.x + size.x, cursor.y + size.y }, IM_COL32(30, 30, 30, 255));
        ImGui::SetCursorScreenPos({ cursor.x + size.x * 0.5f - 60.0f, cursor.y + size.y * 0.5f - 7.0f });
        ImGui::TextDisabled(hdrRT.IsValid() ? "Render Target Unavailable" : "No Render Target");
    }

    const bool inPlayOrPause = ctx.playMode && !ctx.playMode->IsInEditor();

    if (isUIView && !inPlayOrPause)
        DrawCanvasEditorGuides(ctx, viewportMin, size);

    /// @note 仮適用したまま Scene View のドロップ処理を通らなくなった場合 (Play 開始・
    ///       パネル種別の切り替え等) の保険。ドラッグ自体が終わっていれば必ず巻き戻す。
    if (m_materialDrag.applied && (!isSceneView || inPlayOrPause || !ImGui::IsDragDropActive()))
        CancelMaterialDragPreview(ctx, m_materialDrag);

    if (isSceneView && !inPlayOrPause) {
        const ImGuiID viewportDropId = ImGui::GetID("##scene_view_prefab_drop_target");
        /// @note ドラッグ中の .mat を「カーソル下へ仮適用 → 外れたら戻す → リリースで確定」
        ///       という Unity と同じ挙動にするため、AcceptBeforeDelivery で配送前の状態も受け取る。
        bool materialDragHandled = false;
        if (ImGui::BeginDragDropTargetCustom(ImRect(viewportMin, viewportMax), viewportDropId)) {
            const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                "ASSET_PATH", ImGuiDragDropFlags_AcceptBeforeDelivery);
            std::string assetPath;
            if (ReadAssetPayload(payload, assetPath)) {
                const std::string ext =
                    util::StringUtils::ToLower(util::FileSystem::GetExtension(assetPath));
                if (ext == ".mat") {
                    materialDragHandled = true;
                    /// @note 仮適用 → (リリース) → 確定。ビューポート外から入ってきて
                    ///       1 フレーム目でリリースされた場合も取りこぼさないよう、
                    ///       配送フレームでも未適用なら先に仮適用してから確定する。
                    if (!payload->IsDelivery() || !m_materialDrag.applied)
                        UpdateMaterialDragPreview(ctx, m_materialDrag, assetPath, viewportMin);
                    if (payload->IsDelivery() && CommitMaterialDragPreview(ctx, m_materialDrag)) {
                        if (ctx.markSceneDirty) ctx.markSceneDirty();
                    }
                } else if (payload->IsDelivery()) {
                    /// @note 複数選択を運んでいれば全部置く。1 件ずつ選択を置き換えるので、置いたものを集めて選び直す。
                    std::vector<scene::EntityID> placed;
                    for (const std::string& path : dragdrop::DraggedAssetPaths(assetPath)) {
                        if (!InstantiateAssetAtViewport(ctx, path, viewportMin)) continue;
                        placed.insert(placed.end(), ctx.selectedEntities.begin(), ctx.selectedEntities.end());
                    }
                    if (!placed.empty()) {
                        SelectEntities(ctx, std::move(placed));
                        if (ctx.markSceneDirty) ctx.markSceneDirty();
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        /// @note ビューポート外へ出た / ドラッグがキャンセルされたフレームで仮適用を巻き戻す。
        if (!materialDragHandled && m_materialDrag.applied)
            CancelMaterialDragPreview(ctx, m_materialDrag);
    }

    const bool gizmoWantsMouse = ImGuizmo::IsUsing() || ImGuizmo::IsOver()
                              || IsOrientationGizmoHovered() || IsOrientationGizmoActive();
    const bool anyToolActive = ctx.clothPinPainting || (ctx.terrainTool && ctx.terrainTool->IsActive());
    /// @note 頂点スナップ (V ドラッグ) / 面スナップ (Ctrl+Shift ドラッグ)。同じ左ドラッグを使うため、
    ///       選択・矩形選択・ギズモより先に処理して「掴んでいる」間は他の解釈をさせない。
    const bool snapping = (isSceneView && !inPlayOrPause && !ctx.clothPinPainting)
                        ? HandleViewportSnapping(ctx, viewportMin, size)
                        : false;

    /// @note Alt+左ドラッグはカメラオービットに割り当てられているため、選択操作から除外する
    const bool altHeld = ImGui::GetIO().KeyAlt;
    if (isSceneView && !inPlayOrPause && viewportHovered && !altHeld && !snapping && !ctx.clothPinPainting &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !gizmoWantsMouse &&
        !m_prevOverlayHovered)
        PickEntity(ctx, viewportMin);

    /// @name 矩形 (ドラッグ) 選択
    /// @note 複数オブジェクトをまとめて動かす作業は Unity の箱選択が前提。クリック位置から
    ///       しきい値以上ドラッグしたら矩形選択モードへ移行し、離した時点で矩形内の GO を選択する。
    if (isSceneView && !inPlayOrPause && !snapping && !ctx.clothPinPainting) {
        /// @note px: クリックと区別するしきい値
        constexpr float kDragThreshold = 5.0f;

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

    if (isGameView && inPlayOrPause) {
        /// @note 「解放中にゲーム画面をクリックしたら捕獲へ戻す」入口。オーバーレイより先に判定
        ///       しないと、バッジやポップアップの上のクリックまで拾い «見ようとしただけで取られる»。
        if (viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !ImGui::IsAnyItemHovered()
            && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
            ctx.requestGameCursorCapture = true;

        DrawCursorOverlay(ctx, viewportMin, size);
    }

    /// @note Map Editing Mode のツールバーオーバーレイ (半透明ストリップ + 状態表示)
    if (isSceneView && !inPlayOrPause)
        DrawMapToolOverlay(ctx, viewportMin);

    if (isSceneView && !inPlayOrPause) {
        DrawSceneIcons(ctx, viewportMin, size);
        /// @note スナップドラッグ中はギズモを出さない。同じ左ドラッグを ImGuizmo が掴むと
        ///       吸着とギズモ移動が同時に走って挙動が二重になる。
        if (!snapping && !ctx.clothPinPainting)
            DrawGizmo(ctx, viewportMin, size, m_lastGizmoOp, m_lastGizmoMode, m_prevGizmoOver, m_prevGizmoUsing);
        DrawOrientationGizmo(ctx, viewportMin, size);

        /// @name スナップモードのヒント (修飾キーを押している間だけ)
        /// @note モーメンタリ操作は「今その状態に入っている」ことが画面で分からないと使われない。
        ///       押した瞬間に何が起きるかを一行で出す。
        if (ctx.vertexSnapActive || ctx.surfaceSnapActive) {
            const char* hint = ctx.vertexSnapActive
                ? "Vertex Snap — drag to snap a vertex onto another mesh's vertex"
                : "Surface Snap — drag to place the selection on the surface under the cursor";
            const ImVec2 textSize = ImGui::CalcTextSize(hint);
            const ImVec2 pos = { viewportMin.x + (size.x - textSize.x) * 0.5f,
                                 viewportMin.y + size.y - textSize.y - 28.0f };
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled({ pos.x - 8.0f, pos.y - 4.0f },
                              { pos.x + textSize.x + 8.0f, pos.y + textSize.y + 4.0f },
                              IM_COL32(20, 20, 20, 200), 4.0f);
            dl->AddText(pos, IM_COL32(255, 225, 120, 255), hint);
        }

        /// @name Snap インジケーター (ツールバー右隣、ON 時のみ)
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

        /// @name 射影インジケーター (平行投影のときだけ)
        /// @note 正投影は «たまたま真横から見ているだけの遠近視点» と静止画では見分けが付かないため、
        ///       寸法を信じてよい状態かどうかを明示する。
        if (IsEditorCameraOrthographic(ctx)) {
            const char*  label = " ORTHO ";
            const ImVec2 tsz   = ImGui::CalcTextSize(label);
            const float  y     = viewportMin.y + 6.0f + ImGui::GetFrameHeight() + 4.0f
                               + (ctx.snapEnabled ? tsz.y + 4.0f : 0.0f);
            const ImVec2 p     = { viewportMin.x + 6.0f, y };
            ImDrawList*  dl    = ImGui::GetWindowDrawList();
            dl->AddRectFilled({ p.x - 2, p.y - 1 },
                              { p.x + tsz.x + 2, p.y + tsz.y + 1 },
                              IM_COL32(90, 70, 20, 200), 3.0f);
            dl->AddText(p, IM_COL32(255, 210, 110, 255), label);
        }

        /// @name カメラブックマーク HUD (オリエンテーションギズモ下)
        bool bookmarkRowHovered = false;
        {
            constexpr float kSlotSz  = 18.0f;
            constexpr float kSlotGap = 2.0f;
            /// @note margin + gizmo + gap
            constexpr float kGizmoBottom = 8.0f + 120.0f + 6.0f;
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

                /// @note DrawList の飾りだけだとクリックできない («押せそうで押せない» UI) ため、
                ///       マウスでも保存/呼び出しできるようにする。
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

        /// @note オーバーレイ UI (表示モードボタン等、ブックマークスロット) をクリックした瞬間に
        ///       背後の 3D ピッキングが同時に走ると選択が意図せず変わるため、このフレームのホバー
        ///       状態を記録し、次フレームの PickEntity / 矩形選択開始を抑制する。
        m_prevOverlayHovered = bookmarkRowHovered || ImGui::IsAnyItemHovered();

        /// @note F フォーカス / Delete / Ctrl+D / Esc / Ctrl+A は
        ///       EditorApp::RegisterDefaultHotkeys が HotkeyScope::SceneViewport として
        ///       登録している。ここで直接キーを見ると、リバインドしても効かない
        ///       ショートカットが増え、F1 の一覧とも食い違うため書かない。

        /// @note Map Editing Mode 中は数字キー 1-6 をツール切替に使う (下のブックマークより優先)。
        ///       ブックマークとツール切替が同じ 1-9 キーを共有するため、Map モードではツールの
        ///       持ち替えを優先し往復操作を無くす。
        const bool mapToolConsumed = ctx.viewportFocused && HandleMapToolHotkeys(ctx);

        /// @note Camera bookmarks: Shift+1~9 to save, 1~9 to recall.
        ///       Map モードでツール切替に消費されたフレームはブックマーク処理を丸ごとスキップする。
        if (ctx.viewportFocused && ctx.editorCamera && !mapToolConsumed) {
            static const ImGuiKey kNumKeys[9] = {
                ImGuiKey_1, ImGuiKey_2, ImGuiKey_3,
                ImGuiKey_4, ImGuiKey_5, ImGuiKey_6,
                ImGuiKey_7, ImGuiKey_8, ImGuiKey_9
            };
            const bool shiftHeld = ImGui::IsKeyDown(ImGuiKey_LeftShift)
                                || ImGui::IsKeyDown(ImGuiKey_RightShift);
            /// @note Alt + 数字は軸ビュー (view.axis_*) が取る。ここで拾うと、視点を切り替える
            ///       つもりの Alt+1 がブックマーク 1 へ飛んでしまう。
            const bool altHeld = ImGui::IsKeyDown(ImGuiKey_LeftAlt)
                              || ImGui::IsKeyDown(ImGuiKey_RightAlt);
            for (int i = 0; i < 9 && !altHeld; ++i) {
                if (!ImGui::IsKeyPressed(kNumKeys[i])) continue;
                /// @note Map モード中は 1-6 をツールへ譲り、7-9 のみブックマークとして残す。
                if (ctx.mapEditingMode && i < 6) continue;
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
    if (isSceneView) {
        if (ctx.clothPinPainting) m_rectSelecting = false;
        DrawClothPinBrush(ctx, m_clothPinBrush, viewportMin, size,
            viewportHovered && !m_prevOverlayHovered && !IsOrientationGizmoHovered(), !inPlayOrPause);
    }
    bool uiGizmoActive = false;
    if (isUIView && !inPlayOrPause) {
        DrawUISelectionOutlines(ctx, viewportMin, size);
        uiGizmoActive = DrawUIGizmo(ctx, viewportMin, size, m_uiGizmoDrag, m_uiGizmoDragStart, m_uiGizmoStartX, m_uiGizmoStartY, m_uiGizmoStartWidth, m_uiGizmoStartHeight, m_uiGizmoStartAngle, m_uiGizmoStartZ);
    }
    if (isUIView && !inPlayOrPause && viewportHovered
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !uiGizmoActive)
        PickUIEntity(ctx, viewportMin, size);

    /// @note 矢印キーで選択 UI 要素を微移動する（ビューポートにキーボードフォーカスがあるときのみ）。
    if (isUIView && !inPlayOrPause)
        HandleUINudge(ctx, ImGui::IsWindowFocused());

    /// @name TerrainTool: Sculpt / Paint 操作
    if (isSceneView && ctx.terrainTool && ctx.activeScene
        && !(ctx.playMode && !ctx.playMode->IsInEditor()))
    {
        const bool vpHovered = ImGui::IsWindowHovered();
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

    /// @note Show play/pause state with a viewport border.
    if (isGameView && ctx.playMode && ctx.playMode->IsPlaying())
        ImGui::GetWindowDrawList()->AddRect(viewportMin, viewportMax, IM_COL32(80, 200, 80, 220), 0.0f, 0, 3.0f);
    else if (isGameView && ctx.playMode && ctx.playMode->IsPaused())
        ImGui::GetWindowDrawList()->AddRect(viewportMin, viewportMax, IM_COL32(255, 180, 50, 220), 0.0f, 0, 3.0f);

    if (isGameView && ctx.showStats)
        DrawStatsOverlay(ctx);

}

} // namespace fbzz::editor
