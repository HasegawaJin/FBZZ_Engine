/// @file    AlgorithmOptimizationMeasure.cpp
/// @brief   Measure production functions with fixed CPU-side inputs and external clocks.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Psapi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <Core/Logger.hpp>
#include <Core/Profiler/Profiler.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/WaterZoneComponent.hpp>
#include <Engine/Scene/Systems/RenderSceneExtractor.hpp>
#include <Engine/Scene/Systems/RenderFiberExtractor.hpp>
#include <Engine/Scene/Systems/RenderTerrainExtractor.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Systems/RenderWaterExtractor.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Passes/Geometry/FiberRenderPass.hpp>
#include <Graphics/Passes/Geometry/InstanceRenderPasses.hpp>
#include <Graphics/Passes/Geometry/TerrainRenderPass.hpp>
#include <Graphics/Passes/Geometry/WaterRenderPass.hpp>
#include <Graphics/Pipeline/OcclusionCuller.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/FiberGeometry.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Math/MathUtils.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include "../../Engine/src/Scene/Systems/CanvasGeometryGuard.hpp"
#include "../../Engine/src/Scene/Systems/RenderPasses/Geometry/TerrainPatchIndices.hpp"
#include "../../Graphics/src/Renderer/RenderMeshSpatialIndex.hpp"
#include "../../Graphics/src/Renderer/Platform/DX12/DX12StateTracker.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fbzz::bench {
namespace {
using Clock = std::chrono::steady_clock;
double Us(Clock::time_point begin, Clock::time_point end)
{
    return std::chrono::duration<double, std::micro>(end - begin).count();
}
struct Memory { size_t working = 0, commit = 0; };
Memory ReadMemory()
{
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return {};
    return {counters.WorkingSetSize, counters.PagefileUsage};
}
struct Result {
    std::vector<double> total, first, later;
    uint64_t checksum = 0, probes = 0, candidates = 0, visitedNodes = 0, testedLeaves = 0, retainedBytes = 0;
    uint64_t recorded = 0, dropped = 0, descriptorCount = 0;
    uint64_t operationsPerSample = 1;
    Memory cold, warm, retired;
    bool valid = true;
};
uint64_t GeometryHash(const renderer::RenderScene& value)
{
    uint64_t hash = 14695981039346656037ULL;
    const auto word = [&](uint32_t item) { hash = (hash ^ item) * 1099511628211ULL; };
    const auto scalar = [&](float item) { word(std::bit_cast<uint32_t>(item)); };
    for (const auto& object : value.Objects()) {
        word(object.sourceIndex); word(object.sourceGeneration); word(object.layer);
        word(object.firstItem); word(object.itemCount); word(object.colorEligible); word(object.skinned);
        word(object.castShadows); word(object.lodVisible); scalar(object.lodDither);
        for (const auto* matrix : {&object.world, &object.worldInvTranspose, &object.previousWorld})
            for (const auto& row : matrix->m) for (float entry : row) scalar(entry);
        for (const auto& position : {object.sortPosition, object.boundsCenter}) {
            scalar(position.x); scalar(position.y); scalar(position.z);
        }
        scalar(object.boundsRadius);
    }
    for (const auto& item : value.items) {
        word(item.objectIndex); word(item.materialSlot); word(item.sourceSubmesh); word(item.visible); word(item.slotVisible);
        word(item.vertexBuffer.id); word(item.vertexBuffer.gen); word(item.indexBuffer.id); word(item.indexBuffer.gen);
        word(item.vertexCount); word(item.indexCount); word(item.vertexStride); word(item.vertexPositionOffset);
        scalar(item.boundsCenter.x); scalar(item.boundsCenter.y); scalar(item.boundsCenter.z); scalar(item.boundsRadius);
    }
    return hash;
}
void Append(Result& result, int iteration, int warmup, double total, double first = 0, double later = 0)
{
    if (iteration < warmup) return;
    result.total.push_back(total); result.first.push_back(first); result.later.push_back(later);
}
void Reserve(Result& result, int samples)
{
    result.total.reserve(samples); result.first.reserve(samples); result.later.reserve(samples);
}

/// @note This CPU-only renderer creates no native graphics resources and never acquires a window or cursor.
class CpuRenderer final : public renderer::IRenderer {
public:
    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const renderer::DrawCall&, renderer::ResourceManager&) override {}
    void Dispatch(const renderer::ComputeCall&, renderer::ResourceManager&) override {}
    void Resize(uint32_t, uint32_t) override {}
    uint32_t GetWidth() const override { return 1; }
    uint32_t GetHeight() const override { return 1; }
    void SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>, renderer::ResourceManager&) override {}
    void ClearDepth() override {}
    void SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) override {}
private:
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void*, size_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void*, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t) override { return {}; }
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&, uint32_t, renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t, const renderer::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
};

Result ProfilerCase(const std::string& name, int count, int warmup, int samples)
{
    Result result; Reserve(result, samples);
    profiler::Profiler::SetEnabled(false);
    profiler::Profiler::SetEnabled(name != "disabled");
    const int depth = name == "nested" ? 129 : 0;
    const int scopes = name == "cap" ? 8193 : count;
    const int reads = name == "getter2" ? 2 : name == "getter" ? 1 : 0;
    std::vector<std::string> names;
    if (name == "rotation") {
        names.reserve(5000);
        for (int index = 0; index < 5000; ++index) names.push_back("MeasureRotation" + std::to_string(index));
    }
    std::array<profiler::ProfileToken, 129> tokens;
    result.cold = ReadMemory();
    for (int iteration = 0; iteration < warmup + samples; ++iteration) {
        const auto begin = Clock::now();
        profiler::Profiler::BeginFrame();
        if (depth != 0) {
            for (int index = 0; index < depth; ++index)
                tokens[index] = profiler::Profiler::BeginScope({"MeasureNested", "AlgorithmBench"});
            for (int index = depth; index != 0; --index) profiler::Profiler::EndScope(tokens[index - 1]);
        } else {
            for (int index = 0; index < scopes; ++index) {
                const char* marker = names.empty() ? "MeasureRepeated" : names[(iteration * 37 + index) % names.size()].c_str();
                const auto token = profiler::Profiler::BeginScope({marker, "AlgorithmBench"});
                profiler::Profiler::EndScope(token);
            }
        }
        profiler::Profiler::EndFrame();
        const auto published = Clock::now();
        size_t legacyCount = 0;
        for (int read = 0; read < reads; ++read) legacyCount += profiler::Profiler::GetLastFrameRecords().size();
        const auto end = Clock::now();
        Append(result, iteration, warmup, Us(begin, end), Us(begin, published), Us(published, end));
        const auto& snapshot = profiler::Profiler::GetSnapshot();
        result.recorded = snapshot.recordedSampleCount; result.dropped = snapshot.droppedSampleCount;
        result.descriptorCount = snapshot.descriptors.size();
        result.checksum += legacyCount + snapshot.recordedSampleCount + snapshot.droppedSampleCount;
        if (name == "cap" && (snapshot.recordedSampleCount != 8192 || snapshot.droppedSampleCount == 0)) result.valid = false;
        if (name == "nested" && (snapshot.depthDroppedSampleCount == 0 || snapshot.capacityDroppedSampleCount != 0)) result.valid = false;
        if (name == "disabled" && snapshot.recordedSampleCount != 0) result.valid = false;
        if (name != "disabled" && name != "cap" && name != "nested"
            && (snapshot.recordedSampleCount != static_cast<uint32_t>(scopes) || snapshot.droppedSampleCount != 0)) result.valid = false;
        if (reads != 0 && name != "disabled" && legacyCount != static_cast<size_t>(scopes * reads)) result.valid = false;
    }
    result.warm = ReadMemory(); profiler::Profiler::SetEnabled(false); result.retired = ReadMemory();
    return result;
}

