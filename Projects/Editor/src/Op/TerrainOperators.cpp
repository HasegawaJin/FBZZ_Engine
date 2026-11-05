/// @file    TerrainOperators.cpp
/// @brief   Terrain データ編集の Operator。
/// @author  Hasegawa Jin
/// @date    2026-08-22
/// @note Terrain の columns/rows は heightData / splatIndices / splatWeights / holeData のサイズと一体で、数値だけ変えると TerrainRenderPass/PhysicsSystem が不整合な配列を読む。Inspector と同じく TerrainComponent::Resize() を通し、変更前後を丸ごと Undo へ保持する。
/// @see Docs/design/editor-operator-model.md
#include <Editor/Op/OperatorGroups.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Tools/TerrainAuthoring.hpp>
#include <Tools/TerrainBrush.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/TerrainHeightMapLoader.hpp>
#include <Engine/Scene/TerrainSplat.hpp>
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>

#include <algorithm>
#include <memory>
#include <cmath>
#include <filesystem>
#include <string>
#include <utility>

namespace fbzz::editor {

void RegisterTerrainScatterOperators(OperatorRegistry& registry);
void RegisterTerrainGrassOperators(OperatorRegistry& registry);

namespace {

scene::GameObject* ResolveTerrainNode(const OpContext& context, const std::string& nodeId)
{
    if (context.ctx.activeScene == nullptr) return nullptr;
    if (nodeId.empty()) return context.ctx.GetSelectedGO();
    return context.ctx.activeScene->FindByGuid(nodeId);
}

bool IsValidTerrainDimension(int value)
{
    /// @note Inspector と同じ上限に揃え、Undo と GPU 再構築の巨大な一時確保を防ぐ。
    return value >= 2 && value <= 4097;
}

void MarkTerrainDirty(scene::TerrainComponent& terrain)
{
    terrain.RequestHeightRebuild();
    terrain.splatDirty = true;
    terrain.colliderDirty = true;
}

std::unique_ptr<ICommand> MakeTerrainResizeCommand(
    EditorContext& context,
    std::string    nodeId,
    scene::TerrainComponent before,
    scene::TerrainComponent after,
    std::string label = "Resize Terrain", bool geometry = true)
{
    EditorContext* editorContext = &context;
    const auto generation = context.activeScene->GetRenderSceneGeneration();
    const auto* owner = context.activeScene->FindByGuid(nodeId);
    const auto lifetime = context.activeScene->GetComponentLifetime<scene::TerrainComponent>(owner->GetID());
    const auto apply = [editorContext, nodeId, generation, lifetime, geometry](const scene::TerrainComponent& snapshot,
        const scene::TerrainComponent& expected, std::string& reason) {
        if (editorContext->activeScene == nullptr || editorContext->activeScene->GetRenderSceneGeneration() != generation) {
            reason = "編集対象のシーンが変わりました";
            return false;
        }
        scene::GameObject* go = editorContext->activeScene->FindByGuid(nodeId);
        if (go == nullptr || editorContext->activeScene->GetComponentLifetime<scene::TerrainComponent>(go->GetID()) != lifetime) {
            reason = "対象地形が削除または作り直されています";
            return false;
        }
        auto* terrain = go->GetComponent<scene::TerrainComponent>();
        if (terrain == nullptr) { reason = "対象地形がありません"; return false; }
        if (!TerrainAuthoringValuesEqual(*terrain, expected)) {
            reason = "対象地形の編集内容が変更されています";
            return false;
        }
        ApplyTerrainAuthoringSnapshot(*terrain, snapshot, geometry);
        if (editorContext->markSceneDirty) editorContext->markSceneDirty();
        reason.clear();
        return true;
    };

    const auto snapshots = std::make_shared<std::pair<scene::TerrainComponent, scene::TerrainComponent>>(std::move(before), std::move(after));
    return std::make_unique<CheckedLambdaCommand>(
        std::move(label),
        [apply, snapshots](std::string& reason) { return apply(snapshots->second, snapshots->first, reason); },
        [apply, snapshots](std::string& reason) { return apply(snapshots->first, snapshots->second, reason); });
}

OpParam TerrainParameter(std::string name, OpParamType type, std::string description, OpValue defaultValue = {})
{
    OpParam param;
    param.name = std::move(name);
    param.type = type;
    param.desc = std::move(description);
    param.required = std::holds_alternative<std::monostate>(defaultValue);
    param.defaultValue = std::move(defaultValue);
    return param;
}

EditorOperator TerrainMutation(const char* id, const char* label)
{
    EditorOperator op;
    op.id = id;
    op.label = label;
    op.category = "Terrain";
    op.kind = OpKind::Mutation;
    op.undoLabel = label;
    op.params = {TerrainParameter("node", OpParamType::NodeId, "対象地形。省略時は選択中", std::string{})};
    op.poll = [](const OpContext& context, const OpArgs& args) {
        if (context.ctx.playMode && context.ctx.playMode->GetState() != PlayState::Editor) return false;
        auto* go = ResolveTerrainNode(context, args.GetString("node"));
        return go && go->GetComponent<scene::TerrainComponent>();
    };
    return op;
}

std::string ResolveTerrainInput(const EditorContext& context, const std::string& path)
{
    if (path.empty()) return {};
    const auto projectPath = ToProjectAssetDiskPath(context.projectRoot, path);
    std::error_code error;
    if (!projectPath.empty() && std::filesystem::is_regular_file(std::filesystem::u8path(projectPath), error))
        return projectPath;
    /// @note SDKの共有素材も通常のアセット解決規則で読み、編集結果はプロジェクト側へ保存する。
    return asset::AssetManager::ResolveAssetPath(path);
}

OpResult CommitTerrain(OpContext& context, scene::GameObject& go, scene::TerrainComponent after, const char* label, bool geometry = true)
{
    auto& terrain = *go.GetComponent<scene::TerrainComponent>();
    OpResult result;
    result.command = MakeTerrainResizeCommand(context.ctx, go.instanceId, terrain, after, label, geometry);
    ApplyTerrainAuthoringSnapshot(terrain, after, geometry);
    if (context.ctx.markSceneDirty) context.ctx.markSceneDirty();
    result.message = label;
    return result;
}

void RegisterTerrainAuthoringOperators(OperatorRegistry& registry)
{
    auto alpine = TerrainMutation("terrain.alpine.setup", "Alpine Starter");
    alpine.desc = "同梱HeightMapを513角・格子4m・高さ640mで読み、草地・岩肌・雪を設定する。";
    alpine.caution = "対象地形の高さ・寸法・材質レイヤー・塗りを置換します。穴は重なる範囲で保持、Undo可能。";
    alpine.params.push_back(TerrainParameter("path", OpParamType::String, "空は同梱AlpineRange。別のHeightMapへ差し替え可能", std::string{}));
    alpine.exec = [](OpContext& context, const OpArgs& args) {
        auto* go = ResolveTerrainNode(context, args.GetString("node"));
        if (!go || !go->GetComponent<scene::TerrainComponent>()) return OpResult::Err("NO_TERRAIN", "対象地形がありません");
        const char* materialPaths[] = {"Assets/Terrain/Alpine/Grass.mat", "Assets/Terrain/Alpine/Rock.mat", "Assets/Terrain/Alpine/Snow.mat"};
        for (const char* material : materialPaths) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(std::filesystem::u8path(ResolveTerrainInput(context.ctx, material)), error))
                return OpResult::Err("MISSING_ASSETS", "Alpine素材を含むSDKを公開するか、Assets/Terrain/Alpineをプロジェクトへコピーしてください");
        }
        auto after = *go->GetComponent<scene::TerrainComponent>();
        after.Resize(513, 513);
        after.cellSize = 4.0f;
        after.maxHeight = 640.0f;
        auto path = args.GetString("path");
        if (path.empty()) path = "Assets/Terrain/Alpine/AlpineRange_Height16.png";
        if (!scene::LoadHeightMapFromFile(ResolveTerrainInput(context.ctx, path), after, true))
            return OpResult::Err("IMPORT_FAILED", "Alpine HeightMapを読み込めません。地形は未変更です");
        after.layerMaterials = {
            "guid:c1e517a39c8257bd81be5f88fdef2de8|Assets/Terrain/Alpine/Grass.mat",
            "guid:aaa2d58a9d655d1cb84ff5afcc0ff855|Assets/Terrain/Alpine/Rock.mat",
            "guid:cddccaae2812585a831e6439a2a0d14f|Assets/Terrain/Alpine/Snow.mat"};
        TerrainBiomeSettings settings;
        settings.snowHeight += go->transform.worldPosition.y;
        if (!BakeTerrainBiome(after, go->transform, settings))
            return OpResult::Err("BAD_TERRAIN", "地形の変換または高さが不正です");
        after.RequestMaterialRebuild();
        return CommitTerrain(context, *go, std::move(after), "Alpine Starter");
    };
    registry.Register(std::move(alpine));

