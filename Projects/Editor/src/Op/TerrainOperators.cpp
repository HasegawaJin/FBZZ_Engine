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
#include <Editor/Util/TerrainWaterDefaults.hpp>
#include <Tools/TerrainAuthoring.hpp>
#include <Tools/TerrainBrush.hpp>
#include <Tools/TerrainPaintClearance.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/BakeFingerprint.hpp>
#include <Engine/Scene/ComponentReflectionCodec.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/ControlPointAdapters.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/TerrainHeightMapLoader.hpp>
#include <Engine/Scene/TerrainSplat.hpp>
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Engine/Scene/Waterfall.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/MeshBuilder.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
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

constexpr std::size_t MAX_RIVER_SAMPLES = 8192;
constexpr std::size_t MAX_RIVER_TERRAIN_QUERIES = 1048576;
constexpr std::size_t MAX_REPORTED_ISSUES = 64;
constexpr float DEFAULT_RIVER_SAMPLE_SPACING = 2.0f;
constexpr float DEFAULT_MAX_UPHILL_RISE = 0.1f;
constexpr float DEFAULT_MAX_BURIED_DEPTH = 0.1f;
constexpr float DEFAULT_MAX_BED_GAP = 0.5f;
constexpr float DEFAULT_MAX_OUTLET_DIFFERENCE = 0.5f;
constexpr float DEFAULT_MAX_DOWNHILL_GRADE = 0.25f;
constexpr float DEFAULT_MAX_WIDTH_CHANGE_RATE = 0.35f;
constexpr float DEFAULT_MAX_DEPTH_CHANGE_RATE = 0.25f;
constexpr float DEFAULT_BANK_PROBE_OFFSET = 2.0f;
constexpr float DEFAULT_MAX_TERRAIN_SLOPE = 60.0f;
constexpr float DEFAULT_MIN_ISOLATED_RELIEF = 2.0f;

class NatureRuleReport {
public:
    void Add(const char* code, const math::Vector3& position, const char* metric = nullptr,
        double value = 0.0, std::optional<double> rate = std::nullopt)
    {
        ++m_count;
        if (m_issues.AsArray().size() >= MAX_REPORTED_ISSUES) return;
        auto row = OpData::MakeObject();
        row.Set("code", OpData(code));
        row.Set("position", OpData(position));
        if (metric) row.Set(metric, OpData(value));
        if (rate) row.Set("rate", OpData(*rate));
        m_issues.Push(std::move(row));
    }

    bool HasRoom() const { return m_issues.AsArray().size() < MAX_REPORTED_ISSUES; }

    void Finish(OpData& data) &&
    {
        data.Set("valid", OpData(m_count == 0));
        data.Set("issueCount", OpData(m_count));
        data.Set("truncated", OpData(static_cast<std::size_t>(m_count) > m_issues.AsArray().size()));
        data.Set("issues", std::move(m_issues));
    }

private:
    OpData m_issues = OpData::MakeArray();
    int m_count = 0;
};

void AddRuleRange(EditorOperator& op, const char* name, const char* description,
    float initial, float minimum, float maximum)
{
    auto param = TerrainParameter(name, OpParamType::Float, description, initial);
    param.hasRange = true;
    param.minValue = minimum;
    param.maxValue = maximum;
    op.params.push_back(std::move(param));
}

bool BuildAuthoredWater(scene::GameObject& owner, scene::WaterComponent& water,
    scene::WaterControlPointSnapshot& points, scene::WaterSplineSnapshot& mesh)
{
    points = scene::ResolveWaterControlPointAuthoring(*owner.GetScene(), owner.GetID(), water, water.pointRuntimeMode);
    return points.valid && scene::BuildWaterSplineSnapshot(water.surfaceShape, points.points,
        water.splineSamplesPerSegment, owner.transform, mesh, water.buoyancyDepth, water.splineMeshCellSize, water.lakeIslandPoints);
}

struct RiverSection {
    math::Vector2 left;
    math::Vector2 right;

    math::Vector2 Center() const { return {std::midpoint(left.x, right.x), std::midpoint(left.y, right.y)}; }
};

double DistanceXZ(const math::Vector2& a, const math::Vector2& b)
{
    return std::hypot(static_cast<double>(a.x) - b.x, static_cast<double>(a.y) - b.y);
}

bool MakeRiverSections(const scene::WaterSplineSnapshot& mesh, float spacing, bool reverse,
    std::vector<RiverSection>& sections)
{
    const auto& outline = mesh.outlineIndices;
    const auto stationCount = outline.size() / 2;
    for (std::size_t i = 0; i < stationCount; ++i) {
        const auto& left = mesh.vertices[outline[i]].worldPosition;
        const auto& right = mesh.vertices[outline[outline.size() - 1 - i]].worldPosition;
        const RiverSection section{{left.x, left.z}, {right.x, right.z}};
        if (!sections.empty()) {
            const auto previous = sections.back();
            const double steps = std::ceil(DistanceXZ(section.Center(), previous.Center()) / spacing);
            if (steps > MAX_RIVER_SAMPLES - sections.size()) return false;
            for (std::size_t step = 1; step < static_cast<std::size_t>(steps); ++step) {
                const float t = static_cast<float>(step) / static_cast<float>(steps);
                sections.push_back({{std::lerp(previous.left.x, section.left.x, t), std::lerp(previous.left.y, section.left.y, t)},
                    {std::lerp(previous.right.x, section.right.x, t), std::lerp(previous.right.y, section.right.y, t)}});
            }
        }
        if (sections.size() == MAX_RIVER_SAMPLES) return false;
        sections.push_back(section);
    }
    if (reverse) std::reverse(sections.begin(), sections.end());
    return true;
}

OpResult ValidateRiverRules(OpContext& context, const OpArgs& args)
{
    auto* world = context.ctx.activeScene;
    if (!world || (context.ctx.playMode && !context.ctx.playMode->IsInEditor()))
        return OpResult::Err("NOT_EDITABLE", "Edit Scene で川を選択してください。");
    auto* owner = ResolveTerrainNode(context, args.GetString("node"));
    auto* water = owner ? owner->GetComponent<scene::WaterComponent>() : nullptr;
    if (!water || !owner->activeInHierarchy() || !water->enabled
        || water->surfaceShape != scene::WaterSurfaceShape::SPLINE_RIVER)
        return OpResult::Err("NO_RIVER", "有効な Spline River を指定してください。");

    const float spacing = args.GetFloat("sampleSpacing", DEFAULT_RIVER_SAMPLE_SPACING);
    const float maxUphillRise = args.GetFloat("maxUphillRise", DEFAULT_MAX_UPHILL_RISE);
    const float maxBuriedDepth = args.GetFloat("maxBuriedDepth", DEFAULT_MAX_BURIED_DEPTH);
    const float maxBedGap = args.GetFloat("maxBedGap", DEFAULT_MAX_BED_GAP);
    const float maxOutletDifference = args.GetFloat("maxOutletHeightDifference", DEFAULT_MAX_OUTLET_DIFFERENCE);
    const float maxDownhillGrade = args.GetFloat("maxDownhillGrade", DEFAULT_MAX_DOWNHILL_GRADE);
    const float maxWidthChangeRate = args.GetFloat("maxWidthChangeRate", DEFAULT_MAX_WIDTH_CHANGE_RATE);
    const float maxDepthChangeRate = args.GetFloat("maxDepthChangeRate", DEFAULT_MAX_DEPTH_CHANGE_RATE);
    const float minBankRise = args.GetFloat("minBankRise", 0.0f);
    const float bankProbeOffset = args.GetFloat("bankProbeOffset", DEFAULT_BANK_PROBE_OFFSET);
    for (const float value : {spacing, maxUphillRise, maxBuriedDepth, maxBedGap, maxOutletDifference,
            maxDownhillGrade, maxWidthChangeRate, maxDepthChangeRate, minBankRise, bankProbeOffset})
        if (!std::isfinite(value)) return OpResult::Err("BAD_ARG", "検査しきい値は有限値で指定してください。");

    scene::FlushWorldTransforms(*world);
    scene::WaterControlPointSnapshot points;
    scene::WaterSplineSnapshot mesh;
    if (!BuildAuthoredWater(*owner, *water, points, mesh))
        return OpResult::Err("INVALID_RIVER", "川の制御点・変換・水面形状を確認してください。");
    const bool forward = std::any_of(points.points.begin(), points.points.end(),
        [](const auto& point) { return point.flowSpeed > 0.0f; });
    const bool reverse = std::any_of(points.points.begin(), points.points.end(),
        [](const auto& point) { return point.flowSpeed < 0.0f; });
    const bool mixed = forward && reverse;
    std::vector<RiverSection> sections;
    if (!MakeRiverSections(mesh, spacing, reverse && !mixed, sections))
        return OpResult::Err("SAMPLE_BUDGET", "標本が上限を超えました。sampleSpacing を広げるか、川を分けてください。");

    const auto terrainId = args.GetString("terrain");
    std::vector<std::pair<const scene::GameObject*, const scene::TerrainComponent*>> grounds;
    for (auto id : world->GetEntities<scene::TerrainComponent>()) {
        const auto* ground = world->GetGameObject(id);
        const auto* terrain = world->GetComponent<scene::TerrainComponent>(id);
        if (ground && ground->activeInHierarchy() && terrain && terrain->enabled
            && (terrainId.empty() || ground->instanceId == terrainId)) grounds.emplace_back(ground, terrain);
    }
    if (!terrainId.empty() && grounds.empty())
        return OpResult::Err("NO_TERRAIN", "指定した有効な Terrain が見つかりません。");
    const auto groundSamplesPerSection = minBankRise > 0.0f ? 3u : 1u;
    if (grounds.size() > MAX_RIVER_TERRAIN_QUERIES / (sections.size() * groundSamplesPerSection))
        return OpResult::Err("SAMPLE_BUDGET", "地形照会上限を超えました。terrain を指定するか、sampleSpacing を広げてください。");

    const auto sampleTerrain = [&](const math::Vector2& xz) {
        float height = -std::numeric_limits<float>::infinity();
        for (const auto& [ground, terrain] : grounds) {
            scene::TerrainSurfaceSample sample;
            if (scene::TrySampleTerrainWorld(*terrain, ground->transform, xz.x, xz.y, sample))
                height = (std::max)(height, sample.position.y);
        }
        return height;
    };

    NatureRuleReport report;
    float lowestOffset = std::numeric_limits<float>::infinity();
    math::Vector3 downstream;
    scene::WaterSplineSample previousSample;
    double previousWidth = 0.0;
    int terrainSampleCount = 0;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const auto& section = sections[i];
        const auto xz = section.Center();
        scene::WaterSplineSample sample;
        if (!scene::TrySampleWaterSpline(mesh, xz.x, xz.y, sample))
            return OpResult::Err("INVALID_RIVER", "川の中心線で水面を取得できません。");
        const math::Vector3 position{xz.x, sample.baseHeight, xz.y};
        if (mixed && i == 0) report.Add("MIXED_FLOW", position);
        lowestOffset = (std::min)(lowestOffset, sample.baseHeightOffset);
        const double rise = static_cast<double>(sample.baseHeightOffset) - lowestOffset;
        if (!mixed && rise > maxUphillRise) report.Add("UPHILL", position, "differenceMeters", rise);
        downstream = position;

        const double width = DistanceXZ(section.left, section.right);
        if (i != 0) {
            const double distance = DistanceXZ(xz, sections[i - 1].Center());
            const auto checkRate = [&](const char* code, double change, float limit) {
                if (limit > 0.0f && distance > 0.0 && change > limit * distance)
                    report.Add(code, position, "differenceMeters", change, change / distance);
            };
            if (!mixed) checkRate("STEEP_DROP", static_cast<double>(previousSample.baseHeightOffset) - sample.baseHeightOffset, maxDownhillGrade);
            checkRate("ABRUPT_WIDTH", std::abs(width - previousWidth), maxWidthChangeRate);
            checkRate("ABRUPT_DEPTH", std::abs(static_cast<double>(sample.depth) - previousSample.depth), maxDepthChangeRate);
        }
        previousWidth = width;
        previousSample = sample;

        const float terrainHeight = sampleTerrain(xz);
        if (!std::isfinite(terrainHeight)) {
            report.Add("MISSING_TERRAIN", position);
            continue;
        }
        ++terrainSampleCount;
        const double groundAboveWater = static_cast<double>(terrainHeight) - sample.baseHeight;
        const double bedGap = -groundAboveWater - sample.depth;
        if (groundAboveWater > maxBuriedDepth) report.Add("BURIED_SURFACE", position, "differenceMeters", groundAboveWater);
        if (bedGap > maxBedGap) report.Add("FLOATING_BED", position, "differenceMeters", bedGap);

        if (minBankRise <= 0.0f || width <= 0.0) continue;
        const double offset = 2.0 * bankProbeOffset / width;
        for (const auto& edge : {section.left, section.right}) {
            const double x = edge.x + (static_cast<double>(edge.x) - xz.x) * offset;
            const double z = edge.y + (static_cast<double>(edge.y) - xz.y) * offset;
            constexpr double MAX_COORDINATE = std::numeric_limits<float>::max();
            if (std::abs(x) > MAX_COORDINATE || std::abs(z) > MAX_COORDINATE) {
                report.Add("MISSING_BANK_TERRAIN", position);
                continue;
            }
            const math::Vector2 probe{static_cast<float>(x), static_cast<float>(z)};
            const float height = sampleTerrain(probe);
            if (!std::isfinite(height)) {
                report.Add("MISSING_BANK_TERRAIN", {probe.x, position.y, probe.y});
                continue;
            }
            const double relief = static_cast<double>(height) - terrainHeight;
            if (relief < minBankRise)
                report.Add("LOW_BANK", {probe.x, height, probe.y}, "differenceMeters", minBankRise - relief);
        }
    }

    const auto outletId = args.GetString("outlet");
    if (!outletId.empty() && !mixed) {
        auto* outlet = world->FindByGuid(outletId);
        auto* target = outlet ? outlet->GetComponent<scene::WaterComponent>() : nullptr;
        if (!target || !outlet->activeInHierarchy() || !target->enabled
            || target->surfaceShape == scene::WaterSurfaceShape::SPLINE_RIVER)
            return OpResult::Err("INVALID_OUTLET", "有効な湖・矩形水域・Horizon 海を流出先に指定してください。");

        bool inside = false;
        float outletHeight = outlet->transform.worldPosition.y;
        if (target->surfaceShape == scene::WaterSurfaceShape::SPLINE_LAKE) {
            scene::WaterControlPointSnapshot outletPoints;
            scene::WaterSplineSnapshot outletMesh;
            if (!BuildAuthoredWater(*outlet, *target, outletPoints, outletMesh))
                return OpResult::Err("INVALID_OUTLET", "流出先の湖の形状を確認してください。");
            scene::WaterSplineSample sample;
            inside = scene::TrySampleWaterSpline(outletMesh, downstream.x, downstream.z, sample);
            if (inside) outletHeight = sample.baseHeight;
        } else {
            /// @note Flow and waves are irrelevant to the authored outlet footprint.
            scene::WaterComponent flat;
            flat.extentX = target->extentX;
            flat.extentZ = target->extentZ;
            flat.farOceanEnabled = target->farOceanEnabled;
            flat.enableGerstnerWaves = false;
            flat.waveCount = 0;
            if (!scene::IsWaterSurfaceSupported(flat, outlet->transform))
                return OpResult::Err("INVALID_OUTLET", "流出先の寸法と変換を確認してください。");
            inside = scene::WaterSurfaceContainsXZ(flat, outlet->transform, downstream.x, downstream.z, 0.0f);
        }
        const double difference = static_cast<double>(downstream.y) - outletHeight;
        if (!inside) report.Add("OUTLET_OUTSIDE", downstream);
        else if (std::abs(difference) > maxOutletDifference) report.Add("OUTLET_HEIGHT", downstream, "differenceMeters", difference);
    }

    auto data = OpData::MakeObject();
    data.Set("node", OpData(owner->instanceId));
    data.Set("name", OpData(owner->name));
    data.Set("flowDirection", OpData(mixed ? "mixed" : reverse ? "reverse" : "forward"));
    data.Set("sampleCount", OpData(static_cast<int>(sections.size())));
    data.Set("terrainSampleCount", OpData(terrainSampleCount));
    std::move(report).Finish(data);
    return OpResult::Data(std::move(data));
}

