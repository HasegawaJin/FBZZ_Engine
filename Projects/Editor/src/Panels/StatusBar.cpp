// FBZZ Engine
// StatusBar.cpp | fbzz::editor
// ウィンドウ最下部に固定表示される情報バー
#include <Editor/Panels/StatusBar.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <cstdio>
#include <string>

namespace fbzz::editor {

ImGuiWindowFlags StatusBar::GetWindowFlags() const
{
    return ImGuiWindowFlags_NoDecoration
         | ImGuiWindowFlags_NoNav
         | ImGuiWindowFlags_NoMove
         | ImGuiWindowFlags_NoScrollWithMouse
         | ImGuiWindowFlags_NoBringToFrontOnFocus
         | ImGuiWindowFlags_NoDocking;
}

void StatusBar::OnBeforeBegin(EditorContext& /*ctx*/)
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float barHeight = ImGui::GetFrameHeight() + 4.0f;
    ImGui::SetNextWindowPos({ vp->Pos.x, vp->Pos.y + vp->Size.y - barHeight });
    ImGui::SetNextWindowSize({ vp->Size.x, barHeight });
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 4.0f, 2.0f });
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
}

void StatusBar::OnAfterBegin(EditorContext& /*ctx*/)
{
    ImGui::PopStyleVar(2);
}

static void Separator()
{
    ImGui::SameLine();
    ImGui::TextDisabled(" | ");
    ImGui::SameLine();
}

void StatusBar::OnRenderContent(EditorContext& ctx)
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
        Separator();
    }

    // ── FPS ─────────────────────────────────────────────────────────
    const std::string sceneName = ctx.currentScenePath.empty()
        ? "Untitled"
        : util::FileSystem::GetFilename(ctx.currentScenePath);
    ImGui::Text("Scene: %s%s", sceneName.c_str(), ctx.sceneDirty ? "*" : "");
    Separator();

    ImGui::Text("FPS: %.1f  (%.2f ms)", m_fps, ms);

    // ── シーン統計 ────────────────────────────────────────────────────
    if (ctx.activeScene) {
        const int objCount   = static_cast<int>(ctx.activeScene->GameObjectCount());
        const int meshCount  = static_cast<int>(ctx.activeScene->GetComponents<scene::MeshRenderer>().size());
        const int lightCount = static_cast<int>(ctx.activeScene->GetComponents<scene::LightComponent>().size());

        Separator();
        ImGui::Text("Obj: %d  Mesh: %d  Light: %d", objCount, meshCount, lightCount);
    }

    // ── カメラ座標 ────────────────────────────────────────────────────
    if (ctx.editorCamera) {
        const auto& p = ctx.editorCamera->m_position;
        Separator();
        ImGui::Text("Cam: (%.1f, %.1f, %.1f)", p.x, p.y, p.z);
    }

    // ── 選択オブジェクト ──────────────────────────────────────────────
    Separator();
    const char* selName = "None";
    if (auto* go = ctx.GetSelectedGO())
        selName = go->name.c_str();
    ImGui::Text("Sel: %s", selName);

    // ── ホットリロード状態 + 手動リロードボタン (右端) ──────────────────
    {
        const char* reloadText = nullptr;
        ImVec4      reloadColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        switch (ctx.hotReloadState) {
        case EditorContext::HotReloadState::Compiling:
            reloadText  = ctx.hotReloadMessage.empty() ? "Compiling..." : ctx.hotReloadMessage.c_str();
            reloadColor = { 1.0f, 0.85f, 0.2f, 1.0f };  // 黄
            break;
        case EditorContext::HotReloadState::Reloading:
            reloadText  = ctx.hotReloadMessage.empty() ? "Reloading..." : ctx.hotReloadMessage.c_str();
            reloadColor = { 0.5f, 0.8f, 1.0f, 1.0f };   // 水色
            break;
        case EditorContext::HotReloadState::Done:
            reloadText  = ctx.hotReloadMessage.empty() ? "Reload OK" : ctx.hotReloadMessage.c_str();
            reloadColor = { 0.35f, 1.0f, 0.45f, 1.0f };  // 緑
            break;
        case EditorContext::HotReloadState::Failed:
            reloadText  = ctx.hotReloadMessage.empty() ? "Reload Failed" : ctx.hotReloadMessage.c_str();
            reloadColor = { 1.0f, 0.35f, 0.35f, 1.0f };  // 赤
            break;
        default: break;
        }

        if (reloadText) {
            const float msgW   = ImGui::CalcTextSize(reloadText).x + 8.0f;
            const float rightX = ImGui::GetWindowWidth() - msgW;
            if (rightX > ImGui::GetCursorPosX())
                ImGui::SetCursorPosX(rightX);
            ImGui::PushStyleColor(ImGuiCol_Text, reloadColor);
            ImGui::TextUnformatted(reloadText);
            ImGui::PopStyleColor();
        } else if (!m_message.empty()) {
            const float msgW   = ImGui::CalcTextSize(m_message.c_str()).x + 8.0f;
            const float rightX = ImGui::GetWindowWidth() - msgW;
            if (rightX > ImGui::GetCursorPosX())
                ImGui::SetCursorPosX(rightX);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.85f, 1.0f, 1.0f));
            ImGui::TextUnformatted(m_message.c_str());
            ImGui::PopStyleColor();
        }
    }
}

} // namespace fbzz::editor