    auto import = TerrainMutation("terrain.heightmap.import", "Import Terrain Heightmap");
    import.desc = "線形の高さ画像を取り込む。任意の寸法設定と画像を同じ Undo で確定する。";
    import.params.push_back(TerrainParameter("path", OpParamType::String, "PNG / TGA / DDS のパスまたは GUID"));
    import.params.push_back(TerrainParameter("unipolar", OpParamType::Bool, "黒を高さ0にする。falseは灰を0にする", true));
    import.params.push_back(TerrainParameter("columns", OpParamType::Int, "X頂点数2〜4097。0は維持", 0));
    import.params.push_back(TerrainParameter("rows", OpParamType::Int, "Z頂点数2〜4097。0は維持", 0));
    import.params.push_back(TerrainParameter("cellSize", OpParamType::Float, "格子間隔[m]。0は維持", 0.0f));
    import.params.push_back(TerrainParameter("maxHeight", OpParamType::Float, "白の高さ[m]。0は維持", 0.0f));
    import.exec = [](OpContext& context, const OpArgs& args) {
        auto* go = ResolveTerrainNode(context, args.GetString("node"));
        if (!go || !go->GetComponent<scene::TerrainComponent>()) return OpResult::Err("NO_TERRAIN", "対象地形がありません");
        auto after = *go->GetComponent<scene::TerrainComponent>();
        const int columns = args.GetInt("columns", 0);
        const int rows = args.GetInt("rows", 0);
        const float cellSize = args.GetFloat("cellSize", 0.0f);
        const float maxHeight = args.GetFloat("maxHeight", 0.0f);
        if ((columns && !IsValidTerrainDimension(columns)) || (rows && !IsValidTerrainDimension(rows))
            || !std::isfinite(cellSize) || cellSize < 0.0f || cellSize > 100000.0f
            || !std::isfinite(maxHeight) || maxHeight < 0.0f || maxHeight > 100000.0f)
            return OpResult::Err("BAD_ARG", "寸法・格子間隔・高さの範囲が不正です");
        const int nextColumns = columns ? columns : after.columns;
        const int nextRows = rows ? rows : after.rows;
        if (!IsValidTerrainDimension(nextColumns) || !IsValidTerrainDimension(nextRows))
            return OpResult::Err("BAD_ARG", "地形の頂点数は2〜4097で指定してください");
        if (cellSize) after.cellSize = cellSize;
        if (maxHeight) after.maxHeight = maxHeight;
        if (!std::isfinite(after.cellSize) || after.cellSize <= 0.0f || !std::isfinite(after.maxHeight) || after.maxHeight <= 0.0f)
            return OpResult::Err("BAD_ARG", "地形の格子間隔と高さは正の有限値が必要です");
        if (after.columns != nextColumns || after.rows != nextRows) after.Resize(nextColumns, nextRows);
        const auto path = ResolveTerrainInput(context.ctx, args.GetString("path"));
        if (path.empty() || !scene::LoadHeightMapFromFile(path, after, args.GetBool("unipolar", true)))
            return OpResult::Err("IMPORT_FAILED", "高さ画像を読み込めません。地形は変更していません");
        return CommitTerrain(context, *go, std::move(after), "Import Terrain Heightmap");
    };
    registry.Register(std::move(import));