void RegisterRiverRuleOperator(OperatorRegistry& registry)
{
    EditorOperator op;
    op.id = "terrain.river.validate";
    op.label = "Validate River Nature Rules";
    op.category = "Terrain";
    op.kind = OpKind::Query;
    op.desc = "Spline River の流向・落差・川幅と水深の変化・川床・岸・指定した流出先を検査する。";
    op.params = {
        TerrainParameter("node", OpParamType::NodeId, "対象の川。省略時は選択中", std::string{}),
        TerrainParameter("terrain", OpParamType::NodeId, "指定地形に限定。省略時は有効な全地形", std::string{}),
        TerrainParameter("outlet", OpParamType::NodeId, "下流端の湖・矩形水域・Horizon 海。省略時は開放端", std::string{}),
    };
    AddRuleRange(op, "sampleSpacing", "Sample spacing [m]", DEFAULT_RIVER_SAMPLE_SPACING, 0.1f, 100.0f);
    AddRuleRange(op, "maxUphillRise", "Max uphill rise [m]", DEFAULT_MAX_UPHILL_RISE, 0.0f, 1000.0f);
    AddRuleRange(op, "maxBuriedDepth", "Max buried depth [m]", DEFAULT_MAX_BURIED_DEPTH, 0.0f, 1000.0f);
    AddRuleRange(op, "maxBedGap", "Max bed gap [m]", DEFAULT_MAX_BED_GAP, 0.0f, 1000.0f);
    AddRuleRange(op, "maxOutletHeightDifference", "Max outlet height difference [m]", DEFAULT_MAX_OUTLET_DIFFERENCE, 0.0f, 1000.0f);
    AddRuleRange(op, "maxDownhillGrade", "Max downhill grade [m/m, 0 = off]", DEFAULT_MAX_DOWNHILL_GRADE, 0.0f, 1000.0f);
    AddRuleRange(op, "maxWidthChangeRate", "Max width change [m/m, 0 = off]", DEFAULT_MAX_WIDTH_CHANGE_RATE, 0.0f, 1000.0f);
    AddRuleRange(op, "maxDepthChangeRate", "Max depth change [m/m, 0 = off]", DEFAULT_MAX_DEPTH_CHANGE_RATE, 0.0f, 1000.0f);
    AddRuleRange(op, "minBankRise", "Min bank rise [m, 0 = off]", 0.0f, 0.0f, 1000.0f);
    AddRuleRange(op, "bankProbeOffset", "Bank probe offset [m]", DEFAULT_BANK_PROBE_OFFSET, 0.0f, 1000.0f);
    op.exec = ValidateRiverRules;
    registry.Register(std::move(op));
}

OpResult ValidateTerrainRelief(OpContext& context, const OpArgs& args)
{
    auto* world = context.ctx.activeScene;
    if (!world || (context.ctx.playMode && !context.ctx.playMode->IsInEditor()))
        return OpResult::Err("NOT_EDITABLE", "Edit Scene で地形を選択してください。");
    const auto* owner = ResolveTerrainNode(context, args.GetString("node"));
    const auto* terrain = owner ? world->GetComponent<scene::TerrainComponent>(owner->GetID()) : nullptr;
    if (!terrain || !owner->activeInHierarchy() || !terrain->enabled)
        return OpResult::Err("NO_TERRAIN", "有効な Terrain を指定してください。");

    const float maxSlope = args.GetFloat("maxSlopeDegrees", DEFAULT_MAX_TERRAIN_SLOPE);
    const float minRelief = args.GetFloat("minIsolatedRelief", DEFAULT_MIN_ISOLATED_RELIEF);
    if (!std::isfinite(maxSlope) || !std::isfinite(minRelief))
        return OpResult::Err("BAD_ARG", "検査しきい値は有限値で指定してください。");
    if (!IsValidTerrainDimension(terrain->columns) || !IsValidTerrainDimension(terrain->rows)
        || terrain->heightData.size() != terrain->VertexCount()
        || (!terrain->holeData.empty() && terrain->holeData.size() != terrain->CellCount())
        || !std::isfinite(terrain->cellSize) || terrain->cellSize <= 0.0f
        || !std::isfinite(terrain->maxHeight) || terrain->maxHeight <= 0.0f)
        return OpResult::Err("INVALID_TERRAIN", "地形の寸法または高さ場が不正です。");
    float minHeight = terrain->heightData.front();
    float maxHeight = minHeight;
    for (const float height : terrain->heightData) {
        if (!std::isfinite(height)) return OpResult::Err("INVALID_TERRAIN", "高さ場に非有限値があります。");
        minHeight = (std::min)(minHeight, height);
        maxHeight = (std::max)(maxHeight, height);
    }

    scene::FlushWorldTransforms(*world);
    const auto& pose = owner->transform;
    const auto up = pose.worldRotation * math::Vector3::UP;
    math::Vector3 normal;
    if (!scene::TryTerrainNormalToWorld(pose, math::Vector3::UP, normal)
        || std::abs(up.x) > 0.00001f || std::abs(up.z) > 0.00001f)
        return OpResult::Err("INVALID_TRANSFORM", "直立した地形の有効なワールド変換が必要です。");
    const float cell = terrain->cellSize;
    for (const float x : {0.0f, (terrain->columns - 1) * cell}) {
        for (const float z : {0.0f, (terrain->rows - 1) * cell}) {
            for (const float y : {minHeight * terrain->maxHeight, maxHeight * terrain->maxHeight}) {
                const auto corner = ToTerrainWorld(pose, {x, y, z});
                if (!std::isfinite(corner.x) || !std::isfinite(corner.y) || !std::isfinite(corner.z))
                    return OpResult::Err("INVALID_TRANSFORM", "地形のワールド範囲が非有限です。");
            }
        }
    }

    constexpr double RADIANS_PER_DEGREE = std::numbers::pi_v<double> / 180.0;
    const double gradeLimit = std::tan(maxSlope * RADIANS_PER_DEGREE);
    /// @note The heightfield stores float samples; an exact boundary must survive their rounding and tan().
    const double gradeLimitSquared = gradeLimit * gradeLimit * (1.0 + 8.0 * std::numeric_limits<float>::epsilon());
    const double dx = static_cast<double>(cell) * std::abs(pose.worldScale.x);
    const double dz = static_cast<double>(cell) * std::abs(pose.worldScale.z);
    const double heightScale = static_cast<double>(terrain->maxHeight) * pose.worldScale.y * up.y;
    const auto height = [&](int x, int z) {
        return terrain->heightData[static_cast<std::size_t>(z) * terrain->columns + x];
    };
    const auto gradeSquared = [&](double xRise, double zRise) {
        const double xGrade = xRise * heightScale / dx;
        const double zGrade = zRise * heightScale / dz;
        return xGrade * xGrade + zGrade * zGrade;
    };
    const auto worldPoint = [&](float x, float z, float h) {
        return ToTerrainWorld(pose, {x * cell, h * terrain->maxHeight, z * cell});
    };

    NatureRuleReport report;
    int checkedCells = 0;
    int checkedVertices = 0;
    for (int z = 0; z + 1 < terrain->rows; ++z) {
        for (int x = 0; x + 1 < terrain->columns; ++x) {
            if (terrain->IsHoleCell(x, z)) continue;
            const double center = height(x, z);
            const double east = height(x + 1, z);
            const double south = height(x, z + 1);
            if (maxSlope > 0.0f) {
                ++checkedCells;
                const double diagonal = height(x + 1, z + 1);
                /// @note TerrainSurfaceQuery splits the cell along the east-south edge.
                const double grade = (std::max)(gradeSquared(east - center, south - center),
                    gradeSquared(diagonal - south, diagonal - east));
                if (grade > gradeLimitSquared) {
                    const auto position = report.HasRoom()
                        ? worldPoint(x + 0.5f, z + 0.5f, static_cast<float>(std::midpoint(east, south))) : math::Vector3{};
                    const double angle = report.HasRoom() ? std::atan(std::sqrt(grade)) / RADIANS_PER_DEGREE : 0.0;
                    report.Add("STEEP_TERRAIN", position, "slopeDegrees", angle);
                }
            }
            if (minRelief <= 0.0f || x == 0 || z == 0 || terrain->IsHoleCell(x - 1, z)
                || terrain->IsHoleCell(x, z - 1) || terrain->IsHoleCell(x - 1, z - 1)) continue;
            ++checkedVertices;
            const auto [lowest, highest] = std::minmax({east, south,
                static_cast<double>(height(x - 1, z)), static_cast<double>(height(x, z - 1))});
            const double difference = center > highest ? (center - highest) * heightScale
                : center < lowest ? (center - lowest) * heightScale : 0.0;
            if (std::abs(difference) >= minRelief) {
                const auto position = report.HasRoom() ? worldPoint(x, z, static_cast<float>(center)) : math::Vector3{};
                report.Add(difference > 0.0 ? "ISOLATED_SPIKE" : "ISOLATED_PIT", position, "differenceMeters", difference);
            }
        }
    }
    auto data = OpData::MakeObject();
    data.Set("node", OpData(owner->instanceId));
    data.Set("name", OpData(owner->name));
    data.Set("sampleCount", OpData(checkedCells + checkedVertices));
    data.Set("checkedCells", OpData(checkedCells));
    data.Set("checkedVertices", OpData(checkedVertices));
    std::move(report).Finish(data);
    return OpResult::Data(std::move(data));
}

void RegisterTerrainReliefOperator(OperatorRegistry& registry)
{
    EditorOperator op;
    op.id = "terrain.relief.validate";
    op.label = "Validate Terrain Relief";
    op.category = "Terrain";
    op.kind = OpKind::Query;
    op.desc = "Terrain 高さ場の急斜面と孤立した突起・くぼみを検査する。";
    op.params = {TerrainParameter("node", OpParamType::NodeId, "対象の地形。省略時は選択中", std::string{})};
    AddRuleRange(op, "maxSlopeDegrees", "Max slope [deg, 0 = off]", DEFAULT_MAX_TERRAIN_SLOPE, 0.0f, 89.9f);
    AddRuleRange(op, "minIsolatedRelief", "Min isolated relief [m, 0 = off]", DEFAULT_MIN_ISOLATED_RELIEF, 0.0f, 1000.0f);
    op.exec = ValidateTerrainRelief;
    registry.Register(std::move(op));
}

struct TerrainSeamTile {
    const scene::TerrainComponent* terrain;
    double x, y, z, stepX, stepZ, heightScale;

    double East() const { return x + stepX * (terrain->columns - 1); }
    double South() const { return z + stepZ * (terrain->rows - 1); }
};

bool ReadSeamTile(const scene::GameObject& owner, TerrainSeamTile& out)
{
    const auto* terrain = owner.GetScene()->GetComponent<scene::TerrainComponent>(owner.GetID());
    const auto& pose = owner.transform;
    const auto& q = pose.worldRotation;
    const auto scale = pose.worldScale;
    const auto position = pose.worldPosition;
    if (!terrain || !terrain->enabled || !owner.activeInHierarchy()
        || !IsValidTerrainDimension(terrain->columns) || !IsValidTerrainDimension(terrain->rows)
        || terrain->heightData.size() != terrain->VertexCount()
        || !std::isfinite(terrain->cellSize) || terrain->cellSize <= 0.0f
        || !std::isfinite(terrain->maxHeight) || terrain->maxHeight <= 0.0f
        || !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)
        || !std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z)
        || scale.x <= 0.0f || scale.y <= 0.0f || scale.z <= 0.0f
        || q.x != 0.0f || q.y != 0.0f || q.z != 0.0f || std::abs(q.w) != 1.0f) return false;
    out = {terrain, position.x, position.y, position.z, static_cast<double>(scale.x) * terrain->cellSize,
        static_cast<double>(scale.z) * terrain->cellSize, static_cast<double>(scale.y) * terrain->maxHeight};
    constexpr double LIMIT = std::numeric_limits<float>::max();
    return std::abs(out.East()) <= LIMIT && std::abs(out.South()) <= LIMIT;
}

struct TerrainSeamEdge {
    const TerrainSeamTile& tile;
    bool alongZ;
    bool highSide;

    double Origin() const { return alongZ ? tile.z : tile.x; }
    double Step() const { return alongZ ? tile.stepZ : tile.stepX; }
    int Count() const { return alongZ ? tile.terrain->rows : tile.terrain->columns; }
    int CrossCount() const { return alongZ ? tile.terrain->columns : tile.terrain->rows; }
    double CrossStep() const { return alongZ ? tile.stepX : tile.stepZ; }
    double End() const { return Origin() + (Count() - 1) * Step(); }
    double Fixed() const { return alongZ ? (highSide ? tile.East() : tile.x) : (highSide ? tile.South() : tile.z); }

    double GridPosition(double position) const
    {
        return std::clamp((position - Origin()) / Step(), 0.0, static_cast<double>(Count() - 1));
    }

    std::size_t HeightIndex(int vertex, int inward) const
    {
        const int fixed = highSide ? CrossCount() - 1 - inward : inward;
        const int x = alongZ ? fixed : vertex;
        const int z = alongZ ? vertex : fixed;
        return static_cast<std::size_t>(z) * tile.terrain->columns + x;
    }

    double HeightSample(int vertex, int inward = 0) const
    {
        return tile.terrain->heightData[HeightIndex(vertex, inward)];
    }

    double Height(double position) const
    {
        const double grid = GridPosition(position);
        const int first = (std::min)(static_cast<int>(grid), Count() - 2);
        return tile.y + std::lerp(HeightSample(first), HeightSample(first + 1), grid - first) * tile.heightScale;
    }

    /// @note The east–south diagonal puts the high edge on the triangle using the next row or column for cross slope.
    /// @note Both opposing edges report rise per metre in the same positive world axis direction.
    double CrossGradient(double position) const
    {
        const int first = (std::min)(static_cast<int>(GridPosition(position)), Count() - 2);
        const int vertex = highSide ? first + 1 : first;
        const double outer = HeightSample(vertex), inner = HeightSample(vertex, 1);
        if (!std::isfinite(outer) || !std::isfinite(inner)) return std::numeric_limits<double>::quiet_NaN();
        const double rise = highSide ? outer - inner : inner - outer;
        return rise * tile.heightScale / CrossStep();
    }

    double CrossSlopeDegrees(double position) const
    {
        return std::atan(CrossGradient(position)) * (180.0 / std::numbers::pi_v<double>);
    }
};

constexpr double SEAM_EDGE_TOLERANCE = 0.01;

std::optional<std::pair<TerrainSeamEdge, TerrainSeamEdge>> FindTerrainSeam(const TerrainSeamTile& first, const TerrainSeamTile& second)
{
    for (const bool alongZ : {false, true}) {
        for (const bool highSide : {false, true}) {
            const TerrainSeamEdge a{first, alongZ, highSide}, b{second, alongZ, !highSide};
            const double overlap = (std::min)(a.End(), b.End()) - (std::max)(a.Origin(), b.Origin());
            if (std::abs(a.Fixed() - b.Fixed()) <= SEAM_EDGE_TOLERANCE && overlap > SEAM_EDGE_TOLERANCE)
                return std::pair{a, b};
        }
    }
    return std::nullopt;
}