/// @note Exact prior RenderSystem guard algorithm, kept local solely as the Before Canvas adapter.
template<typename Probe>
bool CanvasBefore(scene::Scene& scene, Probe&& probe)
{
    for (scene::EntityID id : scene.GetEntities<scene::UICanvas>()) {
        auto* canvas = scene.GetComponent<scene::UICanvas>(id);
        auto* root = scene.GetGameObject(id);
        if (!canvas || !canvas->enabled || !root || !root->activeInHierarchy()) continue;
        std::vector<scene::GameObject*> pending{root};
        while (!pending.empty()) {
            auto* object = pending.back(); pending.pop_back();
            if (probe(*object)) return false;
            for (int index = 0; index < object->GetChildCount(); ++index)
                if (auto* child = object->GetChild(index)) pending.push_back(child);
        }
    }
    return true;
}
Result CanvasCase(const std::string& name, bool before, int count, int warmup, int samples)
{
    Result result; Reserve(result, samples); scene::Scene scene;
    auto& root = scene.CreateGameObject("CanvasRoot");
    if (name != "none") root.AddComponent<scene::UICanvas>();
    auto* tail = &root;
    if (name.starts_with("nested")) {
        for (int index = 1; index < 64; ++index) {
            auto& child = scene.CreateGameObject("NestedCanvas"); child.SetParent(tail);
            child.AddComponent<scene::UICanvas>(); tail = &child;
        }
    }
    for (int index = 0; index < count; ++index) {
        auto& child = scene.CreateGameObject("CanvasLeaf"); child.SetParent(tail);
        if (index == 0 && name == "nested-leaf") child.AddComponent<scene::MeshRenderer>();
    }
    if (name == "single-root") root.AddComponent<scene::MeshRenderer>();
    const auto probe = [](scene::GameObject& object) {
        return object.GetComponent<scene::MeshRenderer>() || object.GetComponent<scene::SkinnedMeshRenderer>();
    };
    const auto counting = [&](scene::GameObject& object) { ++result.probes; return probe(object); };
    const bool expected = CanvasBefore(scene, probe);
    const bool observed = before ? CanvasBefore(scene, counting)
        : scene::canvas_geometry_detail::CanShareCanvasFrameGeometry(scene, counting);
    result.valid = expected == observed;
    const int batch = name == "none" || name == "single-root" ? 4096 : 1;
    result.operationsPerSample = batch;
    result.cold = ReadMemory();
    for (int iteration = 0; iteration < warmup + samples; ++iteration) {
        const auto begin = Clock::now();
        bool actual = expected;
        for (int operation = 0; operation < batch; ++operation) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
            actual = before ? CanvasBefore(scene, probe)
                : scene::canvas_geometry_detail::CanShareCanvasFrameGeometry(scene);
            result.checksum += actual;
        }
        const auto end = Clock::now();
        result.valid = result.valid && actual == expected;
        Append(result, iteration, warmup, Us(begin, end) / batch);
    }
    result.warm = ReadMemory(); scene.Clear(); result.retired = ReadMemory(); return result;
}

Result GeometryCase(const std::string& name, int count, int warmup, int samples)
{
    Result result; Reserve(result, samples); scene::Scene scene; renderer::Mesh mesh; asset::Model model;
    CpuRenderer backend; renderer::ResourceManager resources(backend);
    mesh.vertexBuffer = {11, 2}; mesh.indexBuffer = {12, 3}; mesh.vertexCount = 24; mesh.indexCount = 36;
    mesh.boundsCenter = {}; mesh.boundsRadius = 1; mesh.boundsExtents = {1, 1, 1};
    for (int index = 0; index < count; ++index) {
        auto& object = scene.CreateGameObject("MeasuredStatic");
        object.AddComponent<scene::MeshRenderer>().mesh = &mesh;
        object.AddComponent<scene::MaterialComponent>().enabled = false;
        object.transform.position = object.transform.worldPosition = {static_cast<float>(index % 100), 0, static_cast<float>(index / 100)};
    }
    if (name == "mixed2") {
        auto submesh = std::make_unique<renderer::Mesh>();
        submesh->vertexBuffer = {21, 2}; submesh->indexBuffer = {22, 3}; submesh->vertexCount = 24; submesh->indexCount = 36;
        submesh->boundsRadius = 1; model.meshes.push_back(std::move(submesh));
        auto& object = scene.CreateGameObject("MeasuredSkinned");
        object.AddComponent<scene::SkinnedMeshRenderer>().model = &model;
        object.AddComponent<scene::MaterialComponent>().enabled = false;
    }
    const bool retireFirst = name == "static4-retired";
    const int views = name == "static1" ? 1 : (name == "static4" || retireFirst) ? 4 : 2;
    const bool ray = name == "ray2";
    renderer::Camera camera; renderer::RenderSettings settings; scene::RenderPassHandles handles;
    if (ray) settings.modeRequest.mode = renderer::RenderMode::HYBRID;
    scene::RenderFrameGeometryCache cache;
    std::array<std::shared_ptr<const renderer::RenderScene>, 4> held;
    result.cold = ReadMemory();
    for (int iteration = 0; iteration < warmup + samples; ++iteration) {
        const auto begin = Clock::now();
        ++Time::frameCount; cache = {};
        double first = 0, later = 0;
        for (int view = 0; view < views; ++view) {
            const auto viewBegin = Clock::now();
            scene::RenderPassContext context(scene, backend, resources, camera, settings, {}, UINT32_MAX, handles);
            context.experimentalRayTracingEnabled = ray;
            scene::ExtractRenderScene(context, &cache); held[view] = std::move(context.renderScene);
            if (retireFirst && view == 0) held[view].reset();
            const auto viewEnd = Clock::now();
            if (view == 0) first = Us(viewBegin, viewEnd); else later += Us(viewBegin, viewEnd);
        }
        const auto end = Clock::now();
        Append(result, iteration, warmup, Us(begin, end), first, later);
        for (int view = 0; view < views; ++view) {
            const auto& value = held[view];
            if (retireFirst && view == 0) continue;
            result.valid = result.valid && value && value->Objects().size() == static_cast<size_t>(count + (name == "mixed2"));
            if (value) result.checksum += value->items.size();
        }
    }
    /// @note Hashing is outside every measured interval; this supplements, rather than replaces, the lifetime/reentry regression suites.
    result.valid = result.valid && cache.rayTracingInputs == ray;
    result.checksum = 14695981039346656037ULL;
    for (int view = retireFirst ? 1 : 0; view < views; ++view)
        result.checksum = (result.checksum ^ GeometryHash(*held[view])) * 1099511628211ULL;
    result.retainedBytes = cache.itemGeometry.capacity() * sizeof(scene::RenderFrameGeometryItem);
    result.probes = sizeof(renderer::RenderMeshItem);
    result.candidates = sizeof(scene::RenderFrameGeometryItem);
    result.warm = ReadMemory(); held = {}; cache = {}; scene.Clear(); result.retired = ReadMemory(); return result;
}