    auto stamp = TerrainMutation("terrain.heightmap.stamp", "Stamp Terrain Heightmap");
    stamp.desc = "高さ画像を対象地形の矩形範囲へ一回貼り付ける。範囲外の高さ・材質・穴は保持する。";
    stamp.params.push_back(TerrainParameter("path", OpParamType::String, "PNG / TGA / DDS のパスまたは GUID"));
    stamp.params.push_back(TerrainParameter("center", OpParamType::Vec3, "貼り付け中心のworld座標[m]"));
    stamp.params.push_back(TerrainParameter("width", OpParamType::Float, "地形ローカルX幅[m]、0より大きく100000以下", 20.0f));
    stamp.params.push_back(TerrainParameter("depth", OpParamType::Float, "地形ローカルZ幅[m]、0より大きく100000以下", 20.0f));
    stamp.params.push_back(TerrainParameter("rotation", OpParamType::Float, "地形ローカル+Y軸回転[度]", 0.0f));
    stamp.params.push_back(TerrainParameter("heightScale", OpParamType::Float, "画像値に掛ける地形ローカル高さ[m]、負で上下反転", 10.0f));
    stamp.params.push_back(TerrainParameter("heightOffset", OpParamType::Float, "地形ローカル基準高[m]。addでは加算量", 0.0f));
    stamp.params.push_back(TerrainParameter("strength", OpParamType::Float, "一回の合成強度[0,1]、時間非依存", 1.0f));
    stamp.params.push_back(TerrainParameter("edgeFade", OpParamType::Float, "各辺から内側へぼかすUV幅[0,0.5]", 0.1f));
    stamp.params.push_back(TerrainParameter("unipolar", OpParamType::Bool, "黒0・白1。falseは黒-1・白1", true));
    auto blend = TerrainParameter("blend", OpParamType::String, "高さの合成方式", std::string{"add"});
    blend.enumValues = {"add", "replace", "max", "min"};
    stamp.params.push_back(std::move(blend));
    stamp.exec = [](OpContext& context, const OpArgs& args) {
        auto* go = ResolveTerrainNode(context, args.GetString("node"));
        if (!go || !go->GetComponent<scene::TerrainComponent>()) return OpResult::Err("NO_TERRAIN", "対象地形がありません");
        math::Vector3 normal;
        if (!scene::TryTerrainNormalToWorld(go->transform, math::Vector3::UP, normal))
            return OpResult::Err("BAD_TERRAIN", "地形の変換が不正です");
        const auto& terrain = *go->GetComponent<scene::TerrainComponent>();
        TerrainHeightStampSettings settings;
        settings.centerLocal = ToTerrainLocal(go->transform, args.GetVec3("center"));
        settings.width = args.GetFloat("width", 20.0f);
        settings.depth = args.GetFloat("depth", 20.0f);
        settings.rotationDegrees = args.GetFloat("rotation", 0.0f);
        settings.heightScale = args.GetFloat("heightScale", 10.0f);
        settings.heightOffset = args.GetFloat("heightOffset", 0.0f);
        settings.strength = args.GetFloat("strength", 1.0f);
        settings.edgeFade = args.GetFloat("edgeFade", 0.1f);
        const auto blendName = args.GetString("blend", "add");
        if (blendName == "replace") settings.blend = TerrainHeightStampBlend::REPLACE;
        else if (blendName == "max") settings.blend = TerrainHeightStampBlend::MAXIMUM;
        else if (blendName == "min") settings.blend = TerrainHeightStampBlend::MINIMUM;
        if (!IsValidTerrainHeightStamp(terrain, settings))
            return OpResult::Err("BAD_ARG", "貼り付け寸法・高さ・強度・ぼかし・中心座標が不正です");
        scene::TerrainComponent image;
        /// @note 読み込みはクリック時だけ。貼り付けの格子密度へ再標本化し、毎フレームの画像I/Oを避ける。
        image.columns = static_cast<int>(std::clamp(std::ceil(static_cast<double>(settings.width) / terrain.cellSize) + 1.0, 2.0, 4097.0));
        image.rows = static_cast<int>(std::clamp(std::ceil(static_cast<double>(settings.depth) / terrain.cellSize) + 1.0, 2.0, 4097.0));
        const auto path = ResolveTerrainInput(context.ctx, args.GetString("path"));
        if (path.empty() || !scene::LoadHeightMapFromFile(path, image, args.GetBool("unipolar", true)))
            return OpResult::Err("IMPORT_FAILED", "高さ画像を読み込めません。地形は未変更です");
        auto after = terrain;
        if (!ApplyTerrainHeightStamp(after, image, settings))
            return OpResult::Err("BAD_TERRAIN", "高さデータが不正です。地形は未変更です");
        if (after.heightData == terrain.heightData) {
            OpResult result;
            result.noChange = true;
            result.message = "貼り付けによる高さの変化はありません";
            return result;
        }
        return CommitTerrain(context, *go, std::move(after), "Stamp Terrain Heightmap");
    };
    registry.Register(std::move(stamp));