OpResult ValidateTerrainSeam(OpContext& context, const OpArgs& args)
{
    auto* world = context.ctx.activeScene;
    if (!world || (context.ctx.playMode && !context.ctx.playMode->IsInEditor()))
        return OpResult::Err("NOT_EDITABLE", "Edit Scene で地形を選択してください。");
    const auto* owner = ResolveTerrainNode(context, args.GetString("node"));
    const auto* neighbor = world->FindByGuid(args.GetString("neighbor"));
    if (!owner || !neighbor || owner == neighbor)
        return OpResult::Err("NO_TERRAIN", "別々の Terrain を node と neighbor に指定してください。");
    constexpr float DEFAULT_HEIGHT_TOLERANCE = 0.05f;
    const float tolerance = args.GetFloat("heightTolerance", DEFAULT_HEIGHT_TOLERANCE);
    const float slopeTolerance = args.GetFloat("maxCrossSlopeDifference", 0.0f);
    if (!std::isfinite(tolerance) || !std::isfinite(slopeTolerance))
        return OpResult::Err("BAD_ARG", "高さと角度の許容値は有限値で指定してください。");
    scene::FlushWorldTransforms(*world);
    TerrainSeamTile first{}, second{};
    if (!ReadSeamTile(*owner, first) || !ReadSeamTile(*neighbor, second))
        return OpResult::Err("INVALID_TERRAIN", "正のスケールを持つ軸平行の有効 Terrain が必要です。");

    /// @note Compare the union of both edge lattices. The difference is linear between consecutive samples.
    /// @see https://dev.epicgames.com/documentation/unreal-engine/landscape-technical-guide-in-unreal-engine Epic Landscape Technical Guide: Landscape Components, shared edge vertices.
    const auto seam = FindTerrainSeam(first, second);
    if (!seam) return OpResult::Err("NO_SHARED_EDGE", "指定した Terrain 同士に共有辺がありません。");
    const auto& [a, b] = *seam;
    const double start = (std::max)(a.Origin(), b.Origin());
    const double end = (std::min)(a.End(), b.End());
    std::vector<double> positions{start, end};
    for (const auto& edge : {a, b}) {
        for (int i = 1; i + 1 < edge.Count(); ++i) {
            const double position = edge.Origin() + i * edge.Step();
            if (position > start && position < end) positions.push_back(position);
        }
    }
    std::sort(positions.begin(), positions.end());
    positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
    const auto worldPoint = [&](double position, double height) {
        return math::Vector3{static_cast<float>(a.alongZ ? a.Fixed() : position), static_cast<float>(height),
            static_cast<float>(a.alongZ ? position : a.Fixed())};
    };
    NatureRuleReport report;
    double maxDifference = 0.0;
    for (const double position : positions) {
        const double firstHeight = a.Height(position), secondHeight = b.Height(position);
        constexpr double LIMIT = std::numeric_limits<float>::max();
        if (!std::isfinite(firstHeight) || !std::isfinite(secondHeight)
            || std::abs(firstHeight) > LIMIT || std::abs(secondHeight) > LIMIT)
            return OpResult::Err("INVALID_TERRAIN", "境界の高さをワールド座標で表せません。");
        const double difference = std::abs(firstHeight - secondHeight);
        maxDifference = (std::max)(maxDifference, difference);
        if (difference > tolerance)
            report.Add("TERRAIN_SEAM_STEP", worldPoint(position, firstHeight), "differenceMeters", difference);
    }
    int slopeSampleCount = 0;
    double maxSlopeDifference = 0.0;
    if (slopeTolerance > 0.0f) {
        for (std::size_t i = 1; i < positions.size(); ++i) {
            const double position = std::midpoint(positions[i - 1], positions[i]);
            if (position <= positions[i - 1] || position >= positions[i]) continue;
            const double difference = std::abs(a.CrossSlopeDegrees(position) - b.CrossSlopeDegrees(position));
            if (!std::isfinite(difference))
                return OpResult::Err("INVALID_TERRAIN", "境界に接する三角形の高さが不正です。");
            ++slopeSampleCount;
            maxSlopeDifference = (std::max)(maxSlopeDifference, difference);
            if (difference > slopeTolerance)
                report.Add("TERRAIN_SEAM_SLOPE", worldPoint(position, a.Height(position)), "slopeDegrees", difference);
        }
    }
    auto data = OpData::MakeObject();
    data.Set("node", OpData(owner->instanceId));
    data.Set("neighbor", OpData(neighbor->instanceId));
    data.Set("sampleCount", OpData(static_cast<int>(positions.size())));
    data.Set("maxDifferenceMeters", OpData(maxDifference));
    data.Set("slopeSampleCount", OpData(slopeSampleCount));
    if (slopeTolerance > 0.0f) data.Set("maxCrossSlopeDifferenceDegrees", OpData(maxSlopeDifference));
    std::move(report).Finish(data);
    return OpResult::Data(std::move(data));
}

