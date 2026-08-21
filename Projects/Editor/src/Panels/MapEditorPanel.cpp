// FBZZ Engine
// MapEditorPanel.cpp | fbzz::editor
// Map Editing Mode のツール選択・設定パネル実装
#include <Editor/Panels/MapEditorPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include "MapToolCommon.hpp"
#include <Engine/Renderer/Camera.hpp>
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
#include <algorithm>
#include <cctype>
#include <cmath>
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

// セル矩形の内側に Terrain のハイトマップを粗いグレースケールで描く簡易ミニマップ。
// WHY: 色付きボタンだけでは「どのセルがどの地形か」が名前ツールチップ頼りだった。
//      12x12 サンプルの矩形塗りならテクスチャ基盤なしで一目で山谷が分かる。
void DrawCellHeightPreview(ImDrawList* draw, const ImVec2& rectMin, const ImVec2& rectMax,
                           const scene::TerrainComponent& tc)
{
    constexpr int kSamples = 12;
    if (tc.heightData.empty() || tc.columns < 2 || tc.rows < 2)
        return;
    const float cellW = (rectMax.x - rectMin.x) / kSamples;
    const float cellH = (rectMax.y - rectMin.y) / kSamples;
    for (int sz = 0; sz < kSamples; ++sz) {
        for (int sx = 0; sx < kSamples; ++sx) {
            const int col = (tc.columns - 1) * sx / (kSamples - 1);
            const int row = (tc.rows - 1) * sz / (kSamples - 1);
            const float h = tc.heightData[static_cast<size_t>(row) * tc.columns + col]; // [-1, 1]
            // 高さ 0 が中間の緑になるよう [-1,1] → [0,1] へマップし、地形らしい緑系で明暗を付ける
            const float t = std::clamp(h * 0.5f + 0.5f, 0.0f, 1.0f);
            const int r = static_cast<int>(40.0f + 150.0f * t);
            const int g = static_cast<int>(70.0f + 160.0f * t);
            const int b = static_cast<int>(45.0f + 110.0f * t);
            draw->AddRectFilled({ rectMin.x + sx * cellW, rectMin.y + sz * cellH },
                                { rectMin.x + (sx + 1) * cellW, rectMin.y + (sz + 1) * cellH },
                                IM_COL32(r, g, b, 255));
        }
    }
}

} // namespace