Result BvhCase(const std::string& name, int count, int warmup, int samples)
{
    using namespace renderer::spatial_detail;
    Result result; Reserve(result, samples);
    const size_t split = name.find('-');
    const int volumeCount = std::atoi(name.substr(1, split - 1).c_str());
    const std::string layout = name.substr(split + 1);
    if (volumeCount <= 0 || volumeCount > 128) { result.valid = false; return result; }
    std::vector<MeshSource> sources;
    const bool separated = layout == "separated" || layout == "moving";
    for (int index = 0; index < count; ++index) {
        const float x = separated ? (index % volumeCount) * 600.0f : static_cast<float>(index % 100 - 50) * 2;
        sources.push_back({{static_cast<uint32_t>(index), 2, static_cast<uint64_t>(index + 1), false},
            {x, static_cast<float>(index / 100 % 10), separated ? 30.0f : 1000.0f}, 1, index % 7 == 0});
    }
    std::vector<math::Frustum> values;
    values.reserve(volumeCount);
    for (int index = 0; index < volumeCount; ++index) {
        const math::Vector3 position{separated ? index * 600.0f : 0, 0, 0};
        const math::Vector3 forward = layout == "offscreen" ? math::Vector3{0, 0, -1} : math::Vector3{0, 0, 1};
        values.push_back(math::Frustum::FromViewProjection(math::Matrix4::Perspective(math::ToRad(70.0f), 1.2f, 0.1f, separated ? 200.0f : 5000.0f)
            * math::Matrix4::LookAt(position, position + forward, math::Vector3::UP)));
    }
    std::vector<const math::Frustum*> volumes;
    for (const auto& value : values) volumes.push_back(&value);
    IndexOptions options; options.mode = QueryMode::BVH; options.minimumTreeItems = 1;
    options.updateMode = UpdateMode::STABLE; options.denseUpdate = DenseUpdate::REFIT;
    RenderMeshSpatialIndex index; const auto initial = index.Refresh(1, sources, options);
    result.retainedBytes = initial.retainedBytes;
    QueryDiagnostics diagnostics; std::vector<uint32_t> expected, actual;
    index.QueryUnion(volumes, expected, diagnostics, QueryMode::LINEAR);
    index.QueryUnion(volumes, actual, diagnostics, QueryMode::BVH);
    result.valid = expected == actual;
    result.cold = ReadMemory();
    for (int iteration = 0; iteration < warmup + samples; ++iteration) {
        const auto begin = Clock::now();
        if (layout == "moving") {
            const float delta = iteration % 2 == 0 ? 0.125f : -0.125f;
            for (int ordinal = 0; ordinal < count; ordinal += 100) sources[ordinal].center.x += delta;
            (void)index.Refresh(1, sources, options);
        }
        const auto updated = Clock::now();
        index.QueryUnion(volumes, actual, diagnostics, QueryMode::BVH);
        const auto end = Clock::now();
        Append(result, iteration, warmup, Us(begin, end), Us(begin, updated), Us(updated, end));
        result.candidates = actual.size(); result.visitedNodes = diagnostics.visitedNodes; result.testedLeaves = diagnostics.testedLeaves;
        result.checksum += actual.size();
    }
    index.QueryUnion(volumes, expected, diagnostics, QueryMode::LINEAR);
    result.valid = result.valid && expected == actual;
    result.warm = ReadMemory(); index = {}; result.retired = ReadMemory(); return result;
}

void HashWord(uint64_t& hash, uint32_t word)
{
    hash = (hash ^ word) * 1099511628211ull;
}

uint64_t WaterMeshHash(const scene::WaterSplineSnapshot& snapshot)
{
    uint64_t hash = 14695981039346656037ull;
    for (const auto& vertex : snapshot.vertices) {
        for (float value : {vertex.worldPosition.x, vertex.worldPosition.y, vertex.worldPosition.z,
                vertex.depth, vertex.worldVelocity.x, vertex.worldVelocity.z})
            HashWord(hash, std::bit_cast<uint32_t>(value));
    }
    for (const auto* triangles : {&snapshot.footprintTriangles, &snapshot.triangles})
        for (const auto& triangle : *triangles)
            for (uint32_t index : triangle.indices) HashWord(hash, index);
    return hash;
}

Result WaterSplineCase(const std::string& scenario, int count, int warmup, int samples)
{
    Result result;
    const bool steady = scenario == "river-steady";
    const bool river = scenario == "river" || scenario == "river-edit" || steady;
    const bool lake = scenario == "lake" || scenario == "lake-edit";
    if ((!river && !lake) || count < 8 || count > 256) { result.valid = false; return result; }
    const bool edit = scenario.ends_with("-edit");
    const auto shape = river ? scene::WaterSurfaceShape::SPLINE_RIVER : scene::WaterSurfaceShape::SPLINE_LAKE;
    std::vector<scene::WaterSplinePoint> points;
    points.reserve(count);
    for (int i = 0; i < count; ++i) {
        const float angle = math::TWO_PI * static_cast<float>(i) / static_cast<float>(count);
        const float radius = (i % 2 == 0) ? 100.0f : 83.0f;
        const auto position = river ? math::Vector3{12.0f * i, -0.2f * i, 8.0f * std::sin(3.0f * angle)}
            : math::Vector3{radius * std::cos(angle), 0.0f, radius * std::sin(angle)};
        points.push_back({static_cast<uint32_t>(i + 1), position, 5.0f, 3.0f, 1.0f});
    }
    const int repetitions = river ? 16 : 8;
    result.operationsPerSample = repetitions;
    const float originalZ = points[count / 2].localPosition.z;
    const scene::Transform transform;
    scene::WaterComponent water;
    if (steady) {
        water.surfaceShape = shape;
        water.splinePoints = points;
        water.splineSamplesPerSegment = 1;
        water.splineMeshCellSize = 100000.0f;
        if (!scene::PrepareWaterSplineSnapshot(water, transform)) { result.valid = false; return result; }
    }
    Reserve(result, samples);
    for (int i = 0; i < warmup + samples; ++i) {
        if (edit) points[count / 2].localPosition.z = originalZ + (i % 2 == 0 ? 0.25f : -0.25f);
        scene::WaterSplineSnapshot mesh;
        const auto begin = Clock::now();
        for (int repetition = 0; repetition < repetitions && result.valid; ++repetition) {
            result.valid = steady ? scene::PrepareWaterSplineSnapshot(water, transform)
                : scene::BuildWaterSplineSnapshot(shape, points, river ? 1u : 8u, transform, mesh, 5.0f, 100000.0f);
        }
        const auto end = Clock::now();
        if (!result.valid) return result;
        Append(result, i, warmup, Us(begin, end) / repetitions);
        if (i >= warmup) result.checksum += WaterMeshHash(steady ? *water.splineSnapshot : mesh);
    }
    return result;
}