void RegisterTerrainSeamOperator(OperatorRegistry& registry)
{
    EditorOperator op;
    op.id = "terrain.seam.validate";
    op.label = "Validate Terrain Seam";
    op.category = "Terrain";
    op.kind = OpKind::Query;
    op.desc = "隣接する2枚の Terrain の共有辺で高さと斜面の折れを検査する。";
    op.params = {TerrainParameter("node", OpParamType::NodeId, "対象地形。省略時は選択中", std::string{}),
        TerrainParameter("neighbor", OpParamType::NodeId, "隣接する地形のGUID")};
    AddRuleRange(op, "heightTolerance", "Height tolerance [m]", 0.05f, 0.0f, 1000.0f);
    AddRuleRange(op, "maxCrossSlopeDifference", "Cross-slope tolerance [deg, 0 = off]", 0.0f, 0.0f, 180.0f);
    op.exec = ValidateTerrainSeam;
    registry.Register(std::move(op));
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

bool ResolveMapSurfaces(scene::Scene& world, const scene::GameObject& owner, const OpArgs& args,
    std::vector<TerrainAnalysisSurface>& surfaces)
{
    const auto* tiles = args.GetData("terrainTiles");
    if (!ResolveTerrainAnalysisSurfaces(world, &owner, tiles, surfaces)) return false;
    if (tiles && !tiles->IsNull()) {
        const auto target = std::find_if(surfaces.begin(), surfaces.end(), [&](const auto& surface) { return surface.pose == &owner.transform; });
        if (target == surfaces.end()) return false;
        std::iter_swap(surfaces.begin(), target);
        return true;
    }
    if (!args.GetBool("includeNeighbors", true)) return true;

    TerrainSeamTile first{};
    if (!ReadSeamTile(owner, first)) return false;
    std::vector<TerrainSeamTile> connected{first};
    struct Candidate { TerrainSeamTile tile; const scene::Transform* pose; };
    std::vector<Candidate> candidates;
    for (const auto id : world.GetEntities<scene::TerrainComponent>()) {
        const auto* node = world.GetGameObject(id);
        TerrainSeamTile tile{};
        if (node && node != &owner && ReadSeamTile(*node, tile)) candidates.push_back({tile, &node->transform});
    }
    for (std::size_t i = 0; i < connected.size(); ++i) {
        for (auto& candidate : candidates) {
            if (!candidate.pose || !FindTerrainSeam(connected[i], candidate.tile)) continue;
            if (surfaces.size() == 16) return false;
            connected.push_back(candidate.tile);
            surfaces.push_back({candidate.tile.terrain, candidate.pose});
            candidate.pose = nullptr;
        }
    }
    return true;
}

std::filesystem::path TerrainAuthoringAssetPath(const EditorContext& context, const std::string& requested, std::string_view suffix,
    bool allowAbsolute = false)
{
    if (context.projectRoot.empty()) return {};
    const auto path = util::FileSystem::PathFromUtf8(requested);
    if ((!allowAbsolute && path.has_root_path()) || !util::FileSystem::PathToUtf8(path.filename()).ends_with(suffix)) return {};
    const auto project = util::FileSystem::PathFromUtf8(context.projectRoot);
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(project / "Assets", error);
    if (error) return {};
    const auto output = std::filesystem::weakly_canonical(project / path, error);
    if (error) return {};
    const auto relative = output.lexically_relative(root);
    return relative.empty() || relative.is_absolute() || *relative.begin() == ".." ? std::filesystem::path{} : output;
}

TerrainFlowRegion ReadFlowRegion(const OpArgs& args, const scene::GameObject& owner, float sizeDefault = 0.0f)
{
    const auto& terrain = *owner.GetScene()->GetComponent<scene::TerrainComponent>(owner.GetID());
    const auto& pose = owner.transform;
    const auto center = pose.worldPosition + math::Vector3{(terrain.columns - 1) * terrain.cellSize * pose.worldScale.x * 0.5f,
        0.0f, (terrain.rows - 1) * terrain.cellSize * pose.worldScale.z * 0.5f};
    return {args.GetVec3("flowCenter", center), args.GetFloat("flowSize", sizeDefault)};
}

TerrainMapSettings ReadMapSettings(const OpArgs& args, const scene::GameObject& owner)
{
    TerrainMapSettings settings;
    settings.resolution = args.GetInt("resolution", settings.resolution);
    settings.radius = args.GetFloat("radius", settings.radius);
    settings.flowFullArea = args.GetFloat("flowFullArea", settings.flowFullArea);
    settings.density = args.GetFloat("density", settings.density);
    for (std::size_t i = 0; i < settings.preferences.size(); ++i)
        settings.preferences[i] = args.GetFloat(TERRAIN_MAP_NAMES[i], settings.preferences[i]);
    settings.flowRegion = ReadFlowRegion(args, owner);
    return settings;
}

float HabitatTransition(float value, float low, float high, float fade)
{
    const float edge = (std::min)(low > 0.0f ? value - low : std::numeric_limits<float>::infinity(),
        high > 0.0f ? high - value : std::numeric_limits<float>::infinity());
    if (fade == 0.0f) return edge >= 0.0f ? 1.0f : 0.0f;
    const float t = std::clamp(edge / fade, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

bool PrepareTerrainHabitat(const OpArgs& args, scene::Scene& world, const scene::GameObject& owner, std::span<const TerrainAnalysisSurface> surfaces,
    std::function<float(const math::Vector3&)>& weight)
{
    TerrainHabitatSettings habitat;
    habitat.patchScale = args.GetFloat("patchScale");
    habitat.patchMin = args.GetFloat("patchMin");
    habitat.patchMax = args.GetFloat("patchMax", 1.0f);
    habitat.seed = args.GetInt("patchSeed", 1);
    habitat.avoidWater = args.GetBool("avoidWater");
    habitat.pathLayer = args.GetInt("pathLayer", -1);
    habitat.water = {args.GetFloat("waterMin"), args.GetFloat("waterMax"), args.GetFloat("waterFade", 8.0f)};
    habitat.path = {args.GetFloat("pathMin"), args.GetFloat("pathMax"), args.GetFloat("pathFade", 8.0f)};
    const auto validBand = [](const TerrainHabitatBand& band) {
        return std::isfinite(band.minimum) && std::isfinite(band.maximum) && std::isfinite(band.fade)
            && (band.maximum == 0.0f || band.maximum >= band.minimum);
    };
    const auto& target = surfaces.front();
    if (!std::isfinite(habitat.patchScale) || !std::isfinite(habitat.patchMin) || !std::isfinite(habitat.patchMax)
        || habitat.patchMin > habitat.patchMax || habitat.seed < 0 || !validBand(habitat.water) || !validBand(habitat.path)
        || habitat.pathLayer < -1 || habitat.pathLayer >= target.terrain->LayerCount()) return false;
    const auto search = [](const TerrainHabitatBand& band) { return (std::max)(band.maximum, band.minimum > 0.0f ? band.minimum + band.fade : 0.0f); };
    const float waterSearch = search(habitat.water);
    const float pathSearch = habitat.pathLayer >= 0 ? search(habitat.path) : 0.0f;
    if (habitat.patchScale <= 0.0f && !habitat.avoidWater && waterSearch == 0.0f && pathSearch == 0.0f) return true;

    std::vector<std::pair<const scene::WaterComponent*, const scene::Transform*>> waters;
    if (waterSearch > 0.0f || habitat.avoidWater) {
        for (const auto id : world.GetEntities<scene::WaterComponent>()) {
            const auto* node = world.GetGameObject(id);
            const auto* water = world.GetComponent<scene::WaterComponent>(id);
            if (node && node->activeInHierarchy() && water && water->enabled) waters.emplace_back(water, &node->transform);
        }
        if (waters.size() > 128) return false;
    }
    std::vector<TerrainPaintClearance> clearances;
    if ((!waters.empty() && waterSearch > 0.0f) || pathSearch > 0.0f) {
        TerrainSeamTile tile{};
        if (!ReadSeamTile(owner, tile)) return false;
        const float west = static_cast<float>(tile.x), east = static_cast<float>(tile.East());
        const float north = static_cast<float>(tile.z), south = static_cast<float>(tile.South());
        const std::array<math::Vector3, 4> corners{{{west, 0, north}, {east, 0, north}, {east, 0, south}, {west, 0, south}}};
        const TerrainScatterBrush region{corners, 0.25f, false, true};
        const std::string pathMaterial = pathSearch > 0.0f ? target.terrain->layerMaterials[habitat.pathLayer] : std::string{};
        const auto pathGuid = asset::AssetDatabase::GuidFromRef(pathMaterial);
        for (const auto& surface : surfaces) {
            const auto scale = surface.pose->worldScale;
            const auto& rotation = surface.pose->worldRotation;
            if (!IsValidTerrainDimension(surface.terrain->columns) || !IsValidTerrainDimension(surface.terrain->rows)
                || !std::isfinite(surface.terrain->cellSize) || surface.terrain->cellSize <= 0.0f
                || surface.terrain->heightData.size() != surface.terrain->VertexCount()
                || !std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z)
                || scale.x <= 0.0f || scale.y <= 0.0f || scale.z <= 0.0f
                || rotation.x != 0.0f || rotation.y != 0.0f || rotation.z != 0.0f || std::abs(rotation.w) != 1.0f) return false;
            std::vector<int> layers;
            for (int layer = 0; layer < surface.terrain->LayerCount() && !pathMaterial.empty(); ++layer) {
                const auto& material = surface.terrain->layerMaterials[layer];
                if (material == pathMaterial || (!pathGuid.empty() && asset::AssetDatabase::GuidFromRef(material) == pathGuid)) layers.push_back(layer);
            }
            clearances.push_back(BuildTerrainPaintClearance(*surface.terrain, *surface.pose, region, layers, waters,
                waterSearch, layers.empty() ? 0.0f : pathSearch, true));
        }
    }
    weight = [habitat, target, waters = std::move(waters), clearances = std::move(clearances), waterSearch, pathSearch](const math::Vector3& point) {
        float density = 1.0f;
        if (habitat.patchScale > 0.0f) {
            const float patch = SampleTerrainPatchNoise(point.x, point.z, habitat.patchScale, static_cast<std::uint32_t>(habitat.seed));
            density = HabitatTransition(patch, habitat.patchMin, habitat.patchMax == 1.0f ? 0.0f : habitat.patchMax, 0.12f);
        }
        if (density == 0.0f) return 0.0f;
        float waterDistance = std::numeric_limits<float>::infinity(), pathDistance = waterDistance;
        for (const auto& [water, pose] : waters) {
            scene::WaterSurfaceHeightSample sample;
            if (scene::TrySampleWaterSurfaceHeightWorld(*water, *pose, point, 0.0f, sample) && sample.insideSurface
                && !sample.excluded && point.y <= sample.position.y + 0.15f) {
                if (habitat.avoidWater) return 0.0f;
                waterDistance = 0.0f;
                break;
            }
        }
        if (pathSearch > 0.0f) {
            const auto& pose = *target.pose;
            const float x = (point.x - pose.worldPosition.x) / pose.worldScale.x;
            const float z = (point.z - pose.worldPosition.z) / pose.worldScale.z;
            if (SampleTerrainLayerWeight(*target.terrain, x, z, habitat.pathLayer) >= 0.35f) pathDistance = 0.0f;
        }
        for (const auto& clearance : clearances) {
            math::Vector3 nearest;
            if (waterSearch > 0.0f && waterDistance > 0.0f && clearance.NearestWater(point, waterSearch, nearest))
                waterDistance = (std::min)(waterDistance, std::hypot(point.x - nearest.x, point.z - nearest.z));
            if (pathSearch > 0.0f && pathDistance > 0.0f)
                pathDistance = (std::min)(pathDistance, clearance.PathDistance(point, pathSearch));
        }
        return density * HabitatTransition(waterDistance, habitat.water.minimum, habitat.water.maximum, habitat.water.fade)
            * (habitat.pathLayer < 0 ? 1.0f : HabitatTransition(pathDistance, habitat.path.minimum, habitat.path.maximum, habitat.path.fade));
    };
    return true;
}

OpResult RunTerrainMap(OpContext& context, const OpArgs& args, OpKind kind)
{
    auto* owner = ResolveTerrainNode(context, args.GetString("node"));
    if (!owner || (kind == OpKind::Mutation && context.ctx.IsLocked(owner->GetID())))
        return OpResult::Err("NO_TERRAIN", "Select a terrain that can be used for this operation.");
    auto& world = *context.ctx.activeScene;
    scene::FlushWorldTransforms(world);
    std::vector<TerrainAnalysisSurface> surfaces;
    if (!ResolveMapSurfaces(world, *owner, args, surfaces))
        return OpResult::Err("BAD_TERRAIN_TILES", "Use up to 16 active terrain tiles and include the target tile.");

    const auto channelName = args.GetString("channel", "Density");
    const auto channel = std::find(TERRAIN_MAP_NAMES.begin(), TERRAIN_MAP_NAMES.end(), channelName);
    if (channel == TERRAIN_MAP_NAMES.end()) return OpResult::Err("BAD_CHANNEL", "Unknown terrain map channel.");
    const auto settings = ReadMapSettings(args, *owner);
    const auto mapChannel = static_cast<TerrainMapChannel>(channel - TERRAIN_MAP_NAMES.begin());
    std::function<float(const math::Vector3&)> habitat;
    if (mapChannel == TerrainMapChannel::DENSITY && !PrepareTerrainHabitat(args, world, *owner, surfaces, habitat))
        return OpResult::Err("BAD_HABITAT", "Check the patch range, distance bands, path layer and water surface count.");

    auto& terrain = *owner->GetComponent<scene::TerrainComponent>();
    const int layer = args.GetInt("layer", -1);
    if (kind == OpKind::Mutation && (layer < 0 || layer >= terrain.LayerCount()))
        return OpResult::Err("BAD_LAYER", "Select an existing terrain material layer.");
    std::filesystem::path exportPath;
    if (kind == OpKind::Action) {
        auto requested = args.GetString("path");
        if (requested.empty()) requested = "Assets/Terrain/Maps/" + channelName + "_" + asset::AssetDatabase::GenerateGuid() + ".png";
        exportPath = TerrainAuthoringAssetPath(context.ctx, requested, ".png");
        if (exportPath.empty()) return OpResult::Err("BAD_PATH", "Enter a .png path under the project's Assets folder.");
    }

    TerrainMapRaster raster;
    if (!BuildTerrainMap(surfaces, settings, mapChannel, raster, habitat))
        return OpResult::Err("MAP_FAILED", "Check the heightfield, map settings, and positive world-aligned terrain transforms.");
    auto data = OpData::MakeObject();
    data.Set("width", raster.width);
    data.Set("height", raster.height);
    data.Set("tileCount", static_cast<int>(surfaces.size()));
    if (kind == OpKind::Query) {
        constexpr char HEX[] = "0123456789abcdef";
        std::string pixels;
        pixels.reserve(TERRAIN_MAP_PREVIEW_SIZE * TERRAIN_MAP_PREVIEW_SIZE * 2);
        for (int z = 0; z < TERRAIN_MAP_PREVIEW_SIZE; ++z) {
            for (int x = 0; x < TERRAIN_MAP_PREVIEW_SIZE; ++x) {
                const auto value = static_cast<unsigned>(std::lround(255.0f * raster.Sample(
                    static_cast<float>(x) / (TERRAIN_MAP_PREVIEW_SIZE - 1), static_cast<float>(z) / (TERRAIN_MAP_PREVIEW_SIZE - 1))));
                pixels.push_back(HEX[value >> 4]);
                pixels.push_back(HEX[value & 15]);
            }
        }
        data.Set("previewSize", TERRAIN_MAP_PREVIEW_SIZE);
        data.Set("previewGray8Hex", std::move(pixels));
        auto result = OpResult::Data(std::move(data));
        result.message = "Preview generated";
        return result;
    }
    if (kind == OpKind::Action) {
        const auto path = util::FileSystem::PathToUtf8(exportPath);
        scene::TerrainComponent image;
        image.columns = raster.width;
        image.rows = raster.height;
        image.heightData = std::move(raster.pixels);
        if (!scene::SaveHeightMapToFile(path, image, true)) return OpResult::Err("WRITE_FAILED", "Could not save " + path);
        context.ctx.requestAssetBrowserRefresh = true;
        const auto guid = asset::AssetDatabase::GuidFromPath(path);
        if (guid.empty()) return OpResult::Err("META_FAILED", "PNG saved but asset metadata could not be created: " + path);
        data.Set("path", NormalizeAssetPath(path));
        data.Set("guid", guid);
        auto result = OpResult::Data(std::move(data));
        result.message = "Saved " + NormalizeAssetPath(path);
        return result;
    }

    auto after = terrain;
    BlendTerrainMap(after, raster, layer, args.GetFloat("strength", 1.0f));
    if (after.splatIndices == terrain.splatIndices && after.splatWeights == terrain.splatWeights) {
        auto result = OpResult::Data(std::move(data));
        result.noChange = true;
        result.message = "Layer weights are unchanged";
        return result;
    }
    auto result = CommitTerrain(context, *owner, std::move(after), "Bake Terrain Map into Layer", false);
    result.data = std::move(data);
    return result;
}

void AddTerrainFlowParameters(EditorOperator& op, float defaultSize)
{
    op.params.push_back(TerrainParameter("terrainTiles", OpParamType::Data, "Explicit tile GUIDs including the target, up to 16", OpData{}));
    op.params.push_back(TerrainParameter("includeNeighbors", OpParamType::Bool, "Use connected terrain tiles when no list is supplied", true));
    auto center = TerrainParameter("flowCenter", OpParamType::Vec3, "World XZ center; omit to use the target center");
    center.required = false;
    op.params.push_back(std::move(center));
    AddRuleRange(op, "flowSize", "Detailed flow region side in world metres; 0 uses regional flow", defaultSize, 0.0f, 100000.0f);
}

void AddTerrainMapSamplingParameters(EditorOperator& op)
{
    auto resolution = TerrainParameter("resolution", OpParamType::Int, "Maximum output samples per axis", 257);
    resolution.hasRange = true;
    resolution.minValue = 2.0f;
    resolution.maxValue = 513.0f;
    op.params.push_back(std::move(resolution));
    const TerrainMapSettings defaults;
    AddRuleRange(op, "radius", "Relief sampling radius in world metres", defaults.radius, 0.25f, 1000.0f);
    AddRuleRange(op, "flowFullArea", "Additional contributing area in m² for white", defaults.flowFullArea, 1.0f, 1.0e12f);
}

void RegisterTerrainMapOperators(OperatorRegistry& registry)
{
    auto bake = TerrainMutation("terrain.maps.bake", "Bake Terrain Map into Layer");
    bake.desc = "Blend a terrain shape or density map into an existing layer.";
    auto channel = TerrainParameter("channel", OpParamType::String, "Map channel", std::string("Density"));
    channel.enumValues.assign(TERRAIN_MAP_NAMES.begin(), TERRAIN_MAP_NAMES.end());
    bake.params.push_back(std::move(channel));
    AddTerrainFlowParameters(bake, 0.0f);
    AddTerrainMapSamplingParameters(bake);
    const TerrainMapSettings defaults;
    AddRuleRange(bake, "density", "Maximum density", defaults.density, 0.0f, 1.0f);
    for (std::size_t i = 0; i < defaults.preferences.size(); ++i)
        AddRuleRange(bake, TERRAIN_MAP_NAMES[i], "Density preference: negative favors black, positive favors white", defaults.preferences[i], -1.0f, 1.0f);
    AddRuleRange(bake, "patchScale", "Habitat patch scale in world metres; 0 disables it", 0.0f, 0.0f, 2048.0f);
    AddRuleRange(bake, "patchMin", "Lowest accepted patch value", 0.0f, 0.0f, 1.0f);
    AddRuleRange(bake, "patchMax", "Highest accepted patch value", 1.0f, 0.0f, 1.0f);
    bake.params.push_back(TerrainParameter("patchSeed", OpParamType::Int, "World-aligned habitat seed", 1));
    bake.params.push_back(TerrainParameter("avoidWater", OpParamType::Bool, "Reject submerged samples", false));
    bake.params.push_back(TerrainParameter("pathLayer", OpParamType::Int, "Painted road/trail layer, or -1", -1));
    for (const auto* name : {"waterMin", "pathMin"})
        AddRuleRange(bake, name, "Minimum distance in world metres", 0.0f, 0.0f, 1000.0f);
    for (const auto* name : {"waterMax", "pathMax"})
        AddRuleRange(bake, name, "Maximum distance in world metres; 0 leaves it open", 0.0f, 0.0f, 1000.0f);
    for (const auto* name : {"waterFade", "pathFade"})
        AddRuleRange(bake, name, "Distance band transition width in world metres", 8.0f, 0.0f, 500.0f);

    auto preview = bake;
    preview.id = "terrain.maps.preview";
    preview.label = "Preview Terrain Map";
    preview.desc = "Return a 48 by 48 grayscale thumbnail as row-major Gray8 hex.";
    preview.kind = OpKind::Query;
    preview.undoLabel.clear();
    preview.exec = [](OpContext& context, const OpArgs& args) { return RunTerrainMap(context, args, OpKind::Query); };
    registry.Register(std::move(preview));

    auto exportMap = bake;
    exportMap.id = "terrain.maps.export";
    exportMap.label = "Export Terrain Map PNG";
    exportMap.desc = "Write the selected map to a linear 16-bit grayscale PNG under project Assets.";
    exportMap.kind = OpKind::Action;
    exportMap.undoLabel.clear();
    exportMap.params.push_back(TerrainParameter("path", OpParamType::String, "Assets-relative .png path; empty creates a unique path", std::string{}));
    exportMap.exec = [](OpContext& context, const OpArgs& args) { return RunTerrainMap(context, args, OpKind::Action); };
    registry.Register(std::move(exportMap));

    bake.params.push_back(TerrainParameter("layer", OpParamType::Int, "Existing material layer index"));
    AddRuleRange(bake, "strength", "Blend strength", 1.0f, 0.0f, 1.0f);
    bake.exec = [](OpContext& context, const OpArgs& args) { return RunTerrainMap(context, args, OpKind::Mutation); };
    registry.Register(std::move(bake));
}

bool ReadMaterialRules(const OpArgs& args, TerrainMaterialBakeSettings& settings)
{
    const auto* rules = args.GetData("rules");
    if (!rules || !rules->IsArray() || rules->AsArray().empty() || rules->AsArray().size() > 8) return false;
    const auto number = [](const OpData& data, float& value) {
        if (data.IsInt()) { value = static_cast<float>(data.AsInt64()); return true; }
        return data.TryGetFloat(value);
    };
    for (const auto& item : rules->AsArray()) {
        TerrainMaterialRule rule;
        const auto* layer = item.Find("layer");
        if (!layer || !layer->TryGetInt32(rule.layer)) return false;
        if (const auto* value = item.Find("weight"); value && !number(*value, rule.weight)) return false;
        if (const auto* preferences = item.Find("preferences")) {
            if (!preferences->IsArray() || preferences->AsArray().size() != rule.preferences.size()) return false;
            for (std::size_t i = 0; i < rule.preferences.size(); ++i)
                if (!number(preferences->AsArray()[i], rule.preferences[i])) return false;
        }
        settings.rules.push_back(rule);
    }
    if (const auto* protectedLayers = args.GetData("protectedLayers"); protectedLayers && !protectedLayers->IsNull()) {
        if (!protectedLayers->IsArray() || protectedLayers->AsArray().size() > 8) return false;
        for (const auto& value : protectedLayers->AsArray()) {
            int layer = -1;
            if (!value.TryGetInt32(layer) || std::find(settings.protectedLayers.begin(), settings.protectedLayers.end(), layer) != settings.protectedLayers.end()) return false;
            settings.protectedLayers.push_back(layer);
        }
    }
    return true;
}

OpResult RunTerrainMaterialBake(OpContext& context, const OpArgs& args)
{
    auto* owner = ResolveTerrainNode(context, args.GetString("node"));
    if (!owner || context.ctx.IsLocked(owner->GetID())) return OpResult::Err("NO_TERRAIN", "Select an editable terrain.");
    auto& world = *context.ctx.activeScene;
    scene::FlushWorldTransforms(world);
    std::vector<TerrainAnalysisSurface> surfaces;
    if (!ResolveMapSurfaces(world, *owner, args, surfaces)) return OpResult::Err("BAD_TERRAIN_TILES", "Select up to 16 active terrain tiles.");
    TerrainMaterialBakeSettings settings;
    if (!ReadMaterialRules(args, settings)) return OpResult::Err("BAD_RULES", "Check the material rules and protected layer list.");
    settings.strength = args.GetFloat("strength", settings.strength);
    settings.protectedThreshold = args.GetFloat("protectedThreshold", settings.protectedThreshold);
    const auto maps = ReadMapSettings(args, *owner);
    const auto& terrain = *surfaces.front().terrain;
    TerrainMapRaster protection;
    if (const auto file = args.GetString("protectionMask"); !file.empty()) {
        scene::TerrainComponent image;
        image.columns = std::clamp(terrain.columns, 2, 4097);
        image.rows = std::clamp(terrain.rows, 2, 4097);
        if (!scene::LoadHeightMapFromFile(ResolveTerrainInput(context.ctx, file), image))
            return OpResult::Err("BAD_PROTECTION_MASK", "Could not read the material protection mask.");
        protection = {image.columns, image.rows, std::move(image.heightData)};
        settings.protection = &protection;
    }
    auto after = terrain;
    if (!BakeTerrainMaterials(after, surfaces, maps, settings))
        return OpResult::Err("MATERIAL_BAKE_FAILED", "Check the layer indices, map settings and terrain transform.");
    if (after.splatIndices == terrain.splatIndices && after.splatWeights == terrain.splatWeights) {
        auto result = OpResult::Ok();
        result.noChange = true;
        result.message = "Layer weights are unchanged";
        return result;
    }
    return CommitTerrain(context, *owner, std::move(after), "Bake Terrain Material Rules", false);
}

void ApplyRiverCarvePlan(scene::TerrainComponent& terrain, const TerrainRiverCarvePlan& plan)
{
    for (const auto& vertex : plan.vertices) terrain.heightData[vertex.index] = vertex.normalizedHeight;
    for (const auto& material : plan.materials) {
        std::copy(material.indices.begin(), material.indices.end(), terrain.splatIndices.data() + material.offset);
        std::copy(material.weights.begin(), material.weights.end(), terrain.splatWeights.data() + material.offset);
    }
}

TerrainFlowCarveSettings ReadFlowCarveSettings(const OpArgs& args, const scene::GameObject& owner)
{
    TerrainFlowCarveSettings settings;
    settings.region = ReadFlowRegion(args, owner, 512.0f);
    settings.minArea = args.GetFloat("minArea", settings.minArea);
    settings.depthScale = args.GetFloat("depthScale", settings.depthScale);
    auto& bed = settings.bed;
    bed.bankWidth = args.GetFloat("bankWidth", bed.bankWidth);
    bed.maxCut = args.GetFloat("maxCut", bed.maxCut);
    bed.bankLayer = args.GetInt("rockLayer", -1);
    bed.gentleBankLayer = args.GetInt("gravelLayer", -1);
    bed.protectedLayer = args.GetInt("protectedLayer", -1);
    bed.gentleGrade = args.GetFloat("gentleGrade", bed.gentleGrade);
    bed.depositHeight = args.GetFloat("depositHeight", bed.depositHeight);
    return settings;
}

OpResult RunTerrainFlowCarve(OpContext& context, const OpArgs& args, bool preview)
{
    auto* owner = ResolveTerrainNode(context, args.GetString("node"));
    if (!owner || (!preview && context.ctx.IsLocked(owner->GetID()))) return OpResult::Err("NO_TERRAIN", "Select an editable terrain.");
    auto& world = *context.ctx.activeScene;
    scene::FlushWorldTransforms(world);
    std::vector<TerrainAnalysisSurface> surfaces;
    if (!ResolveMapSurfaces(world, *owner, args, surfaces)) return OpResult::Err("BAD_TERRAIN_TILES", "Select up to 16 active terrain tiles.");
    const auto settings = ReadFlowCarveSettings(args, *owner);
    const auto& bed = settings.bed;
    const auto& terrain = *surfaces.front().terrain;
    for (const int layer : {bed.bankLayer, bed.gentleBankLayer, bed.protectedLayer})
        if (layer < -1 || layer >= terrain.LayerCount()) return OpResult::Err("BAD_LAYER", "Choose existing terrain layers or -1.");
    TerrainRiverCarvePlan plan;
    std::string diagnostic;
    int reaches = 0;
    if (!PlanTerrainFlowCarve(surfaces, settings, plan, reaches, diagnostic)) return OpResult::Err("FLOW_CARVE_FAILED", diagnostic);
    auto data = OpData::MakeObject();
    data.Set("reaches", reaches);
    data.Set("analysisSpacing", plan.analysisSpacing);
    data.Set("changedVertices", static_cast<std::int64_t>(plan.vertices.size()));
    data.Set("paintedVertices", static_cast<std::int64_t>(plan.materials.size()));
    data.Set("protectedVertices", static_cast<std::int64_t>(plan.protectedVertices));
    data.Set("deepestCut", plan.deepestCut);
    data.Set("highestDeposit", plan.highestDeposit);
    const bool withinLimit = plan.deepestCut <= bed.maxCut;
    data.Set("withinLimit", withinLimit);
    if (preview || (plan.vertices.empty() && plan.materials.empty())) {
        auto result = OpResult::Data(std::move(data));
        result.noChange = !preview;
        result.message = preview ? "Flow channel preview generated" : "No channel edits match the current settings";
        return result;
    }
    if (!withinLimit) {
        auto result = OpResult::Err("FLOW_CUT_LIMIT", "The proposed excavation exceeds the maximum cut.");
        result.data = std::move(data);
        return result;
    }
    auto after = terrain;
    ApplyRiverCarvePlan(after, plan);
    auto result = CommitTerrain(context, *owner, std::move(after), "Carve Terrain Flow Channels", !plan.vertices.empty());
    result.data = std::move(data);
    return result;
}

std::string TerrainNetworkFingerprint(const scene::TerrainComponent& terrain, const scene::Transform& pose)
{
    asset::BakeFingerprint hash;
    hash.Add(static_cast<std::uint64_t>(terrain.columns));
    hash.Add(static_cast<std::uint64_t>(terrain.rows));
    hash.Add(std::array{terrain.cellSize, terrain.maxHeight, pose.worldPosition.x, pose.worldPosition.y, pose.worldPosition.z,
        pose.worldRotation.x, pose.worldRotation.y, pose.worldRotation.z, pose.worldRotation.w,
        pose.worldScale.x, pose.worldScale.y, pose.worldScale.z});
    hash.Add(terrain.heightData);
    hash.Add(terrain.holeData);
    hash.Add(terrain.splatIndices);
    hash.Add(terrain.splatWeights);
    hash.Add(static_cast<std::uint64_t>(terrain.enabled));
    hash.Add(terrain.terrainAssetPath);
    for (const auto& layer : terrain.layerMaterials) hash.Add(layer);
    return hash.Finish();
}

struct TerrainNetworkEdit {
    std::string guid;
    std::uint64_t lifetime = 0;
    std::string beforeFingerprint, afterFingerprint;
    TerrainRiverCarvePlan before, after;
};

struct TerrainNetworkRiver {
    std::string guid;
    std::string name;
    math::Vector3 origin;
    scene::WaterComponent water;
    std::size_t firstSection = 0, lastSection = 0;
    std::uint64_t lifetime = 0;
};

struct TerrainNetworkVisual {
    std::string guid, name;
    math::Vector3 position;
    scene::WaterfallComponent waterfall;
    std::array<math::Vector3, 4> banks;
    std::size_t upstream = 0, downstream = 0;
    std::uint64_t lifetime = 0;
};

struct TerrainNetworkEditBatch {
    EditorContext* context;
    std::uint64_t generation;
    std::vector<TerrainNetworkEdit> terrain;
    std::vector<TerrainNetworkRiver> rivers;
    std::vector<TerrainNetworkVisual> visuals;
};

bool BuildNetworkRivers(std::span<const TerrainRiverCarveSection> sections, const std::string& material,
    std::span<const std::size_t> waterfallLips, std::vector<TerrainNetworkRiver>& rivers)
{
    constexpr std::size_t MAX_RIVERS = 96;
    const auto makeStrip = [&](std::span<const TerrainRiverCarveSection> points, const TerrainRiverCarveSection* next,
        bool straightJoin, bool waterfallReach, TerrainNetworkRiver& river) {
        const auto& start = points.front();
        river.origin = {start.center.x, start.waterHeight, start.center.y};
        auto& water = river.water;
        water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
        water.splineSamplesPerSegment = 1;
        water.materialPath = material;
        water.splashEnabled = false;
        const auto addPoint = [&](const TerrainRiverCarveSection& point) {
            const auto position = math::Vector3{point.center.x, point.waterHeight, point.center.y} - river.origin;
            const float speed = waterfallReach ? std::clamp(std::sqrt(9.81f * point.depth) * 0.55f, 0.35f, 8.0f) : 1.0f;
            water.splinePoints.push_back({static_cast<std::uint32_t>(water.splinePoints.size() + 1), position,
                point.halfWidth * 2.0f, point.depth, speed});
            water.buoyancyDepth = (std::max)(water.buoyancyDepth, point.depth);
        };
        for (const auto& point : points) addPoint(point);
        if (next) {
            const auto& join = points.back();
            const auto delta = next->center - join.center;
            const float length = std::hypot(delta.x, delta.y);
            if (length <= 0.0f) return false;
            const float t = (std::min)(0.5f, join.halfWidth / length);
            auto center = join.center + delta * t;
            if (straightJoin) {
                /// @note A straight terminal overlap fits within the shared bed section when the downstream bend would fold the strip.
                const auto incoming = join.center - points[points.size() - 2].center;
                const float incomingLength = std::hypot(incoming.x, incoming.y);
                if (incomingLength <= 0.0f) return false;
                center = join.center + incoming * ((std::min)(t * length, incomingLength * 0.5f) / incomingLength);
            }
            addPoint({center, std::lerp(join.waterHeight, next->waterHeight, t),
                std::lerp(join.halfWidth, next->halfWidth, t), std::lerp(join.depth, next->depth, t)});
        }
        scene::Transform pose;
        pose.worldPosition = river.origin;
        scene::WaterSplineSnapshot snapshot;
        return scene::BuildWaterSplineSnapshot(water.surfaceShape, water.splinePoints, 1, pose, snapshot,
            water.buoyancyDepth, water.splineMeshCellSize);
    };
    const auto appendStrips = [&](std::size_t cursor, std::size_t end, const TerrainRiverCarveSection* continuation) {
        while (cursor + 1 < end) {
            if (rivers.size() == MAX_RIVERS) return false;
            std::size_t limit = (std::min)(end, cursor + scene::WATER_SPLINE_MAX_CONTROL_POINTS - 1);
            for (;;) {
                const auto points = sections.subspan(cursor, limit - cursor);
                const auto* next = limit < end ? &sections[limit] : continuation;
                const bool waterfallReach = (cursor > 0 && std::binary_search(waterfallLips.begin(), waterfallLips.end(), cursor - 1))
                    || std::binary_search(waterfallLips.begin(), waterfallLips.end(), limit - 1);
                TerrainNetworkRiver river;
                bool valid = makeStrip(points, next, false, waterfallReach, river);
                if (!valid && next && points.size() == 2) {
                    river = {};
                    valid = makeStrip(points, next, true, waterfallReach, river);
                }
                if (valid) {
                    river.name = "Flow River " + std::to_string(rivers.size() + 1);
                    river.firstSection = cursor;
                    river.lastSection = limit - 1;
                    rivers.push_back(std::move(river));
                    break;
                }
                if (points.size() == 2) return false;
                limit = cursor + (std::max)(std::size_t{2}, points.size() / 2);
            }
            cursor = limit - 1;
        }
        return true;
    };
    for (std::size_t begin = 0; begin < sections.size();) {
        std::size_t end = begin + 1;
        while (end < sections.size() && !sections[end].beginsReach) ++end;
        const TerrainRiverCarveSection* continuation = nullptr;
        if (sections[begin].openEnd) {
            const auto downstream = std::find_if(sections.begin(), sections.end(), [&](const auto& point) {
                return point.beginsReach && point.center == sections[end - 1].center;
            });
            if (downstream == sections.end() || downstream + 1 == sections.end()) return false;
            continuation = &*(downstream + 1);
        }
        auto cursor = begin;
        for (const auto lip : waterfallLips) {
            if (lip < begin || lip >= end) continue;
            /// @note Water above and below the cliff remains queryable; the falling curtain is visual geometry.
            if (!appendStrips(cursor, lip + 1, nullptr)) return false;
            cursor = lip + 1;
        }
        if (!appendStrips(cursor, end, continuation)) return false;
        begin = end;
    }
    return true;
}

bool PlanNetworkWaterfall(std::size_t lip, std::vector<TerrainNetworkRiver>& rivers,
    const std::function<float(const math::Vector3&)>& ground, TerrainNetworkVisual& out)
{
    const auto upper = std::find_if(rivers.begin(), rivers.end(), [&](const auto& river) { return river.lastSection == lip; });
    const auto lower = std::find_if(rivers.begin(), rivers.end(), [&](const auto& river) { return river.firstSection == lip + 1; });
    if (upper == rivers.end() || lower == rivers.end()) return false;
    const auto getSection = [](const TerrainNetworkRiver& river, bool atStart, scene::WaterfallSection& section) {
        scene::Transform pose;
        pose.worldPosition = river.origin;
        scene::WaterSplineSnapshot snapshot;
        return scene::BuildWaterSplineSnapshot(river.water.surfaceShape, river.water.splinePoints,
            river.water.splineSamplesPerSegment, pose, snapshot, river.water.buoyancyDepth, 100.0f)
            && scene::GetWaterfallRiverSection(snapshot, atStart, !atStart, section);
    };
    scene::WaterfallGeometry geometry;
    if (!getSection(*upper, false, geometry.source) || !getSection(*lower, true, geometry.receiver)) return false;
    scene::MeshBuilder mesh;
    if (!scene::BuildWaterfallGeometry(geometry, ground, mesh)) {
        geometry.rockAttached = true;
        if (!scene::BuildWaterfallGeometry(geometry, ground, mesh)) return false;
    }
    out.upstream = static_cast<std::size_t>(upper - rivers.begin());
    out.downstream = static_cast<std::size_t>(lower - rivers.begin());
    out.position = math::Vector3::Lerp(geometry.source.left, geometry.source.right, 0.5f);
    out.banks = {geometry.source.left, geometry.source.right, geometry.receiver.left, geometry.receiver.right};
    out.waterfall.rockAttached = geometry.rockAttached;
    return true;
}

bool ApplyTerrainNetworkBatch(TerrainNetworkEditBatch& batch, bool apply, std::string& reason)
{
    auto& ctx = *batch.context;
    auto* world = ctx.activeScene;
    if (!world || world->GetRenderSceneGeneration() != batch.generation
        || (ctx.playMode && !ctx.playMode->IsInEditor())) { reason = "The edit scene changed."; return false; }
    scene::FlushWorldTransforms(*world);
    std::vector<scene::TerrainComponent*> terrains;
    for (const auto& edit : batch.terrain) {
        auto* owner = world->FindByGuid(edit.guid);
        auto* terrain = owner ? owner->GetComponent<scene::TerrainComponent>() : nullptr;
        if (!terrain || !owner->activeInHierarchy() || ctx.IsLocked(owner->GetID())
            || world->GetComponentLifetime<scene::TerrainComponent>(owner->GetID()) != edit.lifetime
            || TerrainNetworkFingerprint(*terrain, owner->transform) != (apply ? edit.beforeFingerprint : edit.afterFingerprint)) {
            reason = "A terrain tile changed after the network was prepared.";
            return false;
        }
        terrains.push_back(terrain);
    }
    if (apply && !world->CanCreateGameObjects(batch.rivers.size() + batch.visuals.size())) {
        reason = "The scene has insufficient room for the rivers and waterfalls.";
        return false;
    }
    std::vector<scene::EntityID> waterNodes;
    for (auto& river : batch.rivers) {
        auto* owner = world->FindByGuid(river.guid);
        if (apply) {
            if (owner) { reason = "A planned river GUID already exists."; return false; }
            continue;
        }
        auto* water = owner ? owner->GetComponent<scene::WaterComponent>() : nullptr;
        if (!water || ctx.IsLocked(owner->GetID()) || owner->GetParent() || owner->GetChildCount() || !owner->activeSelf()
            || owner->name != river.name || owner->tag != "Untagged" || owner->layer != 0 || !owner->prefabAssetPath.empty()
            || owner->transform.position != river.origin || owner->transform.rotation != math::Quaternion::Identity()
            || owner->transform.scale != math::Vector3::ONE
            || world->GetComponentLifetime<scene::WaterComponent>(owner->GetID()) != river.lifetime) {
            reason = "A generated river was edited.";
            return false;
        }
        bool extraComponent = false;
        scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
            if constexpr (!std::is_same_v<T, scene::WaterComponent>) extraComponent |= owner->GetComponent<T>() != nullptr;
        });
        auto authored = *water;
        if (extraComponent || scene::SerializeReflected(authored) != scene::SerializeReflected(river.water)) {
            reason = "A generated river's components were edited.";
            return false;
        }
        waterNodes.push_back(owner->GetID());
    }
    for (auto& visual : batch.visuals) {
        auto* owner = world->FindByGuid(visual.guid);
        if (apply) {
            if (owner) { reason = "A planned waterfall GUID already exists."; return false; }
            continue;
        }
        auto* waterfall = owner ? owner->GetComponent<scene::WaterfallComponent>() : nullptr;
        if (!waterfall || ctx.IsLocked(owner->GetID()) || owner->GetParent()
            || !owner->activeSelf() || owner->name != visual.name || owner->tag != "Untagged" || owner->layer != 0
            || !owner->prefabAssetPath.empty() || owner->transform.position != visual.position
            || owner->transform.rotation != math::Quaternion::Identity() || owner->transform.scale != math::Vector3::ONE
            || world->GetComponentLifetime<scene::WaterfallComponent>(owner->GetID()) != visual.lifetime) {
            reason = "A generated waterfall was edited.";
            return false;
        }
        bool extraComponent = false;
        scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
            if constexpr (!std::is_same_v<T, scene::WaterfallComponent>) extraComponent |= owner->GetComponent<T>() != nullptr;
        });
        for (int child = 0; child < owner->GetChildCount(); ++child) {
            auto* object = owner->GetChild(child);
            extraComponent |= object && !object->GetComponent<scene::WaterfallPart>();
        }
        if (extraComponent || scene::SerializeReflected(*waterfall) != scene::SerializeReflected(visual.waterfall)) {
            reason = "A generated waterfall's components were edited.";
            return false;
        }
        waterNodes.push_back(owner->GetID());
    }
    if (apply) {
        for (auto& river : batch.rivers) {
            auto* owner = world->TryCreateGameObject(river.name);
            if (!owner) { world->DestroyGameObjects(waterNodes); reason = "Could not create the river network."; return false; }
            owner->instanceId = river.guid;
            owner->transform.position = river.origin;
            owner->AddComponent<scene::WaterComponent>(river.water);
            river.lifetime = world->GetComponentLifetime<scene::WaterComponent>(owner->GetID());
            waterNodes.push_back(owner->GetID());
        }
        for (auto& visual : batch.visuals) {
            auto* owner = world->TryCreateGameObject(visual.name);
            if (!owner) { world->DestroyGameObjects(waterNodes); reason = "Could not create the waterfalls."; return false; }
            owner->instanceId = visual.guid;
            owner->transform.position = visual.position;
            owner->AddComponent<scene::WaterfallComponent>(visual.waterfall);
            visual.lifetime = world->GetComponentLifetime<scene::WaterfallComponent>(owner->GetID());
            waterNodes.push_back(owner->GetID());
        }
    } else {
        world->DestroyGameObjects(waterNodes);
    }
    for (std::size_t i = 0; i < terrains.size(); ++i) {
        const auto& plan = apply ? batch.terrain[i].after : batch.terrain[i].before;
        auto& terrain = *terrains[i];
        ApplyRiverCarvePlan(terrain, plan);
        if (!plan.vertices.empty()) { terrain.RequestHeightRebuild(); terrain.colliderDirty = true; }
        if (!plan.materials.empty()) terrain.splatDirty = true;
        if (apply && batch.terrain[i].afterFingerprint.empty()) {
            const auto* owner = world->FindByGuid(batch.terrain[i].guid);
            batch.terrain[i].afterFingerprint = TerrainNetworkFingerprint(terrain, owner->transform);
        }
    }
    scene::FlushWorldTransforms(*world);
    if (ctx.markSceneDirty) ctx.markSceneDirty();
    reason.clear();
    return true;
}

