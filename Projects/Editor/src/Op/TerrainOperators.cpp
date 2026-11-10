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
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/ControlPointAdapters.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/TerrainHeightMapLoader.hpp>
#include <Engine/Scene/TerrainSplat.hpp>
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <cmath>
#include <filesystem>
#include <limits>
#include <numbers>
#include <numeric>
#include <optional>
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
    for (const auto& vertex : plan.vertices) after.heightData[vertex.index] = vertex.normalizedHeight;
    for (const auto& material : plan.materials) {
        std::copy(material.indices.begin(), material.indices.end(), after.splatIndices.data() + material.offset);
        std::copy(material.weights.begin(), material.weights.end(), after.splatWeights.data() + material.offset);
    }
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
        if (!BuildTerrainDrainage(surfaces, settings, graph, diagnostic))
            return OpResult::Err("DRAINAGE_NOT_FOUND", diagnostic);
        const auto describe = [&](int index) {
            const auto& node = graph.nodes[index];
            const auto size = DrainageRiverSize(node, graph.step);
            auto data = OpData::MakeObject();
            data.Set("index", index);
            data.Set("ground", node.ground);
            data.Set("waterHeight", node.waterHeight);
            data.Set("riverSurfaceHeight", node.waterHeight + DRAINAGE_RIVER_SURFACE_CLEARANCE);
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
    RegisterTerrainScatterOperators(registry);
    RegisterTerrainGrassOperators(registry);
}

} /// @note namespace fbzz::editor