Result WaterCacheCase(const std::string& scenario, int count, int warmup, int samples)
{
    Result result;
    const bool grouped = scenario == "zone-steady" || scenario == "zone-change";
    const bool changing = scenario == "zone-change";
    const bool horizon = scenario == "horizon-links";
    if ((!grouped && !horizon && scenario != "independent") || count < 2 || count > 512
        || (horizon && count % 2 != 0)) {
        result.valid = false;
        return result;
    }
    testkit::ResourceTrackingRenderer device;
    renderer::ResourceManager resources(device);
    scene::Scene world;
    std::vector<scene::EntityID> owners;
    for (int i = 0; i < count; ++i) {
        auto& object = world.CreateGameObject("Water");
        auto& water = object.AddComponent<scene::WaterComponent>();
        water.extentX = water.extentZ = 8.0f;
        water.enableGerstnerWaves = false;
        water.resolutionX = water.resolutionZ = 4;
        water.chunkCount = 1;
        object.transform.worldPosition = {25.0f * static_cast<float>(i), 0.0f, 0.0f};
        owners.push_back(object.GetID());
        if (horizon && i % 2 == 0) {
            water.farOceanEnabled = true;
            water.adaptivePatchLod = true;
        } else if (horizon) {
            water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
            water.splineSamplesPerSegment = 1;
            water.splineMeshCellSize = 100.0f;
            water.horizonMaterialAtStart = true;
            water.horizonMaterialReceiver = scene::EntityRef{owners[i - 1]};
            water.splinePoints = {{1, {-25.0f, 0.0f, 0.0f}, 4.0f, 2.0f, 1.0f},
                {2, {-25.0f, 1.0f, 15.0f}, 4.0f, 2.0f, 1.0f}};
        }
    }
    if (grouped) {
        for (int i = 0; i + 1 < count; i += 2) {
            auto& zone = world.GetGameObject(owners[i])->AddComponent<scene::WaterZoneComponent>();
            zone.enabled = !changing;
            zone.members.push_back({scene::EntityRef{owners[i + 1]}, 0});
        }
    }
    renderer::Camera camera;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    scene::RenderPassContext context(world, device, resources, camera, settings, {}, UINT32_MAX, handles);
    const auto extract = [&]() {
        renderer::RenderScene output;
        scene::ExtractRenderWater(context, output);
        return output.water;
    };
    Reserve(result, samples);
    const auto initial = extract();
    if (initial.size() != static_cast<size_t>(grouped && !changing ? (count + 1) / 2 : count)) {
        result.valid = false;
        return result;
    }
    for (int i = 0; i < warmup + samples; ++i) {
        std::vector<renderer::ResourceHandle<renderer::BufferTag>> releasedBuffers;
        if (changing) {
            for (int owner = 0; owner + 1 < count; owner += 2)
                world.GetComponent<scene::WaterZoneComponent>(owners[owner])->enabled = false;
            const auto independent = extract();
            if (independent.size() != static_cast<size_t>(count)) { result.valid = false; return result; }
            for (size_t member = 1; member < independent.size(); member += 2)
                releasedBuffers.push_back(independent[member].patches.front().vertexBuffer);
            for (int owner = 0; owner + 1 < count; owner += 2)
                world.GetComponent<scene::WaterZoneComponent>(owners[owner])->enabled = true;
        }
        const auto begin = Clock::now();
        const auto water = extract();
        const auto end = Clock::now();
        if (water.size() != static_cast<size_t>(grouped ? (count + 1) / 2 : count) ||
            !std::all_of(releasedBuffers.begin(), releasedBuffers.end(), [&](const auto buffer) {
                return buffer.IsValid() && resources.Get(buffer) == nullptr;
            })) { result.valid = false; return result; }
        Append(result, i, warmup, Us(begin, end));
        if (i >= warmup) {
            uint64_t hash = 14695981039346656037ull;
            for (const auto& input : water) {
                HashWord(hash, static_cast<uint32_t>(input.medium.stableOwnerKey));
                HashWord(hash, static_cast<uint32_t>(input.horizonReceiverKey));
                for (const auto& patch : input.patches) HashWord(hash, patch.indexCount);
            }
            if (horizon) {
                for (int riverIndex = 1; riverIndex < count; riverIndex += 2) {
                    const auto& river = water[riverIndex];
                    if (river.horizonReceiverKey == 0 || river.horizonReceiverKey != water[riverIndex - 1].medium.stableOwnerKey) {
                        result.valid = false;
                        return result;
                    }
                }
            }
            result.checksum += hash;
        }
    }
    return result;
}

Result WaterMediumCase(const std::string& scenario, int count, int warmup, int samples)
{
    Result result;
    const bool duplicated = scenario == "duplicates";
    if ((!duplicated && scenario != "unique") || count < 2 || count > 8192 || count % 2 != 0) {
        result.valid = false;
        return result;
    }
    testkit::ResourceTrackingRenderer device;
    renderer::ResourceManager resources(device);
    renderer::Camera camera;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    renderer::RenderPassContext context{{}, device, resources, camera, settings, {}, UINT32_MAX, handles};
    renderer::RenderViewResources view;
    context.width = context.height = 16;
    auto sceneInput = std::make_shared<renderer::RenderScene>();
    for (int i = 0; i < count; ++i) {
        renderer::RenderWaterInput input;
        input.constants.worldMatrix = math::Matrix4::Identity();
        input.medium.stableOwnerKey = 1 + static_cast<uint64_t>(duplicated ? i / 2 : i);
        input.medium.opticalEnabled = true;
        input.medium.worldBoundsXZ = {100000.0f, 100000.0f, 100008.0f, 100008.0f};
        sceneInput->water.push_back(std::move(input));
    }
    context.renderScene = sceneInput;
    Reserve(result, samples);
    for (int i = 0; i < warmup + samples; ++i) {
        const auto begin = Clock::now();
        const bool prepared = renderer::PrepareWaterMediumResources(context, view);
        const auto end = Clock::now();
        const auto& disabled = context.handles.waterMediumDisabledOwnerKeys;
        if (prepared || disabled.size() != static_cast<size_t>(duplicated ? count / 2 : 0)) {
            result.valid = false;
            return result;
        }
        Append(result, i, warmup, Us(begin, end));
        if (i >= warmup) {
            uint64_t hash = 14695981039346656037ull;
            for (uint64_t key : disabled) HashWord(hash, static_cast<uint32_t>(key));
            result.checksum += hash;
        }
    }
    return result;
}

Result StateTrackerCase(const std::string& scenario, int count, int warmup, int samples)
{
    Result result;
    const bool pending = scenario == "half-pending";
    if ((!pending && scenario != "registered") || count < 4 || count > 4096) {
        result.valid = false;
        return result;
    }
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) ||
        FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        result.valid = false;
        return result;
    }
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands))) ||
        FAILED(commands->Close())) { result.valid = false; return result; }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    std::vector<ComPtr<ID3D12Resource>> resources(count);
    for (int i = 0; i < count; ++i) {
        const auto initial = pending && i % 4 == 0 ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr,
                IID_PPV_ARGS(&resources[i])))) { result.valid = false; return result; }
    }
    renderer::DX12StateTracker tracker;
    Reserve(result, samples);
    for (int i = 0; i < warmup + samples; ++i) {
        tracker.Clear();
        if (FAILED(allocator->Reset()) || FAILED(commands->Reset(allocator.Get(), nullptr))) {
            result.valid = false;
            return result;
        }
        for (int index = 0; index < count; ++index) {
            auto* resource = resources[index].Get();
            const auto initial = pending && index % 4 == 0 ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_COPY_DEST;
            tracker.Register(resource, initial);
            if (pending && index % 2 == 0) tracker.QueueTransition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        const auto begin = Clock::now();
        tracker.QueueTransitionAllToCommon();
        const auto end = Clock::now();
        tracker.FlushBarriers(commands.Get());
        if (FAILED(commands->Close())) { result.valid = false; return result; }
        Append(result, i, warmup, Us(begin, end));
    }
    return result;
}