OpResult RunTerrainFlowNetwork(OpContext& context, const OpArgs& args, bool preview)
{
    if (!preview && !context.undo.IsRecordingEnabled())
        return OpResult::Err("UNDO_REQUIRED", "Enable Undo recording before applying a flow network.");
    auto* owner = ResolveTerrainNode(context, args.GetString("node"));
    if (!owner) return OpResult::Err("NO_TERRAIN", "Select an editable terrain.");
    auto& world = *context.ctx.activeScene;
    scene::FlushWorldTransforms(world);
    std::vector<TerrainAnalysisSurface> surfaces;
    if (!ResolveMapSurfaces(world, *owner, args, surfaces)) return OpResult::Err("BAD_TERRAIN_TILES", "Select up to 16 active terrain tiles.");
    std::vector<std::pair<std::string, scene::EntityID>> owners;
    for (const auto id : world.GetEntities<scene::TerrainComponent>()) {
        const auto* node = world.GetGameObject(id);
        if (node && std::any_of(surfaces.begin(), surfaces.end(), [&](const auto& surface) { return surface.pose == &node->transform; }))
            owners.emplace_back(node->instanceId, id);
    }
    std::sort(owners.begin(), owners.end(), [&](const auto& a, const auto& b) {
        if ((a.second == owner->GetID()) != (b.second == owner->GetID())) return a.second == owner->GetID();
        return a.first < b.first;
    });
    if (owners.size() != surfaces.size()) return OpResult::Err("BAD_TERRAIN_TILES", "Could not identify the terrain tiles.");
    surfaces.clear();
    auto batch = std::make_shared<TerrainNetworkEditBatch>();
    batch->context = &context.ctx;
    batch->generation = world.GetRenderSceneGeneration();
    asset::BakeFingerprint token;
    token.Add("fbzz.terrain.flow.network.v3");
    token.Add(batch->generation);
    token.Add(static_cast<std::uint64_t>(context.undo.GetRevision()));
    for (const auto& [guid, id] : owners) {
        const auto* node = world.GetGameObject(id);
        const auto* terrain = world.GetComponent<scene::TerrainComponent>(id);
        if (context.ctx.IsLocked(id) || node->IsRuntimeGenerated())
            return OpResult::Err("LOCKED_TERRAIN", "Unlock the participating terrain tiles before preparing a network.");
        auto& edit = batch->terrain.emplace_back();
        edit.guid = guid;
        edit.lifetime = world.GetComponentLifetime<scene::TerrainComponent>(id);
        edit.beforeFingerprint = TerrainNetworkFingerprint(*terrain, node->transform);
        token.Add(guid);
        token.Add(edit.lifetime);
        token.Add(edit.beforeFingerprint);
        surfaces.push_back({terrain, &node->transform});
    }
    const auto settings = ReadFlowCarveSettings(args, *owner);
    const auto& bed = settings.bed;
    auto material = args.GetString("waterMaterial");
    if (material.empty()) {
        const auto presets = WaterMaterialPresets();
        const auto river = std::find_if(presets.begin(), presets.end(), [](const auto& preset) { return std::string_view(preset.label) == "River"; });
        if (river != presets.end()) material = river->path;
    }
    token.Add(material);
    const bool buildWaterfalls = args.GetBool("buildWaterfalls", false);
    const float waterfallDrop = args.GetFloat("waterfallMinDrop", DEFAULT_WATERFALL_DROP);
    const float waterfallGrade = args.GetFloat("waterfallMinGrade", DEFAULT_WATERFALL_GRADE);
    if (buildWaterfalls && (!std::isfinite(waterfallDrop) || waterfallDrop < 0.5f || waterfallDrop > 1000.0f
        || !std::isfinite(waterfallGrade) || waterfallGrade < 0.25f || waterfallGrade > 10.0f))
        return OpResult::Err("BAD_WATERFALL_RULE", "Check the minimum waterfall drop and rise/run ratio.");
    token.Add(static_cast<std::uint64_t>(buildWaterfalls));
    if (buildWaterfalls) token.Add(std::array{waterfallDrop, waterfallGrade});
    std::vector<scene::WaterSplineSnapshot> sourceLakes;
    if (buildWaterfalls) {
        std::map<std::string, scene::EntityID> lakeOwners;
        for (const auto id : world.GetEntities<scene::WaterComponent>()) {
            const auto* node = world.GetGameObject(id);
            const auto* water = world.GetComponent<scene::WaterComponent>(id);
            if (node && water && node->activeInHierarchy() && water->enabled && !water->farOceanEnabled
                && water->surfaceShape == scene::WaterSurfaceShape::SPLINE_LAKE)
                lakeOwners.emplace(node->instanceId, id);
        }
        for (const auto& [guid, id] : lakeOwners) {
            auto* node = world.GetGameObject(id);
            auto* water = world.GetComponent<scene::WaterComponent>(id);
            scene::WaterControlPointSnapshot controls;
            scene::WaterSplineSnapshot snapshot;
            if (!BuildAuthoredWater(*node, *water, controls, snapshot)) continue;
            token.Add(guid);
            token.Add(snapshot.signature);
            sourceLakes.push_back(std::move(snapshot));
        }
    }
    token.Add(std::array{settings.region.center.x, settings.region.center.z, settings.region.size, settings.minArea, settings.depthScale,
        bed.bankWidth, bed.maxCut, bed.gentleGrade, bed.depositHeight});
    for (const int layer : {bed.bankLayer, bed.gentleBankLayer, bed.protectedLayer})
        token.Add(static_cast<std::uint64_t>(static_cast<std::int64_t>(layer) + 1));
    const auto expected = token.Finish();
    if (!preview && args.GetString("expected") != expected)
        return OpResult::Err("STALE_PREVIEW", "The scene or settings changed. Generate a new network preview.");

    TerrainFlowNetworkPlan plan;
    std::string diagnostic;
    if (!PlanTerrainFlowNetwork(surfaces, settings, plan, diagnostic, sourceLakes)) return OpResult::Err("FLOW_NETWORK_FAILED", diagnostic);
    auto waterfallLips = buildWaterfalls ? FindTerrainWaterfallLips(plan.sections, waterfallDrop, waterfallGrade)
        : std::vector<std::size_t>{};
    if (waterfallLips.size() > 32)
        return OpResult::Err("WATERFALL_LIMIT", "Too many cliff drops. Raise the waterfall thresholds or shrink the region.");
    const auto candidateCount = waterfallLips.size();
    const auto plannedGround = [&](const math::Vector3& point) {
        float height = -std::numeric_limits<float>::infinity();
        for (std::size_t i = 0; i < surfaces.size(); ++i) {
            scene::TerrainSurfaceSample sample;
            if (scene::TrySampleTerrainWorld(*surfaces[i].terrain, *surfaces[i].pose,
                    point.x, point.z, sample, plan.tiles[i].vertices)) height = (std::max)(height, sample.position.y);
        }
        return height;
    };
    for (;;) {
        batch->rivers.clear();
        batch->visuals.clear();
        if (!BuildNetworkRivers(plan.sections, material, waterfallLips, batch->rivers))
            return OpResult::Err("FLOW_WATER_FAILED", "A reach cannot form a valid Spline River. Narrow the drainage selection or raise the catchment threshold.");
        const auto remove = std::remove_if(waterfallLips.begin(), waterfallLips.end(), [&](std::size_t lip) {
            TerrainNetworkVisual visual;
            if (!PlanNetworkWaterfall(lip, batch->rivers, plannedGround, visual)) return true;
            visual.name = "Flow Waterfall " + std::to_string(batch->visuals.size() + 1);
            batch->visuals.push_back(std::move(visual));
            return false;
        });
        if (remove == waterfallLips.end()) break;
        waterfallLips.erase(remove, waterfallLips.end());
    }
    auto data = OpData::MakeObject();
    data.Set("expected", expected);
    data.Set("tileCount", static_cast<int>(surfaces.size()));
    data.Set("riverCount", static_cast<int>(batch->rivers.size()));
    data.Set("waterfallCount", static_cast<int>(waterfallLips.size()));
    data.Set("sourceLakeCount", static_cast<int>(sourceLakes.size()));
    data.Set("sourceOutletCount", plan.sourceOutletCount);
    data.Set("rejectedWaterfallCount", static_cast<int>(candidateCount - waterfallLips.size()));
    data.Set("analysisSpacing", plan.analysisSpacing);
    std::size_t vertexCount = 0, protectedCount = 0;
    float deepestCut = 0.0f, highestDeposit = 0.0f;
    for (const auto& tile : plan.tiles) {
        vertexCount += tile.vertices.size();
        protectedCount += tile.protectedIndices.size();
        deepestCut = (std::max)(deepestCut, tile.deepestCut);
        highestDeposit = (std::max)(highestDeposit, tile.highestDeposit);
    }
    data.Set("changedVertices", static_cast<std::int64_t>(vertexCount));
    data.Set("protectedVertices", static_cast<std::int64_t>(protectedCount));
    data.Set("deepestCut", deepestCut);
    data.Set("highestDeposit", highestDeposit);
    data.Set("withinLimit", deepestCut <= bed.maxCut);
    if (preview) {
        constexpr std::size_t POINTS_PER_KIND = 576;
        const auto stride = (std::max)(std::size_t{1}, (vertexCount + POINTS_PER_KIND - 1) / POINTS_PER_KIND);
        const auto protectedStride = (std::max)(std::size_t{1}, (protectedCount + POINTS_PER_KIND - 1) / POINTS_PER_KIND);
        auto cut = OpData::MakeArray(), deposit = OpData::MakeArray(), protect = OpData::MakeArray(), lines = OpData::MakeArray();
        std::size_t cutCount = 0, depositCount = 0, protectedSamples = 0;
        for (std::size_t i = 0; i < surfaces.size(); ++i) {
            const auto& terrain = *surfaces[i].terrain;
            const auto& pose = *surfaces[i].pose;
            const auto position = [&](std::size_t index) {
                return ToTerrainWorld(pose, {static_cast<float>(index % terrain.columns) * terrain.cellSize,
                    terrain.heightData[index] * terrain.maxHeight, static_cast<float>(index / terrain.columns) * terrain.cellSize});
            };
            for (const auto& vertex : plan.tiles[i].vertices) {
                if (vertex.normalizedHeight < terrain.heightData[vertex.index]) {
                    if (cutCount++ % stride == 0) cut.Push(position(vertex.index));
                } else if (depositCount++ % stride == 0) deposit.Push(position(vertex.index));
            }
            for (const auto index : plan.tiles[i].protectedIndices)
                if (protectedSamples++ % protectedStride == 0) protect.Push(position(index));
        }
        for (const auto& river : batch->rivers) {
            auto line = OpData::MakeArray();
            for (const auto& point : river.water.splinePoints) line.Push(river.origin + point.localPosition);
            lines.Push(std::move(line));
        }
        data.Set("cut", std::move(cut));
        data.Set("deposit", std::move(deposit));
        data.Set("protected", std::move(protect));
        data.Set("waterLines", std::move(lines));
        auto waterfallLines = OpData::MakeArray();
        for (const auto lip : waterfallLips) {
            auto line = OpData::MakeArray();
            for (const auto index : {lip, lip + 1}) {
                const auto& p = plan.sections[index];
                line.Push(math::Vector3{p.center.x, p.waterHeight, p.center.y});
            }
            waterfallLines.Push(std::move(line));
        }
        data.Set("waterfallLines", std::move(waterfallLines));
        auto sections = OpData::MakeArray();
        for (const auto& waterfall : batch->visuals) for (std::size_t index : {0u, 2u}) {
            auto line = OpData::MakeArray();
            line.Push(waterfall.banks[index]);
            line.Push(waterfall.banks[index + 1]);
            sections.Push(std::move(line));
        }
        data.Set("waterfallSections", std::move(sections));
        auto result = OpResult::Data(std::move(data));
        result.message = "Flow network preview generated";
        return result;
    }
    if (deepestCut > bed.maxCut) return OpResult::Err("FLOW_CUT_LIMIT", "The proposed excavation exceeds the maximum cut.");
    if (batch->rivers.empty()) {
        auto result = OpResult::Data(std::move(data));
        result.noChange = true;
        result.message = "No channels match the selected catchment.";
        return result;
    }
    for (std::size_t i = 0; i < surfaces.size(); ++i) {
        const auto& terrain = *surfaces[i].terrain;
        auto& edit = batch->terrain[i];
        edit.after = std::move(plan.tiles[i]);
        edit.after.protectedIndices.clear();
        for (const auto& vertex : edit.after.vertices) edit.before.vertices.push_back({vertex.index, terrain.heightData[vertex.index]});
        for (const auto& materialEdit : edit.after.materials) {
            auto old = materialEdit;
            std::copy_n(terrain.splatIndices.data() + old.offset, scene::TERRAIN_SPLAT_SLOTS, old.indices.begin());
            std::copy_n(terrain.splatWeights.data() + old.offset, scene::TERRAIN_SPLAT_SLOTS, old.weights.begin());
            edit.before.materials.push_back(old);
        }
    }
    auto riverIds = OpData::MakeArray();
    for (auto& river : batch->rivers) { river.guid = asset::AssetDatabase::GenerateGuid(); riverIds.Push(river.guid); }
    data.Set("rivers", std::move(riverIds));
    auto waterfallIds = OpData::MakeArray();
    for (auto& visual : batch->visuals) {
        visual.guid = asset::AssetDatabase::GenerateGuid();
        visual.waterfall.upstream.guid = batch->rivers[visual.upstream].guid;
        visual.waterfall.downstream.guid = batch->rivers[visual.downstream].guid;
        waterfallIds.Push(visual.guid);
    }
    data.Set("waterfallVisuals", std::move(waterfallIds));
    auto command = std::make_unique<CheckedLambdaCommand>("Build Terrain Flow Network",
        [batch](std::string& reason) { return ApplyTerrainNetworkBatch(*batch, true, reason); },
        [batch](std::string& reason) { return ApplyTerrainNetworkBatch(*batch, false, reason); });
    if (!command->TryExecute(diagnostic)) return OpResult::Err("FLOW_NETWORK_CONFLICT", diagnostic);
    auto result = OpResult::Data(std::move(data));
    result.command = std::move(command);
    result.message = "Built the river network and channel beds";
    return result;
}