    auto biome = TerrainMutation("terrain.biome.bake", "Bake Grass Rock Snow");
    biome.desc = "world標高・傾斜から草地・岩肌・雪の3層を確定する。Fiberと配置は保存層を参照できる。";
    biome.caution = "全頂点の塗りを置換します。材質の Auto Blend はオフにしてください。Undo可能。";
    biome.params.push_back(TerrainParameter("grassLayer", OpParamType::Int, "草地の既存層番号", 0));
    biome.params.push_back(TerrainParameter("rockLayer", OpParamType::Int, "岩の既存層番号", 1));
    biome.params.push_back(TerrainParameter("snowLayer", OpParamType::Int, "雪の既存層番号", 2));
    biome.params.push_back(TerrainParameter("snowHeight", OpParamType::Float, "雪線の中心world Y[m]", 420.0f));
    biome.params.push_back(TerrainParameter("snowFade", OpParamType::Float, "雪線の遷移幅[m]。0は段階判定", 60.0f));
    biome.params.push_back(TerrainParameter("rockSlope", OpParamType::Float, "岩に変わる傾斜の中心[度]", 38.0f));
    biome.params.push_back(TerrainParameter("slopeFade", OpParamType::Float, "傾斜の遷移幅[度]", 10.0f));
    biome.exec = [](OpContext& context, const OpArgs& args) {
        auto* go = ResolveTerrainNode(context, args.GetString("node"));
        if (!go || !go->GetComponent<scene::TerrainComponent>()) return OpResult::Err("NO_TERRAIN", "対象地形がありません");
        auto after = *go->GetComponent<scene::TerrainComponent>();
        TerrainBiomeSettings settings;
        settings.grassLayer = args.GetInt("grassLayer", 0);
        settings.rockLayer = args.GetInt("rockLayer", 1);
        settings.snowLayer = args.GetInt("snowLayer", 2);
        settings.snowHeight = args.GetFloat("snowHeight", 420.0f);
        settings.snowFade = args.GetFloat("snowFade", 60.0f);
        settings.rockSlope = args.GetFloat("rockSlope", 38.0f);
        settings.slopeFade = args.GetFloat("slopeFade", 10.0f);
        if (!BakeTerrainBiome(after, go->transform, settings))
            return OpResult::Err("BAD_ARG", "異なる3層と有限の標高・傾斜、正常な地形が必要です");
        return CommitTerrain(context, *go, std::move(after), "Bake Grass Rock Snow", false);
    };
    registry.Register(std::move(biome));

