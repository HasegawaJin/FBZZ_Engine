/// @file    TerrainGrassOperators.cpp
/// @brief   地形の編集済みレイヤーに草を配置する制作操作。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Graphics/Renderer/FiberGeometry.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <tuple>
#include <vector>

namespace fbzz::editor {
namespace {

constexpr int TERRAIN_GRASS_PATCH_CELLS = 8;
constexpr double TERRAIN_GRASS_MAX_WINDOW_CANDIDATES = 500000.0;

/// @note 一ビューの距離窓が覆う隣接パッチの矩形和を走査し、配置場所によらない候補数の上限を評価する。
/// @note パッチごとの float 加算誤差を 0.1% の余裕と切り上げで覆い、既存 Renderer の一パッチ上限も確認する。
/// @note 層・穴は無視した保守的な見積もり。現在と直前二フレーム・複数ビューの保持量は、この一窓上限とは別に合算される。
/// @see Docs/design/terrain-layers.md §5 Fiber を層で制御する
bool TerrainGrassCandidateBudget(const scene::TerrainComponent& terrain, const scene::Transform& transform,
    float density, float farDistance, double& candidates, int& windowPatches, bool& patchBudget)
{
    candidates = 0.0;
    windowPatches = 0;
    patchBudget = true;
    if (terrain.columns < 2 || terrain.rows < 2 || terrain.columns > 4097 || terrain.rows > 4097
        || terrain.heightData.size() != terrain.VertexCount() || !std::isfinite(terrain.cellSize)
        || terrain.cellSize <= 0.0f || !std::isfinite(terrain.maxHeight) || terrain.maxHeight <= 0.0f)
        return false;
    for (float height : terrain.heightData) if (!std::isfinite(height)) return false;
    const auto finite = [](const math::Vector3& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    const float rotationLength = transform.worldRotation.Length();
    if (!finite(transform.worldPosition) || !finite(transform.worldScale)
        || transform.worldScale.x == 0.0f || transform.worldScale.y == 0.0f || transform.worldScale.z == 0.0f
        || !std::isfinite(rotationLength) || std::abs(rotationLength - 1.0f) > 0.001f)
        return false;
    const double scaleX = std::abs(static_cast<double>(transform.worldScale.x));
    const double scaleZ = std::abs(static_cast<double>(transform.worldScale.z));
    const int patchColumns = (terrain.columns - 1 + TERRAIN_GRASS_PATCH_CELLS - 1) / TERRAIN_GRASS_PATCH_CELLS;
    const int patchRows = (terrain.rows - 1 + TERRAIN_GRASS_PATCH_CELLS - 1) / TERRAIN_GRASS_PATCH_CELLS;
    const double localPatchSize = static_cast<double>(terrain.cellSize) * TERRAIN_GRASS_PATCH_CELLS;
    const int windowColumns = static_cast<int>(std::min(static_cast<double>(patchColumns),
        std::ceil(2.0 * farDistance / (localPatchSize * scaleX)) + 2.0));
    const int windowRows = static_cast<int>(std::min(static_cast<double>(patchRows),
        std::ceil(2.0 * farDistance / (localPatchSize * scaleZ)) + 2.0));
    windowPatches = windowColumns * windowRows;
    const size_t prefixStride = static_cast<size_t>(patchColumns) + 1;
    std::vector<double> prefixCandidates(prefixStride * (static_cast<size_t>(patchRows) + 1), 0.0);
    const auto point = [&](int x, int z) {
        return math::Vector3{static_cast<float>(x) * terrain.cellSize,
            terrain.heightData[static_cast<size_t>(z) * terrain.columns + x] * terrain.maxHeight,
            static_cast<float>(z) * terrain.cellSize};
    };
    for (int z = 0; z < terrain.rows - 1; z += TERRAIN_GRASS_PATCH_CELLS) {
        for (int x = 0; x < terrain.columns - 1; x += TERRAIN_GRASS_PATCH_CELLS) {
            float area = 0.0f;
            const int endZ = std::min(z + TERRAIN_GRASS_PATCH_CELLS, terrain.rows - 1);
            const int endX = std::min(x + TERRAIN_GRASS_PATCH_CELLS, terrain.columns - 1);
            for (int cz = z; cz < endZ; ++cz) {
                for (int cx = x; cx < endX; ++cx) {
                    const auto a = point(cx, cz);
                    const auto b = point(cx, cz + 1);
                    const auto c = point(cx + 1, cz);
                    const auto d = point(cx + 1, cz + 1);
                    area += math::Vector3::Cross(b - a, c - a).Length() * 0.5f;
                    area += math::Vector3::Cross(b - c, d - c).Length() * 0.5f;
                }
            }
            if (!std::isfinite(area)) return false;
            const double count = std::ceil(static_cast<double>(area) * density * 1.001);
            patchBudget &= count <= renderer::FIBER_MAX_BLADES;
            const auto index = (static_cast<size_t>(z / TERRAIN_GRASS_PATCH_CELLS) + 1) * prefixStride
                + static_cast<size_t>(x / TERRAIN_GRASS_PATCH_CELLS) + 1;
            prefixCandidates[index] = count + prefixCandidates[index - 1]
                + prefixCandidates[index - prefixStride] - prefixCandidates[index - prefixStride - 1];
        }
    }
    for (int z = windowRows; z <= patchRows; ++z) {
        for (int x = windowColumns; x <= patchColumns; ++x) {
            const auto right = static_cast<size_t>(x), left = static_cast<size_t>(x - windowColumns);
            const auto bottom = static_cast<size_t>(z) * prefixStride;
            const auto top = static_cast<size_t>(z - windowRows) * prefixStride;
            const double count = prefixCandidates[bottom + right] - prefixCandidates[bottom + left]
                - prefixCandidates[top + right] + prefixCandidates[top + left];
            candidates = std::max(candidates, count);
        }
    }
    return std::isfinite(candidates);
}

auto TerrainGrassValues(const scene::FiberComponent& fiber)
{
    return std::tie(fiber.m_enabled, fiber.m_mode, fiber.m_shellCount, fiber.m_distanceLod,
        fiber.m_minShellCount, fiber.m_lodNear, fiber.m_lodFar, fiber.m_terrainSparseLodStart, fiber.m_bladeDensity, fiber.m_bladeWidth,
        fiber.m_terrainGroundCover, fiber.m_terrainGroundColor.x, fiber.m_terrainGroundColor.y, fiber.m_terrainGroundColor.z,
        fiber.m_bladeSegments, fiber.m_shadowBladeSegments, fiber.m_seed, fiber.m_chunkedPlacement,
        fiber.m_chunkSize, fiber.m_maxSlope, fiber.m_maxBlades, fiber.m_paintPath, fiber.m_terrainPatchCells,
        fiber.m_terrainLayer, fiber.m_terrainLayerThreshold, fiber.m_flowChannels, fiber.m_shadowShellCount,
        fiber.m_colorTint.x, fiber.m_colorTint.y, fiber.m_colorTint.z, fiber.m_lengthScale,
        fiber.m_densityScale, fiber.m_clumpingScale, fiber.m_maskPath, fiber.m_materialPath);
}

scene::FiberComponent TerrainGrassSnapshot(const scene::FiberComponent& source)
{
    auto snapshot = source;
    snapshot.m_renderIdentity = 0;
    snapshot.m_motionHistories.clear();
    snapshot.m_placementRootCount = 0;
    snapshot.m_placementChunkCount = 0;
    snapshot.m_placementFailed = false;
    return snapshot;
}

scene::GameObject* TerrainGrassTarget(const EditorContext& context, const OpArgs& args)
{
    if (!context.activeScene) return nullptr;
    const std::string node = args.GetString("node");
    return node.empty() ? context.GetSelectedGO() : context.activeScene->FindByGuid(node);
}

class TerrainGrassCommand final : public ICommand {
public:
    TerrainGrassCommand(EditorContext& context, scene::GameObject& object,
        std::optional<scene::FiberComponent> before, scene::FiberComponent after)
        : m_context(context), m_scene(context.activeScene), m_generation(m_scene->GetRenderSceneGeneration()),
          m_node(object.instanceId), m_before(std::move(before)), m_after(std::move(after)),
          m_terrainLifetime(m_scene->GetComponentLifetime<scene::TerrainComponent>(object.GetID())),
          m_fiberLifetime(m_scene->GetComponentLifetime<scene::FiberComponent>(object.GetID())) {}

    void Execute() override { std::string reason; (void)TryExecute(reason); }
    void Undo() override { std::string reason; (void)TryUndo(reason); }
    std::string GetDescription() const override { return "Set Up Terrain Grass"; }
    bool SupportsLegacyComposition() const override { return false; }
    bool TryExecute(std::string& reason) override { return Apply(true, reason); }
    bool TryUndo(std::string& reason) override { return Apply(false, reason); }
    bool ValidateHistory(std::string& reason) const override
    {
        if (m_context.activeScene != m_scene || m_scene->GetRenderSceneGeneration() != m_generation) {
            reason = "草を設定したシーンが変わりました。";
            return false;
        }
        auto* object = m_scene->FindByGuid(m_node);
        if (!object || !object->GetComponent<scene::TerrainComponent>()
            || m_scene->GetComponentLifetime<scene::TerrainComponent>(object->GetID()) != m_terrainLifetime) {
            reason = "対象の地形が削除または置換されました。";
            return false;
        }
        const auto* fiber = object->GetComponent<scene::FiberComponent>();
        const auto* expected = m_applied ? &m_after : m_before ? &*m_before : nullptr;
        if (expected ? (!fiber || m_scene->GetComponentLifetime<scene::FiberComponent>(object->GetID()) != m_fiberLifetime
                || TerrainGrassValues(*fiber) != TerrainGrassValues(*expected)) : fiber != nullptr) {
            reason = "地形の草が別の操作で変更されています。";
            return false;
        }
        return true;
    }
private:
    bool Apply(bool after, std::string& reason)
    {
        if (after == m_applied || !ValidateHistory(reason)) return false;
        auto* object = m_scene->FindByGuid(m_node);
        const auto* value = after ? &m_after : m_before ? &*m_before : nullptr;
        if (value) {
            if (auto* fiber = object->GetComponent<scene::FiberComponent>()) *fiber = *value;
            else object->AddComponent<scene::FiberComponent>(*value);
            m_fiberLifetime = m_scene->GetComponentLifetime<scene::FiberComponent>(object->GetID());
        } else object->RemoveComponent<scene::FiberComponent>();
        m_applied = after;
        if (m_context.markSceneDirty) m_context.markSceneDirty();
        return true;
    }
    EditorContext& m_context;
    scene::Scene* m_scene;
    uint64_t m_generation;
    std::string m_node;
    std::optional<scene::FiberComponent> m_before;
    scene::FiberComponent m_after;
    uint64_t m_terrainLifetime;
    uint64_t m_fiberLifetime;
    bool m_applied = true;
};

OpParam TerrainGrassParameter(const char* name, OpParamType type, OpValue value, const char* description)
{
    OpParam param;
    param.name = name;
    param.type = type;
    param.required = false;
    param.defaultValue = std::move(value);
    param.desc = description;
    return param;
}

} /// @note namespace

void RegisterTerrainGrassOperators(OperatorRegistry& registry)
{
    EditorOperator op;
    op.id = "terrain.grass.setup";
    op.label = "Set Up Terrain Grass";
    op.category = "Terrain";
    op.desc = "指定レイヤーの重みに沿う草を地形へ追加または再設定する。Undo で復元できる。";
    op.kind = OpKind::Mutation;
    op.undoLabel = "Set Up Terrain Grass";
    op.params = {
        TerrainGrassParameter("node", OpParamType::NodeId, std::string{}, "対象の地形。省略時は選択中の対象。"),
        TerrainGrassParameter("material", OpParamType::String, std::string("Assets/Terrain/Alpine/GrassCards.mat"), "Fiber 用の材質。"),
        TerrainGrassParameter("terrainLayer", OpParamType::Int, 0, "草を生やす編集済みレイヤーの番号。"),
        TerrainGrassParameter("density", OpParamType::Float, 4.0f, "地形ローカル面積 1 m2 あたりの候補本数。0.001〜8、一ビューの距離窓の候補上限は50万。"),
        TerrainGrassParameter("bladeWidth", OpParamType::Float, 0.5f, "草束または葉の幅 [local m]。0.001〜0.5。"),
        TerrainGrassParameter("lodFar", OpParamType::Float, 80.0f, "草の表示距離 [world m]。1〜2000。"),
        TerrainGrassParameter("seed", OpParamType::Int, 1, "分布の種。0〜65535。")
    };
    op.poll = [](const OpContext& context, const OpArgs& args) {
        auto* object = TerrainGrassTarget(context.ctx, args);
        return object && object->GetComponent<scene::TerrainComponent>()
            && (!context.ctx.playMode || context.ctx.playMode->IsInEditor());
    };
    op.exec = [](OpContext& context, const OpArgs& args) -> OpResult {
        auto* object = TerrainGrassTarget(context.ctx, args);
        const auto* terrain = object ? object->GetComponent<scene::TerrainComponent>() : nullptr;
        if (!terrain) return OpResult::Err("NOT_TERRAIN", "対象の地形が見つかりません。");
        const int layer = args.GetInt("terrainLayer", 0);
        const float density = args.GetFloat("density", 4.0f);
        const float width = args.GetFloat("bladeWidth", 0.5f);
        const float farDistance = args.GetFloat("lodFar", 80.0f);
        const int seed = args.GetInt("seed", 1);
        if (layer < 0 || layer >= terrain->LayerCount() || !std::isfinite(density) || density < 0.001f || density > 8.0f
            || !std::isfinite(width) || width < 0.001f || width > 0.5f
            || !std::isfinite(farDistance) || farDistance < 1.0f || farDistance > 2000.0f || seed < 0 || seed > 65535)
            return OpResult::Err("BAD_ARG", "レイヤー・密度・幅・表示距離・種の範囲を確認してください。");
        double candidates = 0.0;
        int windowPatches = 0;
        bool patchBudget = false;
        if (!TerrainGrassCandidateBudget(*terrain, object->transform, density, farDistance, candidates, windowPatches, patchBudget))
            return OpResult::Err("BAD_TERRAIN", "地形の高さ配列・寸法・姿勢が不正です。");
        if (!patchBudget)
            return OpResult::Err("BUDGET_EXCEEDED", "一パッチの草の候補本数が32768を超えます。格子間隔または密度を下げてください。");
        if (candidates > TERRAIN_GRASS_MAX_WINDOW_CANDIDATES)
            return OpResult::Err("BUDGET_EXCEEDED", "一ビューの距離窓で草の候補本数が50万を超えます。表示距離または密度を下げてください。候補上限見積もり: " + std::to_string(candidates));
        const auto material = args.GetString("material", "Assets/Terrain/Alpine/GrassCards.mat");
        auto disk = ToProjectAssetDiskPath(context.ctx.projectRoot, material);
        std::error_code pathError;
        if (disk.empty() || !std::filesystem::is_regular_file(std::filesystem::u8path(disk), pathError))
            disk = asset::AssetManager::ResolveAssetPath(material);
        asset::MaterialAssetAuthoring materialValues;
        if (disk.empty() || !asset::LoadMaterialAssetFromFile(disk, materialValues))
            return OpResult::Err("NO_MATERIAL", "草の材質を読み込めません: " + material);
        const auto guid = asset::AssetDatabase::GuidFromPath(disk);
        if (guid.empty()) return OpResult::Err("NO_GUID", "草の材質の GUID を取得できません。");
        std::optional<scene::FiberComponent> before;
        if (const auto* fiber = object->GetComponent<scene::FiberComponent>()) before = TerrainGrassSnapshot(*fiber);
        scene::FiberComponent after = before ? *before : scene::FiberComponent{};
        after.m_enabled = true;
        after.m_mode = scene::FiberRenderMode::BLADE;
        after.m_materialPath = "guid:" + guid;
        after.m_bladeDensity = density;
        after.m_bladeWidth = width;
        after.m_bladeSegments = 3;
        after.m_shadowBladeSegments = 1;
        after.m_seed = seed;
        after.m_chunkedPlacement = false;
        after.m_terrainPatchCells = TERRAIN_GRASS_PATCH_CELLS;
        after.m_terrainLayer = layer;
        after.m_terrainLayerThreshold = 0.25f;
        after.m_distanceLod = true;
        after.m_lodNear = farDistance * 0.25f;
        after.m_lodFar = farDistance;
        /// @note 地形の草は既存の層重みを根元密度に使う。Mesh 用ペイントや個体マスクを重ねて消さない。
        /// @see Docs/design/terrain-layers.md §5 Fiber を層で制御する
        after.m_paintPath.clear();
        after.m_maskPath.clear();
        after.m_densityScale = 1.0f;
        after.m_lengthScale = 1.0f;
        if (before && TerrainGrassValues(*before) == TerrainGrassValues(after)) {
            OpResult result;
            result.noChange = true;
            result.data = OpData::MakeObject();
            result.data.Set("candidateWindowUpperBound", static_cast<int>(candidates));
            result.data.Set("windowPatchUpperBound", windowPatches);
            result.message = "草の設定は変わりませんでした。";
            return result;
        }
        if (auto* fiber = object->GetComponent<scene::FiberComponent>()) *fiber = after;
        else object->AddComponent<scene::FiberComponent>(after);
        OpResult result;
        result.data = OpData::MakeObject();
        result.data.Set("candidateWindowUpperBound", static_cast<int>(candidates));
        result.data.Set("windowPatchUpperBound", windowPatches);
        result.command = std::make_unique<TerrainGrassCommand>(context.ctx, *object, std::move(before), std::move(after));
        result.message = "指定レイヤーに草を設定しました。地形のペイントで生える範囲を編集できます。";
        if (context.ctx.markSceneDirty) context.ctx.markSceneDirty();
        return result;
    };
    registry.Register(std::move(op));
}

} /// @note namespace fbzz::editor