template<class Container>
bool WriteTerrainRecipeValue(Container& target, std::string_view key, const OpData& source, int depth)
{
    if (depth > 5) return false;
    const auto add = [&]<class T>(T&& value) {
        if constexpr (std::is_same_v<Container, toml::table>) target.insert(key, std::forward<T>(value));
        else target.push_back(std::forward<T>(value));
        return true;
    };
    switch (source.GetType()) {
    case OpData::Type::Bool: return add(source.AsBool());
    case OpData::Type::Int: return add(source.AsInt64());
    case OpData::Type::Float: return std::isfinite(source.AsDouble()) && add(source.AsDouble());
    case OpData::Type::String: return source.AsString().size() < 512 && add(source.AsString());
    case OpData::Type::Array: {
        toml::array values;
        for (const auto& value : source.AsArray()) if (!WriteTerrainRecipeValue(values, {}, value, depth + 1)) return false;
        return add(std::move(values));
    }
    case OpData::Type::Object: {
        toml::table values;
        for (const auto& [name, value] : source.AsObject()) if (!WriteTerrainRecipeValue(values, name, value, depth + 1)) return false;
        return add(std::move(values));
    }
    default: return false;
    }
}

bool ReadTerrainRecipeValue(const toml::node& source, OpData& output, int depth)
{
    if (depth > 5) return false;
    if (const auto* table = source.as_table()) {
        output = OpData::MakeObject();
        for (const auto& [key, field] : *table) {
            OpData value;
            if (!ReadTerrainRecipeValue(field, value, depth + 1)) return false;
            output.Set(std::string(key.str()), std::move(value));
        }
    } else if (const auto* array = source.as_array()) {
        output = OpData::MakeArray();
        for (const auto& field : *array) {
            OpData value;
            if (!ReadTerrainRecipeValue(field, value, depth + 1)) return false;
            output.Push(std::move(value));
        }
    } else if (const auto* flag = source.as_boolean()) output = flag->get();
    else if (const auto* integer = source.as_integer()) output = integer->get();
    else if (const auto* number = source.as_floating_point(); number && std::isfinite(number->get())) output = number->get();
    else if (const auto* string = source.as_string(); string && string->get().size() < 512) output = string->get();
    else return false;
    return true;
}

bool RecipeLayerIndex(const OpData& value)
{
    int layer = -1;
    return value.TryGetInt32(layer) && layer >= -1 && layer < scene::TERRAIN_MAX_LAYERS;
}

bool ValidateTerrainRecipe(const OperatorRegistry& registry, const OpData& recipe)
{
    if (!recipe.IsObject() || recipe.AsObject().empty()) return false;
    std::map<std::string, double, std::less<>> common;
    for (const auto& [group, values] : recipe.AsObject()) {
        if (group == "layerMaterials") {
            if (!values.IsArray() || values.AsArray().size() > scene::TERRAIN_MAX_LAYERS) return false;
            for (const auto& ref : values.AsArray()) if (!ref.IsString()) return false;
            continue;
        }
        const char* operation = group == "flow" ? "terrain.flow.network.preview"
            : group == "materials" ? "terrain.maps.bake_materials" : group == "habitat" ? "terrain.maps.preview" : nullptr;
        const auto* spec = operation ? registry.Find(operation) : nullptr;
        if (!spec || !values.IsObject()) return false;
        OpArgs materialArgs;
        for (const auto& [key, value] : values.AsObject()) {
            if (key == "node" || key == "terrainTiles" || key == "flowCenter" || key == "expected" || key == "path") return false;
            const auto param = std::find_if(spec->params.begin(), spec->params.end(), [&](const auto& item) { return item.name == key; });
            if (param == spec->params.end()) return false;
            if (param->type == OpParamType::Float || param->type == OpParamType::Int) {
                const double numeric = value.IsInt() ? static_cast<double>(value.AsInt64()) : value.AsDouble(std::numeric_limits<double>::quiet_NaN());
                if (!std::isfinite(numeric) || (param->type == OpParamType::Int && !value.IsInt())
                    || (param->hasRange && (numeric < param->minValue || numeric > param->maxValue))) return false;
                if (param->type == OpParamType::Int) {
                    int integer = 0;
                    if (!value.TryGetInt32(integer) || (key.ends_with("Layer") && !RecipeLayerIndex(value))
                        || (key == "patchSeed" && integer < 0)) return false;
                }
            } else if (param->type == OpParamType::String) {
                if (!value.IsString() || (key == "channel" && value.AsString() != "Density")) return false;
            } else if (param->type == OpParamType::Bool) {
                if (!value.IsBool()) return false;
            } else if (param->type == OpParamType::Data && group == "materials") materialArgs.Set(key, value);
            else return false;
            if (key == "includeNeighbors" || key == "flowSize" || key == "resolution" || key == "radius" || key == "flowFullArea") {
                const double number = value.IsBool() ? static_cast<double>(value.AsBool())
                    : value.IsInt() ? static_cast<double>(value.AsInt64()) : value.AsDouble();
                const auto [entry, inserted] = common.try_emplace(key, number);
                if (!inserted && entry->second != number) return false;
            }
        }
        if (group == "materials") {
            const auto* rules = materialArgs.GetData("rules");
            if (rules && (!rules->IsArray() || !rules->AsArray().empty())) {
                TerrainMaterialBakeSettings settings;
                if (!ReadMaterialRules(materialArgs, settings)) return false;
                for (const auto& rule : settings.rules) {
                    if (rule.layer < 0 || rule.layer >= scene::TERRAIN_MAX_LAYERS || rule.weight < 0.0f || rule.weight > 16.0f) return false;
                    for (float preference : rule.preferences) if (!std::isfinite(preference) || std::abs(preference) > 1.0f) return false;
                }
            }
            if (const auto* layers = values.Find("protectedLayers")) {
                if (!layers->IsArray() || layers->AsArray().size() > 8) return false;
                for (const auto& layer : layers->AsArray()) if (!RecipeLayerIndex(layer) || layer.AsInt() < 0) return false;
            }
        }
    }
    return recipe.Find("flow") || recipe.Find("materials") || recipe.Find("habitat");
}