enum class LandscapePass { VEGETATION, GRASS, TERRAIN, TERRAIN_OCCLUSION, SHADOW, TERRAIN_SHADOW };

renderer::RenderMeshItem LandscapeQuad(renderer::ResourceManager& resources)
{
    std::array<renderer::Vertex, 4> vertices{};
    for (size_t i = 0; i < vertices.size(); ++i)
        vertices[i].position = {float(i % 2), 0.0f, float(i / 2)};
    const std::array<uint32_t, 6> indices{0, 2, 1, 1, 2, 3};
    renderer::RenderMeshItem mesh;
    mesh.vertexBuffer = resources.CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(renderer::Vertex));
    mesh.indexBuffer = resources.CreateIndexBuffer(indices.data(), static_cast<uint32_t>(indices.size()));
    mesh.indexCount = static_cast<uint32_t>(indices.size());
    mesh.material.valid = true;
    mesh.material.doubleSided = true;
    mesh.material.shaderCapabilities = {renderer::ShaderVertexContract::STANDARD_SURFACE,
        renderer::ShaderSurfaceContract::STANDARD_PBR, renderer::ShaderOpacityContract::ALPHA_CLIP,
        renderer::ShaderVariantSet::STANDARD_SURFACE};
    return mesh;
}

void AddLandscapeInstances(renderer::RenderScene& scene, const renderer::RenderMeshItem& mesh,
    uint32_t sourceCount, uint32_t placementsPerSource)
{
    scene.instances.reserve(sourceCount);
    auto data = std::make_shared<renderer::InstanceSetData>();
    data->placements.reserve(placementsPerSource);
    for (uint32_t i = 0; i < placementsPerSource; ++i) {
        const float x = static_cast<float>(i % 128) * 2 - 127;
        const float z = static_cast<float>((i / 128) % 128) * 2 - 127;
        data->placements.push_back({i + 1, {x, 0, z}, {}, {1, 1, 1}, true});
    }
    for (uint32_t i = 0; i < sourceCount; ++i) {
        renderer::RenderInstanceInput input;
        input.identity = {1, i + 1};
        input.world.m[0][3] = static_cast<float>(i % 128) * 2;
        input.world.m[2][3] = static_cast<float>((i / 128) % 128) * 2;
        input.localBoundsRadius = 1.5f;
        input.lodCount = 2;
        input.lodHeights = {0.008f, 0, 0, 0};
        input.data = data;
        input.parts = {{mesh, 0}, {mesh, 1}};
        input.vegetationBudget = {true, 96, 48, 64};
        scene.instances.push_back(std::move(input));
    }
}

std::shared_ptr<const renderer::TerrainOcclusionGeometry> LandscapePatchGeometry(
    const renderer::TerrainGpuPatch& grid)
{
    auto geometry = std::make_shared<renderer::TerrainOcclusionGeometry>();
    geometry->heights.resize(grid.columns * grid.rows);
    for (size_t i = 0; i < geometry->heights.size(); ++i)
        geometry->heights[i] = static_cast<float>(grid.originZ + i / grid.columns) * 0.125f;
    scene::TerrainComponent flat;
    flat.columns = static_cast<int>(grid.columns);
    flat.rows = static_cast<int>(grid.rows);
    for (size_t lod = 0; lod < geometry->indices.size(); ++lod) {
        std::vector<uint32_t> indices;
        scene::BuildTerrainPatchLodIndices(flat, 0, 0, flat.columns - 1, flat.rows - 1,
            renderer::TERRAIN_LOD_STEPS[lod], indices);
        geometry->indices[lod].assign(indices.begin(), indices.end());
    }
    geometry->columns.push_back({{float(grid.originX), float(grid.originZ),
        float(grid.originX + grid.columns - 1), float(grid.originZ + grid.rows - 1)}, geometry->heights.front()});
    return geometry;
}

void AddLandscapeTerrain(renderer::RenderScene& scene, renderer::ResourceManager& resources,
    uint32_t count)
{
    constexpr uint32_t STEP = 8;
    const uint32_t width = static_cast<uint32_t>(std::sqrt(count));
    const uint32_t height = count / width;
    renderer::RenderTerrainInput input;
    input.chunkWorldSize = STEP;
    input.constants.worldMatrix = math::Matrix4::Identity();
    input.constants.worldMatrix.m[0][3] = -32;
    input.constants.worldMatrix.m[2][3] = -32;
    std::array<renderer::ResourceHandle<renderer::BufferTag>, renderer::TERRAIN_LOD_STEPS.size()> indexBuffers{};
    input.patches.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t x = (i % width) * STEP;
        const uint32_t z = (i / width) * STEP;
        renderer::RenderTerrainPatch patch;
        patch.gpuPatch = {x, z, STEP + 1, STEP + 1, width * STEP + 1, height * STEP + 1, 1, 0};
        patch.occlusion = LandscapePatchGeometry(patch.gpuPatch);
        patch.aabbMin = {float(x), patch.occlusion->heights.front(), float(z)};
        patch.aabbMax = {float(x + STEP), patch.occlusion->heights.back(), float(z + STEP)};
        std::array<renderer::Vertex, (STEP + 1) * (STEP + 1)> vertices{};
        for (size_t vertex = 0; vertex < vertices.size(); ++vertex)
            vertices[vertex].position = {float(x + vertex % (STEP + 1)), patch.occlusion->heights[vertex],
                float(z + vertex / (STEP + 1))};
        patch.vertexBuffer = resources.CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(renderer::Vertex));
        for (size_t lod = 0; lod < indexBuffers.size(); ++lod) {
            const auto& indices = patch.occlusion->indices[lod];
            if (!indexBuffers[lod].IsValid()) {
                const std::vector<uint32_t> indices32(indices.begin(), indices.end());
                indexBuffers[lod] = resources.CreateIndexBuffer(indices32.data(), static_cast<uint32_t>(indices32.size()));
            }
            patch.indexBufferLOD[lod] = indexBuffers[lod];
            patch.indexCountLOD[lod] = static_cast<uint32_t>(indices.size());
        }
        input.patches.push_back(std::move(patch));
    }
    scene.terrains.push_back(std::move(input));
}

constexpr uint32_t GRASS_ROOTS_PER_PAGE = 1024;