    auto mask = TerrainMutation("terrain.layer.import_mask", "Import Terrain Layer Mask");
    mask.desc = "線形画像の白を対象層へ混ぜ、黒は既存分布を保つ。地形の高さは変えない。";
    mask.params.push_back(TerrainParameter("path", OpParamType::String, "PNG等のマスク画像"));
    mask.params.push_back(TerrainParameter("layer", OpParamType::Int, "対象の既存層番号"));
    mask.params.push_back(TerrainParameter("strength", OpParamType::Float, "適用強度[0,1]", 1.0f));
    mask.params.push_back(TerrainParameter("invert", OpParamType::Bool, "白黒を反転", false));
    mask.exec = [](OpContext& context, const OpArgs& args) {
        auto* go = ResolveTerrainNode(context, args.GetString("node"));
        if (!go || !go->GetComponent<scene::TerrainComponent>()) return OpResult::Err("NO_TERRAIN", "対象地形がありません");
        auto after = *go->GetComponent<scene::TerrainComponent>();
        const int layer = args.GetInt("layer", -1);
        const float strength = args.GetFloat("strength", 1.0f);
        if (layer < 0 || layer >= after.LayerCount() || !std::isfinite(strength) || strength < 0.0f || strength > 1.0f
            || !IsValidTerrainDimension(after.columns) || !IsValidTerrainDimension(after.rows))
            return OpResult::Err("BAD_ARG", "層番号・強度・地形寸法が不正です");
        scene::TerrainComponent image;
        image.columns = after.columns;
        image.rows = after.rows;
        const auto path = ResolveTerrainInput(context.ctx, args.GetString("path"));
        if (path.empty() || !scene::LoadHeightMapFromFile(path, image, true))
            return OpResult::Err("IMPORT_FAILED", "マスク画像を読み込めません");
        after.EnsureSplat();
        for (size_t vertex = 0; vertex < image.heightData.size(); ++vertex) {
            const float coverage = args.GetBool("invert", false) ? 1.0f - image.heightData[vertex] : image.heightData[vertex];
            const auto offset = vertex * scene::TERRAIN_SPLAT_SLOTS;
            scene::terrain_splat::BlendToward(after.splatIndices.data() + offset, after.splatWeights.data() + offset, layer, coverage * strength);
        }
        return CommitTerrain(context, *go, std::move(after), "Import Terrain Layer Mask", false);
    };
    registry.Register(std::move(mask));
}

} /// @note namespace