bool RemapTerrainRecipeLayers(OpData& recipe, const scene::TerrainComponent& terrain)
{
    const auto* references = recipe.Find("layerMaterials");
    const auto map = [&](int original, int& layer) {
        layer = original;
        if (original < 0) return true;
        if (!references) return original < terrain.LayerCount();
        if (original >= static_cast<int>(references->AsArray().size())) return false;
        const auto& reference = references->AsArray()[original].AsString();
        if (reference.empty()) return original < terrain.LayerCount() && terrain.layerMaterials[original].empty();
        const auto guid = asset::AssetDatabase::GuidFromRef(reference);
        const auto found = std::find_if(terrain.layerMaterials.begin(), terrain.layerMaterials.end(), [&](const auto& current) {
            return current == reference || (!guid.empty() && asset::AssetDatabase::GuidFromRef(current) == guid);
        });
        if (found == terrain.layerMaterials.end()) return false;
        layer = static_cast<int>(found - terrain.layerMaterials.begin());
        return true;
    };
    for (const auto* group : {"flow", "materials", "habitat"}) {
        const auto* stored = recipe.Find(group);
        if (!stored) continue;
        auto values = *stored;
        for (const auto* key : {"rockLayer", "gravelLayer", "protectedLayer", "pathLayer"}) {
            if (const auto* old = values.Find(key)) {
                int layer = -1;
                if (!map(old->AsInt(), layer)) return false;
                values.Set(key, layer);
            }
        }
        if (const auto* rules = values.Find("rules")) {
            auto mapped = OpData::MakeArray();
            for (auto rule : rules->AsArray()) {
                int layer = -1;
                if (!map(rule.Find("layer")->AsInt(), layer)) return false;
                rule.Set("layer", layer);
                mapped.Push(std::move(rule));
            }
            values.Set("rules", std::move(mapped));
        }
        if (const auto* layers = values.Find("protectedLayers")) {
            auto mapped = OpData::MakeArray();
            for (const auto& old : layers->AsArray()) {
                int layer = -1;
                if (!map(old.AsInt(), layer)) return false;
                if (std::none_of(mapped.AsArray().begin(), mapped.AsArray().end(), [&](const auto& value) { return value.AsInt() == layer; }))
                    mapped.Push(layer);
            }
            values.Set("protectedLayers", std::move(mapped));
        }
        /// @note layerMaterials is placed after the section keys by save, and Set only replaces existing keys here.
        recipe.Set(group, std::move(values));
    }
    if (references) {
        auto current = OpData::MakeArray();
        for (const auto& reference : terrain.layerMaterials) current.Push(reference);
        recipe.Set("layerMaterials", std::move(current));
    }
    return true;
}

OpResult RunTerrainRecipe(OpContext& context, const OpArgs& args, bool save)
{
    auto requested = args.GetString("path");
    if (!save && asset::AssetDatabase::IsGuidRef(requested)) requested = asset::AssetManager::ResolveAssetPath(requested);
    const auto file = TerrainAuthoringAssetPath(context.ctx, requested, ".terrainrecipe.toml", true);
    if (file.empty()) return OpResult::Err("BAD_PATH", "Choose an Assets path ending in .terrainrecipe.toml.");
    const auto path = util::FileSystem::PathToUtf8(file);
    const auto node = args.GetString("node");
    const auto* owner = !node.empty() ? ResolveTerrainNode(context, node) : nullptr;
    const auto* terrain = owner ? owner->GetScene()->GetComponent<scene::TerrainComponent>(owner->GetID()) : nullptr;
    if (!node.empty() && !terrain) return OpResult::Err("NO_TERRAIN", "The recipe target has no Terrain component.");
    constexpr std::uintmax_t MAX_BYTES = 128 * 1024;
    OpData recipe;
    if (save) {
        const auto* input = args.GetData("recipe");
        if (!input || !input->IsObject()) return OpResult::Err("BAD_RECIPE", "Supply a recipe object.");
        recipe = *input;
        if (terrain) {
            auto references = OpData::MakeArray();
            for (const auto& reference : terrain->layerMaterials) references.Push(reference);
            recipe.Set("layerMaterials", std::move(references));
        }
    } else {
        std::string content;
        std::error_code error;
        const auto size = std::filesystem::file_size(file, error);
        if (error || size > MAX_BYTES || !util::FileSystem::ReadText(path, content)) return OpResult::Err("READ_FAILED", "Could not read the recipe.");
        const auto parsed = toml::parse(content, path);
        if (!parsed || parsed["format"].value<std::string>() != "fbzz.terrainrecipe" || parsed["version"].value<int>() != 1
            || !parsed["recipe"].node() || !ReadTerrainRecipeValue(*parsed["recipe"].node(), recipe, 0))
            return OpResult::Err("BAD_RECIPE", "Unsupported or malformed terrain recipe.");
    }
    if (!context.ctx.operators || !ValidateTerrainRecipe(*context.ctx.operators, recipe))
        return OpResult::Err("BAD_RECIPE", "The recipe contains an unsupported or invalid operator argument.");
    if (terrain && !RemapTerrainRecipeLayers(recipe, *terrain))
        return OpResult::Err("RECIPE_LAYER_MISSING", "Assign the recipe's materials to the selected terrain before loading it.");
    if (save) {
        toml::table table{{"format", "fbzz.terrainrecipe"}, {"version", 1}};
        if (!WriteTerrainRecipeValue(table, "recipe", recipe, 0)) return OpResult::Err("BAD_RECIPE", "A recipe value cannot be saved.");
        std::ostringstream output;
        output << table;
        const auto content = output.str();
        if (content.size() > MAX_BYTES || !util::FileSystem::WriteTextAtomic(path, content))
            return OpResult::Err("WRITE_FAILED", "Could not save the terrain recipe.");
        context.ctx.requestAssetBrowserRefresh = true;
        if (asset::AssetDatabase::GuidFromPath(path).empty())
            return OpResult::Err("META_FAILED", "Recipe saved but its asset metadata could not be created: " + path);
    }
    auto data = OpData::MakeObject();
    data.Set("path", NormalizeAssetPath(path));
    data.Set("recipe", std::move(recipe));
    auto result = OpResult::Data(std::move(data));
    result.message = save ? "Terrain recipe saved" : "Terrain recipe loaded";
    return result;
}

void RegisterNaturalTerrainOperators(OperatorRegistry& registry)
{
    auto materials = TerrainMutation("terrain.maps.bake_materials", "Bake Terrain Material Rules");
    materials.desc = "Blend multiple shape-based material rules in one operation with painted-area protection.";
    AddTerrainFlowParameters(materials, 0.0f);
    AddTerrainMapSamplingParameters(materials);
    materials.params.push_back(TerrainParameter("rules", OpParamType::Data, "1–8 rules: {layer, weight?, preferences?: [Height, Slope, Cavity, Flow, Roughness]}"));
    materials.params.push_back(TerrainParameter("protectedLayers", OpParamType::Data, "Existing layers to protect by combined weight", OpData{}));
    materials.params.push_back(TerrainParameter("protectionMask", OpParamType::String, "Optional linear image; white protects painted weights", std::string{}));
    AddRuleRange(materials, "strength", "Material blend strength", 1.0f, 0.0f, 1.0f);
    AddRuleRange(materials, "protectedThreshold", "Combined existing layer weight that prevents editing", 0.35f, 0.001f, 1.0f);
    materials.exec = RunTerrainMaterialBake;
    registry.Register(std::move(materials));

    auto carve = TerrainMutation("terrain.flow.carve", "Carve Terrain Flow Channels");
    carve.desc = "Carve the selected terrain's drainage channels, including tributaries and bank deposition.";
    AddTerrainFlowParameters(carve, 512.0f);
    AddRuleRange(carve, "minArea", "Minimum upstream area in square metres", 6000.0f, 1.0f, 1.0e12f);
    AddRuleRange(carve, "depthScale", "Channel depth multiplier", 0.65f, 0.01f, 4.0f);
    AddRuleRange(carve, "bankWidth", "Bank width in metres", 8.0f, 0.0f, 512.0f);
    AddRuleRange(carve, "maxCut", "Maximum allowed excavation in metres", 12.0f, 0.01f, 1000.0f);
    AddRuleRange(carve, "gentleGrade", "Maximum rise/run ratio for gentle banks", 0.08f, 0.0f, 1.0f);
    AddRuleRange(carve, "depositHeight", "Maximum addition on gentle, dry banks in metres", 0.0f, 0.0f, 10.0f);
    carve.params.push_back(TerrainParameter("rockLayer", OpParamType::Int, "Steep bank material layer, or -1", -1));
    carve.params.push_back(TerrainParameter("gravelLayer", OpParamType::Int, "Gentle bank material layer, or -1", -1));
    carve.params.push_back(TerrainParameter("protectedLayer", OpParamType::Int, "Road or authored layer to protect, or -1", -1));
    auto preview = carve;
    preview.id = "terrain.flow.carve.preview";
    preview.label = "Preview Terrain Flow Channels";
    preview.kind = OpKind::Query;
    preview.undoLabel.clear();
    preview.exec = [](OpContext& context, const OpArgs& args) { return RunTerrainFlowCarve(context, args, true); };
    registry.Register(std::move(preview));
    auto network = carve;
    network.id = "terrain.flow.network.apply";
    network.label = "Apply Terrain Flow Network";
    network.undoLabel = "Build Terrain Flow Network";
    network.desc = "Apply matching channel beds and Spline Rivers across the selected terrain tiles.";
    network.params.push_back(TerrainParameter("waterMaterial", OpParamType::String, "Optional river material asset reference", std::string{}));
    network.params.push_back(TerrainParameter("buildWaterfalls", OpParamType::Bool, "Generate waterfalls below a Spline Lake outlet and a supported cliff", false));
    AddRuleRange(network, "waterfallMinDrop", "Minimum waterfall drop in metres", DEFAULT_WATERFALL_DROP, 0.5f, 1000.0f);
    AddRuleRange(network, "waterfallMinGrade", "Minimum waterfall grade (drop/run)", DEFAULT_WATERFALL_GRADE, 0.25f, 10.0f);
    auto networkPreview = network;
    networkPreview.id = "terrain.flow.network.preview";
    networkPreview.label = "Preview Terrain Flow Network";
    networkPreview.kind = OpKind::Query;
    networkPreview.undoLabel.clear();
    networkPreview.exec = [](OpContext& context, const OpArgs& args) { return RunTerrainFlowNetwork(context, args, true); };
    registry.Register(std::move(networkPreview));
    network.params.push_back(TerrainParameter("expected", OpParamType::String, "Fresh expected token from terrain.flow.network.preview"));
    network.exec = [](OpContext& context, const OpArgs& args) { return RunTerrainFlowNetwork(context, args, false); };
    registry.Register(std::move(network));
    carve.exec = [](OpContext& context, const OpArgs& args) { return RunTerrainFlowCarve(context, args, false); };
    registry.Register(std::move(carve));
    for (const bool save : {true, false}) {
        EditorOperator recipe;
        recipe.id = save ? "terrain.recipe.save" : "terrain.recipe.load";
        recipe.label = save ? "Save Terrain Recipe" : "Load Terrain Recipe";
        recipe.category = "Terrain";
        recipe.desc = "Store or read portable flow, materials and habitat operator arguments.";
        recipe.kind = save ? OpKind::Action : OpKind::Query;
        recipe.params = {TerrainParameter("path", OpParamType::String, "Project Assets path ending in .terrainrecipe.toml"),
            TerrainParameter("node", OpParamType::NodeId, "Optional terrain for material reference mapping", std::string{})};
        if (save) recipe.params.push_back(TerrainParameter("recipe", OpParamType::Data, "Object containing flow, materials and habitat operator settings"));
        recipe.poll = [](const OpContext& context, const OpArgs&) { return !context.ctx.projectRoot.empty(); };
        recipe.exec = [save](OpContext& context, const OpArgs& args) { return RunTerrainRecipe(context, args, save); };
        registry.Register(std::move(recipe));
    }
}

OpResult BlendTerrainSeam(OpContext& context, const OpArgs& args)
{
    auto* owner = ResolveTerrainNode(context, args.GetString("node"));
    auto& world = *context.ctx.activeScene;
    const auto* neighbor = world.FindByGuid(args.GetString("neighbor"));
    if (!owner || !neighbor || owner == neighbor || context.ctx.IsLocked(owner->GetID()))
        return OpResult::Err("NO_TERRAIN", "編集可能な対象地形と別の参照地形を指定してください。");
    const int requestedRows = args.GetInt("blendRows", 6);
    const float strength = args.GetFloat("strength", 1.0f), limit = args.GetFloat("maxCorrection", 8.0f);
    if (requestedRows < 1 || requestedRows > 64 || !std::isfinite(strength) || !std::isfinite(limit))
        return OpResult::Err("BAD_ARG", "行数と有限な補正量を指定してください。");
    scene::FlushWorldTransforms(world);
    TerrainSeamTile first{}, second{};
    if (!ReadSeamTile(*owner, first) || !ReadSeamTile(*neighbor, second))
        return OpResult::Err("INVALID_TERRAIN", "正のスケールを持つ軸平行の有効 Terrain が必要です。");
    const auto seam = FindTerrainSeam(first, second);
    if (!seam) return OpResult::Err("NO_SHARED_EDGE", "指定した Terrain 同士に共有辺がありません。");
    const auto& [a, b] = *seam;
    const double alongTolerance = (std::min)({SEAM_EDGE_TOLERANCE, a.Step() * 0.001, b.Step() * 0.001});
    if (a.Count() != b.Count() || std::abs(a.Origin() - b.Origin()) > alongTolerance
        || std::abs(a.End() - b.End()) > alongTolerance || a.CrossCount() < 3)
        return OpResult::Err("UNALIGNED_SEAM", "両側の共有辺全体で頂点格子が揃い、対象に内側の行が必要です。");
    for (int vertex = 0; vertex < a.Count(); ++vertex) {
        const double firstHeight = first.y + a.HeightSample(vertex) * first.heightScale;
        const double secondHeight = second.y + b.HeightSample(vertex) * second.heightScale;
        if (!std::isfinite(firstHeight) || !std::isfinite(secondHeight) || std::abs(firstHeight - secondHeight) > 0.05)
            return OpResult::Err("SEAM_HEIGHT_MISMATCH", "補正前に共有辺の高さを 0.05 m 以内へ揃えてください。");
    }

    const int rows = (std::min)(requestedRows, a.CrossCount() - 2);
    std::array<double, 64> weights{};
    for (int i = 0; i < rows; ++i) {
        const double t = static_cast<double>(i) / rows;
        weights[i] = 1.0 - t * t * (3.0 - 2.0 * t);
    }
    auto after = *first.terrain;
    int changed = 0;
    /// @note Each edge triangle's gradient is controlled by one vertex of the first inner row. Skip outer border vertices.
    for (int vertex = 1; vertex + 1 < a.Count(); ++vertex) {
        const double middle = a.Origin() + (vertex + (a.highSide ? -0.5 : 0.5)) * a.Step();
        const double gradient = b.CrossGradient(middle);
        const double target = a.HeightSample(vertex) * first.heightScale + (a.highSide ? -1.0 : 1.0) * gradient * a.CrossStep();
        const double correction = target - a.HeightSample(vertex, 1) * first.heightScale;
        if (!std::isfinite(correction)) return OpResult::Err("INVALID_TERRAIN", "境界に接する三角形の高さが不正です。");
        const double delta = std::clamp(correction, -static_cast<double>(limit), static_cast<double>(limit)) * strength;
        if (delta == 0.0) continue;
        for (int i = 0; i < rows; ++i) {
            auto& height = after.heightData[a.HeightIndex(vertex, i + 1)];
            if (!std::isfinite(height)) return OpResult::Err("INVALID_TERRAIN", "補正範囲の高さが不正です。");
            const float next = static_cast<float>(std::clamp(height + delta * weights[i] / first.heightScale, -1.0, 1.0));
            if (next != height) { height = next; ++changed; }
        }
    }
    if (changed == 0) {
        OpResult result;
        result.noChange = true;
        result.message = "境界の補正対象がありません。";
        return result;
    }
    auto result = CommitTerrain(context, *owner, std::move(after), "Blend Terrain Seam");
    result.data = OpData::MakeObject();
    result.data.Set("changedVertices", OpData(changed));
    return result;
}