void AddLandscapeGrass(renderer::RenderScene& scene, renderer::ResourceManager& resources, uint32_t pageCount)
{
    const std::vector<renderer::FiberBladeRoot> roots(GRASS_ROOTS_PER_PAGE);
    const auto buffer = resources.CreateStructuredBuffer(roots.data(), GRASS_ROOTS_PER_PAGE, sizeof(roots[0]));
    scene.fibers.reserve(pageCount);
    for (uint32_t i = 0; i < pageCount; ++i) {
        renderer::RenderFiberInput surface;
        surface.identity = i + 1;
        surface.settings.m_mode = renderer::FiberRenderMode::BLADE;
        surface.settings.m_distanceLod = true;
        surface.settings.m_lodNear = 180;
        surface.settings.m_lodFar = 320;
        surface.material.m_length = 0.2f;
        surface.material.m_density = 1;
        surface.hasBounds = true;
        surface.boundsCenter = surface.lodCenter = {float(i % 32) * 8 - 124, 0, float(i / 32) * 8 - 124};
        surface.boundsRadius = 6;
        surface.object.world = math::Matrix4::Identity();
        surface.object.world.m[0][3] = surface.boundsCenter.x;
        surface.object.world.m[2][3] = surface.boundsCenter.z;
        surface.blades.m_blades = buffer;
        surface.blades.m_bladeCount = GRASS_ROOTS_PER_PAGE;
        scene.fibers.push_back(std::move(surface));
    }
}

void ConfigureLandscapeShadows(renderer::RenderPassContext& ctx, float cameraOffset)
{
    ctx.shadowCascadeCount = 4;
    for (int i = 0; i < ctx.shadowCascadeCount; ++i) {
        renderer::Camera light;
        light.m_projection = renderer::ProjectionMode::Orthographic;
        light.m_orthoHeight = 160.0f + static_cast<float>(i) * 100.0f;
        light.m_aspect = 1;
        light.m_near = 0.1f;
        light.m_far = 600.0f;
        light.m_position = {cameraOffset + 40, 180, -180};
        light.LookAt({cameraOffset + 40, 0, 0});
        auto& cascade = ctx.shadowCascades[i];
        cascade.view = light.GetViewMatrix();
        cascade.viewProjection = light.GetViewProjection();
        cascade.frustum = math::Frustum::FromViewProjection(cascade.viewProjection);
        cascade.eyePos = light.m_position;
        cascade.viewportSize = 1024;
        cascade.viewportX = (i % 2) * cascade.viewportSize;
        cascade.viewportY = (i / 2) * cascade.viewportSize;
        cascade.texelWorldSize = light.m_orthoHeight / static_cast<float>(cascade.viewportSize);
    }
}

Result LandscapeCase(LandscapePass pass, const std::string& scenario, uint32_t count, int warmup, int samples)
{
    Result result;
    const bool instanceShadow = pass == LandscapePass::SHADOW;
    const bool shadow = instanceShadow || pass == LandscapePass::TERRAIN_SHADOW;
    const bool vegetation = pass == LandscapePass::VEGETATION;
    const bool grass = pass == LandscapePass::GRASS;
    const bool occlusion = pass == LandscapePass::TERRAIN_OCCLUSION;
    const bool replace = scenario == "replace-static" || scenario == "replace-moving";
    const bool moving = scenario == "moving" || scenario == "replace-moving";
    const uint32_t width = static_cast<uint32_t>(std::sqrt(count));
    if ((replace && !instanceShadow) || (!replace && !moving && scenario != "static")
        || (!instanceShadow && !vegetation && !grass && count % width != 0)) {
        result.valid = false;
        return result;
    }
    testkit::ResourceTrackingRenderer device;
    device.m_supportStructuredRegions = true;
    renderer::ResourceManager resources(device);
    renderer::RenderSettings settings;
    settings.gpuInstancing = true;
    settings.bulkInstanceGpuCulling = false;
    settings.shadowEnabled = true;
    settings.geometryDrawMode = renderer::GeometryDrawMode::CPU;
    renderer::RenderPassHandles handles;
    handles.frameCB = resources.CreateConstantBuffer(sizeof(renderer::PerFrameCB));
    handles.objectCB = resources.CreateConstantBuffer(sizeof(renderer::PerObjectCB));
    if (pass == LandscapePass::TERRAIN_SHADOW) {
        handles.shadowShader = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/ShadowMap.hlsl");
        handles.defaultPSO = resources.CreatePipelineState({renderer::RasterizerMode::SOLID,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON});
    }
    const auto target = (shadow || grass) ? resources.CreateRenderTarget(2048, 2048,
        {0, renderer::Format::RGBA16F, true, false}) : renderer::ResourceHandle<renderer::RenderTargetTag>{};
    auto snapshot = std::make_shared<renderer::RenderScene>();
    if (vegetation || instanceShadow) {
        const auto mesh = LandscapeQuad(resources);
        AddLandscapeInstances(*snapshot, mesh, shadow ? count : 1, shadow ? (replace ? 8 : 64) : count);
    } else if (grass) AddLandscapeGrass(*snapshot, resources, count);
    else AddLandscapeTerrain(*snapshot, resources, count);
    std::shared_ptr<renderer::RenderScene> prior;
    /// @note The same resource frame sees two immutable snapshots whose instance identities match.
    if (replace) {
        prior = std::make_shared<renderer::RenderScene>(*snapshot);
        for (auto& input : prior->instances) input.world.m[1][3] += 0.25f;
    }
    renderer::TerrainRenderPass terrainPass(renderer::TerrainDrawMode::GBuffer);
    renderer::Camera camera;
    camera.m_projection = occlusion ? renderer::ProjectionMode::Perspective : renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 210;
    camera.m_aspect = 1;
    camera.m_near = 0.1f;
    camera.m_far = 550;
    renderer::OcclusionCuller occlusionCuller;
    std::array<math::Vector4, renderer::OcclusionCuller::kGpuRows> occlusionRows{};
    Reserve(result, samples);
    result.operationsPerSample = count;
    result.checksum = 14695981039346656037ULL;
    const auto hash = [&](uint32_t item) { result.checksum = (result.checksum ^ item) * 1099511628211ULL; };
    for (int sample = -warmup; sample < samples; ++sample) {
        resources.AdvanceFrame();
        const int phase = (sample % 24 + 24) % 24;
        const float offset = moving ? static_cast<float>(12 - std::abs(phase - 12)) * 6 : 0;
        camera.m_position = {offset, 100, -115};
        camera.LookAt({offset, 0, 0});
        const auto frustum = math::Frustum::FromViewProjection(camera.GetGpuViewProjection());
        renderer::RenderPassContext ctx{{}, device, resources, camera, settings, {}, UINT32_MAX, handles};
        ctx.renderScene = snapshot;
        ctx.width = 1024;
        ctx.height = 1024;
        ctx.isDeferred = true;
        ctx.frustumCullingEnabled = true;
        ctx.cameraFrustum = &frustum;
        if (shadow) {
            ctx.resourceRegistry.BindTarget("ShadowMap", target);
            ConfigureLandscapeShadows(ctx, offset);
        }
        if (grass) ctx.resourceRegistry.BindTarget("GBuffer", target);
        if (prior) {
            ctx.renderScene = prior;
            renderer::ExecuteShadowPass(ctx);
            if (ctx.statsShadowDrawCalls == 0) result.valid = false;
            ctx.statsShadowDrawCalls = 0;
            ctx.statsShadowTriangleCount = 0;
            ctx.renderScene = snapshot;
        }
        if (occlusion) {
            ctx.occlusionCuller = &occlusionCuller;
            ctx.occlusionCullingEnabled = true;
            occlusionCuller.Reset(camera);
        }
        const std::vector<renderer::RenderGraph::ResourceAccess> accesses;
        renderer::PassResources passResources(ctx.resourceRegistry, accesses, terrainPass.Name());
        const auto begin = Clock::now();
        switch (pass) {
        case LandscapePass::VEGETATION: renderer::ExecuteInstanceGBuffer(ctx); break;
        case LandscapePass::GRASS: renderer::ExecuteFiberGBufferPass(ctx); break;
        case LandscapePass::TERRAIN: terrainPass.Execute(passResources, ctx); break;
        case LandscapePass::TERRAIN_OCCLUSION: renderer::RasterTerrainOccluders(ctx); break;
        case LandscapePass::TERRAIN_SHADOW:
        case LandscapePass::SHADOW: renderer::ExecuteShadowPass(ctx); break;
        }
        const auto end = Clock::now();
        if (sample < 0) continue;
        Append(result, sample, 0, Us(begin, end));
        if (occlusion) {
            occlusionCuller.CopyGpuRows(occlusionRows);
            for (const auto& row : occlusionRows)
                for (float value : {row.x, row.y, row.z, row.w}) hash(std::bit_cast<uint32_t>(value));
            result.recorded += occlusionRows[8].w > 0 ? 1 : 0;
            continue;
        }
        const int draws = shadow ? ctx.statsShadowDrawCalls : ctx.statsDrawCalls;
        const int triangles = shadow ? ctx.statsShadowTriangleCount : ctx.statsTriangleCount;
        result.valid = result.valid && draws > 0 && triangles > 0 && (shadow || grass || ctx.statsTotalObjects == count);
        result.recorded += draws;
        result.candidates += triangles;
        for (const int stat : {draws, triangles, ctx.statsTotalObjects, ctx.statsFrustumCulled, ctx.statsDistanceCulled})
            hash(static_cast<uint32_t>(stat));
    }
    if (occlusion) result.valid = result.recorded == static_cast<uint64_t>(samples);
    return result;
}

