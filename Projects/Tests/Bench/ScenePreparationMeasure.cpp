/// @file    ScenePreparationMeasure.cpp
/// @brief   同一の固定入力で CPU プロファイラー集約、影境界の準備、シーン階層の操作を計測する。
/// @author  Hasegawa Jin
/// @date    2026-10-25
#include <Core/Profiler/Profiler.hpp>
#include <Engine/Asset/Model/Model.hpp>
#include <Engine/Core/Timing/Time.hpp>
#include <Graphics/Renderer/Geometry/Mesh.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/Transform/TransformSystem.hpp>
#include <Engine/Scene/Components/Geometry/MeshRenderer.hpp>
#include <Engine/Scene/Components/Geometry/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/Terrain/TerrainComponent.hpp>
#include "../../Editor/src/Profiler/AnalysisProfilerCpu.hpp"
#include "../../Engine/src/Rendering/Extraction/Lighting/RenderShadowPreparation.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::bench {
namespace {
using Clock = std::chrono::steady_clock;
struct Options {
    std::string family = "all", selected = "all", label = "unlabelled", output;
    int warmup = 5, samples = 31;
};
double Us(Clock::time_point first, Clock::time_point last)
{
    return std::chrono::duration<double, std::micro>(last - first).count();
}
std::string Quote(std::string_view value)
{
    constexpr char HEX[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') { result += '\\'; result += static_cast<char>(character); }
        else if (character < 32) { result += "\\u00"; result += HEX[character >> 4]; result += HEX[character & 15]; }
        else result += static_cast<char>(character);
    }
    return result + '"';
}
bool Parse(int argc, char** argv, Options& options)
{
    for (int index = 1; index < argc; ++index) {
        if (index + 1 == argc) {
            return false;
        }
        const std::string_view key = argv[index++];
        const char* value = argv[index];
        if (key == "--family") options.family = value;
        else if (key == "--case") options.selected = value;
        else if (key == "--label") options.label = value;
        else if (key == "--output") options.output = value;
        else if (key == "--samples" || key == "--warmup") {
            char* end = nullptr;
            const long number = std::strtol(value, &end, 10);
            if (end == value || *end != 0 || number < (key == "--samples" ? 1 : 0) || number > 256) {
                return false;
            }
            (key == "--samples" ? options.samples : options.warmup) = static_cast<int>(number);
        } else return false;
    }
    return options.family == "all" || options.family == "profiler_tree" || options.family == "shadow_bounds"
        || options.family == "shadow_views" || options.family == "hierarchy";
}
struct Hash {
    uint64_t value = 14695981039346656037ULL;
    void Word(uint64_t word) { for (int byte = 0; byte < 8; ++byte) { value = (value ^ (word & 255)) * 1099511628211ULL; word >>= 8; } }
    void Text(std::string_view text) { Word(text.size()); for (const unsigned char byte : text) value = (value ^ byte) * 1099511628211ULL; }
    void Scalar(float scalar) { Word(std::bit_cast<uint32_t>(scalar)); }
    void Scalar(double scalar) { Word(std::bit_cast<uint64_t>(scalar)); }
};
uint64_t TreeHash(const editor::detail::AnalysisProfTree& tree)
{
    Hash hash;
    hash.Word(tree.complete); hash.Word(tree.frameTotalAvailable); hash.Scalar(tree.frameMs);
    hash.Text(tree.unavailableReason ? tree.unavailableReason : "");
    hash.Word(tree.nodes.size()); hash.Word(tree.flat.size()); hash.Word(tree.roots.size());
    for (const int root : tree.roots) hash.Word(static_cast<uint64_t>(root));
    for (const auto& node : tree.nodes) {
        hash.Text(node.name); hash.Text(node.category.name); hash.Word(node.category.color); hash.Text(node.key);
        hash.Word(node.pathId.low); hash.Word(node.pathId.high); hash.Word(node.calls); hash.Word(node.depth);
        hash.Scalar(node.totalMs); hash.Scalar(node.selfMs); hash.Word(node.totalAvailable); hash.Word(node.selfAvailable);
        hash.Word(node.children.size()); for (const int child : node.children) hash.Word(static_cast<uint64_t>(child));
    }
    for (const auto& row : tree.flat) {
        hash.Text(row.name); hash.Text(row.category.name); hash.Word(row.category.color); hash.Text(row.key);
        hash.Scalar(row.totalMs); hash.Scalar(row.selfMs); hash.Word(row.totalAvailable); hash.Word(row.selfAvailable); hash.Word(row.calls);
    }
    return hash.value;
}
uint64_t BoundsHash(const renderer::ShadowBounds& bounds)
{
    Hash hash; hash.Word(bounds.valid); hash.Scalar(bounds.center.x); hash.Scalar(bounds.center.y);
    hash.Scalar(bounds.center.z); hash.Scalar(bounds.radius); return hash.value;
}

uint64_t ViewBoundsHash(const std::array<renderer::ShadowBounds, 3>& views)
{
    Hash hash;
    for (const auto& bounds : views) {
        hash.Word(BoundsHash(bounds));
        hash.Word(bounds.terrainRevision);
    }
    return hash.value;
}
double Percentile(std::vector<double> values, double quantile)
{
    std::sort(values.begin(), values.end());
    return values[static_cast<size_t>(std::ceil(quantile * static_cast<double>(values.size()))) - 1];
}
void Series(std::ostream& output, const char* name, const std::vector<double>& values)
{
    output << ',' << Quote(name) << ":{\"p50Us\":" << Percentile(values, 0.50)
        << ",\"p95Us\":" << Percentile(values, 0.95) << ",\"p99Us\":" << Percentile(values, 0.99) << ",\"rawUs\":[";
    for (size_t index = 0; index < values.size(); ++index) { if (index) output << ','; output << values[index]; }
    output << "]}";
}
struct Result {
    std::vector<double> total, operation, update;
    std::vector<uint64_t> hashes;
    std::array<uint64_t, 2> expected{};
    renderer::ShadowBounds lastBounds;
    size_t operationsPerSample = 1;
    bool hasBounds = false;
    uint64_t checksum = 14695981039346656037ULL;
    bool valid = true, complete = true;
    explicit Result(int samples) { total.reserve(samples); operation.reserve(samples); update.reserve(samples); hashes.reserve(samples); }
    void Observe(int iteration, const Options& options, double totalUs, double operationUs, double updateUs, uint64_t hash, uint64_t expectedHash, bool finished)
    {
        valid &= hash == expectedHash; complete &= finished;
        if (iteration < options.warmup) {
            return;
        }
        total.push_back(totalUs); operation.push_back(operationUs); update.push_back(updateUs);
        hashes.push_back(hash);
        checksum = (checksum ^ hash) * 1099511628211ULL;
    }
};
bool Selected(const Options& options, std::string_view family, std::string_view name)
{
    return (options.family == "all" || options.family == family) && (options.selected == "all" || options.selected == name);
}
void Write(std::ostream& output, const Options& options, const char* family, const std::string& name,
    size_t objects, size_t heights, size_t descriptors, size_t parents, const Result& result)
{
    output << "{\"kind\":\"case\",\"family\":" << Quote(family) << ",\"case\":" << Quote(name)
        << ",\"label\":" << Quote(options.label) << ",\"warmup\":" << options.warmup << ",\"samples\":" << options.samples
        << ",\"inputCount\":" << objects << ",\"heightSamples\":" << heights << ",\"descriptors\":" << descriptors
        << ",\"parentRoots\":" << parents << ",\"operationsPerSample\":" << result.operationsPerSample
        << ",\"valid\":" << (result.valid ? "true" : "false")
        << ",\"complete\":" << (result.complete ? "true" : "false") << ",\"checksum\":" << Quote(std::to_string(result.checksum))
        << ",\"expectedStateHashes\":[" << Quote(std::to_string(result.expected[0])) << ',' << Quote(std::to_string(result.expected[1]))
        << "],\"memoryBytes\":null,\"allocationCount\":null,\"rawHashes\":[";
    for (size_t index = 0; index < result.hashes.size(); ++index) { if (index) output << ','; output << Quote(std::to_string(result.hashes[index])); }
    output << "],\"lastBounds\":";
    if (result.hasBounds) output << "{\"valid\":" << (result.lastBounds.valid ? "true" : "false")
        << ",\"center\":[" << result.lastBounds.center.x << ',' << result.lastBounds.center.y << ',' << result.lastBounds.center.z
        << "],\"radius\":" << result.lastBounds.radius << '}';
    else output << "null";
    Series(output, "total", result.total); Series(output, "operation", result.operation); Series(output, "update", result.update);
    output << "}\n";
}
profiler::PerformanceSnapshot TreeInput(size_t count, bool sparse, bool branched)
{
    profiler::PerformanceSnapshot snapshot;
    snapshot.available = snapshot.recording = snapshot.complete = true;
    snapshot.recordedSampleCount = static_cast<uint32_t>(count);
    const size_t descriptorCount = branched ? (std::min)(size_t{32}, count) : 1;
    const size_t roots = branched ? (std::min)(size_t{8}, count) : count;
    const auto id = [=](size_t index) { return uint64_t{100000} + index * (sparse ? 3u : 1u); };
    for (size_t index = 0; index < descriptorCount; ++index)
        snapshot.descriptors.push_back({index + 1, "MeasureScope" + std::to_string(index), "ScenePreparationBench", 0xff4fa3ff});
    snapshot.samples.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        profiler::ProfileSample sample;
        sample.sampleId = id(index);
        const bool root = index < roots;
        sample.sampleKey = !branched ? 1 : root ? index + 1 : roots + 1 + (index - roots) % (descriptorCount - roots);
        sample.parentSampleId = root ? 0 : id((index - roots) % roots);
        sample.depth = root ? 0 : 1; sample.completionOrder = static_cast<uint32_t>(index + 1);
        sample.inclusiveMs = root ? static_cast<double>(count) + 1.0 : 0.125;
        sample.selfMs = root ? 1.0 : 0.125;
        if (root) snapshot.scopeRootSumMs += sample.inclusiveMs;
        snapshot.samples.push_back(sample);
    }
    return snapshot;
}
bool RunTrees(const Options& options, std::ostream& output, size_t& executed)
{
    bool valid = true;
    for (const size_t count : {size_t{1}, size_t{128}, size_t{4096}, size_t{8192}})
        for (const bool branched : {false, true}) for (const bool sparse : {false, true}) {
            const std::string name = std::string(sparse ? "sparse_" : "dense_") + (branched ? "branched_" : "repeated_") + std::to_string(count);
            if (!Selected(options, "profiler_tree", name)) {
                continue;
            }
            auto snapshot = TreeInput(count, sparse, branched);
            const auto reference = editor::detail::BuildRecordedProfTree(snapshot);
            Result result(options.samples); result.expected.fill(TreeHash(reference));
            const auto semanticTwin = TreeInput(count, !sparse, branched);
            result.valid = result.expected[0] == TreeHash(editor::detail::BuildRecordedProfTree(semanticTwin));
            for (int iteration = 0; iteration < options.warmup + options.samples; ++iteration) {
                const auto begin = Clock::now();
                const auto tree = editor::detail::BuildRecordedProfTree(snapshot);
                const auto end = Clock::now();
                result.Observe(iteration, options, Us(begin, end), Us(begin, end), 0, TreeHash(tree), result.expected[0], tree.complete);
            }
            Write(output, options, "profiler_tree", name, count, 0, snapshot.descriptors.size(), reference.roots.size(), result);
            valid &= result.valid && result.complete; ++executed;
        }
    return valid;
}
bool RunRootSiblingRanks(const Options& options, std::ostream& output, size_t& executed)
{
    constexpr std::string_view FAMILY = "hierarchy";
    const std::string name = "root_sibling_4096";
    if (!Selected(options, FAMILY, name)) {
        return true;
    }

    constexpr size_t COUNT = 4096;
    constexpr size_t QUERIES = 32;
    scene::Scene owner;
    std::array<scene::GameObject*, QUERIES> objects{};
    std::array<int, QUERIES> expected{};
    std::array<int, QUERIES> actual{};
    for (size_t query = 0; query < QUERIES; ++query)
        expected[query] = static_cast<int>(query * (COUNT - 1) / (QUERIES - 1));
    size_t query = 0;
    for (size_t index = 0; index < COUNT; ++index) {
        auto& object = owner.CreateGameObject("Root");
        if (query < QUERIES && index == static_cast<size_t>(expected[query]))
            objects[query++] = &object;
    }

    const auto hashRanks = [](const std::array<int, QUERIES>& ranks) {
        Hash hash;
        for (const int rank : ranks) hash.Word(static_cast<uint64_t>(rank));
        return hash.value;
    };
    Result result(options.samples);
    result.operationsPerSample = QUERIES;
    result.expected.fill(hashRanks(expected));
    for (int iteration = 0; iteration < options.warmup + options.samples; ++iteration) {
        const auto begin = Clock::now();
        for (size_t index = 0; index < QUERIES; ++index)
            actual[index] = objects[index]->GetSiblingIndex();
        const auto end = Clock::now();
        result.Observe(iteration, options, Us(begin, end), Us(begin, end), 0,
            hashRanks(actual), result.expected[0], actual == expected);
    }
    Write(output, options, FAMILY.data(), name, COUNT, 0, 0, COUNT, result);
    ++executed;
    return result.valid && result.complete;
}