void MapEditorPanel::OnRenderContent(EditorContext& ctx)
{
    DrawEditorToolbar(ctx, "World / Terrain");
    ImGui::Separator();
    if (!ctx.mapEditingMode || !ctx.activeScene) {
        // WHY: 旧実装は「モードが無効」と表示するだけの死に画面で、入る手段が別メニューだった。
        //      その場で入れるボタンを置き、モードへの導線をパネル内で完結させる。
        ImGui::TextDisabled("Map Editing Mode is not active.");
        ImGui::Spacing();
        // 実行可否と実体は operator が持つ。ここでフラグを直に立てると、
        // メニュー / ツールバーが従っている条件 (Play 中は不可) を素通りする。
        if (CanInvokeOperator(ctx, "tools.map_editing_mode")
            && ImGui::Button("Enter Map Editing Mode", { -1.0f, 0.0f }))
            InvokeOperator(ctx, "tools.map_editing_mode");
        if (!ctx.activeScene)
            ImGui::TextDisabled("Open a scene first.");
        return;
    }

    // ツール実体のアクティブ状態を毎フレーム同期する。
    // (ビューポートの数字キー / オーバーレイからも ctx.mapActiveTool が書き換わるため)
    ActivateMapTool(ctx, ctx.mapActiveTool);

    // ── ヘッダー: モード表示 + Exit ボタン ──────────────────────────────
    ImGui::TextColored({ 0.35f, 0.88f, 0.48f, 1.0f }, "MAP EDITING MODE");
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 68.0f);
    if (ImGui::SmallButton("Exit Mode"))
        InvokeOperator(ctx, "tools.map_editing_mode");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Return to normal editor layout");
    ImGui::Separator();

    // 現在アクティブなツールとサブモードを常時表示する。
    // WHY: Sculpt のサブモード(Raise 等)や Paint のレイヤー番号は設定欄を見ないと分からない。
    //      「今どの操作中か」を1行で示し、ビューポートとパネルを視線往復せず把握できるようにする。
    {
        const char* toolName = "-";
        for (const MapToolDef& def : kMapToolDefs)
            if (def.tool == ctx.mapActiveTool)
                toolName = def.label;
        std::string status = toolName;
        if (ctx.terrainTool) {
            if (ctx.mapActiveTool == EditorContext::MapTool::TerrainSculpt) {
                static const char* kSub[] = { "Raise", "Lower", "Smooth", "Flatten", "Stamp" };
                status += "  -  ";
                status += kSub[static_cast<int>(ctx.terrainTool->GetSculptMode())];
            } else if (ctx.mapActiveTool == EditorContext::MapTool::TerrainPaint) {
                status += "  -  Layer " + std::to_string(ctx.terrainTool->GetPaintLayer());
            }
        }
        ImGui::TextDisabled("Active:");
        ImGui::SameLine();
        ImGui::TextColored({ 0.95f, 0.85f, 0.4f, 1.0f }, "%s", status.c_str());
    }

    // ── ツール選択: 3列のコンパクトグリッド ─────────────────────────────
    // WHY: 旧実装の全幅縦積みボタンはツール6個で画面の1/3を占有し、肝心の
    //      ブラシ設定・レイヤー選択が下へ押し出されていた。3列に畳んで設定領域を最大化する。
    {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float buttonW = (ImGui::GetContentRegionAvail().x - spacing * 2.0f) / 3.0f;
        int column = 0;
        for (const MapToolDef& def : kMapToolDefs) {
            if (column > 0) ImGui::SameLine();
            const bool active = ctx.mapActiveTool == def.tool;
            if (active)
        ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(ThemeColor::Secondary));
            char label[32];
            std::snprintf(label, sizeof(label), "%s (%s)", def.label, def.shortcut);
            if (ImGui::Button(label, { buttonW, 0.0f }))
                ActivateMapTool(ctx, def.tool);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("%s\nShortcut: %s (Scene View)", def.tooltip, def.shortcut);
            if (active)
                ImGui::PopStyleColor();
            column = (column + 1) % 3;
        }
    }
    ImGui::TextDisabled("Keys 1-6 switch tools while the Scene View is focused");

    ImGui::Separator();
    ImGui::BeginChild("##MapToolSettings", { 0.0f, 0.0f }, false);

    switch (ctx.mapActiveTool) {
    case EditorContext::MapTool::TerrainSculpt:
        if (ctx.terrainTool) {
            ctx.terrainTool->DrawSculptContent(
                *ctx.activeScene, ctx.undoStack, ctx.markSceneDirty);
            ctx.terrainTool->DrawBrushSettings();
            ctx.terrainTool->DrawImportSection(
                *ctx.activeScene, ctx.undoStack, ctx.markSceneDirty);
        }
        break;
    case EditorContext::MapTool::TerrainPaint:
        if (ctx.terrainTool) {
            ctx.terrainTool->DrawPaintContent(
                *ctx.activeScene, ctx.undoStack, ctx.markSceneDirty);
            ctx.terrainTool->DrawBrushSettings();
        }
        break;
    case EditorContext::MapTool::Water:
        if (ctx.waterTool) {
            ctx.waterTool->DrawContent(
                *ctx.activeScene, ctx.projectRoot, ctx.markSceneDirty, ctx.undoStack);
        }
        break;
    case EditorContext::MapTool::Detail:
        if (ctx.detailTool)
            ctx.detailTool->DrawContent(*ctx.activeScene, ctx.markSceneDirty);
        break;
    case EditorContext::MapTool::Foliage:
        if (ctx.foliageTool)
            ctx.foliageTool->DrawContent(*ctx.activeScene, ctx.markSceneDirty);
        break;
    case EditorContext::MapTool::Grid:
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

    // ── 2D グリッドミニマップ ─────────────────────────────────────────
    // 各セルにハイトマップのグレースケールを敷き、エディターカメラの位置マーカーを重ねる。
    // クリックで選択 / ダブルクリックでカメラフォーカス / 右クリックで操作メニュー。
    const float cellPx  = 64.0f; // セル表示サイズ [px]
    const float spacing = 3.0f;
    ImGui::TextDisabled("Click: select  /  Double-click: focus camera  /  Right-click: actions");

    // セル1枚のワールドサイズ (CreateTerrainInGridCell と同じ推定方法)
    float cellWorldSize = static_cast<float>(grid->defaultColumns - 1) * grid->defaultCellSize;
    for (const scene::EntityID id : grid->cells) {
        if (!scene.IsValid(id)) continue;
        if (const auto* tc = scene.GetComponent<scene::TerrainComponent>(id)) {
            cellWorldSize = static_cast<float>(tc->columns - 1) * tc->cellSize;
            break;
        }
    }

    const ImVec2 gridOrigin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    for (int gz = 0; gz < grid->cellCountZ; ++gz) {
        for (int gx = 0; gx < grid->cellCountX; ++gx) {
            if (gx > 0) ImGui::SameLine(0.0f, spacing);

            const scene::EntityID cellId = grid->GetCell(gx, gz);
            const bool hasCell  = scene.IsValid(cellId);
            const bool selected = (m_gridSelectedX == gx && m_gridSelectedZ == gz);

            ImGui::PushID(gz * 32 + gx);
            ImGui::InvisibleButton("##cell", { cellPx, cellPx });
            const ImVec2 rectMin = ImGui::GetItemRectMin();
            const ImVec2 rectMax = ImGui::GetItemRectMax();
            const bool hovered = ImGui::IsItemHovered();

            // セル背景: 割り当て済みはハイトマッププレビュー、空はダークグレー + "+"
            const scene::TerrainComponent* tc = hasCell
                ? scene.GetComponent<scene::TerrainComponent>(cellId)
                : nullptr;
            if (tc && !tc->heightData.empty()) {
                DrawCellHeightPreview(draw, rectMin, rectMax, *tc);
            } else {
                draw->AddRectFilled(rectMin, rectMax, IM_COL32(45, 48, 55, 255));
                const ImVec2 plusSize = ImGui::CalcTextSize("+");
                draw->AddText({ (rectMin.x + rectMax.x - plusSize.x) * 0.5f,
                                (rectMin.y + rectMax.y - plusSize.y) * 0.5f },
                              IM_COL32(130, 135, 150, 255), "+");
            }
            // 座標ラベル (左上に小さく)
            char coordLabel[16];
            std::snprintf(coordLabel, sizeof(coordLabel), "%d,%d", gx, gz);
            draw->AddText({ rectMin.x + 3.0f, rectMin.y + 2.0f }, IM_COL32(235, 240, 255, 200), coordLabel);
            // 枠: 選択中=黄 / ホバー=白 / 通常=グレー
            const ImU32 border = selected ? IM_COL32(255, 210, 90, 255)
                               : hovered  ? IM_COL32(220, 224, 235, 200)
                                          : IM_COL32(80, 86, 100, 255);
            draw->AddRect(rectMin, rectMax, border, 0.0f, 0, selected ? 2.0f : 1.0f);

            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                m_gridSelectedX = gx;
                m_gridSelectedZ = gz;
            }
            // ダブルクリックでカメラフォーカス (右クリックメニューの Focus Camera を1操作に短縮)
            if (hasCell && hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (const auto* go = scene.GetGameObject(cellId)) {
                    ctx.focusTargetPosition    = go->transform.worldPosition;
                    ctx.focusTargetRadius      = cellWorldSize * 0.6f;
                    ctx.requestFocusOnSelected = true;
                }
            }

            if (hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::BeginTooltip();
                ImGui::Text("Cell (%d, %d)", gx, gz);
                if (hasCell) {
                    if (const auto* go = scene.GetGameObject(cellId))
                        ImGui::Text("GO: %s", go->name.c_str());
                    ImGui::TextDisabled("Double-click: focus / Right-click: actions");
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
                        ctx.focusTargetRadius      = cellWorldSize * 0.6f;
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

    // ── エディターカメラ位置マーカー ────────────────────────────────────
    // WHY: 俯瞰ミニマップとして機能させるには「自分がどこを見ているか」が必須。
    //      Grid の GameObject 位置を原点とみなし、カメラの XZ をセル座標へ射影する。
    //      Grid GO の回転は想定しない (Grid Terrain は軸整列配置が前提)。
    if (ctx.editorCamera && cellWorldSize > 0.0f) {
        math::Vector3 gridWorldOrigin{};
        if (const auto* gridGo = scene.GetGameObject(gridEntity))
            gridWorldOrigin = gridGo->transform.worldPosition;
        const float fx = (ctx.editorCamera->m_position.x - gridWorldOrigin.x) / cellWorldSize;
        const float fz = (ctx.editorCamera->m_position.z - gridWorldOrigin.z) / cellWorldSize;
        const bool inside = fx >= 0.0f && fz >= 0.0f
            && fx <= static_cast<float>(grid->cellCountX)
            && fz <= static_cast<float>(grid->cellCountZ);
        if (inside) {
            const ImVec2 markerPos(gridOrigin.x + fx * (cellPx + spacing),
                                   gridOrigin.y + fz * (cellPx + spacing));
            // カメラの向き (ヨー) を三角形マーカーの向きに反映する
            const math::Vector3 forward = ctx.editorCamera->GetForward();
            const float yaw = std::atan2(forward.x, forward.z);
            const float s = std::sin(yaw);
            const float c = std::cos(yaw);
            auto rotate = [&](float x, float y) {
                // ミニマップはワールド +X → 画面右 / +Z → 画面下
                return ImVec2(markerPos.x + x * c + y * s, markerPos.y - x * s + y * c);
            };
            draw->AddTriangleFilled(rotate(0.0f, 8.0f), rotate(-5.0f, -5.0f), rotate(5.0f, -5.0f),
                                    IM_COL32(255, 90, 90, 255));
            draw->AddCircle(markerPos, 2.0f, IM_COL32(255, 255, 255, 220));
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
