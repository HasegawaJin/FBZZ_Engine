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
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Editor/Util/TerrainWaterDefaults.hpp>
#include <imgui.h>
#include <cstdio>

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
    ImGui::Spacing();
    ImGui::TextDisabled("  Map");
    {
        const bool active = m_activeTool == Tool::Grid;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, { 0.22f, 0.55f, 0.32f, 1.0f });
        if (ImGui::Button("Grid", { -1.0f, 0.0f })) ActivateTool(ctx, Tool::Grid);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Manage terrain grid layout and cell assignment");
        if (active) ImGui::PopStyleColor();
    }

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
    case Tool::Grid:
        DrawGridContent(ctx);
        break;
    }

    ImGui::EndChild();
}

void MapEditorPanel::DrawGridContent(EditorContext& ctx)
{
    auto& scene = *ctx.activeScene;

    // グリッドコンポーネントを取得または作成する
    auto grids = scene.GetComponents<scene::TerrainGridComponent>();
    scene::TerrainGridComponent* grid = grids.empty() ? nullptr : grids.front();

    if (!grid) {
        ImGui::TextDisabled("No Terrain Grid in scene.");
        if (ImGui::Button("Create Grid Object")) {
            auto& go = scene.CreateGameObject("TerrainGrid");
            scene::TerrainGridComponent tgc;
            tgc.EnsureSize();
            go.AddComponent<scene::TerrainGridComponent>(tgc);
            ctx.markSceneDirty();
            grid = go.GetComponent<scene::TerrainGridComponent>();
        }
        return;
    }

    // グリッドサイズ設定
    ImGui::TextDisabled("Grid Size");
    int cx = grid->cellCountX;
    int cz = grid->cellCountZ;
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputInt("Cols (X)", &cx)) {
        cx = std::max(1, std::min(cx, 16));
        grid->cellCountX = cx;
        grid->EnsureSize();
        ctx.markSceneDirty();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputInt("Rows (Z)", &cz)) {
        cz = std::max(1, std::min(cz, 16));
        grid->cellCountZ = cz;
        grid->EnsureSize();
        ctx.markSceneDirty();
    }

    ImGui::Separator();

    // 2D グリッドビジュアル
    // 各セルをボタンで表示。クリックで選択。
    const float cellPx  = 52.0f; // セルボタンの表示サイズ [px]
    const float spacing = 4.0f;
    ImGui::TextDisabled("Click a cell to select / assign");

    for (int gz = 0; gz < grid->cellCountZ; ++gz) {
        for (int gx = 0; gx < grid->cellCountX; ++gx) {
            if (gx > 0) ImGui::SameLine(0.0f, spacing);

            const scene::EntityID cellId = grid->GetCell(gx, gz);
            const bool hasCell  = scene.IsValid(cellId);
            const bool selected = (m_gridSelectedX == gx && m_gridSelectedZ == gz);

            // セルの色: 選択中=青、割り当て済み=緑、空=グレー
            ImVec4 col = hasCell
                ? (selected ? ImVec4{0.2f,0.45f,0.9f,1.0f} : ImVec4{0.22f,0.55f,0.32f,1.0f})
                : (selected ? ImVec4{0.4f,0.4f,0.8f,1.0f}  : ImVec4{0.3f,0.3f,0.3f,1.0f});

            ImGui::PushStyleColor(ImGuiCol_Button, col);
            char label[32];
            if (hasCell) {
                const auto* go = scene.GetGameObject(cellId);
                std::snprintf(label, sizeof(label), "%s\n(%d,%d)",
                    go ? go->name.c_str() : "?", gx, gz);
            } else {
                std::snprintf(label, sizeof(label), "+\n(%d,%d)", gx, gz);
            }
            ImGui::PushID(gz * 32 + gx);
            if (ImGui::Button(label, { cellPx, cellPx })) {
                m_gridSelectedX = gx;
                m_gridSelectedZ = gz;
            }
            ImGui::PopID();
            ImGui::PopStyleColor();

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::BeginTooltip();
                ImGui::Text("Cell (%d, %d)", gx, gz);
                if (hasCell) {
                    if (const auto* go = scene.GetGameObject(cellId))
                        ImGui::Text("GO: %s", go->name.c_str());
                    ImGui::TextDisabled("Right-click to remove");
                } else {
                    ImGui::TextDisabled("Click to select, then assign below");
                }
                ImGui::EndTooltip();
            }

            // 右クリックでセルをクリア
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && hasCell) {
                grid->ClearCell(gx, gz);
                if (m_gridSelectedX == gx && m_gridSelectedZ == gz) {
                    m_gridSelectedX = m_gridSelectedZ = -1;
                }
                ctx.markSceneDirty();
            }
        }
    }

    ImGui::Separator();

    // 選択セルの操作
    if (m_gridSelectedX < 0 || m_gridSelectedX >= grid->cellCountX ||
        m_gridSelectedZ < 0 || m_gridSelectedZ >= grid->cellCountZ) {
        ImGui::TextDisabled("No cell selected.");
        return;
    }

    ImGui::Text("Selected: (%d, %d)", m_gridSelectedX, m_gridSelectedZ);
    const scene::EntityID selId = grid->GetCell(m_gridSelectedX, m_gridSelectedZ);

    if (scene.IsValid(selId)) {
        // 割り当て済みセルの情報表示
        if (const auto* go = scene.GetGameObject(selId)) {
            ImGui::Text("GO: %s", go->name.c_str());
            if (const auto* tc = scene.GetComponent<scene::TerrainComponent>(selId)) {
                ImGui::Text("Size: %d x %d  cellSize: %.2f",
                    tc->columns, tc->rows, tc->cellSize);
                ImGui::Text("Layer 0: %s",
                    tc->layerMaterials[0].empty() ? "(none)" : tc->layerMaterials[0].c_str());
            }
        }
        if (ImGui::Button("Remove from Grid")) {
            grid->ClearCell(m_gridSelectedX, m_gridSelectedZ);
            ctx.markSceneDirty();
        }
    } else {
        // 空セル: 新規 Terrain 生成 または 既存 GO から選択
        if (ImGui::Button("Add New Terrain Here")) {
            // グリッド上の配置位置を計算する
            // 全セルが同じ cellWorldSize を持つ前提: columns * cellSize
            // 既存セルから推定するか、デフォルト値を使う
            float worldSize = 128.0f;
            for (int i = 0; i < grid->cellCountX * grid->cellCountZ; ++i) {
                if (i < (int)grid->cells.size() && scene.IsValid(grid->cells[i])) {
                    if (const auto* tc = scene.GetComponent<scene::TerrainComponent>(grid->cells[i]))
                        worldSize = static_cast<float>(tc->columns - 1) * tc->cellSize;
                    break;
                }
            }
            const float wx = static_cast<float>(m_gridSelectedX) * worldSize;
            const float wz = static_cast<float>(m_gridSelectedZ) * worldSize;

            char goName[64];
            std::snprintf(goName, sizeof(goName), "Terrain_%d_%d", m_gridSelectedX, m_gridSelectedZ);
            auto& newGo = scene.CreateGameObject(goName);
            newGo.transform.position = { wx, 0.0f, wz };

            scene::TerrainComponent tc;
            tc.InitFlat(0.0f);
            for (int li = 0; li < 4; ++li)
                tc.layerMaterials[li] = DefaultTerrainLayerMaterialPath(li);
            tc.colliderDirty   = true;
            newGo.AddComponent<scene::TerrainComponent>(tc);

            grid->SetCell(m_gridSelectedX, m_gridSelectedZ, newGo.GetID());
            ctx.markSceneDirty();
        }
    }
}

} // namespace fbzz::editor
