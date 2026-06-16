// FBZZ Engine
// StatusBar.cpp | fbzz::editor
// DockSpaceHost 内でインライン描画される情報バー
#include <Editor/Panels/StatusBar.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/TerrainDetailComponent.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <cstdio>
#include <string>

namespace fbzz::editor {

namespace {
void Sep()
{
    ImGui::SameLine();
    ImGui::TextDisabled(" | ");
    ImGui::SameLine();
}
} // namespace

void StatusBar::Draw(EditorContext& ctx)
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
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.13f, 0.13f, 1.0f));
    ImGui::BeginChild("##statusbar_strip", { 0.0f, barH }, false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImGui::SetCursorPosY((barH - ImGui::GetTextLineHeight()) * 0.5f);

    // ── プレイ状態 ────────────────────────────────────────────────────
    if (ctx.playMode) {
        switch (ctx.playMode->GetState()) {
        case PlayState::Playing:
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.95f, 0.45f, 1.0f));
            ImGui::TextUnformatted("  PLAYING ");
            ImGui::PopStyleColor();
            break;
        case PlayState::Paused:
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.15f, 1.0f));
            ImGui::TextUnformatted("  PAUSED ");
            ImGui::PopStyleColor();
            break;
        default:
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
            ImGui::TextUnformatted("  EDITOR ");
            ImGui::PopStyleColor();
            break;
        }
        Sep();
    }

    // ── シーン名 + FPS ────────────────────────────────────────────────
    const std::string sceneName = ctx.currentScenePath.empty()
        ? "Untitled"
        : util::FileSystem::GetFilename(ctx.currentScenePath);
    ImGui::Text("Scene: %s%s", sceneName.c_str(), ctx.sceneDirty ? "*" : "");
    Sep();
    ImGui::Text("FPS: %.1f  (%.2f ms)", m_fps, ms);

    // ── シーン統計 ────────────────────────────────────────────────────
    if (ctx.activeScene) {
        const int objCount   = static_cast<int>(ctx.activeScene->GameObjectCount());
        const int meshCount  = static_cast<int>(ctx.activeScene->GetComponents<scene::MeshRenderer>().size());
        const int lightCount = static_cast<int>(ctx.activeScene->GetComponents<scene::LightComponent>().size());
        Sep();
        ImGui::Text("Obj: %d  Mesh: %d  Light: %d", objCount, meshCount, lightCount);
    }

    // ── カメラ座標 ────────────────────────────────────────────────────
    if (ctx.editorCamera) {
        const auto& p = ctx.editorCamera->m_position;
        Sep();
        ImGui::Text("Cam: (%.1f, %.1f, %.1f)", p.x, p.y, p.z);
    }

    // ── 選択オブジェクト ──────────────────────────────────────────────
    Sep();
    const char* selName = "None";
    if (auto* go = ctx.GetSelectedGO())
        selName = go->name.c_str();
    ImGui::Text("Sel: %s", selName);

    // ── スナップ設定 ──────────────────────────────────────────────────
    Sep();
    ImGui::PushStyleColor(ImGuiCol_Text,
        ctx.snapEnabled ? ImVec4(0.4f, 0.9f, 1.0f, 1.0f) : ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
    ImGui::Checkbox("Snap", &ctx.snapEnabled);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Toggle gizmo snapping\nRight-click each value for presets");
    if (ctx.snapEnabled) {
        // Position
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextDisabled("Pos");
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::SetNextItemWidth(48.0f);
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
        ImGui::SetNextItemWidth(48.0f);
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
        ImGui::SetNextItemWidth(48.0f);
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

    // ── ホットリロード / Detail Bake 状態（右端）─────────────────────
    {
        const float pbW    = 180.0f;
        const float rightX = ImGui::GetWindowWidth() - pbW - 4.0f;
        const float t      = fmodf(static_cast<float>(ImGui::GetTime()) * 0.7f, 1.0f);

        const auto& hrs      = ctx.hotReloadState;
        const bool compiling = (hrs == EditorContext::HotReloadState::Compiling);
        const bool reloading = (hrs == EditorContext::HotReloadState::Reloading);

        bool detailBaking = false;
        if (!compiling && !reloading && ctx.activeScene) {
            for (scene::EntityID eid : ctx.activeScene->GetEntities<scene::TerrainDetailComponent>()) {
                auto* tdc     = ctx.activeScene->GetComponent<scene::TerrainDetailComponent>(eid);
                auto* terrain = ctx.activeScene->GetComponent<scene::TerrainComponent>(eid);
                if (tdc && terrain && tdc->enabled && !tdc->layers.empty()
                    && terrain->enabled && !terrain->heightData.empty() && tdc->needsBake) {
                    detailBaking = true;
                    break;
                }
            }
        }

        if (compiling || reloading) {
            const char* label = compiling
                ? (ctx.hotReloadMessage.empty() ? "Compiling Scripts..." : ctx.hotReloadMessage.c_str())
                : (ctx.hotReloadMessage.empty() ? "Reloading DLL..."    : ctx.hotReloadMessage.c_str());
            const ImVec4 barCol = compiling
                ? ImVec4(0.85f, 0.65f, 0.05f, 1.0f)
                : ImVec4(0.25f, 0.60f, 0.95f, 1.0f);
            if (rightX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(rightX);
            ImGui::SetCursorPosY(1.0f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barCol);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
            ImGui::ProgressBar(t, { pbW, barH - 4.0f }, label);
            ImGui::PopStyleColor(2);
        } else if (detailBaking) {
            if (rightX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(rightX);
            ImGui::SetCursorPosY(1.0f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.30f, 0.75f, 0.40f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
            ImGui::ProgressBar(t, { pbW, barH - 4.0f }, "Baking Detail...");
            ImGui::PopStyleColor(2);
        } else {
            const char* reloadText  = nullptr;
            ImVec4      reloadColor = { 1.0f, 1.0f, 1.0f, 1.0f };
            if (hrs == EditorContext::HotReloadState::Done) {
                reloadText  = ctx.hotReloadMessage.empty() ? "Reload OK" : ctx.hotReloadMessage.c_str();
                reloadColor = { 0.35f, 1.0f, 0.45f, 1.0f };
            } else if (hrs == EditorContext::HotReloadState::Failed) {
                reloadText  = ctx.hotReloadMessage.empty() ? "Reload Failed" : ctx.hotReloadMessage.c_str();
                reloadColor = { 1.0f, 0.35f, 0.35f, 1.0f };
            } else if (!m_message.empty()) {
                reloadText  = m_message.c_str();
                reloadColor = { 0.6f, 0.85f, 1.0f, 1.0f };
            }
            if (reloadText) {
                const float msgW = ImGui::CalcTextSize(reloadText).x + 8.0f;
                const float rx   = ImGui::GetWindowWidth() - msgW;
                if (rx > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(rx);
                ImGui::PushStyleColor(ImGuiCol_Text, reloadColor);
                ImGui::TextUnformatted(reloadText);
                ImGui::PopStyleColor();
            }
        }
    }

    ImGui::EndChild();
}

} // namespace fbzz::editor