bool RunHierarchySync(const Options& options, std::ostream& output, size_t& executed, bool flushLeaf)
{
    constexpr std::string_view FAMILY = "hierarchy";
    const std::string name = flushLeaf ? "flush_leaf_512" : "flush_subtree_4096";
    if (!Selected(options, FAMILY, name)) {
        return true;
    }

    const size_t count = flushLeaf ? 512 : 4096;
    const size_t branches = flushLeaf ? 1 : 4;
    scene::Scene owner;
    auto& ancestor = owner.CreateGameObject("Ancestor");
    ancestor.transform.position = {1, 2, 3};
    std::vector<scene::GameObject*> nodes;
    nodes.reserve(count);
    std::vector<math::Vector3> basePositions;
    basePositions.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        auto& node = owner.CreateGameObject("Node");
        const size_t parent = index == 0 ? 0 : (index - 1) / branches;
        node.SetParent(index == 0 ? ancestor : *nodes[parent]);
        node.transform.position = {1, static_cast<float>(index % branches + 1), 1};
        basePositions.push_back((index == 0 ? ancestor.transform.position : basePositions[parent])
            + node.transform.position);
        nodes.push_back(&node);
    }

    const auto offsetFor = [](int state) { return math::Vector3{state ? 11.0f : 5.0f, 0, 0}; };
    const auto addPosition = [](Hash& hash, const math::Vector3& position) {
        hash.Scalar(position.x);
        hash.Scalar(position.y);
        hash.Scalar(position.z);
    };
    Result result(options.samples);
    for (int state = 0; state < 2; ++state) {
        Hash expected;
        for (const auto& position : basePositions) addPosition(expected, position + offsetFor(state));
        result.expected[state] = expected.value;
    }

    // ルートのローカル位置を交互に変更し、部分木または末端までの祖先が同期したことを計測外で照合する。
    scene::GameObject& root = *nodes.front();
    scene::GameObject& target = flushLeaf ? *nodes.back() : root;
    const math::Vector3 rootPosition = root.transform.position;
    for (int iteration = 0; iteration < options.warmup + options.samples; ++iteration) {
        const int state = iteration % 2;
        const auto offset = offsetFor(state);
        root.transform.position = rootPosition + offset;
        const auto begin = Clock::now();
        scene::FlushWorldTransforms(target);
        const auto end = Clock::now();

        Hash actual;
        bool matches = true;
        for (size_t index = 0; index < count; ++index) {
            const auto& position = nodes[index]->transform.worldPosition;
            const auto expected = basePositions[index] + offset;
            addPosition(actual, position);
            matches &= position.x == expected.x && position.y == expected.y && position.z == expected.z;
        }
        result.Observe(iteration, options, Us(begin, end), Us(begin, end), 0,
            actual.value, result.expected[state], matches);
    }
    Write(output, options, FAMILY.data(), name, count, 0, 0, 1, result);
    ++executed;
    return result.valid && result.complete;
}

