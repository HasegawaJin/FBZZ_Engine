// FBZZ Engine
// ViewportPanel.cpp | fbzz::editor
// IRenderTarget をテクスチャとして表示しカメラ操作を受け付ける
#include <editor/Panels/ViewportPanel.hpp>
#include <editor/EditorContext.hpp>
#include <engine/Renderer/IRenderTarget.hpp>
#include <engine/Scene/Scene.hpp>
#include <imgui.h>

namespace fbzz::editor {

void ViewportPanel::OnRender(EditorContext& ctx)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.0f, 0.0f });
    bool open = ImGui::Begin("Viewport");
    ImGui::PopStyleVar();

    if (!open) { ImGui::End(); return; }

    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 1.0f) size.x = 1.0f;
    if (size.y < 1.0f) size.y = 1.0f;

    ctx.viewportWidth   = size.x;
    ctx.viewportHeight  = size.y;
    ctx.viewportFocused = ImGui::IsWindowFocused();

    if (hdrRT) {
        // ImTextureID は ImU64 (uint64_t)。void* → uintptr_t → uint64_t の順でキャストする
        ImTextureID texID = static_cast<ImTextureID>(
            reinterpret_cast<uintptr_t>(hdrRT->GetNativeSRV(0)));
        ImGui::Image(texID, size);
    } else {
        // レンダーターゲット未接続時のプレースホルダー
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            cursor, { cursor.x + size.x, cursor.y + size.y },
            IM_COL32(30, 30, 30, 255));
        ImGui::SetCursorScreenPos({ cursor.x + size.x * 0.5f - 60.0f,
                                    cursor.y + size.y * 0.5f - 7.0f });
        ImGui::TextDisabled("No Render Target");
    }

    // Stats オーバーレイ (Viewport 右上)
    if (ctx.showSceneStats) {
        int entityCount = 0;
        int meshCount   = 0;
        if (ctx.activeScene) {
            for ([[maybe_unused]] auto& go : ctx.activeScene->GameObjects())
                ++entityCount;
            meshCount = static_cast<int>(ctx.activeScene->GetEntities<scene::MeshRenderer>().size());
        }

        ImVec2 winPos  = ImGui::GetWindowPos();
        ImVec2 winSize = ImGui::GetWindowSize();
        ImGui::SetNextWindowPos({ winPos.x + winSize.x - 8.0f, winPos.y + 8.0f },
                                ImGuiCond_Always, { 1.0f, 0.0f });
        ImGui::SetNextWindowBgAlpha(0.55f);
        constexpr ImGuiWindowFlags kOverlayFlags =
            ImGuiWindowFlags_NoDecoration     |
            ImGuiWindowFlags_NoNav            |
            ImGuiWindowFlags_NoMove           |
            ImGuiWindowFlags_NoSavedSettings  |
            ImGuiWindowFlags_NoDocking        |
            ImGuiWindowFlags_NoInputs         |
            ImGuiWindowFlags_NoFocusOnAppearing;
        if (ImGui::Begin("##vp_stats", nullptr, kOverlayFlags)) {
            ImGui::Text("FPS      %.1f", ImGui::GetIO().Framerate);
            ImGui::Text("Entities %d",   entityCount);
            ImGui::Text("Meshes   %d",   meshCount);
        }
        ImGui::End();
    }

    ImGui::End();
}

} // namespace fbzz::editor