OpResult CarveTerrainRiver(OpContext& context, const OpArgs& args, bool preview)
{
    auto* river = ResolveTerrainNode(context, args.GetString("node"));
    auto* ground = ResolveTerrainNode(context, args.GetString("terrain"));
    auto* water = river ? river->GetComponent<scene::WaterComponent>() : nullptr;
    const auto* terrain = ground ? ground->GetComponent<scene::TerrainComponent>() : nullptr;
    if (!terrain || !terrain->enabled || !ground->activeInHierarchy() || (!preview && context.ctx.IsLocked(ground->GetID())))
        return OpResult::Err("NO_TERRAIN", "対象の Terrain を指定してください。");
    if (!water || !water->enabled || !river->activeInHierarchy() || water->farOceanEnabled
        || water->surfaceShape != scene::WaterSurfaceShape::SPLINE_RIVER)
        return OpResult::Err("NO_RIVER", "有効な Spline River を指定してください。");
    scene::FlushWorldTransforms(*context.ctx.activeScene);
    scene::WaterControlPointSnapshot controls;
    scene::WaterSplineSnapshot mesh;
    std::vector<RiverSection> outline;
    if (!BuildAuthoredWater(*river, *water, controls, mesh)
        || !MakeRiverSections(mesh, std::numeric_limits<float>::max(), false, outline))
        return OpResult::Err("INVALID_RIVER", "川の制御点から水面を作れません。");
    std::vector<TerrainRiverCarveSection> sections;
    sections.reserve(outline.size());
    for (const auto& section : outline) {
        const auto center = section.Center();
        scene::WaterSplineSample sample;
        if (!scene::TrySampleWaterSpline(mesh, center.x, center.y, sample))
            return OpResult::Err("INVALID_RIVER", "川の中心から高さと水深を取得できません。");
        sections.push_back({center, sample.baseHeight, static_cast<float>(0.5 * DistanceXZ(section.left, section.right)), sample.depth});
    }
    const TerrainRiverCarveSettings defaults;
    const TerrainRiverCarveSettings settings{
        args.GetFloat("bankWidth", defaults.bankWidth), args.GetFloat("bankRise", defaults.bankRise),
        args.GetFloat("endFade", defaults.endFade), args.GetFloat("maxCut", defaults.maxCut),
        args.GetInt("bankLayer", defaults.bankLayer), args.GetInt("protectedLayer", defaults.protectedLayer)};
    TerrainRiverCarvePlan plan;
    if (!PlanTerrainRiverCarve(*terrain, ground->transform, sections, settings, plan))
        return OpResult::Err("BAD_RIVER_CARVE", "変換、掘削範囲、地形の高さ範囲、材質レイヤーを確認してください。");

    auto data = OpData::MakeObject();
    data.Set("changedVertices", static_cast<int64_t>(plan.vertices.size()));
    data.Set("paintedVertices", static_cast<int64_t>(plan.materials.size()));
    data.Set("protectedVertices", static_cast<int64_t>(plan.protectedVertices));
    data.Set("deepestCut", plan.deepestCut);
    data.Set("maxCut", settings.maxCut);
    data.Set("withinLimit", plan.deepestCut <= settings.maxCut);
    if (preview) return OpResult::Data(std::move(data));
    if (plan.deepestCut > settings.maxCut) {
        auto result = OpResult::Err("RIVER_CUT_LIMIT", "掘削が最大切り下げ量を超えています。");
        result.data = std::move(data);
        return result;
    }
    if (plan.vertices.empty() && plan.materials.empty()) {
        auto result = OpResult::Data(std::move(data));
        result.noChange = true;
        return result;
    }
    auto after = *terrain;
    ApplyRiverCarvePlan(after, plan);
    auto result = CommitTerrain(context, *ground, std::move(after), "Carve Terrain River Bed", !plan.vertices.empty());
    result.data = std::move(data);
    return result;
}

void RegisterTerrainAuthoringOperators(OperatorRegistry& registry)
{
    auto carve = TerrainMutation("terrain.river.carve", "Carve Terrain River Bed");
    carve.desc = "既存 Spline River に沿って川床と川岸を成形します。";
    carve.params[0] = TerrainParameter("node", OpParamType::NodeId, "対象の川。省略時は選択中", std::string{});
    carve.params.push_back(TerrainParameter("terrain", OpParamType::NodeId, "掘削する Terrain の GUID"));
    carve.poll = [](const OpContext& context, const OpArgs& args) {
        if (context.ctx.playMode && context.ctx.playMode->GetState() != PlayState::Editor) return false;
        auto* river = ResolveTerrainNode(context, args.GetString("node"));
        auto* terrain = ResolveTerrainNode(context, args.GetString("terrain"));
        return river && river->GetComponent<scene::WaterComponent>()
            && terrain && terrain->GetComponent<scene::TerrainComponent>();
    };
    const TerrainRiverCarveSettings carveDefaults;
    AddRuleRange(carve, "bankWidth", "水面の外側に成形する幅[m]", carveDefaults.bankWidth, 0.0f, 512.0f);
    AddRuleRange(carve, "bankRise", "水面から肩までの高さ[m]", carveDefaults.bankRise, 0.0f, 100.0f);
    AddRuleRange(carve, "endFade", "両端の掘削を弱める長さ[m]", carveDefaults.endFade, 0.0f, 512.0f);
    AddRuleRange(carve, "maxCut", "掘削の最大切り下げ[m]", carveDefaults.maxCut, 0.01f, 1000.0f);
    carve.params.push_back(TerrainParameter("bankLayer", OpParamType::Int, "岸に塗る既存レイヤー。-1で無効", carveDefaults.bankLayer));
    carve.params.push_back(TerrainParameter("protectedLayer", OpParamType::Int,
        "掘削・塗り分けから守る道などの既存レイヤー。-1で無効", carveDefaults.protectedLayer));
    auto carvePreview = carve;
    carvePreview.id = "terrain.river.carve.preview";
    carvePreview.label = "Preview Terrain River Bed";
    carvePreview.kind = OpKind::Query;
    carvePreview.undoLabel.clear();
    carvePreview.exec = [](OpContext& context, const OpArgs& args) { return CarveTerrainRiver(context, args, true); };
    carve.exec = [](OpContext& context, const OpArgs& args) { return CarveTerrainRiver(context, args, false); };
    registry.Register(std::move(carvePreview));
    registry.Register(std::move(carve));

    auto blendSeam = TerrainMutation("terrain.seam.blend", "Blend Terrain Seam");
    blendSeam.desc = "共有辺を保持して、選択地形の内側を隣の斜面になじませる。";
    blendSeam.params.push_back(TerrainParameter("neighbor", OpParamType::NodeId, "隣接する参照地形のGUID"));
    auto rows = TerrainParameter("blendRows", OpParamType::Int, "補正を広げる内側の行数", 6);
    rows.hasRange = true;
    rows.minValue = 1.0f;
    rows.maxValue = 64.0f;
    blendSeam.params.push_back(std::move(rows));
    AddRuleRange(blendSeam, "strength", "Blend strength", 1.0f, 0.0f, 1.0f);
    AddRuleRange(blendSeam, "maxCorrection", "Max height correction [m]", 8.0f, 0.0f, 1000.0f);
    blendSeam.exec = BlendTerrainSeam;
    registry.Register(std::move(blendSeam));

    auto ridge = TerrainMutation("terrain.relief.sculpt", "Sculpt Terrain Ridge or Valley");
    ridge.desc = "軸・曲がり・左右の裾幅を指定して尾根または谷を加える。端と裾は既存の地形へ滑らかにつなぐ。";
    ridge.params.push_back(TerrainParameter("start", OpParamType::Vec3, "起点のワールドXZ"));
    ridge.params.push_back(TerrainParameter("end", OpParamType::Vec3, "終点のワールドXZ"));
    const TerrainRidgeSettings defaults;
    AddRuleRange(ridge, "height", "高さの増分[m]。負は谷", defaults.height, -10000.0f, 10000.0f);
    AddRuleRange(ridge, "leftWidth", "左側の裾幅[m]", defaults.leftWidth, 0.25f, 10000.0f);
    AddRuleRange(ridge, "rightWidth", "右側の裾幅[m]", defaults.rightWidth, 0.25f, 10000.0f);
    AddRuleRange(ridge, "bend", "中央の左向きの曲がり[m]", defaults.bend, -10000.0f, 10000.0f);
    AddRuleRange(ridge, "peakAlong", "軸に沿ったピーク位置[0.1,0.9]", defaults.peakAlong, 0.1f, 0.9f);
    ridge.exec = [defaults](OpContext& context, const OpArgs& args) {
        auto* owner = ResolveTerrainNode(context, args.GetString("node"));
        auto* terrain = owner ? owner->GetComponent<scene::TerrainComponent>() : nullptr;
        if (!terrain || !terrain->enabled || !owner->activeInHierarchy() || context.ctx.IsLocked(owner->GetID()))
            return OpResult::Err("NO_TERRAIN", "編集可能な有効 Terrain を指定してください。");
        scene::FlushWorldTransforms(*context.ctx.activeScene);
        const TerrainRidgeSettings settings{args.GetVec3("start"), args.GetVec3("end"),
            args.GetFloat("height", defaults.height), args.GetFloat("leftWidth", defaults.leftWidth),
            args.GetFloat("rightWidth", defaults.rightWidth), args.GetFloat("bend", defaults.bend),
            args.GetFloat("peakAlong", defaults.peakAlong)};
        auto after = *terrain;
        if (!SculptTerrainRidge(after, owner->transform, settings))
            return OpResult::Err("BAD_RELIEF", "尾根の寸法・地形の変換・編集予算を確認してください。");
        if (after.heightData == terrain->heightData) {
            OpResult result;
            result.noChange = true;
            result.message = "起伏の変更がありません。";
            return result;
        }
        return CommitTerrain(context, *owner, std::move(after), "Sculpt Terrain Ridge or Valley");
    };
    registry.Register(std::move(ridge));

    auto massif = TerrainMutation("terrain.relief.massif", "Sculpt Mountain Ridges and Valleys");
    massif.desc = "主尾根から左右に支尾根を伸ばし、その間の谷を成形します。";
    massif.params.push_back(TerrainParameter("start", OpParamType::Vec3, "主尾根の起点のワールドXZ"));
    massif.params.push_back(TerrainParameter("end", OpParamType::Vec3, "主尾根の終点のワールドXZ"));
    massif.params.push_back(TerrainParameter("branchesPerSide", OpParamType::Int, "各斜面の支尾根数。2〜4", 3));
    massif.params.push_back(TerrainParameter("seed", OpParamType::Int, "支尾根と侵食の非負シード", 1));
    AddRuleRange(massif, "height", "主尾根の増分[m]", 65.0f, 0.1f, 10000.0f);
    AddRuleRange(massif, "leftWidth", "左斜面の広がり[m]", 95.0f, 4.0f, 4000.0f);
    AddRuleRange(massif, "rightWidth", "右斜面の広がり[m]", 60.0f, 4.0f, 4000.0f);
    AddRuleRange(massif, "bend", "主尾根の中央の曲がり[m]", 0.0f, -4000.0f, 4000.0f);
    AddRuleRange(massif, "erosion", "谷筋の水食の強さ。0で無効", 0.35f, 0.0f, 1.0f);
    massif.exec = [](OpContext& context, const OpArgs& args) {
        auto* owner = ResolveTerrainNode(context, args.GetString("node"));
        const auto* terrain = owner ? owner->GetComponent<scene::TerrainComponent>() : nullptr;
        if (!terrain || !terrain->enabled || !owner->activeInHierarchy() || context.ctx.IsLocked(owner->GetID()))
            return OpResult::Err("NO_TERRAIN", "編集可能な Terrain を指定してください。");
        const int seed = args.GetInt("seed", 1);
        if (seed < 0) return OpResult::Err("BAD_SEED", "非負のシードを指定してください。");
        scene::FlushWorldTransforms(*context.ctx.activeScene);
        TerrainMassifSettings settings;
        settings.spine = {args.GetVec3("start"), args.GetVec3("end"), args.GetFloat("height", 65.0f),
            args.GetFloat("leftWidth", 95.0f), args.GetFloat("rightWidth", 60.0f), args.GetFloat("bend")};
        settings.branchesPerSide = args.GetInt("branchesPerSide", settings.branchesPerSide);
        settings.seed = static_cast<std::uint32_t>(seed);
        settings.erosion = args.GetFloat("erosion", settings.erosion);
        auto after = *terrain;
        if (!SculptTerrainMassif(after, owner->transform, settings))
            return OpResult::Err("BAD_MASSIF", "主尾根の長さ、裾幅、地形の変換と編集範囲を確認してください。");
        if (after.heightData == terrain->heightData) {
            auto result = OpResult::Ok();
            result.noChange = true;
            return result;
        }
        return CommitTerrain(context, *owner, std::move(after), "Sculpt Mountain Ridges and Valleys");
    };
    registry.Register(std::move(massif));

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
    EditorOperator drainage;
    drainage.id = "terrain.drainage.inspect";
    drainage.label = "Inspect Terrain Drainage";
    drainage.category = "Terrain";
    drainage.desc = "地形の集水面積、流下先、窪地の流出口を計算する。";
    drainage.kind = OpKind::Query;
    drainage.params = {
        TerrainParameter("node", OpParamType::NodeId, "対象の地形。省略時は選択中", std::string{}),
        TerrainParameter("seed", OpParamType::Vec3, "解析の中心となるワールドXZ"),
        TerrainParameter("maxEdges", OpParamType::Int, "返却する流路の最大数", 512)};
    AddRuleRange(drainage, "width", "解析のX幅[m]", 120.0f, 0.1f, 100000.0f);
    AddRuleRange(drainage, "depth", "解析のZ幅[m]", 120.0f, 0.1f, 100000.0f);
    AddRuleRange(drainage, "spacing", "格子間隔[m]", 2.0f, 0.1f, 100000.0f);
    AddRuleRange(drainage, "minArea", "表示する最小集水面積[m²]", 16.0f, 0.0f, 1.0e12f);
    drainage.params.push_back(TerrainParameter("terrainTiles", OpParamType::Data,
        "複数の Terrain を解析する場合の GUID 配列。最大16", OpData{}));
    drainage.exec = [](OpContext& context, const OpArgs& args) {
        auto* world = context.ctx.activeScene;
        if (!world) return OpResult::Err("NO_TERRAIN", "有効な Terrain を指定してください。");
        const auto* owner = ResolveTerrainNode(context, args.GetString("node"));
        std::vector<TerrainAnalysisSurface> surfaces;
        if (!ResolveTerrainAnalysisSurfaces(*world, owner, args.GetData("terrainTiles"), surfaces))
            return OpResult::Err("NO_TERRAIN", "有効な Terrain を指定してください。");
        const int maxEdges = args.GetInt("maxEdges", 512);
        const float minArea = args.GetFloat("minArea", 16.0f);
        if (maxEdges < 0 || maxEdges > 2048 || !std::isfinite(minArea) || minArea < 0)
            return OpResult::Err("BAD_ARG", "maxEdges は0〜2048、minArea は有限の非負値です。");
        scene::FlushWorldTransforms(*world);
        TerrainShoreSettings settings;
        settings.seed = args.GetVec3("seed");
        settings.width = args.GetFloat("width", settings.width);
        settings.depth = args.GetFloat("depth", settings.depth);
        settings.spacing = args.GetFloat("spacing", settings.spacing);
        TerrainDrainage graph;
        std::string diagnostic;
        if (!BuildTerrainDrainage(surfaces, settings, graph, diagnostic, minArea))
            return OpResult::Err("DRAINAGE_NOT_FOUND", diagnostic);
        const auto describe = [&](int index) {
            const auto& node = graph.nodes[index];
            const auto size = DrainageRiverSize(node, graph.step);
            auto data = OpData::MakeObject();
            data.Set("index", index);
            data.Set("ground", node.ground);
            data.Set("waterHeight", node.waterHeight);
            data.Set("riverSurfaceHeight", node.riverHeight);
            data.Set("area", node.area);
            data.Set("downstream", node.downstream);
            data.Set("width", size.x);
            data.Set("depth", size.y);
            return data;
        };
        auto data = OpData::MakeObject();
        data.Set("tileCount", static_cast<int>(surfaces.size()));
        data.Set("seed", describe(graph.seed));
        data.Set("spill", describe(graph.spill));
        data.Set("outlet", describe(graph.outlet));
        auto edges = OpData::MakeArray();
        int total = 0;
        for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
            const auto& node = graph.nodes[i];
            if (node.downstream < 0 || node.area < minArea) continue;
            if (total++ >= maxEdges) continue;
            auto edge = describe(static_cast<int>(i));
            edge.Set("destination", graph.nodes[node.downstream].ground);
            edges.Push(std::move(edge));
        }
        data.Set("edgeCount", total);
        data.Set("truncated", total > maxEdges);
        data.Set("edges", std::move(edges));
        return OpResult::Data(std::move(data));
    };
    registry.Register(std::move(drainage));

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
    RegisterRiverRuleOperator(registry);
    RegisterTerrainReliefOperator(registry);
    RegisterTerrainSeamOperator(registry);
    RegisterTerrainAuthoringOperators(registry);
    RegisterTerrainMapOperators(registry);
    RegisterNaturalTerrainOperators(registry);
    RegisterTerrainScatterOperators(registry);
    RegisterTerrainGrassOperators(registry);
}

} /// @note namespace fbzz::editor