enum class ShadowWorkload { RAW_STATIONARY, RAW_DIRECT_UPDATES, EMPTY_MASK, ELIGIBLE_VIEWS, DIRECT_EDIT_VIEWS };

/// @note Mesh と Model の所有者は Scene より長く生存する。レンダラー・資源管理者・ウィンドウ・デバイスは作らない。
bool RunBoundsCase(const Options& options, std::ostream& output, const std::string& shape,
    ShadowWorkload workload, size_t& executed, bool uncachedViews = false)
{
    const bool raw = workload == ShadowWorkload::RAW_STATIONARY || workload == ShadowWorkload::RAW_DIRECT_UPDATES;
    const bool mutate = workload == ShadowWorkload::RAW_DIRECT_UPDATES || workload == ShadowWorkload::DIRECT_EDIT_VIEWS;
    const bool expectBounds = workload != ShadowWorkload::EMPTY_MASK;
    const char* family = raw ? "shadow_bounds" : "shadow_views";
    const char* suffix = raw ? (mutate ? "_direct_updates" : "_stationary")
        : (!expectBounds ? "_empty_mask" : (mutate ? "_edits_between_views" : "_eligible_views"));
    const std::string name = shape + suffix + (uncachedViews ? "_uncached" : "");
    if (!Selected(options, family, name)) {
        return true;
    }
    asset::Model model; renderer::Mesh mesh; scene::Scene owner;
    mesh.boundsRadius = 1; mesh.boundsExtents = {1, 1, 1};
    auto skin = std::make_unique<renderer::Mesh>(); skin->boundsRadius = 1; skin->isSkinned = true;
    model.meshes.push_back(std::move(skin));
    const size_t count = shape == "static10000" ? 10000 : shape == "mixed1000" ? 1000 : 16;
    const int side = shape == "terrain16x129" ? 129 : 65;
    std::vector<scene::EntityID> entities, terrains;
    for (size_t index = 0; index < count; ++index) {
        auto& object = owner.CreateGameObject("ScenePreparationMeasure"); entities.push_back(object.GetID());
        object.transform.worldPosition = object.transform.position = {static_cast<float>(index % 100) * 3, 0, static_cast<float>(index / 100) * 3};
        if (shape == "terrain16x129" || (shape == "mixed1000" && index % 100 == 0)) {
            auto& terrain = object.AddComponent<scene::TerrainComponent>();
            terrain.columns = terrain.rows = side; terrain.cellSize = 1; terrain.maxHeight = 20;
            terrain.heightData.resize(static_cast<size_t>(side * side));
            for (size_t point = 0; point < terrain.heightData.size(); ++point)
                terrain.heightData[point] = static_cast<float>(static_cast<int>(point % 17) - 8) / 16;
            terrains.push_back(object.GetID());
        } else if (shape == "mixed1000" && index % 10 == 0) object.AddComponent<scene::SkinnedMeshRenderer>().model = &model;
        else object.AddComponent<scene::MeshRenderer>().mesh = &mesh;
    }
    const auto update = [&](int state) {
        for (size_t index = 0; index < (std::max)(size_t{1}, count / 100); ++index) {
            auto* object = owner.GetGameObject(entities[index]);
            object->transform.worldPosition.y = state ? 0.25f : -0.25f; object->layer = state ? 1 : 0;
        }
        for (const auto id : terrains) owner.GetComponent<scene::TerrainComponent>(id)->heightData[0] = state ? 1.0f : -1.0f;
    };
    const LayerMask mask = expectBounds ? Layer::Mask(0) : Layer::Nothing;
    const auto compute = [&] { return scene::shadow_preparation_detail::ComputeSceneBounds(owner, mask); };
    Result result(options.samples);
    if (!raw) {
        const auto resolve = [&] {
            return uncachedViews ? compute()
                : scene::shadow_preparation_detail::ResolveSceneBounds(owner, mask);
        };
        std::array<renderer::ShadowBounds, 2> references{};
        if (mutate) update(0);
        references[0] = compute();
        if (mutate) update(1);
        references[1] = compute();
        for (int state = 0; state < 2; ++state)
            result.expected[state] = ViewBoundsHash({references[state], references[state], references[1 - state]});
        result.operationsPerSample = 3;
        for (int iteration = 0; iteration < options.warmup + options.samples; ++iteration) {
            const int state = iteration % 2;
            ++Time::frameCount;
            const auto begin = Clock::now();
            if (mutate) update(state);
            const auto first = Clock::now();
            const auto firstView = resolve();
            const auto secondView = resolve();
            const auto beforeEdit = Clock::now();
            // 同一フレームの 3 ビュー目だけが通知なしの入力変更を見る。
            if (mutate) update(1 - state);
            const auto afterEdit = Clock::now();
            const auto thirdView = resolve();
            const auto end = Clock::now();
            result.Observe(iteration, options, Us(begin, end), Us(first, beforeEdit) + Us(afterEdit, end),
                Us(begin, first) + Us(beforeEdit, afterEdit), ViewBoundsHash({firstView, secondView, thirdView}),
                result.expected[state], firstView.valid == expectBounds && secondView.valid == expectBounds
                    && thirdView.valid == expectBounds);
            result.lastBounds = thirdView;
            result.hasBounds = true;
        }
        Write(output, options, family, name, count, terrains.size() * static_cast<size_t>(side * side), 0, 0, result);
        ++executed;
        return result.valid && result.complete;
    }
    if (mutate) {
        for (int state = 0; state < 2; ++state) { update(state); result.expected[state] = BoundsHash(compute()); }
        update(1);
    } else result.expected.fill(BoundsHash(compute()));
    for (int iteration = 0; iteration < options.warmup + options.samples; ++iteration) {
        const int state = iteration % 2;
        const auto begin = Clock::now();
        if (mutate) update(state);
        const auto updated = Clock::now();
        const auto bounds = compute();
        const auto end = Clock::now();
        result.Observe(iteration, options, Us(begin, end), Us(updated, end), Us(begin, updated), BoundsHash(bounds),
            result.expected[mutate ? state : 0], bounds.valid);
        result.lastBounds = bounds; result.hasBounds = true;
    }
    Write(output, options, family, name, count, terrains.size() * static_cast<size_t>(side * side), 0, 0, result);
    ++executed; return result.valid && result.complete;
}
}
}

