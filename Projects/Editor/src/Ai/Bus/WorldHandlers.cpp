/// @file    WorldHandlers.cpp
/// @brief   terrain.* / navmesh.* / environment / audio / ui のハンドラー。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include "BusInternal.hpp"

#include <Editor/Ai/JsonReflector.hpp>
#include <Editor/Util/BuildConsole.hpp>
#include <Tools/TerrainBrush.hpp>
#include <Editor/Util/SpriteSlicer.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/Components/AudioListenerComponent.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Systems/NavMeshBakeSystem.hpp>
#include <Engine/Scene/Systems/NavMeshQuery.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/Vector3.hpp>
#include <Engine/Audio/AudioManager.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::editor::ai::bus {

using scene::GameObject;
using scene::EntityID;

namespace {

/// @name Terrain

/// @brief heightData / スプラット / 穴の統計を返す。
/// @note 値そのもの (65x65 で 4225 個) は返さず起伏の要約のみ。実値は terrain.sample で読む。
/// @see Docs/design/terrain-layers.md
JsonValue TerrainStatsJson(const scene::TerrainComponent& terrain)
{
    JsonValue stats = JsonValue::MakeObject();
    const size_t expected = static_cast<size_t>(terrain.columns) * static_cast<size_t>(terrain.rows);
    const bool hasHeight = terrain.heightData.size() == expected && expected > 0;
    stats.Set("hasHeightData", JsonValue(hasHeight));
    if (hasHeight) {
        float minValue = terrain.heightData[0];
        float maxValue = terrain.heightData[0];
        double sum = 0.0;
        for (float value : terrain.heightData) {
            minValue = std::min(minValue, value);
            maxValue = std::max(maxValue, value);
            sum += static_cast<double>(value);
        }
        /// @note 正規化値でなくワールド高さで返す。AI が指定する targetHeight と単位を揃えるため。
        stats.Set("minHeight", JsonValue(minValue * terrain.maxHeight));
        stats.Set("maxHeight", JsonValue(maxValue * terrain.maxHeight));
        stats.Set("meanHeight", JsonValue(sum / static_cast<double>(expected) * terrain.maxHeight));
        stats.Set("flat", JsonValue((maxValue - minValue) * terrain.maxHeight < 0.001f));
    }
    const bool hasSplat = terrain.HasValidSplat() && expected > 0;
    stats.Set("hasSplatData", JsonValue(hasSplat));
    if (hasSplat) {
        /// @note 層ごとの «重みの総和 / 頂点数»。4 枠に入らない層は 0 になる。
        std::vector<double> layerSum(static_cast<size_t>(terrain.LayerCount()), 0.0);
        const size_t slots = static_cast<size_t>(scene::TERRAIN_SPLAT_SLOTS);
        for (size_t i = 0; i < expected * slots; ++i) {
            const size_t layer = terrain.splatIndices[i];
            if (terrain.splatWeights[i] != 0 && layer < layerSum.size())
                layerSum[layer] += static_cast<double>(terrain.splatWeights[i]);
        }
        JsonValue coverage = JsonValue::MakeArray();
        for (double sum : layerSum)
            coverage.Push(JsonValue(sum / (static_cast<double>(expected) * 255.0)));
        stats.Set("layerCoverage", std::move(coverage));
    }
    return stats;
}

Outcome DoTerrainInspect(editor::EditorContext& ctx, const JsonValue& payload)
{
    scene::Scene* activeScene = ctx.activeScene;
    if (activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
    const std::string filterId = StringField(payload, "id");

    JsonValue terrains = JsonValue::MakeArray();
    for (scene::EntityID eid : activeScene->GetEntities<scene::TerrainComponent>()) {
        GameObject* go = activeScene->GetGameObject(eid);
        auto* terrain = activeScene->GetComponent<scene::TerrainComponent>(eid);
        if (go == nullptr || terrain == nullptr) continue;
        if (!filterId.empty() && go->instanceId != filterId) continue;

        JsonValue entry = JsonValue::MakeObject();
        entry.Set("id", JsonValue(go->instanceId));
        entry.Set("name", JsonValue(go->name));
        entry.Set("enabled", JsonValue(terrain->enabled));
        entry.Set("columns", JsonValue(terrain->columns));
        entry.Set("rows", JsonValue(terrain->rows));
        entry.Set("cellSize", JsonValue(terrain->cellSize));
        entry.Set("maxHeight", JsonValue(terrain->maxHeight));
        entry.Set("chunkSize", JsonValue(terrain->chunkSize));
        entry.Set("terrainAssetPath", JsonValue(terrain->terrainAssetPath));
        /// @note ローカル寸法とワールド原点。ブラシ位置をワールドで指定するために両方要る。
        entry.Set("localSize", VectorToJson({
            static_cast<float>(terrain->columns - 1) * terrain->cellSize,
            terrain->maxHeight,
            static_cast<float>(terrain->rows - 1) * terrain->cellSize }));
        entry.Set("worldOrigin", VectorToJson(go->transform.worldPosition));
        entry.Set("heightBlendDepth", JsonValue(terrain->heightBlendDepth));
        entry.Set("layerCount", JsonValue(terrain->LayerCount()));
        entry.Set("holeCount", JsonValue(static_cast<int>(terrain->CountHoles())));
        JsonValue layers = JsonValue::MakeArray();
        for (int i = 0; i < terrain->LayerCount(); ++i) {
            JsonValue layer = JsonValue::MakeObject();
            layer.Set("index", JsonValue(i));
            layer.Set("material", JsonValue(terrain->layerMaterials[static_cast<size_t>(i)]));
            layers.Push(std::move(layer));
        }
        entry.Set("layers", std::move(layers));
        entry.Set("stats", TerrainStatsJson(*terrain));
        terrains.Push(std::move(entry));
    }

    if (!filterId.empty() && terrains.AsArray().empty())
        return Outcome::Err("NOT_PRESENT", "TerrainComponent を持つノードが見つかりません: " + filterId);

    JsonValue result = JsonValue::MakeObject();
    result.Set("count", JsonValue(static_cast<int>(terrains.AsArray().size())));
    result.Set("terrains", std::move(terrains));
    return Outcome::Ok(std::move(result));
}

/// @brief ワールド座標のブラシ中心に対して、重なる Terrain を列挙する。
struct TerrainHit {
    GameObject*              go = nullptr;
    scene::TerrainComponent* terrain = nullptr;
    math::Vector3            local;   ///< Terrain ローカル座標へ変換したブラシ中心
};

std::vector<TerrainHit> CollectTerrainsUnderBrush(scene::Scene& activeScene,
                                                  const math::Vector3& worldCenter,
                                                  float radius,
                                                  const std::string& restrictToNodeId)
{
    std::vector<TerrainHit> hits;
    for (scene::EntityID eid : activeScene.GetEntities<scene::TerrainComponent>()) {
        GameObject* go = activeScene.GetGameObject(eid);
        auto* terrain = activeScene.GetComponent<scene::TerrainComponent>(eid);
        if (go == nullptr || terrain == nullptr || !terrain->enabled) continue;
        if (!restrictToNodeId.empty() && go->instanceId != restrictToNodeId) continue;
        const math::Vector3 local = ToTerrainLocal(go->transform, worldCenter);
        /// @note 半径 0 の問い合わせ (sample) でも矩形内なら拾えるよう、下限を 0 として扱う。
        if (!BrushOverlapsTerrainXZ(*terrain, local, std::max(radius, 0.0f))) continue;
        hits.push_back({ go, terrain, local });
    }
    return hits;
}

/// @brief 指定ワールド点の高さ・法線・レイヤー重みを返す。
/// @note オブジェクトを置く前の下見や、sculpt 後の平坦確認に使う。
Outcome DoTerrainSample(editor::EditorContext& ctx, const JsonValue& payload)
{
    scene::Scene* activeScene = ctx.activeScene;
    if (activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
    const JsonValue* points = payload.Find("points");
    if (points == nullptr || !points->IsArray() || points->AsArray().empty())
        return Outcome::Err("BAD_ARG", "points ([[x,y,z], ...]) が必要です");
    if (points->AsArray().size() > 256)
        return Outcome::Err("BAD_ARG", "points は 256 点までです");
    const std::string restrictTo = StringField(payload, "id");

    JsonValue samples = JsonValue::MakeArray();
    for (const JsonValue& pointValue : points->AsArray()) {
        if (!pointValue.IsArray() || pointValue.AsArray().size() < 2) continue;
        const auto& array = pointValue.AsArray();
        /// @note [x, z] の 2 要素も許す (高さを問い合わせるのに y は不要なため)。
        const bool hasY = array.size() >= 3;
        const math::Vector3 world{
            static_cast<float>(array[0].AsNumber()),
            hasY ? static_cast<float>(array[1].AsNumber()) : 0.0f,
            static_cast<float>(array[hasY ? 2 : 1].AsNumber())
        };
        JsonValue sample = JsonValue::MakeObject();
        sample.Set("query", VectorToJson(world));
        const std::vector<TerrainHit> hits = CollectTerrainsUnderBrush(*activeScene, world, 0.0f, restrictTo);
        if (hits.empty()) {
            sample.Set("onTerrain", JsonValue(false));
            samples.Push(std::move(sample));
            continue;
        }
        const TerrainHit& hit = hits.front();
        const scene::TerrainComponent& terrain = *hit.terrain;
        const float localHeight = terrain.GetHeightAt(hit.local.x, hit.local.z);
        const math::Vector3 localNormal = terrain.GetNormalAt(hit.local.x, hit.local.z);
        sample.Set("onTerrain", JsonValue(true));
        sample.Set("terrainId", JsonValue(hit.go->instanceId));
        sample.Set("local", VectorToJson({ hit.local.x, localHeight, hit.local.z }));
        /// @note 高さはワールド Y で返す (ブラシの targetHeight もワールド系で受けるため)。
        sample.Set("worldHeight", JsonValue(ToTerrainWorld(hit.go->transform,
            { hit.local.x, localHeight, hit.local.z }).y));
        sample.Set("normal", VectorToJson(localNormal));
        /// @note 斜度は NavMesh の歩行可否と直結するので、法線から算出して添える。
        sample.Set("slopeDegrees", JsonValue(math::ToDeg(std::acos(
            std::clamp(localNormal.y, -1.0f, 1.0f)))));
        sample.Set("hole", JsonValue(terrain.IsHoleAtLocal(hit.local.x, hit.local.z)));
        if (terrain.HasValidSplat() && terrain.cellSize > 0.0f) {
            /// @note 最寄り頂点の 4 枠のうち重みを持つ層だけを返す。正準形なので重みの降順に並ぶ。
            const int gx = std::clamp(static_cast<int>(hit.local.x / terrain.cellSize + 0.5f), 0, terrain.columns - 1);
            const int gz = std::clamp(static_cast<int>(hit.local.z / terrain.cellSize + 0.5f), 0, terrain.rows - 1);
            const size_t base = (static_cast<size_t>(gz) * static_cast<size_t>(terrain.columns)
                               + static_cast<size_t>(gx)) * static_cast<size_t>(scene::TERRAIN_SPLAT_SLOTS);
            JsonValue weights = JsonValue::MakeArray();
            for (size_t slot = 0; slot < static_cast<size_t>(scene::TERRAIN_SPLAT_SLOTS); ++slot) {
                const std::uint8_t weight = terrain.splatWeights[base + slot];
                if (weight == 0) continue;
                JsonValue layerWeight = JsonValue::MakeObject();
                layerWeight.Set("layer", JsonValue(static_cast<int>(terrain.splatIndices[base + slot])));
                layerWeight.Set("weight", JsonValue(static_cast<float>(weight) / 255.0f));
                weights.Push(std::move(layerWeight));
            }
            sample.Set("layerWeights", std::move(weights));
        }
        samples.Push(std::move(sample));
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("count", JsonValue(static_cast<int>(samples.AsArray().size())));
    result.Set("samples", std::move(samples));
    return Outcome::Ok(std::move(result));
}

/// @brief payload からブラシ形状を読む。sculpt / paint の両方で共用する。
bool ReadTerrainBrush(const JsonValue& payload, TerrainBrush& outBrush, std::string& outError)
{
    if (const JsonValue* v = payload.Find("radius"); v != nullptr && v->IsNumber())
        outBrush.radius = static_cast<float>(v->AsNumber());
    if (const JsonValue* v = payload.Find("strength"); v != nullptr && v->IsNumber())
        outBrush.strength = static_cast<float>(v->AsNumber());
    const std::string falloff = LowerAscii(StringField(payload, "falloff"));
    if (!falloff.empty()) {
        if (falloff == "linear")        outBrush.falloff = TerrainFalloff::Linear;
        else if (falloff == "smooth")   outBrush.falloff = TerrainFalloff::Smooth;
        else if (falloff == "gaussian") outBrush.falloff = TerrainFalloff::Gaussian;
        else { outError = "falloff は linear / smooth / gaussian です"; return false; }
    }
    if (!(outBrush.radius > 0.0f) || outBrush.radius > 500.0f) {
        outError = "radius は 0 より大きく 500 以下で指定してください"; return false;
    }
    if (!(outBrush.strength > 0.0f) || outBrush.strength > 1.0f) {
        outError = "strength は 0 より大きく 1 以下で指定してください"; return false;
    }
    return true;
}

/// @brief sculpt の op 固有パラメーター (ノイズ・侵食・段々) を読む。未指定は TerrainBrush の既定値のまま。
/// @see https://history.siggraph.org/learning/the-synthesis-and-rendering-of-eroded-fractal-terrains-by-musgrave-kolb-and-mace/
/// @see https://www.firespark.de/resources/downloads/implementation%20of%20a%20methode%20for%20hydraulic%20erosion.pdf
bool ReadTerrainSculptParams(const JsonValue& payload, TerrainBrush& outBrush, std::string& outError)
{
    const auto readNumber = [&payload](const char* key, double& out) {
        const JsonValue* v = payload.Find(key);
        if (v == nullptr || !v->IsNumber()) return false;
        out = v->AsNumber();
        return true;
    };
    double value = 0.0;
    if (readNumber("noiseScale", value)) {
        if (!(value > 0.0) || value > 1000.0) { outError = "noiseScale は 0 より大きく 1000 以下 [m] で指定してください"; return false; }
        outBrush.noiseScale = static_cast<float>(value);
    }
    if (readNumber("noiseOctaves", value)) {
        if (value < 1.0 || value > 8.0) { outError = "noiseOctaves は 1〜8 で指定してください"; return false; }
        outBrush.noiseOctaves = static_cast<int>(value);
    }
    if (readNumber("seed", value)) {
        if (value < 0.0 || value > 4294967295.0) { outError = "seed は 0〜4294967295 の整数で指定してください"; return false; }
        outBrush.seed = static_cast<std::uint32_t>(value);
    }
    if (readNumber("terraceStep", value)) {
        if (!(value > 0.0) || value > 1000.0) { outError = "terraceStep は 0 より大きく 1000 以下 [m] で指定してください"; return false; }
        outBrush.terraceStep = static_cast<float>(value);
    }
    if (readNumber("terraceSharpness", value)) {
        if (value < 0.0 || value > 1.0) { outError = "terraceSharpness は 0〜1 で指定してください"; return false; }
        outBrush.terraceSharpness = static_cast<float>(value);
    }
    if (readNumber("talus", value)) {
        if (!(value > 0.0) || !(value < 90.0)) { outError = "talus は 0 より大きく 90 未満 [度] で指定してください"; return false; }
        outBrush.talusDegrees = static_cast<float>(value);
    }
    if (readNumber("droplets", value)) {
        if (value < 1.0 || value > 4096.0) { outError = "droplets は 1〜4096 で指定してください"; return false; }
        outBrush.erosionDroplets = static_cast<int>(value);
    }
    return true;
}

/// @brief Terrain の Undo 単位。触れた Terrain を丸ごとスナップショットして戻す (TerrainTool と同じ方式)。
/// @note 高さ・スプラット・穴は差分記述が複雑なため、部分復元でなくストローク単位のコピーで揃える。
struct TerrainSnapshot {
    std::string             instanceId;
    scene::TerrainComponent component;
};

std::unique_ptr<ICommand> MakeTerrainEditCommand(scene::Scene* activeScene,
                                                 const char* label,
                                                 std::vector<TerrainSnapshot> before,
                                                 std::vector<TerrainSnapshot> after,
                                                 std::function<void()> markDirty)
{
    auto apply = [activeScene, markDirty](const std::vector<TerrainSnapshot>& values) {
        for (const TerrainSnapshot& snapshot : values) {
            GameObject* target = activeScene->FindByGuid(snapshot.instanceId);
            if (target == nullptr) continue;
            auto* component = target->GetComponent<scene::TerrainComponent>();
            if (component == nullptr) continue;
            *component = snapshot.component;
            component->heightDirty = true;
            component->splatDirty = true;
            component->materialParamDirty = true;
            component->colliderDirty = true;
        }
        if (markDirty) markDirty();
    };
    auto beforeShared = std::make_shared<std::vector<TerrainSnapshot>>(std::move(before));
    auto afterShared  = std::make_shared<std::vector<TerrainSnapshot>>(std::move(after));
    return std::make_unique<LambdaCommand>(label,
        [apply, afterShared]()  { apply(*afterShared); },
        [apply, beforeShared]() { apply(*beforeShared); });
}

/// @name NavMesh

/// @brief Surface の設定・ベイク結果・XZ バウンドを返す。
JsonValue NavMeshSurfaceJson(GameObject& go, const scene::NavMeshSurfaceComponent& surface)
{
    JsonValue entry = JsonValue::MakeObject();
    entry.Set("id", JsonValue(go.instanceId));
    entry.Set("name", JsonValue(go.name));
    entry.Set("enabled", JsonValue(surface.enabled));
    entry.Set("agentTypeId", JsonValue(surface.agentTypeId));
    entry.Set("collectObjects", JsonValue(
        surface.collectObjects == scene::NavMeshCollectObjects::Volume ? "volume" : "thisObject"));
    entry.Set("size", VectorToJson(surface.size));
    entry.Set("cellSize", JsonValue(surface.cellSize));
    entry.Set("maxSlopeAngleDeg", JsonValue(surface.maxSlopeAngleDeg));
    entry.Set("agentRadius", JsonValue(surface.agentRadius));
    entry.Set("agentHeight", JsonValue(surface.agentHeight));
    entry.Set("maxClimb", JsonValue(surface.maxClimb));
    const char* stateName = "idle";
    if (surface.bakeState == scene::NavMeshBakeState::Baking)    stateName = "baking";
    else if (surface.bakeState == scene::NavMeshBakeState::Done) stateName = "done";
    entry.Set("bakeState", JsonValue(stateName));
    entry.Set("bakeProgress", JsonValue(surface.bakeProgress));
    entry.Set("needsBake", JsonValue(surface.needsBake));
    entry.Set("polygonCount", JsonValue(static_cast<int>(surface.navMesh.polygons.size())));
    entry.Set("offMeshLinkCount", JsonValue(static_cast<int>(surface.navMesh.offMeshLinks.size())));
    /// @note polygonCount=0 だけでは傾斜・障害物・エージェント半径のどれで落ちたか分からないため、内訳を添える。
    if (!surface.bakeStats.failReason.empty())
        entry.Set("failReason", JsonValue(surface.bakeStats.failReason));
    if (surface.bakeStats.cellsX > 0) {
        JsonValue bakeInfo = JsonValue::MakeObject();
        bakeInfo.Set("seconds",     JsonValue(surface.bakeStats.bakeSeconds));
        bakeInfo.Set("cellsX",      JsonValue(surface.bakeStats.cellsX));
        bakeInfo.Set("cellsZ",      JsonValue(surface.bakeStats.cellsZ));
        bakeInfo.Set("walkable",    JsonValue(surface.bakeStats.walkableCells));
        bakeInfo.Set("tooSteep",    JsonValue(surface.bakeStats.steepCells));
        bakeInfo.Set("tooHighStep", JsonValue(surface.bakeStats.stepCells));
        bakeInfo.Set("obstructed",  JsonValue(surface.bakeStats.obstructedCells));
        bakeInfo.Set("eroded",      JsonValue(surface.bakeStats.erodedCells));
        bakeInfo.Set("areaSquareMeters", JsonValue(surface.bakeStats.areaSquareMeters));
        entry.Set("bake", std::move(bakeInfo));
    }
    /// @note 歩ける範囲そのもの。AI が目的地を選ぶ唯一の手掛かりになる。
    if (!surface.navMesh.polygons.empty()) {
        math::Vector3 boundsMin{ 1e30f, 1e30f, 1e30f };
        math::Vector3 boundsMax{ -1e30f, -1e30f, -1e30f };
        std::unordered_map<int, int> areaHistogram;
        for (const scene::NavMeshPolygon& polygon : surface.navMesh.polygons) {
            ++areaHistogram[polygon.areaType];
            for (const math::Vector3& vertex : polygon.vertices) {
                boundsMin = { std::min(boundsMin.x, vertex.x), std::min(boundsMin.y, vertex.y), std::min(boundsMin.z, vertex.z) };
                boundsMax = { std::max(boundsMax.x, vertex.x), std::max(boundsMax.y, vertex.y), std::max(boundsMax.z, vertex.z) };
            }
        }
        JsonValue bounds = JsonValue::MakeObject();
        bounds.Set("min", VectorToJson(boundsMin));
        bounds.Set("max", VectorToJson(boundsMax));
        entry.Set("bounds", std::move(bounds));
        JsonValue areas = JsonValue::MakeArray();
        for (const auto& [areaType, count] : areaHistogram) {
            JsonValue area = JsonValue::MakeObject();
            area.Set("areaType", JsonValue(areaType));
            area.Set("polygons", JsonValue(count));
            area.Set("cost", JsonValue(surface.areaCosts[static_cast<size_t>(std::clamp(areaType, 0, 31))]));
            areas.Push(std::move(area));
        }
        entry.Set("areas", std::move(areas));
    }
    return entry;
}

Outcome DoNavMeshState(editor::EditorContext& ctx, const JsonValue& payload)
{
    scene::Scene* activeScene = ctx.activeScene;
    if (activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
    const std::string filterId = StringField(payload, "id");

    JsonValue surfaces = JsonValue::MakeArray();
    for (scene::EntityID eid : activeScene->GetEntities<scene::NavMeshSurfaceComponent>()) {
        GameObject* go = activeScene->GetGameObject(eid);
        auto* surface = activeScene->GetComponent<scene::NavMeshSurfaceComponent>(eid);
        if (go == nullptr || surface == nullptr) continue;
        if (!filterId.empty() && go->instanceId != filterId) continue;
        JsonValue entry = NavMeshSurfaceJson(*go, *surface);
        /// @note 古い NavMesh でも navmesh_find_path は成功を返すため、地形変更後の不整合を stale で明示する。
        if (surface->navMesh.IsValid()) {
            const uint64_t current = scene::HashNavMeshBakeSources(*activeScene, eid);
            entry.Set("stale", JsonValue(current != surface->bakedSourceHash));
        }
        surfaces.Push(std::move(entry));
    }

    /// @note Agent 側も併せて返す。「経路が引けない」の原因が Surface 側か Agent 設定側かは、両方を並べて初めて切り分けられる (agentTypeId の食い違いが典型)。
    JsonValue agents = JsonValue::MakeArray();
    for (scene::EntityID eid : activeScene->GetEntities<scene::NavMeshAgentComponent>()) {
        GameObject* go = activeScene->GetGameObject(eid);
        auto* agent = activeScene->GetComponent<scene::NavMeshAgentComponent>(eid);
        if (go == nullptr || agent == nullptr) continue;
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("id", JsonValue(go->instanceId));
        entry.Set("name", JsonValue(go->name));
        entry.Set("enabled", JsonValue(agent->enabled));
        entry.Set("agentTypeId", JsonValue(agent->agentTypeId));
        entry.Set("areaMask", JsonValue(agent->areaMask));
        entry.Set("position", VectorToJson(go->transform.worldPosition));
        entry.Set("hasDestination", JsonValue(agent->hasDestination));
        if (agent->hasDestination) entry.Set("destination", VectorToJson(agent->destination));
        const char* agentState = "idle";
        if (agent->state == scene::NavMeshAgentState::MOVING)                agentState = "moving";
        else if (agent->state == scene::NavMeshAgentState::TRAVERSING_LINK)  agentState = "traversingLink";
        entry.Set("state", JsonValue(agentState));
        entry.Set("pathWaypoints", JsonValue(static_cast<int>(agent->path.size())));
        entry.Set("currentWaypoint", JsonValue(static_cast<int>(agent->currentWaypoint)));
        entry.Set("isStopped", JsonValue(agent->isStopped));
        entry.Set("destinationReached", JsonValue(agent->destinationReached));
        entry.Set("currentSpeed", JsonValue(agent->currentSpeed));
        agents.Push(std::move(entry));
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("surfaceCount", JsonValue(static_cast<int>(surfaces.AsArray().size())));
    result.Set("surfaces", std::move(surfaces));
    result.Set("agentCount", JsonValue(static_cast<int>(agents.AsArray().size())));
    result.Set("agents", std::move(agents));
    return Outcome::Ok(std::move(result));
}

/// @brief agentTypeId に一致し、ベイク済みの Surface を選ぶ。id 指定があればそれを優先する。
scene::NavMeshSurfaceComponent* ResolveNavMeshSurface(scene::Scene& activeScene,
                                                      const std::string& nodeId,
                                                      int agentTypeId,
                                                      GameObject** outGo)
{
    for (scene::EntityID eid : activeScene.GetEntities<scene::NavMeshSurfaceComponent>()) {
        GameObject* go = activeScene.GetGameObject(eid);
        auto* surface = activeScene.GetComponent<scene::NavMeshSurfaceComponent>(eid);
        if (go == nullptr || surface == nullptr) continue;
        if (!nodeId.empty()) {
            if (go->instanceId != nodeId) continue;
        } else {
            if (!surface->enabled || surface->agentTypeId != agentTypeId) continue;
            if (!surface->navMesh.IsValid()) continue;
        }
        if (outGo != nullptr) *outGo = go;
        return surface;
    }
    return nullptr;
}

/// @brief 2 点間の経路を、Agent が実際に使うのと同じ A* + Funnel で引く。
/// @note 「敵がここへ来ない」の原因は BT の条件・Agent 設定・NavMesh の穴の 3 通りで、3 つ目は経路を引くまで分からない。
Outcome DoNavMeshPath(editor::EditorContext& ctx, const JsonValue& payload)
{
    scene::Scene* activeScene = ctx.activeScene;
    if (activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");

    math::Vector3 from;
    math::Vector3 to;
    /// @note from は座標でも Agent の NodeId でも指定できる (「今いる場所から」が最も多い問い合わせ)。
    const std::string fromNodeId = StringField(payload, "fromId");
    int agentTypeId = 0;
    int areaMask = -1;
    if (const JsonValue* v = payload.Find("agentTypeId"); v != nullptr && v->IsNumber())
        agentTypeId = v->AsInt();
    if (const JsonValue* v = payload.Find("areaMask"); v != nullptr && v->IsNumber())
        areaMask = v->AsInt();
    if (!fromNodeId.empty()) {
        GameObject* fromGo = activeScene->FindByGuid(fromNodeId);
        if (fromGo == nullptr) return Outcome::Err("NODE_NOT_FOUND", "fromId が見つかりません: " + fromNodeId);
        from = fromGo->transform.worldPosition;
        /// @note Agent があればその agentTypeId / areaMask を既定として引き継ぐ。
        if (auto* agent = fromGo->GetComponent<scene::NavMeshAgentComponent>()) {
            if (payload.Find("agentTypeId") == nullptr) agentTypeId = agent->agentTypeId;
            if (payload.Find("areaMask") == nullptr)    areaMask = agent->areaMask;
        }
    } else if (!ReadVec3(payload, "from", from)) {
        return Outcome::Err("BAD_ARG", "from ([x,y,z]) または fromId が必要です");
    }
    const std::string toNodeId = StringField(payload, "toId");
    if (!toNodeId.empty()) {
        GameObject* toGo = activeScene->FindByGuid(toNodeId);
        if (toGo == nullptr) return Outcome::Err("NODE_NOT_FOUND", "toId が見つかりません: " + toNodeId);
        to = toGo->transform.worldPosition;
    } else if (!ReadVec3(payload, "to", to)) {
        return Outcome::Err("BAD_ARG", "to ([x,y,z]) または toId が必要です");
    }

    GameObject* surfaceGo = nullptr;
    scene::NavMeshSurfaceComponent* surface =
        ResolveNavMeshSurface(*activeScene, StringField(payload, "surfaceId"), agentTypeId, &surfaceGo);
    if (surface == nullptr) {
        return Outcome::Err("NO_NAVMESH",
            "agentTypeId=" + std::to_string(agentTypeId) + " に対応するベイク済み NavMesh Surface がありません");
    }
    if (!surface->navMesh.IsValid())
        return Outcome::Err("NAVMESH_NOT_BAKED", "NavMesh が未ベイクです。navmesh_bake を実行してください");

    const scene::NavMesh& navMesh = surface->navMesh;
    const int startPoly = scene::FindNearestPolygon(navMesh, from);
    const int goalPoly  = scene::FindNearestPolygon(navMesh, to);

    JsonValue result = JsonValue::MakeObject();
    result.Set("surfaceId", JsonValue(surfaceGo != nullptr ? surfaceGo->instanceId : std::string{}));
    result.Set("agentTypeId", JsonValue(agentTypeId));
    result.Set("areaMask", JsonValue(areaMask));
    result.Set("from", VectorToJson(from));
    result.Set("to", VectorToJson(to));
    result.Set("startPolygon", JsonValue(startPoly));
    result.Set("goalPolygon", JsonValue(goalPoly));

    if (startPoly < 0 || goalPoly < 0) {
        result.Set("found", JsonValue(false));
        result.Set("reason", JsonValue("START_OR_GOAL_OFF_NAVMESH"));
        return Outcome::Ok(std::move(result));
    }

    std::vector<int> polyPath;
    if (!scene::FindPolygonPath(navMesh, startPoly, goalPoly, polyPath, areaMask, surface->areaCosts, agentTypeId)) {
        result.Set("found", JsonValue(false));
        /// @note 到達不能とエリアマスクによる遮断は A* からは区別できない。areaMask を返すので -1 で引き直せば切り分けられる。
        result.Set("reason", JsonValue("NO_PATH"));
        return Outcome::Ok(std::move(result));
    }

    const std::vector<math::Vector3> corners = scene::BuildFunnelPath(navMesh, polyPath, from, to);
    JsonValue cornerArray = JsonValue::MakeArray();
    float length = 0.0f;
    for (size_t i = 0; i < corners.size(); ++i) {
        cornerArray.Push(VectorToJson(corners[i]));
        if (i > 0) length += (corners[i] - corners[i - 1]).Length();
    }
    result.Set("found", JsonValue(true));
    result.Set("corners", std::move(cornerArray));
    result.Set("cornerCount", JsonValue(static_cast<int>(corners.size())));
    result.Set("polygonCount", JsonValue(static_cast<int>(polyPath.size())));
    result.Set("length", JsonValue(length));
    /// @note 直線距離との比。大きいほど遠回り = 障害物か穴を迂回している。
    const float straight = (to - from).Length();
    result.Set("straightDistance", JsonValue(straight));
    result.Set("detourRatio", JsonValue(straight > 0.0001f ? length / straight : 1.0f));
    return Outcome::Ok(std::move(result));
}

/// @brief 指定点が NavMesh 上か、面上ならその高さを返す。Agent の湧き位置を決めるのに使う。
Outcome DoNavMeshSample(editor::EditorContext& ctx, const JsonValue& payload)
{
    scene::Scene* activeScene = ctx.activeScene;
    if (activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
    const JsonValue* points = payload.Find("points");
    if (points == nullptr || !points->IsArray() || points->AsArray().empty())
        return Outcome::Err("BAD_ARG", "points ([[x,y,z], ...]) が必要です");
    if (points->AsArray().size() > 256) return Outcome::Err("BAD_ARG", "points は 256 点までです");

    int agentTypeId = 0;
    if (const JsonValue* v = payload.Find("agentTypeId"); v != nullptr && v->IsNumber())
        agentTypeId = v->AsInt();
    GameObject* surfaceGo = nullptr;
    scene::NavMeshSurfaceComponent* surface =
        ResolveNavMeshSurface(*activeScene, StringField(payload, "surfaceId"), agentTypeId, &surfaceGo);
    if (surface == nullptr || !surface->navMesh.IsValid())
        return Outcome::Err("NO_NAVMESH", "ベイク済み NavMesh Surface がありません");

    JsonValue samples = JsonValue::MakeArray();
    for (const JsonValue& pointValue : points->AsArray()) {
        if (!pointValue.IsArray() || pointValue.AsArray().size() < 3) continue;
        const auto& array = pointValue.AsArray();
        const math::Vector3 query{
            static_cast<float>(array[0].AsNumber()),
            static_cast<float>(array[1].AsNumber()),
            static_cast<float>(array[2].AsNumber())
        };
        JsonValue sample = JsonValue::MakeObject();
        sample.Set("query", VectorToJson(query));
        const int polygon = scene::FindNearestPolygon(surface->navMesh, query);
        if (polygon < 0) {
            sample.Set("onNavMesh", JsonValue(false));
            samples.Push(std::move(sample));
            continue;
        }
        const bool inside = surface->navMesh.polygons[static_cast<size_t>(polygon)].ContainsXZ(query.x, query.z);
        const float height = scene::SampleNavMeshHeight(surface->navMesh, query);
        sample.Set("onNavMesh", JsonValue(inside));
        sample.Set("polygon", JsonValue(polygon));
        sample.Set("areaType", JsonValue(surface->navMesh.polygons[static_cast<size_t>(polygon)].areaType));
        /// @note 面の外なら最近傍ポリゴンへ寄せた点を返す。Agent を置き直す座標としてそのまま使える。
        sample.Set("nearest", VectorToJson({ query.x, height <= -1e6f ? query.y : height, query.z }));
        if (!inside) {
            sample.Set("distanceXZ", JsonValue(std::sqrt(
                surface->navMesh.polygons[static_cast<size_t>(polygon)].DistanceSqXZ(query.x, query.z))));
        }
        samples.Push(std::move(sample));
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("surfaceId", JsonValue(surfaceGo != nullptr ? surfaceGo->instanceId : std::string{}));
    result.Set("count", JsonValue(static_cast<int>(samples.AsArray().size())));
    result.Set("samples", std::move(samples));
    return Outcome::Ok(std::move(result));
}

/// @name Environment (空・光・霧・ポストプロセス)

/// @brief 環境系コンポーネントの一覧。Reflect 済みの値を editor_catalog のフィールド定義と 1 対 1 で返す。
/// @note 空・太陽・霧・IBL・雲・ポストは別々の GameObject に散らばっており、明るさの理由を 1 回で読めるようにする。
Outcome DoEnvironmentInspect(editor::EditorContext& ctx)
{
    scene::Scene* activeScene = ctx.activeScene;
    if (activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");

    /// @note 対象は ComponentCategory::Environment 登録型すべて。名前表を書くと新規コンポーネントが AI から見えなくなる。
    std::unordered_set<std::string> environmentTypes;
    scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
        if constexpr (Reg::category == scene::ComponentCategory::Environment
                   && Reg::inspectorMode != scene::ComponentInspectorMode::Hidden) {
            environmentTypes.insert(Reg::serializedName);
        }
    });

    JsonValue nodes = JsonValue::MakeArray();
    for (GameObject& gameObject : activeScene->GameObjects()) {
        GameObject* go = &gameObject;
        JsonValue components = SnapshotComponents(*go);
        JsonValue matched = JsonValue::MakeArray();
        for (const JsonValue& component : components.AsArray()) {
            const JsonValue* typeValue = component.Find("type");
            if (typeValue == nullptr || !typeValue->IsString()) continue;
            if (environmentTypes.count(typeValue->AsString()) != 0) matched.Push(component);
        }
        if (matched.AsArray().empty()) continue;
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("id", JsonValue(go->instanceId));
        entry.Set("name", JsonValue(go->name));
        entry.Set("active", JsonValue(go->activeSelf()));
        entry.Set("activeInHierarchy", JsonValue(go->activeInHierarchy()));
        entry.Set("components", std::move(matched));
        nodes.Push(std::move(entry));
    }

    /// @note ライトは環境の一部だが数が多いので、種別と強度だけの要約にする。
    JsonValue lights = JsonValue::MakeArray();
    for (scene::EntityID eid : activeScene->GetEntities<scene::LightComponent>()) {
        GameObject* go = activeScene->GetGameObject(eid);
        auto* light = activeScene->GetComponent<scene::LightComponent>(eid);
        if (go == nullptr || light == nullptr) continue;
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("id", JsonValue(go->instanceId));
        entry.Set("name", JsonValue(go->name));
        entry.Set("active", JsonValue(go->activeSelf()));
        entry.Set("activeInHierarchy", JsonValue(go->activeInHierarchy()));
        entry.Set("position", VectorToJson(go->transform.worldPosition));
        entry.Set("forward", VectorToJson(go->transform.Forward()));
        JsonReadReflector reader(true);
        light->Reflect(reader);
        entry.Set("fields", reader.Result());
        lights.Push(std::move(entry));
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("nodeCount", JsonValue(static_cast<int>(nodes.AsArray().size())));
    result.Set("nodes", std::move(nodes));
    result.Set("lightCount", JsonValue(static_cast<int>(lights.AsArray().size())));
    result.Set("lights", std::move(lights));
    return Outcome::Ok(std::move(result));
}

/// @name Audio

Outcome DoAudioInspect(editor::EditorContext& ctx, const JsonValue& payload)
{
    scene::Scene* activeScene = ctx.activeScene;
    if (activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
    const std::string filterId = StringField(payload, "id");

    JsonValue sources = JsonValue::MakeArray();
    for (scene::EntityID eid : activeScene->GetEntities<scene::AudioSourceComponent>()) {
        GameObject* go = activeScene->GetGameObject(eid);
        auto* source = activeScene->GetComponent<scene::AudioSourceComponent>(eid);
        if (go == nullptr || source == nullptr) continue;
        if (!filterId.empty() && go->instanceId != filterId) continue;
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("id", JsonValue(go->instanceId));
        entry.Set("name", JsonValue(go->name));
        entry.Set("active", JsonValue(go->activeSelf()));
        entry.Set("activeInHierarchy", JsonValue(go->activeInHierarchy()));
        entry.Set("position", VectorToJson(go->transform.worldPosition));
        JsonReadReflector reader(true);
        source->Reflect(reader);
        entry.Set("fields", reader.Result());
        /// @note 再生状態は保存対象でなく Reflect に載らないため、runtime として明示的に足す。
        JsonValue runtime = JsonValue::MakeObject();
        runtime.Set("playing", JsonValue(source->m_isPlaying));
        runtime.Set("paused", JsonValue(source->m_isPaused));
        runtime.Set("voiceId", JsonValue(static_cast<int>(source->m_voiceId)));
        runtime.Set("playOnAwakeFired", JsonValue(source->m_played));
        entry.Set("runtime", std::move(runtime));
        sources.Push(std::move(entry));
    }

    JsonValue listeners = JsonValue::MakeArray();
    for (scene::EntityID eid : activeScene->GetEntities<scene::AudioListenerComponent>()) {
        GameObject* go = activeScene->GetGameObject(eid);
        if (go == nullptr) continue;
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("id", JsonValue(go->instanceId));
        entry.Set("name", JsonValue(go->name));
        entry.Set("active", JsonValue(go->activeSelf()));
        entry.Set("activeInHierarchy", JsonValue(go->activeInHierarchy()));
        entry.Set("position", VectorToJson(go->transform.worldPosition));
        listeners.Push(std::move(entry));
    }

    /// @note AudioSource の busName が指せる名前の一覧。未知の名前はエラーにならず Master へ落ちるだけになる。
    JsonValue buses = JsonValue::MakeArray();
    int voiceLimit   = 0;
    int activeVoices = 0;
    if (auto* audioManager = core::Application::Get().GetAudioManager()) {
        voiceLimit   = static_cast<int>(audioManager->VoiceLimit());
        activeVoices = static_cast<int>(audioManager->ActiveVoiceCount());
        for (const audio::BusDesc& desc : audioManager->BusLayout()) {
            JsonValue entry = JsonValue::MakeObject();
            entry.Set("name", JsonValue(desc.name));
            entry.Set("parent", JsonValue(desc.parent));
            entry.Set("volume", JsonValue(desc.volume));
            entry.Set("lowPassCutoff", JsonValue(desc.lowPassCutoff));
            /// @note AudioReverbZone が効くのは reverb=true のバスへ出している音だけ。
            entry.Set("reverb", JsonValue(desc.reverb));
            buses.Push(std::move(entry));
        }
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("sourceCount", JsonValue(static_cast<int>(sources.AsArray().size())));
    result.Set("sources", std::move(sources));
    result.Set("listenerCount", JsonValue(static_cast<int>(listeners.AsArray().size())));
    result.Set("listeners", std::move(listeners));
    result.Set("buses", std::move(buses));
    /// @note 同時発音の上限に張り付いていると、優先度の低い音から畳まれて鳴らなくなる。
    result.Set("voiceLimit", JsonValue(voiceLimit));
    result.Set("activeVoices", JsonValue(activeVoices));
    /// @note 3D 減衰は Listener が無いと成立しない。「音が聞こえない」の最頻出原因なので明示する。
    if (listeners.AsArray().empty())
        result.Set("warning", JsonValue("AudioListener がシーンにありません (3D 音の距離減衰が効きません)"));
    return Outcome::Ok(std::move(result));
}

/// @name UI

/// @brief Canvas を根とする UI ツリーを、矩形と描画順が読める形で返す。
/// @note scene_get_tree の階層だけでは画面のどこに何が出るか分からず、viewport_capture の絵と突き合わせられない。
JsonValue UIElementJson(GameObject& go)
{
    JsonValue entry = JsonValue::MakeObject();
    entry.Set("id", JsonValue(go.instanceId));
    entry.Set("name", JsonValue(go.name));
    entry.Set("active", JsonValue(go.activeSelf()));
    entry.Set("activeInHierarchy", JsonValue(go.activeInHierarchy()));
    entry.Set("components", SnapshotComponents(go));
    JsonValue children = JsonValue::MakeArray();
    for (int i = 0; i < go.GetChildCount(); ++i) {
        GameObject* child = go.GetChild(i);
        if (child == nullptr || child->runtimeGenerated) continue;
        children.Push(UIElementJson(*child));
    }
    entry.Set("children", std::move(children));
    return entry;
}

Outcome DoUIInspect(editor::EditorContext& ctx, const JsonValue& payload)
{
    scene::Scene* activeScene = ctx.activeScene;
    if (activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
    const std::string filterId = StringField(payload, "id");

    JsonValue canvases = JsonValue::MakeArray();
    for (scene::EntityID eid : activeScene->GetEntities<scene::UICanvas>()) {
        GameObject* go = activeScene->GetGameObject(eid);
        if (go == nullptr) continue;
        if (!filterId.empty() && go->instanceId != filterId) continue;
        canvases.Push(UIElementJson(*go));
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("count", JsonValue(static_cast<int>(canvases.AsArray().size())));
    result.Set("canvases", std::move(canvases));
    /// @note 編集対象として選ばれている Canvas。UIViewport の操作対象と AI の対象を一致させる。
    if (ctx.activeUICanvas.IsValid()) {
        if (GameObject* activeCanvas = activeScene->GetGameObject(ctx.activeUICanvas))
            result.Set("activeCanvasId", JsonValue(activeCanvas->instanceId));
    }
    /// @note UI は Game View の解像度で座標が決まるので、基準の画面サイズも返す。
    JsonValue viewport = JsonValue::MakeObject();
    viewport.Set("width", JsonValue(ctx.gameViewportWidth));
    viewport.Set("height", JsonValue(ctx.gameViewportHeight));
    result.Set("gameViewport", std::move(viewport));
    return Outcome::Ok(std::move(result));
}

std::unique_ptr<ICommand> BuildTerrainBrushCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    /// @name Terrain: ブラシ操作
    /// @note マウスドラッグを持たない AI のため、ストローク 1 回分を 1 コマンドとして受ける。iterations は押し続けた回数相当 (Smooth/Flatten は 1 回で収束しない)。
    if (type == "terrain.sculpt" || type == "terrain.paint") {
        math::Vector3 center;
        if (!ReadVec3(payload, "position", center)) {
            err = Outcome::Err("BAD_ARG", "position ([x,y,z] ワールド座標) が必要です"); return nullptr;
        }
        TerrainBrush brush;
        std::string brushError;
        if (!ReadTerrainBrush(payload, brush, brushError)) { err = Outcome::Err("BAD_ARG", brushError); return nullptr; }
        int iterations = 1;
        if (const JsonValue* v = payload.Find("iterations"); v != nullptr && v->IsNumber())
            iterations = std::clamp(v->AsInt(), 1, 64);

        const std::string restrictTo = StringField(payload, "id");
        std::vector<TerrainHit> hits = CollectTerrainsUnderBrush(*scene, center, brush.radius, restrictTo);
        if (hits.empty()) {
            err = Outcome::Err("NO_TERRAIN", restrictTo.empty()
                ? "その位置に重なる TerrainComponent がありません"
                : "指定ノードの Terrain はブラシ範囲と重なりません: " + restrictTo);
            return nullptr;
        }

        const bool isSculpt = (type == "terrain.sculpt");
        TerrainSculptOp sculptOp = TerrainSculptOp::Raise;
        float flattenTarget = 0.0f;
        int paintLayer = 0;
        if (isSculpt) {
            const std::string op = LowerAscii(StringField(payload, "op"));
            if (op.empty() || op == "raise")   sculptOp = TerrainSculptOp::Raise;
            else if (op == "lower")            sculptOp = TerrainSculptOp::Lower;
            else if (op == "smooth")           sculptOp = TerrainSculptOp::Smooth;
            else if (op == "flatten")          sculptOp = TerrainSculptOp::Flatten;
            else if (op == "stamp")            sculptOp = TerrainSculptOp::Stamp;
            else if (op == "noise")            sculptOp = TerrainSculptOp::Noise;
            else if (op == "thermalerosion")   sculptOp = TerrainSculptOp::ThermalErosion;
            else if (op == "hydraulicerosion") sculptOp = TerrainSculptOp::HydraulicErosion;
            else if (op == "terrace")          sculptOp = TerrainSculptOp::Terrace;
            else {
                err = Outcome::Err("BAD_ARG",
                    "op は raise/lower/smooth/flatten/stamp/noise/thermalErosion/hydraulicErosion/terrace です");
                return nullptr;
            }
            std::string paramError;
            if (!ReadTerrainSculptParams(payload, brush, paramError)) { err = Outcome::Err("BAD_ARG", paramError); return nullptr; }
            if (sculptOp == TerrainSculptOp::Flatten) {
                /// @note 対話ツールは最初にクリックした高さを基準にする。AI にはクリックが無いため、明示指定が無ければブラシ中心の現在高さを基準にする (同じ意味論)。
                if (const JsonValue* v = payload.Find("targetHeight"); v != nullptr && v->IsNumber()) {
                    flattenTarget = static_cast<float>(v->AsNumber());
                } else {
                    const TerrainHit& primary = hits.front();
                    flattenTarget = primary.terrain->GetHeightAt(primary.local.x, primary.local.z);
                }
            }
        } else {
            const JsonValue* layerValue = payload.Find("layer");
            /// @note 層数は Terrain ごとに可変。番号の範囲は基準になる先頭の Terrain で決める。
            const int layerCount = hits.front().terrain->LayerCount();
            if (layerCount <= 0) {
                err = Outcome::Err("NO_LAYER", "Terrain に層がありません。terrain_set_layer_material で layer=0 を追加してください");
                return nullptr;
            }
            const std::string layerRange = "0〜" + std::to_string(layerCount - 1);
            if (layerValue == nullptr || !layerValue->IsNumber()) {
                err = Outcome::Err("BAD_ARG", "layer (" + layerRange + ") が必要です"); return nullptr;
            }
            paintLayer = layerValue->AsInt();
            if (paintLayer < 0 || paintLayer >= layerCount) {
                err = Outcome::Err("BAD_ARG", "layer は " + layerRange + " です (Terrain " + hits.front().go->instanceId
                    + " の層数 " + std::to_string(layerCount) + ")");
                return nullptr;
            }
        }

        /// @note Paint は Terrain ごとに塗る層を解決する。隣接 Terrain は layerMaterials の並びが異なり得るため、同じ index を塗ると別マテリアルへ塗ってしまう (TerrainTool と同じ規則)。
        const std::string sourceMaterial = isSculpt ? std::string{}
            : hits.front().terrain->layerMaterials[static_cast<size_t>(paintLayer)];

        /// @note before/after を先に作り、コマンドはスナップショットの入れ替えだけを行う。
        std::vector<TerrainSnapshot> before;
        std::vector<TerrainSnapshot> after;
        std::vector<std::string> touched;
        before.reserve(hits.size());
        after.reserve(hits.size());
        for (const TerrainHit& hit : hits) {
            int layerForThisTerrain = paintLayer;
            if (!isSculpt && hit.terrain != hits.front().terrain) {
                layerForThisTerrain = ResolvePaintLayerForTerrain(*hit.terrain, sourceMaterial, paintLayer);
                /// @note 対応する層が無い Terrain には塗らない。
                if (layerForThisTerrain < 0) continue;
            }
            before.push_back({ hit.go->instanceId, *hit.terrain });
            scene::TerrainComponent edited = *hit.terrain;
            /// @note 反復番号を strokeStep に渡す。侵食の乱数が人の長押しと同じ列になる。
            for (int i = 0; i < iterations; ++i) {
                if (isSculpt) ApplyTerrainSculpt(edited, hit.local, brush, sculptOp, flattenTarget, 1.0f,
                                                 static_cast<std::uint32_t>(i));
                else          ApplyTerrainPaint(edited, hit.local, brush, layerForThisTerrain, 1.0f);
            }
            after.push_back({ hit.go->instanceId, std::move(edited) });
            touched.push_back(hit.go->instanceId);
        }
        if (touched.empty()) {
            err = Outcome::Err("NO_TERRAIN", "塗る対象のレイヤーを持つ Terrain がありません");
            return nullptr;
        }

        if (detailSink != nullptr) {
            JsonValue terrainIds = JsonValue::MakeArray();
            for (const std::string& touchedId : touched) terrainIds.Push(JsonValue(touchedId));
            detailSink->Set("terrains", std::move(terrainIds));
            detailSink->Set("iterations", JsonValue(iterations));
            if (isSculpt && sculptOp == TerrainSculptOp::Flatten)
                detailSink->Set("targetHeight", JsonValue(flattenTarget));
        }
        return MakeTerrainEditCommand(scene, isSculpt ? "AI: Sculpt Terrain" : "AI: Paint Terrain",
                                      std::move(before), std::move(after), markDirty);
    }

    /// @name Terrain: 坂
    /// @note 人の Ramp ツールは «押した点から離した点まで» の 1 ストロークで 1 回だけ当てる。AI も両端を渡して 1 回で済ませる。
    /// @see Docs/design/terrain-layers.md
    if (type == "terrain.ramp") {
        math::Vector3 start;
        math::Vector3 end;
        if (!ReadVec3(payload, "start", start) || !ReadVec3(payload, "end", end)) {
            err = Outcome::Err("BAD_ARG", "start と end ([x,y,z] ワールド座標) が必要です"); return nullptr;
        }
        TerrainBrush brush;
        std::string brushError;
        if (!ReadTerrainBrush(payload, brush, brushError)) { err = Outcome::Err("BAD_ARG", brushError); return nullptr; }
        const math::Vector3 delta = end - start;
        const float horizontalLength = std::sqrt(delta.x * delta.x + delta.z * delta.z);
        if (!(horizontalLength > 0.001f)) {
            err = Outcome::Err("BAD_ARG", "start と end が水平方向に同じ位置です (坂の向きが決まりません)"); return nullptr;
        }
        if (horizontalLength > 5000.0f) {
            err = Outcome::Err("BAD_ARG", "start と end の水平距離は 5000 m 以下で指定してください"); return nullptr;
        }

        /// @note 線分を包む円 (中点 + 半長 + radius) で重なる Terrain を拾う。多めに拾っても坂の外は変わらない。
        const math::Vector3 middle = (start + end) * 0.5f;
        const std::string restrictTo = StringField(payload, "id");
        std::vector<TerrainHit> hits =
            CollectTerrainsUnderBrush(*scene, middle, horizontalLength * 0.5f + brush.radius, restrictTo);
        if (hits.empty()) {
            err = Outcome::Err("NO_TERRAIN", restrictTo.empty()
                ? "坂の範囲に重なる TerrainComponent がありません"
                : "指定ノードの Terrain は坂の範囲と重なりません: " + restrictTo);
            return nullptr;
        }

        std::vector<TerrainSnapshot> before;
        std::vector<TerrainSnapshot> after;
        JsonValue terrainIds = JsonValue::MakeArray();
        for (const TerrainHit& hit : hits) {
            before.push_back({ hit.go->instanceId, *hit.terrain });
            scene::TerrainComponent edited = *hit.terrain;
            ApplyTerrainRamp(edited,
                             ToTerrainLocal(hit.go->transform, start),
                             ToTerrainLocal(hit.go->transform, end),
                             brush);
            after.push_back({ hit.go->instanceId, std::move(edited) });
            terrainIds.Push(JsonValue(hit.go->instanceId));
        }
        if (detailSink != nullptr) {
            detailSink->Set("terrains", std::move(terrainIds));
            detailSink->Set("length", JsonValue(horizontalLength));
        }
        return MakeTerrainEditCommand(scene, "AI: Ramp Terrain", std::move(before), std::move(after), markDirty);
    }

    /// @name Terrain: 穴
    /// @note 穴は三角形を作らないことで表すので、描画・物理・NavMesh が同じ holeData を読む。掘った後は navmesh.bake が要る。
    if (type == "terrain.hole") {
        math::Vector3 center;
        if (!ReadVec3(payload, "position", center)) {
            err = Outcome::Err("BAD_ARG", "position ([x,y,z] ワールド座標) が必要です"); return nullptr;
        }
        TerrainBrush brush;
        if (const JsonValue* v = payload.Find("radius"); v != nullptr && v->IsNumber())
            brush.radius = static_cast<float>(v->AsNumber());
        if (!(brush.radius > 0.0f) || brush.radius > 500.0f) {
            err = Outcome::Err("BAD_ARG", "radius は 0 より大きく 500 以下で指定してください"); return nullptr;
        }
        bool erase = false;
        if (const JsonValue* v = payload.Find("erase"); v != nullptr) {
            if (!v->IsBool()) { err = Outcome::Err("BAD_ARG", "erase は true / false で指定してください"); return nullptr; }
            erase = v->AsBool();
        }

        const std::string restrictTo = StringField(payload, "id");
        std::vector<TerrainHit> hits = CollectTerrainsUnderBrush(*scene, center, brush.radius, restrictTo);
        if (hits.empty()) {
            err = Outcome::Err("NO_TERRAIN", restrictTo.empty()
                ? "その位置に重なる TerrainComponent がありません"
                : "指定ノードの Terrain はブラシ範囲と重なりません: " + restrictTo);
            return nullptr;
        }

        std::vector<TerrainSnapshot> before;
        std::vector<TerrainSnapshot> after;
        JsonValue terrainIds = JsonValue::MakeArray();
        for (const TerrainHit& hit : hits) {
            scene::TerrainComponent edited = *hit.terrain;
            if (!ApplyTerrainHole(edited, hit.local, brush, !erase)) continue;
            before.push_back({ hit.go->instanceId, *hit.terrain });
            after.push_back({ hit.go->instanceId, std::move(edited) });
            terrainIds.Push(JsonValue(hit.go->instanceId));
        }
        if (after.empty()) {
            err = Outcome::Err("NO_CHANGE", erase
                ? "ブラシ範囲に消せる穴がありません (セル中心が円に入るセルだけが対象です)"
                : "ブラシ範囲に新しく穴を開けるセルがありません (既に穴か、radius がセルより小さい)");
            return nullptr;
        }
        if (detailSink != nullptr) {
            detailSink->Set("terrains", std::move(terrainIds));
            detailSink->Set("erase", JsonValue(erase));
        }
        return MakeTerrainEditCommand(scene, erase ? "AI: Fill Terrain Holes" : "AI: Cut Terrain Holes",
                                      std::move(before), std::move(after), markDirty);
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildTerrainLayerMaterialCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    /// @note Terrain レイヤーへ .mat を割り当てる。Paint 前にレイヤーの中身を決めないと「塗ったのに見た目が変わらない (レイヤーが空)」になる。
    if (type == "terrain.setLayerMaterial") {
        const std::string id = StringField(payload, "id");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        auto* terrain = go->GetComponent<scene::TerrainComponent>();
        if (terrain == nullptr) { err = Outcome::Err("NOT_PRESENT", "TerrainComponent が装着されていません"); return nullptr; }
        /// @note layer == 層数 は末尾への追加。Undo は追加した層を消す (差し替えなら旧 .mat へ戻す)。
        const int layerCount = terrain->LayerCount();
        const std::string layerRange = "0〜" + std::to_string(layerCount);
        const JsonValue* layerValue = payload.Find("layer");
        if (layerValue == nullptr || !layerValue->IsNumber()) {
            err = Outcome::Err("BAD_ARG", "layer (" + layerRange + "。" + std::to_string(layerCount) + " で末尾に追加) が必要です");
            return nullptr;
        }
        const int layer = layerValue->AsInt();
        if (layer < 0 || layer > layerCount) {
            err = Outcome::Err("BAD_ARG", "layer は " + layerRange + " です (" + std::to_string(layerCount) + " で末尾に追加)");
            return nullptr;
        }
        const bool appends = (layer == layerCount);
        if (appends && layerCount >= scene::TERRAIN_MAX_LAYERS) {
            err = Outcome::Err("LAYER_LIMIT", "層数が上限 (" + std::to_string(scene::TERRAIN_MAX_LAYERS) + ") に達しています");
            return nullptr;
        }
        const std::string materialPath = StringField(payload, "material");
        if (!materialPath.empty()) {
            std::filesystem::path resolved;
            std::string relative;
            if (!ResolveProjectFile(ctx, materialPath, resolved, relative)
                || LowerAscii(resolved.extension().string()) != ".mat") {
                err = Outcome::Err("BAD_PATH", "material は projectRoot 配下の .mat で指定してください"); return nullptr;
            }
            std::error_code ec;
            if (!std::filesystem::is_regular_file(resolved, ec)) {
                err = Outcome::Err("MATERIAL_NOT_FOUND", "マテリアルが見つかりません: " + relative); return nullptr;
            }
        }
        const std::string oldMaterial = appends ? std::string{} : terrain->layerMaterials[static_cast<size_t>(layer)];
        if (detailSink != nullptr) {
            detailSink->Set("appended", JsonValue(appends));
            detailSink->Set("layerCount", JsonValue(appends ? layerCount + 1 : layerCount));
        }
        return std::make_unique<LambdaCommand>(appends ? "AI: Add Terrain Layer" : "AI: Set Terrain Layer Material",
            [scene, id, layer, materialPath, markDirty]() {
                if (GameObject* g = scene->FindByGuid(id))
                    if (auto* t = g->GetComponent<scene::TerrainComponent>()) t->SetLayerMaterial(layer, materialPath);
                markDirty();
            },
            [scene, id, layer, appends, oldMaterial, markDirty]() {
                if (GameObject* g = scene->FindByGuid(id)) {
                    if (auto* t = g->GetComponent<scene::TerrainComponent>()) {
                        /// @note 追加の取り消しは «末尾がまだこの層» のときだけ消す。後続の編集で並びが変わっていたら触らない。
                        if (!appends)                              t->SetLayerMaterial(layer, oldMaterial);
                        else if (t->LayerCount() == layer + 1)     t->RemoveLayer(layer);
                    }
                }
                markDirty();
            });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildNavMeshBakeCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    /// @name NavMesh: 再ベイク要求
    /// @note 地形を彫った直後の NavMesh は古い形のままで、経路を引くと壁を通り抜ける経路が返る。sculpt の後は必ずこれを通す運用にする。
    if (type == "navmesh.bake") {
        const std::string id = StringField(payload, "id");
        std::vector<std::string> targets;
        for (scene::EntityID eid : scene->GetEntities<scene::NavMeshSurfaceComponent>()) {
            GameObject* surfaceGo = scene->GetGameObject(eid);
            auto* surface = scene->GetComponent<scene::NavMeshSurfaceComponent>(eid);
            if (surfaceGo == nullptr || surface == nullptr) continue;
            if (!id.empty() && surfaceGo->instanceId != id) continue;
            if (id.empty() && !surface->enabled) continue;
            targets.push_back(surfaceGo->instanceId);
        }
        if (targets.empty()) {
            err = Outcome::Err("NO_NAVMESH_SURFACE", id.empty()
                ? "有効な NavMeshSurfaceComponent がシーンにありません"
                : "NavMeshSurfaceComponent が見つかりません: " + id);
            return nullptr;
        }
        if (detailSink != nullptr) {
            JsonValue surfaceIds = JsonValue::MakeArray();
            for (const std::string& target : targets) surfaceIds.Push(JsonValue(target));
            detailSink->Set("surfaces", std::move(surfaceIds));
            /// @note ベイクはバックグラウンドスレッドで走る。完了は navmesh_get_state で確認させる。
            detailSink->Set("async", JsonValue(true));
            detailSink->Set("poll", JsonValue("navmesh_get_state で bakeState=done を確認してください"));
        }
        auto targetsShared = std::make_shared<std::vector<std::string>>(std::move(targets));
        /// @note Undo はベイク要求を取り消せない (結果は Terrain/Collider から再生成されるキャッシュでシーンにも保存されない)。履歴に残すのは AI が操作列を追えるようにするため。
        auto request = [scene, targetsShared]() {
            for (const std::string& target : *targetsShared) {
                if (GameObject* g = scene->FindByGuid(target))
                    if (auto* surface = g->GetComponent<scene::NavMeshSurfaceComponent>()) surface->needsBake = true;
            }
        };
        return std::make_unique<LambdaCommand>("AI: Bake NavMesh", request, request);
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildAudioControlCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    /// @name Audio: 再生制御
    /// @note AudioSource は pending フラグを立てると AudioSystem が次フレームに実行する。Undo 可能にするのは run_transaction で束ねたときも戻せるようにするため。
    if (type == "audio.control") {
        const std::string id = StringField(payload, "id");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        auto* source = go->GetComponent<scene::AudioSourceComponent>();
        if (source == nullptr) { err = Outcome::Err("NOT_PRESENT", "AudioSourceComponent が装着されていません"); return nullptr; }
        const std::string action = LowerAscii(StringField(payload, "action"));
        if (action != "play" && action != "stop" && action != "pause" && action != "resume") {
            err = Outcome::Err("BAD_ARG", "action は play / stop / pause / resume です"); return nullptr;
        }
        if (action == "play" && source->clipPath.empty()) {
            err = Outcome::Err("NO_CLIP", "clipPath が空です (component_set で設定してください)"); return nullptr;
        }
        auto request = [scene, id](const std::string& requestedAction) {
            GameObject* g = scene->FindByGuid(id);
            if (g == nullptr) return;
            auto* audio = g->GetComponent<scene::AudioSourceComponent>();
            if (audio == nullptr) return;
            if (requestedAction == "play")        audio->m_pendingPlay = true;
            else if (requestedAction == "stop")   audio->m_pendingStop = true;
            else if (requestedAction == "pause")  audio->m_pendingPause = true;
            else if (requestedAction == "resume") audio->m_pendingPlay = true;
        };
        /// @note 取り消し方向は再生なら停止・それ以外は再生。元の再生状態へ戻す。
        const std::string undoAction = (action == "play" || action == "resume") ? "stop"
            : (source->m_isPlaying ? "play" : "stop");
        return std::make_unique<LambdaCommand>("AI: Audio Control",
            [request, action]()     { request(action); },
            [request, undoAction]() { request(undoAction); });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}
} /// namespace

void RegisterWorldHandlers(BusHandlerTable& table)
{
    table.AddQuery("terrain.inspect", [](BusCall& call) { return DoTerrainInspect(call.ctx, call.payload); });
    table.AddQuery("terrain.sample", [](BusCall& call) { return DoTerrainSample(call.ctx, call.payload); });
    table.AddQuery("navmesh.state", [](BusCall& call) { return DoNavMeshState(call.ctx, call.payload); });
    table.AddQuery("navmesh.path", [](BusCall& call) { return DoNavMeshPath(call.ctx, call.payload); });
    table.AddQuery("navmesh.sample", [](BusCall& call) { return DoNavMeshSample(call.ctx, call.payload); });
    table.AddQuery("environment.inspect", [](BusCall& call) { return DoEnvironmentInspect(call.ctx); });
    table.AddQuery("audio.inspect", [](BusCall& call) { return DoAudioInspect(call.ctx, call.payload); });
    table.AddQuery("ui.inspect", [](BusCall& call) { return DoUIInspect(call.ctx, call.payload); });

    table.AddBuilder("terrain.sculpt", BuildTerrainBrushCommand);
    table.AddBuilder("terrain.paint", BuildTerrainBrushCommand);
    table.AddBuilder("terrain.ramp", BuildTerrainBrushCommand);
    table.AddBuilder("terrain.hole", BuildTerrainBrushCommand);
    table.AddBuilder("terrain.setLayerMaterial", BuildTerrainLayerMaterialCommand);
    table.AddBuilder("navmesh.bake", BuildNavMeshBakeCommand);
    table.AddBuilder("audio.control", BuildAudioControlCommand);
}

} /// namespace fbzz::editor::ai::bus