void RegisterTerrainOperators(OperatorRegistry& registry)
{
    EditorOperator op;
    op.id        = "terrain.resize";
    op.label     = "Resize Terrain";
    op.category  = "Terrain";
    op.desc      = "Terrain の columns / rows を変更する。既存データは重なる範囲を保持し、"
                   "拡張領域は平坦な layer 0 として初期化する。Inspector と同じ Resize() を通る。";
    op.caution   = "縮小すると範囲外の height / splat データは破棄される (Undo で復元できる)。";
    op.kind      = OpKind::Mutation;
    op.undoLabel = "AI: Resize Terrain";

    OpParam nodeParam;
    nodeParam.name = "node";
    nodeParam.type = OpParamType::NodeId;
    nodeParam.desc = "対象 Terrain を持つ GameObject。省略時は選択中のもの";
    nodeParam.required = false;

    OpParam columnsParam;
    columnsParam.name = "columns";
    columnsParam.type = OpParamType::Int;
    columnsParam.desc = "X 方向の頂点数 (2〜4097)";

    OpParam rowsParam;
    rowsParam.name = "rows";
    rowsParam.type = OpParamType::Int;
    rowsParam.desc = "Z 方向の頂点数 (2〜4097)";

    op.params = { nodeParam, columnsParam, rowsParam };
    op.poll = [](const OpContext& context, const OpArgs& args) {
        scene::GameObject* go = ResolveTerrainNode(context, args.GetString("node"));
        if (go == nullptr || go->GetComponent<scene::TerrainComponent>() == nullptr)
            return false;
        return IsValidTerrainDimension(args.GetInt("columns"))
            && IsValidTerrainDimension(args.GetInt("rows"));
    };

    op.exec = [](OpContext& context, const OpArgs& args) -> OpResult {
        scene::GameObject* go = ResolveTerrainNode(context, args.GetString("node"));
        if (go == nullptr) return OpResult::Err("NO_NODE", "対象の GameObject が見つかりません");
        auto* terrain = go->GetComponent<scene::TerrainComponent>();
        if (terrain == nullptr)
            return OpResult::Err("NOT_PRESENT", "TerrainComponent が装着されていません");

        const int columns = args.GetInt("columns");
        const int rows = args.GetInt("rows");
        if (!IsValidTerrainDimension(columns) || !IsValidTerrainDimension(rows))
            return OpResult::Err("BAD_ARG", "columns と rows は 2〜4097 で指定してください");
        if (terrain->columns == columns && terrain->rows == rows) {
            OpResult result;
            result.noChange = true;
            result.message = "Terrain のサイズは変わりませんでした";
            return result;
        }

        const scene::TerrainComponent before = *terrain;
        scene::TerrainComponent after = before;
        after.Resize(columns, rows);
        MarkTerrainDirty(after);

        /// @note OperatorRegistry::Invoke は返却コマンドを再実行しないため、画面へ反映してから同じ状態を履歴へ渡す。
        *terrain = after;
        if (context.ctx.markSceneDirty) context.ctx.markSceneDirty();

        OpResult result;
        result.command = MakeTerrainResizeCommand(
            context.ctx, go->instanceId, before, std::move(after));
        return result;
    };

    registry.Register(std::move(op));
    RegisterTerrainAuthoringOperators(registry);
    RegisterTerrainScatterOperators(registry);
    RegisterTerrainGrassOperators(registry);
}

} /// @note namespace fbzz::editor
