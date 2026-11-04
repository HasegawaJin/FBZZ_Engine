/// @file    MapGridOperators.cpp
/// @brief   地形グリッドの生成・セル編集・照会を直接操作する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <Editor/Panels/MapEditorPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <cmath>
namespace fbzz::editor {
void MapEditorPanel::OnRegisterOperators(OperatorRegistry& registry)
{
    for (const char* action : {"inspect", "create", "configure", "cell", "fill"}) {
        EditorOperator op; const std::string name = action;
        op.id = "map.grid." + name; op.label = op.id; op.category = "Map"; op.kind = name == "inspect" ? OpKind::Query : OpKind::Action;
        op.desc = "地形グリッドの直接操作。cellはcreate/remove/select/focus。removeは割当のみ外す。fillは空セルへ新規地形を作る。configureで範囲外になる使用中セルは拒否する。地形ファイルはAssets/Terrain/Gridへ保存する。";
        OpParam node; node.name = "node"; node.type = OpParamType::NodeId; node.required = false; op.params.push_back(node);
        if (name == "configure") { OpParam p; p.name = "settings"; p.type = OpParamType::Data; op.params.push_back(p); }
        if (name == "cell") {
            OpParam p; p.name = "action"; p.enumValues = {"create", "remove", "select", "focus"}; op.params.push_back(p);
            for (const char* key : {"x", "z"}) { OpParam coordinate; coordinate.name = key; coordinate.type = OpParamType::Int; op.params.push_back(coordinate); }
        }
        op.poll = [name](const OpContext& context, const OpArgs&) { return context.ctx.activeScene && (name == "inspect" || !context.ctx.playMode || context.ctx.playMode->IsInEditor()); };
        op.exec = [this, name](OpContext& context, const OpArgs& args) {
            auto& ctx = context.ctx; auto& scene = *ctx.activeScene; const auto requested = args.GetString("node");
            auto* go = requested.empty() ? nullptr : scene.FindByGuid(requested);
            if (!requested.empty() && !go) return OpResult::Err("NO_NODE", "グリッドが見つかりません。");
            if (!go && name != "create") {
                const auto nodes = scene.GetEntities<scene::TerrainGridComponent>(); if (!nodes.empty()) go = scene.GetGameObject(nodes.front());
            }
            if (name == "create") {
                if (!requested.empty()) return OpResult::Err("BAD_ARG", "createに既存nodeは指定しません。");
                go = scene.TryCreateGameObject("TerrainGrid"); if (!go) return OpResult::Err("CAPACITY", "オブジェクトを作成できません。");
                go->AddComponent<scene::TerrainGridComponent>(); if (ctx.markSceneDirty) ctx.markSceneDirty();
            }
            auto* grid = go ? go->GetComponent<scene::TerrainGridComponent>() : nullptr;
            if (!grid) return OpResult::Err("NO_GRID", "TerrainGridを指定してください。");
            const auto gridId = go->GetID();
            if (grid->cellCountX < 1 || grid->cellCountX > 16 || grid->cellCountZ < 1 || grid->cellCountZ > 16) return OpResult::Err("BAD_GRID", "グリッドの寸法は1..16です。");
            int created = 0;
            if (name == "configure") {
                const auto* patch = args.GetData("settings"); if (!patch || !patch->IsObject()) return OpResult::Err("BAD_ARG", "settings");
                auto next = *grid;
                for (const auto& [key, value] : patch->AsObject()) {
                    if (key == "defaultCellSize") {
                        const double number = value.IsFloat() ? value.AsDouble() : value.IsInt() ? static_cast<double>(value.AsInt64()) : -1;
                        if (!std::isfinite(number) || number < 0.01 || number > 1000) return OpResult::Err("BAD_ARG", key); next.defaultCellSize = static_cast<float>(number);
                    } else {
                        int number = 0; if (!value.TryGetInt32(number)) return OpResult::Err("BAD_ARG", key);
                        if (key == "cellCountX" && number >= 1 && number <= 16) next.cellCountX = number;
                        else if (key == "cellCountZ" && number >= 1 && number <= 16) next.cellCountZ = number;
                        else if (key == "defaultColumns" && number >= 2 && number <= 1025) next.defaultColumns = number;
                        else if (key == "defaultRows" && number >= 2 && number <= 1025) next.defaultRows = number;
                        else if (key == "defaultChunkSize" && number >= 1 && number <= 128) next.defaultChunkSize = number;
                        else return OpResult::Err("BAD_ARG", key);
                    }
                }
                next.cells.assign(static_cast<std::size_t>(next.cellCountX * next.cellCountZ), scene::EntityID::INVALID);
                for (int z = 0; z < grid->cellCountZ; ++z) for (int x = 0; x < grid->cellCountX; ++x) {
                    const auto cell = grid->GetCell(x, z);
                    if (x >= next.cellCountX || z >= next.cellCountZ) { if (scene.IsValid(cell)) return OpResult::Err("OCCUPIED_CELL", "範囲外になるセルを先に外してください。"); }
                    else next.SetCell(x, z, cell);
                }
                *grid = std::move(next); RefreshGrid(ctx, gridId); if (ctx.markSceneDirty) ctx.markSceneDirty();
            } else if (name == "fill" || name == "cell") {
                const int x = args.GetInt("x", -1), z = args.GetInt("z", -1); const auto action = args.GetString("action");
                if (name == "cell" && (x < 0 || x >= grid->cellCountX || z < 0 || z >= grid->cellCountZ)) return OpResult::Err("BAD_ARG", "セル座標が範囲外です。");
                if (name == "fill" || action == "create") {
                    if (ctx.projectRoot.empty()) return OpResult::Err("NO_PROJECT", "地形アセットの保存先が必要です。");
                    int needed = name == "cell" ? 1 : 0;
                    if (name == "cell" && scene.IsValid(grid->GetCell(x, z))) return OpResult::Err("OCCUPIED_CELL", "セルには地形があります。");
                    if (name == "fill") for (int cz = 0; cz < grid->cellCountZ; ++cz) for (int cx = 0; cx < grid->cellCountX; ++cx) if (!scene.IsValid(grid->GetCell(cx, cz))) ++needed;
                    if (!scene.CanCreateGameObjects(static_cast<std::size_t>(needed))) return OpResult::Err("CAPACITY", "空セルを埋める容量がありません。");
                    const int width = grid->cellCountX, height = grid->cellCountZ;
                    for (int cz = 0; cz < height; ++cz) for (int cx = 0; cx < width; ++cx) {
                        if (name == "cell" && (cx != x || cz != z)) continue;
                        grid = scene.GetComponent<scene::TerrainGridComponent>(gridId);
                        if (!scene.IsValid(grid->GetCell(cx, cz))) {
                            if (!scene.IsValid(CreateGridTerrain(ctx, gridId, cx, cz))) return OpResult::Err("CREATE_FAILED", "地形を作成できません。"); ++created;
                        }
                    }
                    RefreshGrid(ctx, gridId); if (ctx.markSceneDirty) ctx.markSceneDirty();
                } else {
                    const auto cell = grid->GetCell(x, z); auto* target = scene.GetGameObject(cell);
                    if (!target) return OpResult::Err("EMPTY_CELL", "セルは空です。");
                    m_gridSelectedX = x; m_gridSelectedZ = z;
                    if (action == "remove") { DetachGridCell(ctx, gridId, x, z); if (ctx.markSceneDirty) ctx.markSceneDirty(); }
                    else if (action == "select") SelectEntity(ctx, cell);
                    else if (action == "focus") {
                        const auto* terrain = target->GetComponent<scene::TerrainComponent>();
                        ctx.focusTargetPosition = target->transform.worldPosition;
                        ctx.focusTargetRadius = terrain ? static_cast<float>(terrain->columns - 1) * terrain->cellSize * 0.6f : 1;
                        ctx.requestFocusOnSelected = true;
                    }
                }
            }
            grid = scene.GetComponent<scene::TerrainGridComponent>(gridId); go = scene.GetGameObject(gridId);
            if (name != "inspect") grid->SyncInstanceIds(scene);
            OpData data = OpData::MakeObject(), cells = OpData::MakeArray(), settings = OpData::MakeObject();
            for (int z = 0; z < grid->cellCountZ; ++z) for (int x = 0; x < grid->cellCountX; ++x) {
                const auto* target = scene.GetGameObject(grid->GetCell(x, z)); OpData row = OpData::MakeObject();
                row.Set("x", OpData(x)); row.Set("z", OpData(z)); row.Set("node", OpData(target ? target->instanceId : "")); cells.Push(std::move(row));
            }
            settings.Set("cellCountX", OpData(grid->cellCountX)); settings.Set("cellCountZ", OpData(grid->cellCountZ));
            settings.Set("defaultColumns", OpData(grid->defaultColumns)); settings.Set("defaultRows", OpData(grid->defaultRows));
            settings.Set("defaultCellSize", OpData(grid->defaultCellSize)); settings.Set("defaultChunkSize", OpData(grid->defaultChunkSize));
            data.Set("node", OpData(go->instanceId)); data.Set("cells", std::move(cells)); data.Set("settings", std::move(settings)); data.Set("created", OpData(created));
            return OpResult::Data(std::move(data));
        }; registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
