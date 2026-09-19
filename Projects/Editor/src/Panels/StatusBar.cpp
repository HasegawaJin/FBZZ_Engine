/// @file    StatusBar.cpp
/// @brief   DockSpaceHost の画面下端でインライン描画される情報バー。
/// @author  Hasegawa Jin
/// @date    2026-05-21
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

/// 直前に描いた項目の右端 (ウィンドウローカル X)。バーは 1 行なので、これが次の項目の開始位置になる。
float LastItemEndX()
{
    return ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
}

} // namespace

/// 下端ドロワーの開閉ボタン。開いている間は押下色にして状態を一目で分かるようにする。
/// @note どのレイアウトからでも同じ位置で開閉できることに価値がある (Unreal の Content Drawer 同様)。
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
    /// @note FPS / フレームタイム
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

    /// @note 行の基準は枠付きウィジェット (Checkbox / DragFloat) の高さに合わせる。テキスト基準だと
    ///       枠の下端がバー下端をはみ出す。AlignTextToFramePadding でテキストをこの行に合わせ直す。
    const float rowY = (barH - ImGui::GetFrameHeight()) * 0.5f;
    ImGui::SetCursorPosY(rowY);
    ImGui::AlignTextToFramePadding();

    /// @note 右端に置く内容を描画前に決めておく。左から流し込む情報の打ち切り位置は右端領域と
    ///       Snap 群の幅が確定しないと判定できず、しないとウィンドウが狭いとき Snap が外へ押し出される。
    const float pbW       = 280.0f;
    const auto& hrs       = ctx.hotReloadState;
    const bool  compiling = (hrs == EditorContext::HotReloadState::Compiling);
    const bool  reloading = (hrs == EditorContext::HotReloadState::Reloading);
    const bool  completed = (hrs == EditorContext::HotReloadState::Done && ctx.hotReloadProgress >= 1.0f);
    const bool  failed    = (hrs == EditorContext::HotReloadState::Failed && !ctx.hotReloadMessage.empty());

    const bool showProgress = compiling || reloading || completed;
    /// @note 確定までは今の時刻、確定後は FinishTime で止めた経過秒。
    const double elapsedSec = (compiling || reloading ? ImGui::GetTime() : ctx.hotReloadFinishTime)
                              - ctx.hotReloadStartTime;

    /// @note 恒常表示: 直近ビルドの結果を「消さずに」出す。従来は Done/Failed が数秒で消えて
    ///       ビルド状況を後から確認できなかったため、BuildConsole の最新レコードを常時表示する。
    const BuildRecord* latest       = ctx.buildConsole ? ctx.buildConsole->Latest() : nullptr;
    const char*        reloadText   = nullptr;
    ImVec4             reloadColor  = EditorTheme::Color(ThemeColor::Text);
    char               summary[160] = {};
    if (!showProgress) {
        if (!m_message.empty()) {
            /// @note 一時的な操作メッセージ (保存など) を最優先で表示する。
            reloadText  = m_message.c_str();
            reloadColor = EditorTheme::Color(ThemeColor::Info);
        } else if (failed) {
            /// @note ビルド記録より先に見る。コンパイル成功後のロード失敗は記録上 «成功» なので、記録だけだと OK と出る。
            reloadText  = ctx.hotReloadMessage.c_str();
            reloadColor = EditorTheme::Color(ThemeColor::Danger);
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

    /// @name 下端ドロワー
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

    /// @name シーン名 + 未保存インジケーター
    /// @note 未保存状態はタイトルバーの * と AssetBrowser の Save* (N) に分散していたため、ここへ
    ///       シーン未保存の橙ドットと未保存アセット件数を集約する。
    const std::string sceneName = ctx.currentScenePath.empty()
        ? "Untitled"
        : util::FileSystem::GetFilename(ctx.currentScenePath);
    ImGui::Text("Scene: %s", sceneName.c_str());
    /// @note Play 中に LoadScene で移った先は「今走っているシーン」であって編集対象ではない。
    ///       名前を並べておかないと、遷移したこと自体が画面のどこにも出ない。
    if (!ctx.playSceneName.empty()) {
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Success), "\xE2\x96\xB6 %s",
                           ctx.playSceneName.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Running scene (loaded during play) \xe2\x80\x94 "
                              "saving still targets %s, so it is refused until you stop",
                              sceneName.c_str());
    }
    if (ctx.sceneDirty) {
        ImGui::SameLine(0.0f, 4.0f);
        /// @note ● 未保存
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning), "\xE2\x97\x8F");
        /// @note クリックで即保存できるようにする (タイトルバー * と StatusBar 表示の導線を一致させる)。
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

    /// @note 選択名は長さが読めないため先に測り、シーン統計やカメラ座標より優先で場所を確保する。
    const char* selName = "None";
    if (auto* go = ctx.GetSelectedGO())
        selName = go->name.c_str();
    char selText[160];
    std::snprintf(selText, sizeof(selText), "Sel: %s", selName);
    const float selW = SepWidth() + ImGui::CalcTextSize(selText).x;

    /// @note 残り幅に入らない項目は落とす。reserved は「この項目より優先する後続項目」の幅。
    auto DrawIfFits = [leftBudget](const char* text, float reserved) {
        if (LastItemEndX() + SepWidth() + ImGui::CalcTextSize(text).x + reserved > leftBudget)
            return;
        Sep();
        ImGui::TextUnformatted(text);
    };

    /// @name シーン統計
    if (ctx.activeScene) {
        const int objCount   = static_cast<int>(ctx.activeScene->GameObjectCount());
        const int meshCount  = static_cast<int>(ctx.activeScene->GetComponents<scene::MeshRenderer>().size());
        const int lightCount = static_cast<int>(ctx.activeScene->GetComponents<scene::LightComponent>().size());
        char stats[96];
        std::snprintf(stats, sizeof(stats), "Obj: %d  Mesh: %d  Light: %d", objCount, meshCount, lightCount);
        DrawIfFits(stats, selW);
    }

    /// @name カメラ座標
    if (ctx.editorCamera) {
        const auto& p = ctx.editorCamera->m_position;
        char cam[64];
        std::snprintf(cam, sizeof(cam), "Cam: (%.1f, %.1f, %.1f)", p.x, p.y, p.z);
        DrawIfFits(cam, selW);
    }

    /// @name 選択オブジェクト
    DrawIfFits(selText, 0.0f);

    /// @name スナップ設定
    Sep();
    ImGui::PushStyleColor(ImGuiCol_Text,
        ctx.snapEnabled ? EditorTheme::Color(ThemeColor::Accent)
                        : EditorTheme::Color(ThemeColor::TextMuted));
    ImGui::Checkbox("Snap", &ctx.snapEnabled);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Toggle gizmo snapping\nRight-click each value for presets");
    /// @note 左側を削っても入力欄 3 つ分が残らない極端な幅では、チェックボックスだけ残す。
    const bool roomForSnapFields = (windowW - rightW - LastItemEndX()) >= SnapFieldsWidth();
    if (ctx.snapEnabled && roomForSnapFields) {
        /// @note Position
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
        /// @note Rotation
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
        /// @note Scale
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

    /// @name ホットリロード状態（右端）
    {
        const float rightX = windowW - pbW - 4.0f;
        const float pbH    = barH - 4.0f;
        const float pbY    = (barH - pbH) * 0.5f;

        /// @note SameLine を挟まないと右端の内容が次の行 = バーの外へ落ちる。
        ImGui::SameLine(0.0f, 0.0f);

        if (compiling || reloading || completed) {
            const bool  shaders = ctx.hotReloadTarget == EditorContext::HotReloadTarget::Shaders;
            const char* label = completed ? (shaders ? "Shaders reloaded" : "Scripts reloaded")
                              : reloading ? "Reloading scripts..."
                              : (shaders ? "Compiling shaders..." : "Compiling scripts...");
            const ImVec4 barCol = completed
                ? EditorTheme::Color(ThemeColor::Success)
                : (compiling ? EditorTheme::Color(ThemeColor::Warning)
                             : EditorTheme::Color(ThemeColor::Info));
            if (rightX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(rightX);
            ImGui::SetCursorPosY(pbY);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barCol);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, EditorTheme::Color(ThemeColor::Field));
            /// @note 進捗が取れないときは掃引アニメーションで「動いている」ことだけ示す。唯一の
            ///       使い手であるこのブロックへ寄せる (Detail Bake との共用は廃止済み)。
            const bool  hasProgress = ctx.hotReloadProgress >= 0.0f;
            const float sweep       = fmodf(static_cast<float>(ImGui::GetTime()) * 0.7f, 1.0f);
            const float progress    = hasProgress ? ctx.hotReloadProgress : sweep;
            /// @note 擬似進捗の % は出さない (MSBuild は総数を返さず 89% で止まって見える)。代わりに経過秒と現在ファイルを出す。
            const char* curFile = (compiling && ctx.buildConsole && !ctx.buildConsole->CurrentFile().empty())
                                      ? ctx.buildConsole->CurrentFile().c_str() : nullptr;
            char progressLabel[256];
            if (curFile)
                std::snprintf(progressLabel, sizeof(progressLabel), "%s %.1fs  %s", label, elapsedSec, curFile);
            else
                std::snprintf(progressLabel, sizeof(progressLabel), "%s %.1fs", label, elapsedSec);
            ImGui::ProgressBar(progress, { pbW, pbH }, progressLabel);
            /// @note クリックで Build Output を開けるようにする。
            if (ImGui::IsItemClicked()) ctx.requestOpenBuildOutput = true;
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                if (!ctx.hotReloadMessage.empty()) ImGui::TextUnformatted(ctx.hotReloadMessage.c_str());
                if (curFile) ImGui::Text("Current: %s", curFile);
                if (compiling || reloading)
                    ImGui::TextDisabled("Play is disabled until the reload finishes");
                ImGui::TextDisabled("Click to open Build Output");
                ImGui::EndTooltip();
            }
            ImGui::PopStyleColor(2);
        } else if (reloadText) {
            const float rx = windowW - rightW;
            if (rx > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(rx);
            ImGui::PushStyleColor(ImGuiCol_Text, reloadColor);
            ImGui::TextUnformatted(reloadText);
            ImGui::PopStyleColor();
            /// @note ビルド結果テキストのクリックで Build Output を開く。
            const bool opensBuildOutput = latest || failed;
            if (opensBuildOutput && ImGui::IsItemClicked())  ctx.requestOpenBuildOutput = true;
            if (opensBuildOutput && ImGui::IsItemHovered())  ImGui::SetTooltip("Click to open Build Output");
        }
    }

    ImGui::EndChild();
}

} // namespace fbzz::editor
