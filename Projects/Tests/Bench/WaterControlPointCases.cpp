/// @file    WaterControlPointCases.cpp
/// @brief   Water TargetPoint の同期・解決・snapshot 生成を分離して計時する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include "WaterControlPointCases.hpp"

#include <Engine/Scene/Components/Gameplay/TargetPointComponent.hpp>
#include <Engine/Scene/Components/Water/WaterComponent.hpp>
#include <Engine/Scene/Components/Water/WaterZoneComponent.hpp>
#include <Engine/Scene/ControlPoints/ControlPointAdapters.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/Transform/TransformSystem.hpp>
#include <Engine/Scene/Systems/Water/WaterSystem.hpp>
#include <Engine/Scene/Water/Surface/WaterZoneSnapshot.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::bench {
namespace {

constexpr std::array<std::size_t, 4> WORKSET_SIZES{10, 100, 1000, 10000};
constexpr std::size_t MAX_POINTS_PER_WATER = scene::WATER_SPLINE_MAX_CONTROL_POINTS;
constexpr std::size_t MAX_WATERS_PER_ZONE = scene::WATER_ZONE_MAX_BODIES;
constexpr float TARGET_MOVE_DISTANCE = 0.01f;
volatile std::uint64_t g_waterMeasureSink = 0;

enum class Motion : std::uint8_t {
    NONE,
    ONE_POINT,
    ALL_POINTS,
};

struct Scenario {
    const char* name; ///< 各計測結果に付けるシナリオ名。
    scene::ControlPointSourceMode sourceMode; ///< 水面が制御点を取得する経路。
    scene::ControlPointRuntimeMode runtimeMode; ///< 実行時に変換を反映するか事前に焼き込むかの設定。
    Motion motion; ///< 計測ごとに移動させる制御点の範囲。
};

constexpr std::array<Scenario, 7> SCENARIOS{{
    {"inline-static", scene::ControlPointSourceMode::INLINE,
        scene::ControlPointRuntimeMode::LIVE, Motion::NONE},
    {"live-static", scene::ControlPointSourceMode::POINT_OBJECTS,
        scene::ControlPointRuntimeMode::LIVE, Motion::NONE},
    {"live-move-one", scene::ControlPointSourceMode::POINT_OBJECTS,
        scene::ControlPointRuntimeMode::LIVE, Motion::ONE_POINT},
    {"live-move-all", scene::ControlPointSourceMode::POINT_OBJECTS,
        scene::ControlPointRuntimeMode::LIVE, Motion::ALL_POINTS},
    {"baked-static", scene::ControlPointSourceMode::POINT_OBJECTS,
        scene::ControlPointRuntimeMode::BAKED_STATIC, Motion::NONE},
    {"baked-move-one", scene::ControlPointSourceMode::POINT_OBJECTS,
        scene::ControlPointRuntimeMode::BAKED_STATIC, Motion::ONE_POINT},
    {"baked-move-all", scene::ControlPointSourceMode::POINT_OBJECTS,
        scene::ControlPointRuntimeMode::BAKED_STATIC, Motion::ALL_POINTS},
}};

struct PointTarget {
    scene::EntityID entity{}; ///< 制御点として参照する TargetPoint の識別子。
    math::Vector3 localPosition = math::Vector3::ZERO; ///< 親の Water オブジェクトを基準とする初期座標。
};

struct WaterOwner {
    scene::EntityID entity{}; ///< 水面コンポーネントを持つオブジェクトの識別子。
    std::vector<scene::WaterSplinePoint> inlinePoints; ///< INLINE シナリオで使うオブジェクトローカルの制御点列。
};

struct ZoneGroup {
    scene::EntityID owner{}; ///< WaterZone コンポーネントと最初の水面を持つオブジェクトの識別子。
    std::size_t firstOwner = 0; ///< このゾーンに含む最初の水面の Workset::owners 内の添字。
    std::size_t ownerCount = 0; ///< ゾーンの所有元を含む水面の数。
};

struct Workset {
    scene::Scene world; ///< 計測対象の水面と制御点を所有するシーン。
    std::vector<WaterOwner> owners; ///< 計測対象の水面ごとのオブジェクト情報。
    std::vector<PointTarget> targets; ///< 移動・復元操作に使う全制御点の配列。
    std::vector<std::vector<scene::WaterSplinePoint>> resolvedPoints; ///< owners の添字で引く実行時の制御点列。
    std::vector<scene::WaterSplineSnapshot> meshScratch; ///< owners の添字で引く生成済みの水面メッシュ。
    std::vector<ZoneGroup> zones; ///< 連続した水面の範囲をまとめてメッシュ生成するゾーン。
};

void Consume(std::uint64_t value)
{
    g_waterMeasureSink = static_cast<std::uint64_t>(g_waterMeasureSink) ^ value;
}

std::size_t OwnerCountFor(std::size_t pointCount)
{
    return (std::max)(std::size_t{2}, (pointCount + MAX_POINTS_PER_WATER - 1) / MAX_POINTS_PER_WATER);
}

bool CreateWorkset(std::size_t pointCount, Workset& workset)
{
    // 制御点数の上限を守りつつ、余りを先頭の水面から 1 点ずつ配分する。
    const std::size_t ownerCount = OwnerCountFor(pointCount);
    const std::size_t basePoints = pointCount / ownerCount;
    const std::size_t remainder = pointCount % ownerCount;
    workset.owners.reserve(ownerCount);
    workset.targets.reserve(pointCount);
    workset.resolvedPoints.resize(ownerCount);
    workset.meshScratch.resize(ownerCount);

    for (std::size_t ownerIndex = 0; ownerIndex < ownerCount; ++ownerIndex) {
        const std::size_t ownerPoints = basePoints + (ownerIndex < remainder ? 1u : 0u);
        if (ownerPoints < 2 || ownerPoints > MAX_POINTS_PER_WATER) {
            return false;
        }
        scene::GameObject& owner = workset.world.CreateGameObject("Water Control Owner");
        const scene::EntityID ownerID = owner.GetID();
        owner.transform.position = {
            static_cast<float>((ownerIndex % 8) * 6), 0.0f,
            static_cast<float>((ownerIndex / 8) * 6)};
        scene::WaterComponent water;
        water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
        water.splineSamplesPerSegment = 1;
        water.splineMeshCellSize = 2.0f;
        water.buoyancyDepth = 5.0f;
        workset.world.AddComponent<scene::WaterComponent>(owner.GetID(), std::move(water));
        WaterOwner ownerEntry;
        ownerEntry.entity = ownerID;
        ownerEntry.inlinePoints.reserve(ownerPoints);

        // 同じ初期配置を INLINE と TargetPoint の両方で再現できるようにする。
        for (std::size_t pointIndex = 0; pointIndex < ownerPoints; ++pointIndex) {
            const float normalizedPointIndex = ownerPoints == 1 ? 0.0f
                : static_cast<float>(pointIndex) / static_cast<float>(ownerPoints - 1);
            const math::Vector3 localPosition{-1.0f + 2.0f * normalizedPointIndex, 0.0f, 0.0f};
            scene::WaterSplinePoint point;
            point.id = static_cast<std::uint32_t>(pointIndex + 1);
            point.localPosition = localPosition;
            point.width = 1.5f;
            point.depth = 2.0f;
            point.flowSpeed = 0.75f;
            ownerEntry.inlinePoints.push_back(point);

            scene::GameObject& target = workset.world.CreateGameObject("Target Point");
            scene::GameObject* ownerGameObject = workset.world.GetGameObject(ownerID);
            if (!ownerGameObject || !target.SetParent(ownerGameObject)) {
                return false;
            }
            target.transform.position = localPosition;
            target.AddComponent<scene::TargetPointComponent>();
            workset.targets.push_back({target.GetID(), localPosition});

            scene::WaterControlPointBinding binding;
            binding.id = point.id;
            binding.target.guid = target.instanceId;
            binding.target.id = target.GetID();
            binding.target.sceneGeneration = workset.world.GetRenderSceneGeneration();
            binding.width = point.width;
            binding.depth = point.depth;
            binding.flowSpeed = point.flowSpeed;
            scene::WaterComponent* ownerWater = workset.world.GetComponent<scene::WaterComponent>(ownerID);
            if (!ownerWater) {
                return false;
            }
            ownerWater->pointBindings.push_back(std::move(binding));
        }
        workset.owners.push_back(std::move(ownerEntry));
    }

    // 同じ水面をゾーン単位の生成にも使う。グループの先頭の水面をゾーン所有元にする。
    for (std::size_t firstOwner = 0; firstOwner < ownerCount; firstOwner += MAX_WATERS_PER_ZONE) {
        const std::size_t groupCount = (std::min)(MAX_WATERS_PER_ZONE, ownerCount - firstOwner);
        if (groupCount < 2) {
            return false;
        }
        ZoneGroup group{workset.owners[firstOwner].entity, firstOwner, groupCount};
        scene::WaterZoneComponent zone;
        zone.meshCellSize = 2.0f;
        for (std::size_t memberIndex = 1; memberIndex < groupCount; ++memberIndex) {
            scene::WaterZoneMember member;
            member.water.id = workset.owners[firstOwner + memberIndex].entity;
            zone.members.push_back(member);
        }
        workset.world.AddComponent<scene::WaterZoneComponent>(group.owner, std::move(zone));
        workset.zones.push_back(group);
    }

    scene::FlushWorldTransforms(workset.world);
    return true;
}

void SetTargetMotion(Workset& workset, Motion motion, std::uint64_t iteration)
{
    const float direction = (iteration & 1u) == 0 ? 1.0f : -1.0f;
    std::size_t moveCount = 0;
    if (motion == Motion::ONE_POINT) {
        moveCount = 1;
    } else if (motion == Motion::ALL_POINTS) {
        moveCount = workset.targets.size();
    }

    for (std::size_t index = 0; index < moveCount; ++index) {
        scene::GameObject* target = workset.world.GetGameObject(workset.targets[index].entity);
        if (!target) {
            continue;
        }
        math::Vector3 position = workset.targets[index].localPosition;
        position.y += direction * TARGET_MOVE_DISTANCE;
        target->transform.position = position;
    }
}

void RestoreTargetTransforms(Workset& workset)
{
    for (const PointTarget& point : workset.targets) {
        scene::GameObject* target = workset.world.GetGameObject(point.entity);
        if (target) {
            target->transform.position = point.localPosition;
        }
    }
}

bool ConfigureScenario(Workset& workset, const Scenario& scenario)
{
    // 前のシナリオの移動を戻し、現在の設定に依存するスナップショットを破棄する。
    RestoreTargetTransforms(workset);
    scene::FlushWorldTransforms(workset.world);
    for (std::size_t ownerIndex = 0; ownerIndex < workset.owners.size(); ++ownerIndex) {
        scene::WaterComponent* water = workset.world.GetComponent<scene::WaterComponent>(
            workset.owners[ownerIndex].entity);
        if (!water) {
            return false;
        }
        water->pointSourceMode = scenario.sourceMode;
        water->pointRuntimeMode = scenario.runtimeMode;
        water->simulationReadEnabled = true;
        water->surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
        if (scenario.sourceMode == scene::ControlPointSourceMode::INLINE) {
            water->splinePoints = workset.owners[ownerIndex].inlinePoints;
        } else {
            water->splinePoints.clear();
        }
        water->bakeValid = false;
        water->splineSnapshot.reset();
        water->splineSnapshotAttempted = false;
        water->splineSnapshotAttemptSignature = 0;
    }

    // 焼き込みを要求するシナリオでは、計時の前に実行時が参照するデータを用意する。
    if (scenario.runtimeMode == scene::ControlPointRuntimeMode::BAKED_STATIC) {
        for (const WaterOwner& owner : workset.owners) {
            scene::WaterComponent* water = workset.world.GetComponent<scene::WaterComponent>(owner.entity);
            if (!water) {
                return false;
            }
            const scene::WaterControlPointSnapshot authoring = scene::ResolveWaterControlPointAuthoring(
                workset.world, owner.entity, *water, scene::ControlPointRuntimeMode::BAKED_STATIC);
            if (!authoring.valid) {
                return false;
            }
            water->bakedSplinePoints = authoring.points;
            water->bakedInputSignature = authoring.inputSignature;
            water->bakedSourceMode = water->pointSourceMode;
            water->bakedSurfaceShape = authoring.surfaceShape;
            water->bakedLakePlaneHeight = authoring.lakePlaneHeight;
            water->bakedAdapterVersion = scene::CONTROL_POINT_ADAPTER_VERSION;
            water->bakeValid = true;
        }
    }
    return true;
}

bool ResolveAll(Workset& workset)
{
    std::uint64_t checksum = 14695981039346656037ull;
    for (std::size_t ownerIndex = 0; ownerIndex < workset.owners.size(); ++ownerIndex) {
        const WaterOwner& owner = workset.owners[ownerIndex];
        scene::WaterComponent* water = workset.world.GetComponent<scene::WaterComponent>(owner.entity);
        if (!water) {
            return false;
        }
        scene::WaterControlPointSnapshot snapshot = scene::ResolveWaterControlPoints(
            workset.world, owner.entity, *water, scene::ControlPointReadContext::RUNTIME);
        if (!snapshot.valid) {
            return false;
        }
        checksum = (checksum ^ snapshot.inputSignature) * 1099511628211ull;
        checksum = (checksum ^ static_cast<std::uint64_t>(snapshot.points.size())) * 1099511628211ull;
        workset.resolvedPoints[ownerIndex] = std::move(snapshot.points);
    }
    Consume(checksum);
    return true;
}

bool BuildMeshes(Workset& workset)
{
    std::uint64_t checksum = 0;
    for (std::size_t ownerIndex = 0; ownerIndex < workset.owners.size(); ++ownerIndex) {
        const WaterOwner& owner = workset.owners[ownerIndex];
        const scene::WaterComponent* water = workset.world.GetComponent<scene::WaterComponent>(owner.entity);
        const scene::GameObject* gameObject = workset.world.GetGameObject(owner.entity);
        if (!water || !gameObject || workset.resolvedPoints[ownerIndex].empty()) {
            return false;
        }
        if (!scene::BuildWaterSplineSnapshot(water->surfaceShape, workset.resolvedPoints[ownerIndex],
                water->splineSamplesPerSegment, gameObject->transform, workset.meshScratch[ownerIndex],
                water->buoyancyDepth, water->splineMeshCellSize)) {
            return false;
        }
        checksum ^= workset.meshScratch[ownerIndex].signature
            + static_cast<std::uint64_t>(workset.meshScratch[ownerIndex].vertices.size());
    }
    Consume(checksum);
    return true;
}

template<typename Setup, typename Operation>
bool MeasurePhase(WaterControlPointMeasureResult& result, int warmupIterations, int repeats,
    Setup setup, Operation operation)
{
    for (int iteration = 0; iteration < warmupIterations + repeats; ++iteration) {
        // 入力の準備とウォームアップは計測値に含めない。失敗判定は測定する反復と同じにする。
        if (!setup(static_cast<std::uint64_t>(iteration))) {
            return false;
        }
        std::uint64_t regenerations = 0;
        if (iteration < warmupIterations) {
            if (!operation(regenerations)) {
                return false;
            }
            continue;
        }

        const auto begin = std::chrono::steady_clock::now();
        const bool success = operation(regenerations);
        const auto end = std::chrono::steady_clock::now();
        if (!success) {
            return false;
        }
        result.samples.push_back(std::chrono::duration<double, std::micro>(end - begin).count());
        result.regenerationSamples.push_back(regenerations);
    }
    return true;
}

bool MeasureScenario(Workset& workset, const Scenario& scenario,
    int warmupIterations, int repeats, std::vector<WaterControlPointMeasureResult>& results)
{
    if (!ConfigureScenario(workset, scenario)) {
        return false;
    }
    const std::string prefix = "TargetPoint / " + std::to_string(workset.targets.size())
        + " / " + scenario.name;
    const auto add = [&](const char* phase) -> WaterControlPointMeasureResult& {
        results.push_back({});
        WaterControlPointMeasureResult& result = results.back();
        result.name = prefix;
        result.phase = phase;
        result.note = "CPU-only; GPU/UI not measured; STL allocation counts unavailable.";
        return result;
    };

    if (scenario.motion != Motion::NONE) {
        auto& movement = add("target-move");
        std::uint64_t moveIteration = 0;
        if (!MeasurePhase(movement, warmupIterations, repeats,
                [](std::uint64_t) { return true; },
                [&](std::uint64_t& regenerated) {
                    SetTargetMotion(workset, scenario.motion, moveIteration++);
                    regenerated = 0;
                    return true;
                })) return false;
    }

    auto& sync = add("transform-sync");
    if (!MeasurePhase(sync, warmupIterations, repeats,
            [&](std::uint64_t iteration) {
                SetTargetMotion(workset, scenario.motion, iteration);
                return true;
            },
            [&](std::uint64_t&) {
                scene::FlushWorldTransforms(workset.world);
                const PointTarget& target = workset.targets.back();
                const scene::GameObject* go = workset.world.GetGameObject(target.entity);
                if (!go) {
                    return false;
                }
                Consume(std::bit_cast<std::uint32_t>(go->transform.worldPosition.x));
                return true;
            })) return false;

    auto& resolve = add("resolve-signature");
    if (!MeasurePhase(resolve, warmupIterations, repeats,
            [&](std::uint64_t iteration) {
                SetTargetMotion(workset, scenario.motion, iteration);
                scene::FlushWorldTransforms(workset.world);
                return true;
            },
            [&](std::uint64_t&) { return ResolveAll(workset); })) return false;

    auto& mesh = add("mesh-bvh-build");
    if (!MeasurePhase(mesh, warmupIterations, repeats,
            [&](std::uint64_t iteration) {
                SetTargetMotion(workset, scenario.motion, iteration);
                scene::FlushWorldTransforms(workset.world);
                return ResolveAll(workset);
            },
            [&](std::uint64_t& regenerated) {
                if (!BuildMeshes(workset)) {
                    return false;
                }
                regenerated = workset.owners.size();
                return true;
            })) return false;

    auto& prepare = add("surface-prepare");
    if (!MeasurePhase(prepare, warmupIterations, repeats,
            [&](std::uint64_t iteration) {
                SetTargetMotion(workset, scenario.motion, iteration);
                scene::FlushWorldTransforms(workset.world);
                return true;
            },
            [&](std::uint64_t& regenerated) {
                for (const WaterOwner& owner : workset.owners) {
                    scene::WaterComponent* water = workset.world.GetComponent<scene::WaterComponent>(owner.entity);
                    const scene::GameObject* go = workset.world.GetGameObject(owner.entity);
                    if (!water || !go) {
                        return false;
                    }
                    const auto* before = water->splineSnapshot.get();
                    if (!scene::PrepareWaterSplineSnapshot(workset.world, owner.entity, *water, go->transform))
                        return false;
                    regenerated += before != water->splineSnapshot.get() ? 1u : 0u;
                }
                return true;
            })) return false;
    return true;
}

bool MeasureZoneGeneration(Workset& workset, int warmupIterations, int repeats,
    std::vector<WaterControlPointMeasureResult>& results)
{
    Scenario scenario{"live-static", scene::ControlPointSourceMode::POINT_OBJECTS,
        scene::ControlPointRuntimeMode::LIVE, Motion::NONE};
    if (!ConfigureScenario(workset, scenario)) {
        return false;
    }
    scene::PrepareWaterSceneSurfaces(workset.world);
    for (const ZoneGroup& group : workset.zones) {
        const scene::WaterZoneComponent* zone = workset.world.GetComponent<scene::WaterZoneComponent>(group.owner);
        if (!zone || !zone->snapshot) {
            return false;
        }
    }

    WaterControlPointMeasureResult result;
    result.name = "TargetPoint / " + std::to_string(workset.targets.size()) + " / Zone union";
    result.phase = "zone-generation";
    result.note = "CPU-only; GPU/UI not measured; STL allocation counts unavailable.";
    if (!MeasurePhase(result, warmupIterations, repeats,
            [&](std::uint64_t) {
                for (const ZoneGroup& group : workset.zones) {
                    scene::WaterZoneComponent* zone = workset.world.GetComponent<scene::WaterZoneComponent>(group.owner);
                    if (!zone) {
                        return false;
                    }
                    zone->snapshot.reset();
                    zone->attempted = false;
                    zone->attemptSignature = 0;
                    zone->diagnostic.clear();
                }
                return true;
            },
            [&](std::uint64_t& regenerated) {
                scene::PrepareWaterZoneSnapshots(workset.world);
                for (const ZoneGroup& group : workset.zones) {
                    const scene::WaterZoneComponent* zone = workset.world.GetComponent<scene::WaterZoneComponent>(group.owner);
                    if (!zone || !zone->snapshot) {
                        return false;
                    }
                    ++regenerated;
                    Consume(zone->snapshot->geometry.signature
                        + static_cast<std::uint64_t>(zone->snapshot->geometry.vertices.size()));
                }
                return true;
            })) return false;
    results.push_back(std::move(result));
    return true;
}

} // namespace

bool RunWaterControlPointMeasures(int warmupIterations, int repeats,
    std::vector<WaterControlPointMeasureResult>& out)
{
    if (warmupIterations < 0 || repeats <= 0) {
        return false;
    }
    std::vector<WaterControlPointMeasureResult> results;
    const int boundedWarmup = (std::min)(warmupIterations, 2);
    for (const std::size_t pointCount : WORKSET_SIZES) {
        Workset workset;
        if (!CreateWorkset(pointCount, workset)) {
            return false;
        }
        for (const Scenario& scenario : SCENARIOS) {
            if (!MeasureScenario(workset, scenario, boundedWarmup, repeats, results)) {
                return false;
            }
        }
        if (!MeasureZoneGeneration(workset, boundedWarmup, repeats, results)) {
            return false;
        }
    }
    out = std::move(results);
    return true;
}

} // namespace fbzz::bench