Result TerrainExtractionCase(const std::string& scenario, uint32_t count, int warmup, int samples)
{
    Result result;
    const bool moving = scenario == "moving" || scenario == "edit-moving";
    const bool edit = scenario == "edit-static" || scenario == "edit-moving";
    const uint32_t width = static_cast<uint32_t>(std::sqrt(count));
    if (count % width != 0 || (!moving && !edit && scenario != "static")) {
        result.valid = false;
        return result;
    }
    testkit::ResourceTrackingRenderer device;
    renderer::ResourceManager resources(device);
    scene::Scene scene;
    renderer::RenderSettings settings;
    settings.geometryDrawMode = renderer::GeometryDrawMode::CPU;
    renderer::RenderPassHandles handles;
    auto& grid = scene.CreateGameObject("Grid").AddComponent<scene::TerrainGridComponent>();
    grid.cellCountX = width;
    grid.cellCountZ = count / width;
    grid.EnsureSize();
    for (uint32_t i = 0; i < count; ++i) {
        auto& tile = scene.CreateGameObject("Tile");
        auto& terrain = tile.AddComponent<scene::TerrainComponent>();
        terrain.columns = terrain.rows = 3;
        terrain.chunkSize = 2;
        terrain.cellSize = 1.0f;
        terrain.InitFlat();
        const int x = i % width, z = i / width;
        tile.transform.position = {static_cast<float>(x * 2), 0, static_cast<float>(z * 2)};
        grid.SetCell(x, z, tile.GetID());
    }
    scene::FlushWorldTransforms(scene);
    renderer::Camera camera;
    renderer::RenderScene output;
    Reserve(result, samples);
    result.operationsPerSample = count;
    result.checksum = 14695981039346656037ULL;
    for (int sample = -warmup; sample < samples; ++sample) {
        resources.AdvanceFrame();
        const int phase = (sample % 24 + 24) % 24;
        const float offset = moving ? static_cast<float>(12 - std::abs(phase - 12)) : 0;
        camera.m_position = {offset, 30, -30};
        camera.LookAt({offset, 0, 0});
        if (edit) for (const auto id : grid.cells) {
            auto* terrain = scene.GetComponent<scene::TerrainComponent>(id);
            terrain->heightData[4] = phase % 2 ? 0.01f : 0.02f;
            terrain->heightDirty = true;
        }
        output.terrains.clear();
        scene::RenderPassContext ctx(scene, device, resources, camera, settings, {}, UINT32_MAX, handles);
        const auto begin = Clock::now();
        scene::ExtractRenderTerrains(ctx, output);
        const auto end = Clock::now();
        if (sample < 0) continue;
        Append(result, sample, 0, Us(begin, end));
        result.valid = result.valid && output.terrains.size() == count;
        for (const auto& input : output.terrains) {
            result.valid = result.valid && input.patches.size() == 1;
            for (const auto& patch : input.patches) {
                result.valid = result.valid && resources.Get(patch.vertexBuffer) && patch.occlusion;
                if (!patch.occlusion) continue;
                result.checksum = (result.checksum ^ patch.indexCountLOD[0]) * 1099511628211ULL;
                for (float height : patch.occlusion->heights)
                    result.checksum = (result.checksum ^ std::bit_cast<uint32_t>(height)) * 1099511628211ULL;
                ++result.recorded;
            }
        }
    }
    return result;
}

Result RayGrassCase(const std::string& scenario, uint32_t count, int warmup, int samples,
    const std::filesystem::path& reportPath)
{
    Result result;
    if (scenario != "resident") { result.valid = false; return result; }
    testkit::ResourceTrackingRenderer device;
    renderer::ResourceManager resources(device);
    scene::Scene scene;
    const auto directory = reportPath.parent_path() / ("GrassBenchmark-" + std::to_string(GetCurrentProcessId()));
    std::error_code error;
    if (!std::filesystem::create_directory(directory, error)) {
        result.valid = false;
        return result;
    }
    const auto materialPath = directory / "Grass.mat";
    asset::MaterialAssetAuthoring material;
    material.params["fiberLength"] = {0.2f};
    material.params["fiberDensity"] = {1.0f};
    if (!asset::SaveMaterialAssetToFile(materialPath.generic_string(), material)) {
        std::filesystem::remove(directory, error);
        result.valid = false;
        return result;
    }
    asset::AssetManager::Init(resources, directory.generic_string() + "/");
    const auto materialReference = materialPath.generic_string();
    renderer::RenderScene output;
    output.fibers.reserve(count * 4);
    for (uint32_t i = 0; i < count; ++i) {
        auto& object = scene.CreateGameObject("Grass");
        object.AddComponent<scene::TerrainComponent>();
        auto& fiber = object.AddComponent<scene::FiberComponent>();
        fiber.m_materialPath = materialReference;
        fiber.m_renderIdentity = i + 1;
        fiber.m_mode = renderer::FiberRenderMode::BLADE;
        for (int patch = 0; patch < 4; ++patch) {
            auto& surface = output.fibers.emplace_back();
            surface.identity = i + 1;
            surface.blades.m_blades = {1, 1};
            surface.blades.m_bladeCount = patch == 3 && i % 2 == 0 ? 1024 : 0;
        }
    }
    Reserve(result, samples);
    result.checksum = 14695981039346656037ULL;
    for (int sample = -warmup; sample < samples; ++sample) {
        output.rayUnsupportedEffects.clear();
        const auto begin = Clock::now();
        scene::ExtractRayFiberSources(scene, resources, output, true);
        const auto end = Clock::now();
        if (sample < 0) continue;
        Append(result, sample, 0, Us(begin, end));
        result.valid = result.valid && output.rayUnsupportedEffects.size() == (count + 1) / 2;
        result.recorded += output.rayUnsupportedEffects.size();
        for (const auto& effect : output.rayUnsupportedEffects)
            for (uint32_t key : {effect.sourceIndex, effect.sourceGeneration, effect.layerMask})
                result.checksum = (result.checksum ^ key) * 1099511628211ULL;
    }
    asset::AssetManager::UnloadAll();
    std::filesystem::remove(materialPath, error);
    auto metaPath = materialPath;
    metaPath += ".meta";
    std::filesystem::remove(metaPath, error);
    std::filesystem::remove(directory, error);
    return result;
}

