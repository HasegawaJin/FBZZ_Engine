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
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/TerrainAssetSerializer.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/TerrainWaterDefaults.hpp>
#include <imgui.h>
#include <cctype>
#include <cstdio>
#include <string>

namespace fbzz::editor {

namespace {

std::string SanitizeTerrainAssetName(std::string name)
{
    if (name.empty())
        name = "Terrain";

    for (char& c : name) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ';
        if (!ok)
            c = '_';
    }
    return name;
}

std::string UniqueGridTerrainAssetPath(const EditorContext& ctx, const std::string& objectName)
{
    // WHY: Grid Terrain はセル単位で独立編集されるため、Scene に埋め込まず
    //      最初から Assets/Terrain/Grid 配下の外部 .terrain として管理する。
    const std::string assetRoot = ctx.projectRoot.empty()
        ? "Assets"
        : ctx.projectRoot + "/Assets";
    const std::string terrainDir = assetRoot + "/Terrain/Grid";
    util::FileSystem::EnsureDirectory(terrainDir);

    const std::string base = terrainDir + "/" + SanitizeTerrainAssetName(objectName);
    std::string path = base + ".terrain";
    for (int i = 1; util::FileSystem::Exists(path) && i < 10000; ++i)
        path = base + " " + std::to_string(i) + ".terrain";
    return NormalizeAssetPath(path);
}

bool EnsureTerrainAsset(EditorContext& ctx, scene::GameObject& go, scene::TerrainComponent& terrain)
{
    if (!terrain.terrainAssetPath.empty())
        return false;

    const std::string assetPath = UniqueGridTerrainAssetPath(ctx, go.name);
    if (!scene::TerrainAssetSerializer::Save(terrain, ToProjectAssetDiskPath(ctx.projectRoot, assetPath)))
        return false;

    terrain.terrainAssetPath = assetPath;
    ctx.requestAssetBrowserRefresh = true;
    return true;
}

bool EnsureTerrainCollider(scene::Scene& scene, scene::EntityID id)
{
    if (!scene.IsValid(id))
        return false;

    auto* go = scene.GetGameObject(id);
    if (!go || !go->GetComponent<scene::TerrainComponent>())
        return false;

    // WHY: Grid 作成経路で古い Terrain が TerrainColliderComponent を持たない場合がある。
    //      PhysicsSystem は TerrainColliderComponent を入口に HeightFieldCollider を構築するため、
    //      Grid 管理下の Terrain には自動で TerrainCollider を補う。
    if (!go->GetComponent<scene::TerrainColliderComponent>()) {
        go->AddComponent<scene::TerrainColliderComponent>();
        return true;
    }
    return false;
}

bool EnsureTerrainGridColliders(scene::Scene& scene, scene::TerrainGridComponent& grid)
{
    bool changed = false;
    for (const scene::EntityID id : grid.cells) {
        if (EnsureTerrainCollider(scene, id)) {
            if (auto* terrain = scene.GetComponent<scene::TerrainComponent>(id))
                terrain->colliderDirty = true;
            changed = true;
        }
    }
    return changed;
}

bool EnsureTerrainGridAssets(EditorContext& ctx, scene::TerrainGridComponent& grid)
{
    bool changed = false;
    for (const scene::EntityID id : grid.cells) {
        if (!ctx.activeScene || !ctx.activeScene->IsValid(id))
            continue;

        auto* go = ctx.activeScene->GetGameObject(id);
        auto* terrain = ctx.activeScene->GetComponent<scene::TerrainComponent>(id);
        if (!go || !terrain)
            continue;

        if (EnsureTerrainAsset(ctx, *go, *terrain))
            changed = true;
    }
    return changed;
}

void MarkTerrainGridDirty(scene::Scene& scene, scene::TerrainGridComponent& grid)
{
    // WHY: TerrainRenderPass は TerrainGrid の隣接関係を使って境界頂点を補正する。
    //      グリッド編集後に既存チャンクキャッシュが残ると補正が見えないため、
    //      セル内 Terrain を再構築対象にする。
    for (const scene::EntityID id : grid.cells) {
        if (!scene.IsValid(id))
            continue;
        (void)EnsureTerrainCollider(scene, id);
        if (auto* terrain = scene.GetComponent<scene::TerrainComponent>(id)) {
            terrain->heightDirty = true;
            terrain->colliderDirty = true;
        }
    }
}

void MarkTerrainDirty(scene::Scene& scene, scene::EntityID id)
{
    if (!scene.IsValid(id))
        return;
    (void)EnsureTerrainCollider(scene, id);
    if (auto* terrain = scene.GetComponent<scene::TerrainComponent>(id)) {
        terrain->heightDirty = true;
        terrain->colliderDirty = true;
    }
}

// Grid の指定セルへ新規 Terrain を生成して割り当てる。
// WHY: "Add New Terrain Here" と "Fill All Empty Cells" で同じ生成手順を共有するため切り出す。
//      grid / gridObject は EntityID 経由で都度引き直す。CreateGameObject で内部ストレージが
//      再配置されてポインタが無効化されても安全に扱えるようにするため。
scene::EntityID CreateTerrainInGridCell(EditorContext& ctx, scene::Scene& scene,
                                        scene::EntityID gridEntity, int gx, int gz)
{
    auto* grid = scene.GetComponent<scene::TerrainGridComponent>(gridEntity);
    if (!grid)
        return scene::EntityID::INVALID;

    // セルのワールド配置サイズは既存セルがあればそのサイズ、無ければ grid のデフォルトを使う。
    float worldSize = static_cast<float>(grid->defaultColumns - 1) * grid->defaultCellSize;
    for (int i = 0; i < grid->cellCountX * grid->cellCountZ; ++i) {
        if (i < static_cast<int>(grid->cells.size()) && scene.IsValid(grid->cells[i])) {
            if (const auto* tc = scene.GetComponent<scene::TerrainComponent>(grid->cells[i]))
                worldSize = static_cast<float>(tc->columns - 1) * tc->cellSize;
            break;
        }
    }
    const float wx = static_cast<float>(gx) * worldSize;
    const float wz = static_cast<float>(gz) * worldSize;

    char goName[64];
    std::snprintf(goName, sizeof(goName), "Terrain_%d_%d", gx, gz);
    auto& newGo = scene.CreateGameObject(goName);
    newGo.transform.position = { wx, 0.0f, wz };

    // CreateGameObject 後はコンポーネント配列が再配置され得るため引き直す。
    if (auto* gridObject = scene.GetGameObject(gridEntity))
        newGo.SetParent(*gridObject);

    scene::TerrainComponent tc;
    tc.columns   = grid->defaultColumns;
    tc.rows      = grid->defaultRows;
    tc.cellSize  = grid->defaultCellSize;
    tc.chunkSize = grid->defaultChunkSize;
    tc.InitFlat(0.0f);
    for (int li = 0; li < 4; ++li)
        tc.layerMaterials[li] = DefaultTerrainLayerMaterialPath(li);
    tc.heightDirty   = true;
    tc.colliderDirty = true;
    newGo.AddComponent<scene::TerrainComponent>(tc);
    newGo.AddComponent<scene::TerrainColliderComponent>();
    if (auto* newTerrain = newGo.GetComponent<scene::TerrainComponent>())
        (void)EnsureTerrainAsset(ctx, newGo, *newTerrain);

    grid = scene.GetComponent<scene::TerrainGridComponent>(gridEntity);
    if (grid)
        grid->SetCell(gx, gz, newGo.GetID());
    return newGo.GetID();
}

} // namespace

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

    // 現在アクティブなツールとサブモードを常時表示する。
    // WHY: ツール自体はボタンのハイライトで分かるが、Sculpt のサブモード(Raise 等)や
    //      Paint のレイヤー番号は設定欄を見ないと分からない。「今どの操作中か」を1行で示し、
    //      ビューポートとパネルを視線往復せず把握できるようにする。
    {
        const char* toolName = "-";
        switch (m_activeTool) {
        case Tool::TerrainSculpt: toolName = "Terrain Sculpt"; break;
        case Tool::TerrainPaint:  toolName = "Terrain Paint";  break;
        case Tool::Water:         toolName = "Water";          break;
        case Tool::Detail:        toolName = "Detail";         break;
        case Tool::Foliage:       toolName = "Foliage";        break;
        case Tool::Grid:          toolName = "Grid";           break;
        }
        std::string status = toolName;
        if (ctx.terrainTool) {
            if (m_activeTool == Tool::TerrainSculpt) {
                static const char* kSub[] = { "Raise", "Lower", "Smooth", "Flatten", "Stamp" };
                status += "  -  ";
                status += kSub[static_cast<int>(ctx.terrainTool->GetSculptMode())];
            } else if (m_activeTool == Tool::TerrainPaint) {
                status += "  -  Layer " + std::to_string(ctx.terrainTool->GetPaintLayer());
            }
        }
        ImGui::TextDisabled("Active:");
        ImGui::SameLine();
        ImGui::TextColored({ 0.95f, 0.85f, 0.4f, 1.0f }, "%s", status.c_str());
        ImGui::Separator();
    }

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
            ctx.foliageTool->DrawContent(*ctx.activeScene, ctx.markSceneDirty);
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

    // グリッドコンポーネントとそれを所有する Entity を取得する。
    // WHY: Grid から生成する Terrain は CreateTerrainInGridCell 内で gridEntity 経由に
    //      親へぶら下げる。ここでは安定した EntityID だけ保持し、ポインタは都度引き直す。
    const auto gridEntities = scene.GetEntities<scene::TerrainGridComponent>();
    const scene::EntityID gridEntity = gridEntities.empty()
        ? scene::EntityID::INVALID
        : gridEntities.front();
    scene::TerrainGridComponent* grid = scene.IsValid(gridEntity)
        ? scene.GetComponent<scene::TerrainGridComponent>(gridEntity)
        : nullptr;

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
    if (EnsureTerrainGridColliders(scene, *grid))
        ctx.markSceneDirty();
    if (EnsureTerrainGridAssets(ctx, *grid))
        ctx.markSceneDirty();

    // グリッドサイズ設定
    ImGui::TextDisabled("Grid Size");
    int cx = grid->cellCountX;
    int cz = grid->cellCountZ;
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputInt("Cols (X)", &cx)) {
        cx = std::max(1, std::min(cx, 16));
        grid->cellCountX = cx;
        grid->EnsureSize();
        MarkTerrainGridDirty(scene, *grid);
        ctx.markSceneDirty();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputInt("Rows (Z)", &cz)) {
        cz = std::max(1, std::min(cz, 16));
        grid->cellCountZ = cz;
        grid->EnsureSize();
        MarkTerrainGridDirty(scene, *grid);
        ctx.markSceneDirty();
    }

    // 空セルを一括で埋める。WHY: 大きな Grid を1セルずつ "Add New Terrain Here" で
    //      埋めるのは手数が多く面倒なため、空セルへまとめてフラット Terrain を生成する。
    if (ImGui::Button("Fill All Empty Cells", { -1.0f, 0.0f })) {
        int created = 0;
        for (int z = 0; z < grid->cellCountZ; ++z)
            for (int x = 0; x < grid->cellCountX; ++x)
                if (!scene.IsValid(grid->GetCell(x, z))) {
                    CreateTerrainInGridCell(ctx, scene, gridEntity, x, z);
                    ++created;
                }
        if (created > 0) {
            MarkTerrainGridDirty(scene, *grid);
            ctx.markSceneDirty();
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Create a flat Terrain in every empty grid cell");

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
            ImGui::PopStyleColor();

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::BeginTooltip();
                ImGui::Text("Cell (%d, %d)", gx, gz);
                if (hasCell) {
                    if (const auto* go = scene.GetGameObject(cellId))
                        ImGui::Text("GO: %s", go->name.c_str());
                    ImGui::TextDisabled("Right-click for actions");
                } else {
                    ImGui::TextDisabled("Click to select, then assign below");
                }
                ImGui::EndTooltip();
            }

            // 右クリックでセル操作メニューを開く。
            // WHY: 旧実装は右クリックで即セルをクリアしていたが、選択や確認の間もなく
            //      割り当て済み Terrain を外してしまい誤操作が多かった。メニュー経由にして
            //      Focus / Select / Remove を明示的に選べるようにする。
            if (hasCell && ImGui::BeginPopupContextItem("cell_ctx")) {
                m_gridSelectedX = gx;
                m_gridSelectedZ = gz;
                if (const auto* go = scene.GetGameObject(cellId))
                    ImGui::TextDisabled("%s", go->name.c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Focus Camera")) {
                    if (const auto* go = scene.GetGameObject(cellId)) {
                        ctx.focusTargetPosition    = go->transform.worldPosition;
                        ctx.requestFocusOnSelected = true;
                    }
                }
                if (ImGui::MenuItem("Select in Hierarchy"))
                    ctx.selectedEntities = { cellId };
                ImGui::Separator();
                if (ImGui::MenuItem("Remove from Grid")) {
                    MarkTerrainDirty(scene, cellId);
                    grid->ClearCell(gx, gz);
                    MarkTerrainGridDirty(scene, *grid);
                    if (m_gridSelectedX == gx && m_gridSelectedZ == gz)
                        m_gridSelectedX = m_gridSelectedZ = -1;
                    ctx.markSceneDirty();
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
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
            const auto& p = go->transform.worldPosition;
            ImGui::Text("World: %.1f, %.1f, %.1f", p.x, p.y, p.z);
        }
        // 4 近傍のうち割り当て済みのセル数。継ぎ目処理の対象がどれだけ揃っているかの目安。
        int neighborCount = 0;
        const int dirs[4][2] = { {-1,0}, {1,0}, {0,-1}, {0,1} };
        for (auto& d : dirs)
            if (scene.IsValid(grid->GetCell(m_gridSelectedX + d[0], m_gridSelectedZ + d[1])))
                ++neighborCount;
        ImGui::Text("Assigned neighbors: %d / 4", neighborCount);

        if (ImGui::Button("Remove from Grid")) {
            MarkTerrainDirty(scene, selId);
            grid->ClearCell(m_gridSelectedX, m_gridSelectedZ);
            MarkTerrainGridDirty(scene, *grid);
            ctx.markSceneDirty();
        }
    } else {
        // 空セル: 新規 Terrain 生成 または 既存 GO から選択
        if (ImGui::Button("Add New Terrain Here", { -1.0f, 0.0f })) {
            CreateTerrainInGridCell(ctx, scene, gridEntity, m_gridSelectedX, m_gridSelectedZ);
            MarkTerrainGridDirty(scene, *grid);
            ctx.markSceneDirty();
        }
    }
}

} // namespace fbzz::editor
