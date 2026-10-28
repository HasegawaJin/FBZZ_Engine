/// @file    AlgorithmOptimizationMeasure.cpp
/// @brief   Measure four source iterations using external clocks and fixed CPU inputs.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Psapi.h>
#include <Core/Profiler/Profiler.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Systems/RenderSceneExtractor.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Math/MathUtils.hpp>
#include "../../Engine/src/Scene/Systems/CanvasGeometryGuard.hpp"
#include "../../Graphics/src/Renderer/RenderMeshSpatialIndex.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