void Array(std::ostream& output, const std::vector<double>& values)
{
    output << '[';
    for (size_t index = 0; index < values.size(); ++index) { if (index) output << ','; output << values[index]; }
    output << ']';
}
void Write(std::ostream& output, const Result& value)
{
    output << "{\"valid\":" << (value.valid ? "true" : "false") << ",\"totalUs\":"; Array(output, value.total);
    output << ",\"firstUs\":"; Array(output, value.first); output << ",\"laterUs\":"; Array(output, value.later);
    output << ",\"checksum\":" << value.checksum << ",\"probes\":" << value.probes
        << ",\"candidates\":" << value.candidates << ",\"visitedNodes\":" << value.visitedNodes
        << ",\"testedLeaves\":" << value.testedLeaves << ",\"retainedBytes\":" << value.retainedBytes
        << ",\"recorded\":" << value.recorded << ",\"dropped\":" << value.dropped << ",\"descriptorCount\":" << value.descriptorCount
        << ",\"operationsPerSample\":" << value.operationsPerSample
        << ",\"coldMemory\":{\"working\":" << value.cold.working << ",\"commit\":" << value.cold.commit << '}'
        << ",\"warmMemory\":{\"working\":" << value.warm.working << ",\"commit\":" << value.warm.commit << '}'
        << ",\"retiredMemory\":{\"working\":" << value.retired.working << ",\"commit\":" << value.retired.commit << "}}";
}
}
int Measure(int argc, char** argv)
{
    if (argc != 9) return 2;
    const std::string family = argv[1], scenario = argv[2], label = argv[3];
    const int count = std::atoi(argv[4]), warmup = std::atoi(argv[5]), samples = std::atoi(argv[6]);
    if (count <= 0 || warmup < 0 || samples <= 0 || samples > 10000 || (label != "before" && label != "after")) return 2;
    profiler::Profiler::RequestCaptureFilter({}); profiler::Profiler::SetEnabled(false);
    std::vector<Result> results;
    const auto begin = Clock::now();
    if (family == "profiler" && scenario == "tls2") {
        results.resize(2);
        std::thread first([&] { results[0] = ProfilerCase("repeated", count, warmup, samples); });
        std::thread second([&] { results[1] = ProfilerCase("repeated", count, warmup, samples); });
        first.join(); second.join();
    } else if (family == "profiler") results.push_back(ProfilerCase(scenario, count, warmup, samples));
    else if (family == "canvas") results.push_back(CanvasCase(scenario, label == "before", count, warmup, samples));
    else if (family == "geometry") results.push_back(GeometryCase(scenario, count, warmup, samples));
    else if (family == "bvh") results.push_back(BvhCase(scenario, count, warmup, samples));
    else if (family == "water-spline") results.push_back(WaterSplineCase(scenario, count, warmup, samples));
    else if (family == "water-cache") {
        /// @note The CPU backend reports missing dynamic-texture support. Keep console I/O outside the measured function.
        core::Logger::SetMinLevel(core::LogLevel::LOG_ERROR);
        results.push_back(WaterCacheCase(scenario, count, warmup, samples));
        core::Logger::SetMinLevel(core::LogLevel::INFO);
    }
    else if (family == "water-medium") results.push_back(WaterMediumCase(scenario, count, warmup, samples));
    else if (family == "dx12-common") results.push_back(StateTrackerCase(scenario, count, warmup, samples));
    else if (family == "vegetation") results.push_back(LandscapeCase(LandscapePass::VEGETATION, scenario, count, warmup, samples));
    else if (family == "grass") results.push_back(LandscapeCase(LandscapePass::GRASS, scenario, count, warmup, samples));
    else if (family == "grass-ray") results.push_back(RayGrassCase(scenario, count, warmup, samples,
        std::filesystem::absolute(argv[7])));
    else if (family == "terrain") results.push_back(LandscapeCase(LandscapePass::TERRAIN, scenario, count, warmup, samples));
    else if (family == "terrain-extract") results.push_back(TerrainExtractionCase(scenario, count, warmup, samples));
    else if (family == "terrain-occlusion")
        results.push_back(LandscapeCase(LandscapePass::TERRAIN_OCCLUSION, scenario, count, warmup, samples));
    else if (family == "terrain-shadow")
        results.push_back(LandscapeCase(LandscapePass::TERRAIN_SHADOW, scenario, count, warmup, samples));
    else if (family == "shadow") results.push_back(LandscapeCase(LandscapePass::SHADOW, scenario, count, warmup, samples));
    else return 2;
    const auto end = Clock::now();
    std::ofstream output(argv[7], std::ios::binary);
    if (!output) return 2;
    output.precision(12);
    output << "{\"schema\":\"fbzz-algorithm-measure/1\",\"family\":\"" << family << "\",\"scenario\":\"" << scenario
        << "\",\"label\":\"" << label << "\",\"buildLabel\":\"" << argv[8]
        << "\",\"count\":" << count << ",\"warmup\":" << warmup << ",\"samples\":" << samples
        << ",\"batchWallUs\":" << Us(begin, end)
        << ",\"gpu\":\"not measured\",\"stlAllocations\":null,\"physicalCopyBytes\":null,\"runtimeModules\":[";
    bool moduleValid = true;
    bool moduleFirst = true;
    for (const char* moduleName : {"FBZZEngine.dll", "FBZZGraphics.dll", "FBZZCore.dll", "FBZZMath.dll"}) {
        std::array<char, MAX_PATH> buffer{};
        const auto module = GetModuleHandleA(moduleName);
        const DWORD length = module ? GetModuleFileNameA(module, buffer.data(), static_cast<DWORD>(buffer.size())) : 0;
        if (length == 0 || length >= buffer.size()) { moduleValid = false; continue; }
        std::string path(buffer.data(), length); std::replace(path.begin(), path.end(), '\\', '/');
        if (!moduleFirst) output << ',';
        moduleFirst = false;
        output << "{\"name\":\"" << moduleName << "\",\"path\":\"" << path << "\"}";
    }
    output << "],\"workers\":[";
    bool valid = true;
    for (size_t index = 0; index < results.size(); ++index) { if (index) output << ','; Write(output, results[index]); valid = valid && results[index].valid; }
    output << "]}"; output.flush();
    std::printf("ALGORITHM_MEASURE valid=%d family=%s scenario=%s workers=%zu samples=%d\n", valid, family.c_str(), scenario.c_str(), results.size(), samples);
    return valid && moduleValid && output.good() ? 0 : 1;
}
}
int main(int argc, char** argv) { return fbzz::bench::Measure(argc, argv); }
