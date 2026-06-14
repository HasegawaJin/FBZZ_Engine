// FBZZ Engine
// MapEditorPanel.cpp | fbzz::editor
// Map Editing Mode のツール選択・設定パネル実装
#include <Editor/Panels/MapEditorPanel.hpp>
#include <Editor/EditorContext.hpp>
#include "../Tools/TerrainTool.hpp"
#include "../Tools/WaterTool.hpp"
#include "../Tools/DetailTool.hpp"
#include "../Tools/FoliageTool.hpp"
#include <Engine/Scene/Scene.hpp>
#include <imgui.h>

namespace fbzz::editor {

void MapEditorPanel::ActivateTool(EditorContext& ctx, Tool tool)
{
    m_activeTool = tool;
    if (ctx.terrainTool) {
        const bool terrainActive =
            tool == Tool::TerrainSculpt || tool == Tool::TerrainPaint;
        ctx.terrainTool->SetActive(terrainActive);
        ctx.terrainTool->SetMode(
            tool == Tool::TerrainPaint
                ? TerrainTool::Mode::Paint
                : TerrainTool::Mode::Sculpt);
    }
    if (ctx.waterTool)
        ctx.waterTool->SetActive(tool == Tool::Water);
    if (ctx.detailTool)
        ctx.detailTool->SetActive(tool == Tool::Detail);
    if (ctx.foliageTool)
        ctx.foliageTool->SetActive(tool == Tool::Foliage);
}

void MapEditorPanel::OnRenderContent(EditorContext& ctx)
{
    if (!ctx.mapEditingMode || !ctx.activeScene) {
        ImGui::TextDisabled("Map Editing Mode is not active.");
        return;
    }

    ActivateTool(ctx, m_activeTool);

    // ── ヘッダー: モード表示 + Exit ボタン ──────────────────────────────
    ImGui::TextColored({ 0.35f, 0.88f, 0.48f, 1.0f }, "MAP EDITING MODE");
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 68.0f);
    if (ImGui::SmallButton("Exit Mode"))
        ctx.requestMapEditingModeToggle = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Return to normal editor layout");
    ImGui::Separator();

    // ── ツール選択 ──────────────────────────────────────────────────────
    struct ToolDef { Tool tool; const char* label; const char* tooltip; };

    static constexpr ToolDef k_terrainTools[] = {
        { Tool::TerrainSculpt, "Sculpt", "Raise / lower / smooth terrain height with a brush" },
        { Tool::TerrainPaint,  "Paint",  "Paint splatmap texture layers onto the terrain" },
    };
    static constexpr ToolDef k_envTools[] = {
        { Tool::Water, "Water", "Place and configure water volumes" },
    };
    static constexpr ToolDef k_decorTools[] = {
        { Tool::Detail,  "Detail",  "Scatter detail meshes (grass, rocks) across terrain" },
        { Tool::Foliage, "Foliage", "Paint foliage instances (trees, bushes) onto terrain" },
    };

    auto drawToolButton = [&](const ToolDef& def) {
        const bool active = m_activeTool == def.tool;
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button, { 0.22f, 0.55f, 0.32f, 1.0f });
        if (ImGui::Button(def.label, { -1.0f, 0.0f }))
            ActivateTool(ctx, def.tool);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%s", def.tooltip);
        if (active)
            ImGui::PopStyleColor();
    };

    ImGui::TextDisabled("  Terrain");
    for (const auto& def : k_terrainTools) drawToolButton(def);
    ImGui::Spacing();
    ImGui::TextDisabled("  Environment");
    for (const auto& def : k_envTools) drawToolButton(def);
    ImGui::Spacing();
    ImGui::TextDisabled("  Decoration");
    for (const auto& def : k_decorTools) drawToolButton(def);

    ImGui::Separator();
    ImGui::BeginChild("##MapToolSettings", { 0.0f, 0.0f }, false);

    switch (m_activeTool) {
    case Tool::TerrainSculpt:
        if (ctx.terrainTool) {
            ctx.terrainTool->DrawSculptContent(
                *ctx.activeScene, ctx.undoStack, ctx.markSceneDirty);
            ctx.terrainTool->DrawBrushSettings();
            ctx.terrainTool->DrawImportSection(
                *ctx.activeScene, ctx.undoStack, ctx.markSceneDirty);
        }
        break;
    case Tool::TerrainPaint:
        if (ctx.terrainTool) {
            ctx.terrainTool->DrawPaintContent(
                *ctx.activeScene, ctx.undoStack, ctx.markSceneDirty);
            ctx.terrainTool->DrawBrushSettings();
        }
        break;
    case Tool::Water:
        if (ctx.waterTool) {
            ctx.waterTool->DrawContent(
                *ctx.activeScene, ctx.projectRoot, ctx.markSceneDirty, ctx.undoStack);
        }
        break;
    case Tool::Detail:
        if (ctx.detailTool)
            ctx.detailTool->DrawContent(*ctx.activeScene, ctx.markSceneDirty);
        break;
    case Tool::Foliage:
        if (ctx.foliageTool)
            ctx.foliageTool->DrawContent(*ctx.activeScene);
        break;
    }

    ImGui::EndChild();
}

} // namespace fbzz::editor
