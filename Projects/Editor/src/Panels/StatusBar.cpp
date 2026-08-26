// FBZZ Engine
// StatusBar.cpp | fbzz::editor
// DockSpaceHost の画面下端でインライン描画される情報バー
#include <Editor/Panels/StatusBar.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/BuildConsole.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <cstdio>
#include <string>

namespace fbzz::editor {

namespace {
constexpr float      kSnapFieldWidth   = 48.0f;
constexpr const char* kSnapAxisLabels[] = { "Pos", "Rot", "Scl" };

void Sep()
{
    ImGui::SameLine();
    ImGui::TextDisabled(" | ");
    ImGui::SameLine();
}

float SepWidth()
{
    return ImGui::GetStyle().ItemSpacing.x * 2.0f + ImGui::CalcTextSize(" | ").x;
}

float SnapToggleWidth()
{
    return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("Snap").x;
}

float SnapFieldsWidth()
{
    float w = 0.0f;
    for (const char* label : kSnapAxisLabels)
        w += 4.0f + ImGui::CalcTextSize(label).x + 2.0f + kSnapFieldWidth;
    return w;
}

// 直前に描いた項目の右端 (ウィンドウローカル X)。バーは 1 行なので、これが次の項目の開始位置になる。
float LastItemEndX()
{
    return ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
}

} // namespace

// 下端ドロワーの開閉ボタン。開いている間は押下色にして状態を一目で分かるようにする。
// WHY: Unreal の Content Drawer と同じで、Viewメニューやドックタブを探さずに
//      どのレイアウトからでも同じ位置で開閉できることに価値がある。
void DrawDrawerToggle(const char* label, bool* visible, const char* tooltipTarget)
{
    if (visible == nullptr) return;
    const bool wasVisible = *visible;
    if (wasVisible)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    char buttonLabel[64];
    std::snprintf(buttonLabel, sizeof(buttonLabel), "%s  %s", label, wasVisible ? "v" : "^");
    if (ImGui::SmallButton(buttonLabel))
        *visible = !wasVisible;
    if (wasVisible)
        ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s %s", *visible ? "Hide" : "Show", tooltipTarget);
    Sep();
}

void StatusBar::Draw(EditorContext& ctx, bool* assetBrowserVisible, bool* consoleVisible)
{
    // FPS / フレームタイム
    m_fpsTimer += ImGui::GetIO().DeltaTime;
    ++m_fpsCount;
    if (m_fpsTimer >= 0.5f) {
        m_fps      = static_cast<float>(m_fpsCount) / m_fpsTimer;
        m_fpsTimer = 0.0f;
        m_fpsCount = 0;
    }
    const float ms = (m_fps > 0.0f) ? (1000.0f / m_fps) : 0.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 4.0f, 2.0f });
    const float barH = ImGui::GetFrameHeight() + 2.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Color(ThemeColor::Canvas));
    ImGui::BeginChild("##statusbar_strip", { 0.0f, barH }, false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    // WHY: 行の基準をテキストではなく枠付きウィジェット (Checkbox / DragFloat) の高さに合わせる。
    //      テキスト基準のままだと枠の下端がバー = ウィンドウ下端をはみ出して切れる。
    //      AlignTextToFramePadding でテキスト側をこの行に合わせ直すので、文字の位置は変わらない。
    const float rowY = (barH - ImGui::GetFrameHeight()) * 0.5f;
    ImGui::SetCursorPosY(rowY);
    ImGui::AlignTextToFramePadding();

    // 右端に置く内容を描画前に決めておく。
    // WHY: 左から流し込む情報をどこで打ち切るかは、右端領域と Snap 群の幅が確定しないと判定できない。
    //      判定しないままだと、ウィンドウが狭いときに Snap のチェックボックスと入力欄が外へ押し出される。
    const float pbW       = 180.0f;
    const auto& hrs       = ctx.hotReloadState;
    const bool  compiling = (hrs == EditorContext::HotReloadState::Compiling);
    const bool  reloading = (hrs == EditorContext::HotReloadState::Reloading);
    const bool  completed = (hrs == EditorContext::HotReloadState::Done && ctx.hotReloadProgress >= 1.0f);

    const bool showProgress = compiling || reloading || completed;

    // 恒常表示: 直近ビルドの結果を「消さずに」出す。従来は Done/Failed が数秒で消えて
    // ビルド状況を後から確認できなかったため、BuildConsole の最新レコードを常時表示する。
    const BuildRecord* latest       = ctx.buildConsole ? ctx.buildConsole->Latest() : nullptr;
    const char*        reloadText   = nullptr;
    ImVec4             reloadColor  = EditorTheme::Color(ThemeColor::Text);
    char               summary[160] = {};
    if (!showProgress) {
        if (!m_message.empty()) {
            // 一時的な操作メッセージ (保存など) を最優先で表示する。
            reloadText  = m_message.c_str();
            reloadColor = EditorTheme::Color(ThemeColor::Info);
        } else if (latest && latest->result == BuildRecord::Result::Failed) {
            std::snprintf(summary, sizeof(summary), "Build failed  %d error(s)  %s",
                          latest->errorCount, latest->startClock.c_str());
            reloadText  = summary;
            reloadColor = EditorTheme::Color(ThemeColor::Danger);
        } else if (latest && latest->result == BuildRecord::Result::Success) {
            const char* kind = latest->kind == BuildRecord::Kind::Script ? "Scripts" : "HLSL";
            if (latest->warnCount > 0)
                std::snprintf(summary, sizeof(summary), "%s OK  %dW  %s (%.1fs)",
                              kind, latest->warnCount, latest->startClock.c_str(), latest->durationSec);
            else
                std::snprintf(summary, sizeof(summary), "%s OK  %s (%.1fs)",
                              kind, latest->startClock.c_str(), latest->durationSec);
            reloadText  = summary;
            reloadColor = EditorTheme::Color(ThemeColor::Success);
        }
    }

    const float windowW    = ImGui::GetWindowWidth();
    const float rightW     = showProgress ? (pbW + 4.0f)
                                          : (reloadText ? ImGui::CalcTextSize(reloadText).x + 8.0f : 0.0f);
    const float snapW      = SepWidth() + SnapToggleWidth() + (ctx.snapEnabled ? SnapFieldsWidth() : 0.0f);
    const float leftBudget = windowW - rightW - snapW;

    // ── 下端ドロワー ──────────────────────────────────────────────────
    DrawDrawerToggle("Asset Browser", assetBrowserVisible, "Asset Browser");
    DrawDrawerToggle("Console", consoleVisible, "Console (debug log)");

    if (ctx.playMode) {
        switch (ctx.playMode->GetState()) {
        case PlayState::Playing:
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Success));
            ImGui::TextUnformatted("  PLAYING ");
            ImGui::PopStyleColor();
            break;
        case PlayState::Paused:
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Warning));
            ImGui::TextUnformatted("  PAUSED ");
            ImGui::PopStyleColor();
            break;
        default:
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::TextMuted));
            ImGui::TextUnformatted("  EDITOR ");
            ImGui::PopStyleColor();
            break;
        }
        Sep();
    }

    // ── シーン名 + 未保存インジケーター ───────────────────────────────
    // WHY: 未保存状態は「タイトルバーの *」「AssetBrowser の Save* (N)」に分散していた。
    //      ここへ シーン未保存の橙ドットと 未保存アセット件数を集約し、一目で保存漏れを把握できるようにする。
    const std::string sceneName = ctx.currentScenePath.empty()
        ? "Untitled"
        : util::FileSystem::GetFilename(ctx.currentScenePath);
    ImGui::Text("Scene: %s", sceneName.c_str());
    if (ctx.sceneDirty) {
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning), "\xE2\x97\x8F"); // ● 未保存
        // クリックで即保存できるようにする (タイトルバー * と StatusBar 表示の導線を一致させる)。
        if (ImGui::IsItemClicked()) ctx.requestSaveScene = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scene has unsaved changes \xe2\x80\x94 click to save (Ctrl+S)");
    }
    if (const int dirtyAssets = static_cast<int>(AssetDirtyRegistry::GetAll().size()); dirtyAssets > 0) {
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning), "%d unsaved asset%s",
                           dirtyAssets, dirtyAssets == 1 ? "" : "s");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Unsaved assets — Save from the Asset Browser");
    }
    Sep();
    ImGui::Text("FPS: %.1f  (%.2f ms)", m_fps, ms);

    // 選択名は長さが読めないため先に測り、シーン統計やカメラ座標より優先で場所を確保する。
    const char* selName = "None";
    if (auto* go = ctx.GetSelectedGO())
        selName = go->name.c_str();
    char selText[160];
    std::snprintf(selText, sizeof(selText), "Sel: %s", selName);
    const float selW = SepWidth() + ImGui::CalcTextSize(selText).x;

    // 残り幅に入らない項目は落とす。reserved は「この項目より優先する後続項目」の幅。
    auto DrawIfFits = [leftBudget](const char* text, float reserved) {
        if (LastItemEndX() + SepWidth() + ImGui::CalcTextSize(text).x + reserved > leftBudget)
            return;
        Sep();
        ImGui::TextUnformatted(text);
    };

    // ── シーン統計 ────────────────────────────────────────────────────
    if (ctx.activeScene) {
        const int objCount   = static_cast<int>(ctx.activeScene->GameObjectCount());
        const int meshCount  = static_cast<int>(ctx.activeScene->GetComponents<scene::MeshRenderer>().size());
        const int lightCount = static_cast<int>(ctx.activeScene->GetComponents<scene::LightComponent>().size());
        char stats[96];
        std::snprintf(stats, sizeof(stats), "Obj: %d  Mesh: %d  Light: %d", objCount, meshCount, lightCount);
        DrawIfFits(stats, selW);
    }

    // ── カメラ座標 ────────────────────────────────────────────────────
    if (ctx.editorCamera) {
        const auto& p = ctx.editorCamera->m_position;
        char cam[64];
        std::snprintf(cam, sizeof(cam), "Cam: (%.1f, %.1f, %.1f)", p.x, p.y, p.z);
        DrawIfFits(cam, selW);
    }

    // ── 選択オブジェクト ──────────────────────────────────────────────
    DrawIfFits(selText, 0.0f);

    // ── スナップ設定 ──────────────────────────────────────────────────
    Sep();
    ImGui::PushStyleColor(ImGuiCol_Text,
        ctx.snapEnabled ? EditorTheme::Color(ThemeColor::Accent)
                        : EditorTheme::Color(ThemeColor::TextMuted));
    ImGui::Checkbox("Snap", &ctx.snapEnabled);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Toggle gizmo snapping\nRight-click each value for presets");
    // 左側を削っても入力欄 3 つ分が残らない極端な幅では、チェックボックスだけ残す。
    const bool roomForSnapFields = (windowW - rightW - LastItemEndX()) >= SnapFieldsWidth();
    if (ctx.snapEnabled && roomForSnapFields) {
        // Position
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextDisabled("Pos");
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::SetNextItemWidth(kSnapFieldWidth);
        ImGui::DragFloat("##snap_pos", &ctx.snapPos, 0.01f, 0.001f, 1000.0f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Position snap (m)\nRight-click for presets");
        if (ImGui::BeginPopupContextItem("##snap_pos_ctx")) {
            ImGui::TextDisabled("Position snap");
            ImGui::Separator();
            static constexpr float kPosP[] = { 0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f };
            for (float v : kPosP) {
                char lbl[16]; std::snprintf(lbl, sizeof(lbl), "%.2f", v);
                if (ImGui::MenuItem(lbl, nullptr, ctx.snapPos == v)) ctx.snapPos = v;
            }
            ImGui::EndPopup();
        }
        // Rotation
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextDisabled("Rot");
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::SetNextItemWidth(kSnapFieldWidth);
        ImGui::DragFloat("##snap_rot", &ctx.snapRot, 0.5f, 0.1f, 180.0f, "%.1f\xc2\xb0");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotation snap (degrees)\nRight-click for presets");
        if (ImGui::BeginPopupContextItem("##snap_rot_ctx")) {
            ImGui::TextDisabled("Rotation snap");
            ImGui::Separator();
            static constexpr float kRotP[] = { 1.0f, 5.0f, 10.0f, 15.0f, 22.5f, 30.0f, 45.0f, 90.0f };
            for (float v : kRotP) {
                char lbl[16]; std::snprintf(lbl, sizeof(lbl), "%.1f\xc2\xb0", v);
                if (ImGui::MenuItem(lbl, nullptr, ctx.snapRot == v)) ctx.snapRot = v;
            }
            ImGui::EndPopup();
        }
        // Scale
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextDisabled("Scl");
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::SetNextItemWidth(kSnapFieldWidth);
        ImGui::DragFloat("##snap_scale", &ctx.snapScale, 0.01f, 0.001f, 100.0f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scale snap\nRight-click for presets");
        if (ImGui::BeginPopupContextItem("##snap_scale_ctx")) {
            ImGui::TextDisabled("Scale snap");
            ImGui::Separator();
            static constexpr float kSclP[] = { 0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f };
            for (float v : kSclP) {
                char lbl[16]; std::snprintf(lbl, sizeof(lbl), "%.2f", v);
                if (ImGui::MenuItem(lbl, nullptr, ctx.snapScale == v)) ctx.snapScale = v;
            }
            ImGui::EndPopup();
        }
    }

    // ── ホットリロード状態（右端）─────────────────────────────────────
    {
        const float rightX = windowW - pbW - 4.0f;
        const float pbH    = barH - 4.0f;
        const float pbY    = (barH - pbH) * 0.5f;

        // WHY: SameLine を挟まないと右端の内容が次の行 = バーの外へ落ちる。
        //      行を保つことで、テキストは AlignTextToFramePadding の基準線に乗ったままになる。
        ImGui::SameLine(0.0f, 0.0f);

        if (compiling || reloading || completed) {
            const char* label = compiling
                ? (ctx.hotReloadMessage.empty() ? "Compiling Scripts..." : ctx.hotReloadMessage.c_str())
                : (ctx.hotReloadMessage.empty() ? "Reloading DLL..."    : ctx.hotReloadMessage.c_str());
            const ImVec4 barCol = completed
                ? EditorTheme::Color(ThemeColor::Success)
                : (compiling ? EditorTheme::Color(ThemeColor::Warning)
                             : EditorTheme::Color(ThemeColor::Info));
            if (rightX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(rightX);
            ImGui::SetCursorPosY(pbY);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barCol);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, EditorTheme::Color(ThemeColor::Field));
            // 進捗が取れないときは掃引アニメーションで「動いている」ことだけ示す。
            // WHY ここで作るか: 以前は Detail Bake のバーと共用していて外側に置いていたが、
            //     そちらが無くなったので、唯一の使い手であるこのブロックへ寄せる。
            const bool  hasProgress = ctx.hotReloadProgress >= 0.0f;
            const float sweep       = fmodf(static_cast<float>(ImGui::GetTime()) * 0.7f, 1.0f);
            const float progress    = hasProgress ? ctx.hotReloadProgress : sweep;
            // コンパイル中は現在コンパイル対象のファイル名を重畳し、擬似進捗を実感のある表示にする。
            const char* curFile = (compiling && ctx.buildConsole && !ctx.buildConsole->CurrentFile().empty())
                                      ? ctx.buildConsole->CurrentFile().c_str() : nullptr;
            char progressLabel[256];
            if (curFile) {
                std::snprintf(progressLabel, sizeof(progressLabel), "%s  %s", label, curFile);
            } else if (hasProgress) {
                std::snprintf(progressLabel, sizeof(progressLabel), "%s %.0f%%", label, progress * 100.0f);
            }
            const bool hasLabel = curFile || hasProgress;
            ImGui::ProgressBar(progress, { pbW, pbH }, hasLabel ? progressLabel : label);
            // クリックで Build Output を開けるようにする。
            if (ImGui::IsItemClicked()) ctx.requestOpenBuildOutput = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to open Build Output");
            ImGui::PopStyleColor(2);
        } else if (reloadText) {
            const float rx = windowW - rightW;
            if (rx > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(rx);
            ImGui::PushStyleColor(ImGuiCol_Text, reloadColor);
            ImGui::TextUnformatted(reloadText);
            ImGui::PopStyleColor();
            // ビルド結果テキストのクリックで Build Output を開く。
            if (latest && ImGui::IsItemClicked())  ctx.requestOpenBuildOutput = true;
            if (latest && ImGui::IsItemHovered())  ImGui::SetTooltip("Click to open Build Output");
        }
    }

    ImGui::EndChild();
}

} // namespace fbzz::editor