int main(int argc, char** argv)
{
    using namespace fbzz::bench;
    Options options;
    if (!Parse(argc, argv, options)) {
        std::cerr << "Usage: FBZZScenePreparationBench [--family all|profiler_tree|shadow_bounds|shadow_views|hierarchy] [--case all|name] [--samples 1..256] [--warmup 0..256] [--label text] [--output path]\n";
        return 2;
    }
    fbzz::profiler::Profiler::SetEnabled(false);
    std::ofstream file;
    if (!options.output.empty()) { file.open(options.output, std::ios::binary); if (!file) return 2; }
    std::ostream& output = options.output.empty() ? std::cout : file;
    const char* workset = std::getenv("FBZZ_RENDERER_WORKSET_MODE");
    output << std::setprecision(17) << "{\"kind\":\"metadata\",\"label\":" << Quote(options.label)
        << ",\"config\":" << Quote(FBZZ_SCENE_PREPARATION_CONFIG)
        << ",\"clock\":\"steady_clock\",\"percentile\":\"nearest_rank\",\"unit\":\"microseconds\",\"cpuOnly\":true"
        << ",\"treeDestructionTimed\":false,\"validationTimed\":false,\"sceneCreationTimed\":false,\"mutationTimedInTotalOnly\":true"
        << ",\"labelSelectsImplementation\":false,\"worksetEnvironment\":" << Quote(workset ? workset : "unset")
        << ",\"memoryBytes\":null,\"allocationCount\":null}\n";
    size_t executed = 0;
    bool valid = RunTrees(options, output, executed);
    valid = RunRootSiblingRanks(options, output, executed) && valid;
    valid = RunHierarchySync(options, output, executed, false) && valid;
    valid = RunHierarchySync(options, output, executed, true) && valid;
    for (const std::string shape : {"static10000", "mixed1000", "terrain16x129"}) {
        for (const auto workload : {ShadowWorkload::RAW_STATIONARY, ShadowWorkload::RAW_DIRECT_UPDATES,
                 ShadowWorkload::EMPTY_MASK, ShadowWorkload::ELIGIBLE_VIEWS, ShadowWorkload::DIRECT_EDIT_VIEWS})
            valid = RunBoundsCase(options, output, shape, workload, executed) && valid;
        for (const auto workload : {ShadowWorkload::EMPTY_MASK, ShadowWorkload::ELIGIBLE_VIEWS, ShadowWorkload::DIRECT_EDIT_VIEWS})
            valid = RunBoundsCase(options, output, shape, workload, executed, true) && valid;
    }
    output << "{\"kind\":\"summary\",\"cases\":" << executed << ",\"valid\":" << (valid && executed != 0 ? "true" : "false") << "}\n";
    output.flush();
    return output.good() && valid && executed != 0 ? 0 : 1;
}
