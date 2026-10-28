/// @file    MaterialVisibilityMeasure.cpp
/// @brief   Compare production per-slot and prepared-object material demand using identical immutable CPU inputs.
/// @author  Hasegawa Jin
/// @date    2026-10-25
#include "../../Engine/src/Scene/Systems/MaterialDemandTrace.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::bench {
namespace {
namespace demand = scene::material_demand_detail;
using Clock = std::chrono::steady_clock;
constexpr uint64_t HASH_SEED = 14695981039346656037ULL;
constexpr uint64_t HASH_FACTOR = 1099511628211ULL;

struct Options {
    std::string selected = "all", label = "unlabelled", output;
    int warmup = 10, samples = 201;
};

bool Parse(int argc, char** argv, Options& options)
{
    for (int index = 1; index < argc; ++index) {
        if (index + 1 == argc) return false;
        const std::string_view key = argv[index++];
        const char* value = argv[index];
        if (key == "--case") options.selected = value;
        else if (key == "--label") options.label = value;
        else if (key == "--output") options.output = value;
        else if (key == "--samples" || key == "--warmup") {
            char* end = nullptr;
            const long number = std::strtol(value, &end, 10);
            if (end == value || *end != 0 || number < (key == "--samples" ? 1 : 0) || number > 256) return false;
            (key == "--samples" ? options.samples : options.warmup) = static_cast<int>(number);
        } else return false;
    }
    return true;
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

/// @note Shared slot prototypes avoid allocating N*M large mesh items; both algorithms visit the same N*M logical slots.
/// @note No resource is resolved: the predicates only inspect the nonzero CPU handle values.
struct Input {
    Input(size_t objectCount, size_t slotCount)
        : frustum(math::Frustum::FromViewProjection(math::Matrix4::Perspective(1.5707963268f, 1.0f, 0.1f, 250.0f))),
          objects(objectCount), items(slotCount)
    {
        view.culling.frustum = &frustum;
        view.culling.projectionScaleY = 1.0f;
        view.culling.smallObjectScreenHeight = 0.001f;
        view.maxDistance = 200.0f;
        view.boundsPadding = 0.05f;
        view.layerDistances.fill(200.0f);
        view.hasLayerDistances = true;
        view.staticGpuFrustumPossible = true;
        view.directionalShadowPotential = view.punctualShadowPotential = true;
        view.reflectionProbePotential = view.lightProbePotential = view.rayPotential = true;
        view.consumerScopeCertified = view.materialCoverageKnown = true;
        for (size_t index = 0; index < objects.size(); ++index) {
            auto& object = objects[index];
            object.colorEligible = true;
            object.skinned = (index / 2) % 4 == 0;
            object.previousSkinningValid = index % 3 != 0;
            object.layer = static_cast<uint32_t>(index % 32);
            object.itemCount = static_cast<uint32_t>(slotCount);
            object.boundsCenter = {index % 2 == 0 ? 0.0f : 100.0f, 0.0f, 10.0f + static_cast<float>(index % 20)};
            object.boundsRadius = 0.5f;
        }
        for (size_t slot = 0; slot < items.size(); ++slot) {
            auto& item = items[slot];
            item.vertexBuffer = item.skinningVertexBuffer = {1, 1};
            item.indexBuffer = slot % 11 == 10 ? renderer::ResourceHandle<renderer::BufferTag>{}
                : renderer::ResourceHandle<renderer::BufferTag>{2, 1};
            item.slotVisible = slot % 13 != 12;
            item.material.capabilities.blend = slot % 4 == 3
                ? renderer::BlendMode::ALPHA_BLEND : renderer::BlendMode::OPAQUE_BLEND;
        }
    }

    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    math::Frustum frustum;
    demand::View view;
    std::vector<renderer::RenderObject> objects;
    std::vector<renderer::RenderMeshItem> items;
};

#if defined(_MSC_VER)
__declspec(noinline)
#endif
void Legacy(const Input& input, std::vector<demand::Result>& output)
{
    size_t index = 0;
    for (const auto& object : input.objects)
        for (const auto& item : input.items)
            output[index++] = demand::Evaluate(input.view, object, item);
}

#if defined(_MSC_VER)
__declspec(noinline)
#endif
void Prepared(const Input& input, std::vector<demand::Result>& output)
{
    const demand::PreparedView preparedView(input.view);
    size_t index = 0;
    for (const auto& object : input.objects) {
        demand::ObjectEvaluator evaluator(preparedView, object);
        for (const auto& item : input.items)
            output[index++] = evaluator.Evaluate(item);
    }
}

/// @note Output remains observable through field-by-field validation; fences prevent moving its stores across the clock boundaries.
template<typename Operation>
double Time(Operation&& operation)
{
    std::atomic_signal_fence(std::memory_order_seq_cst);
    const auto begin = Clock::now();
    std::atomic_signal_fence(std::memory_order_seq_cst);
    operation();
    std::atomic_signal_fence(std::memory_order_seq_cst);
    const auto end = Clock::now();
    return std::chrono::duration<double, std::micro>(end - begin).count();
}

using Digest = std::array<uint64_t, 5>;
constexpr std::array<const char*, 5> FIELD_NAMES{
    "cameraEvaluated", "cameraResult", "numericUnproven", "reasons", "matchesView"
};

Digest Checksums(const std::vector<demand::Result>& results)
{
    Digest digest;
    digest.fill(HASH_SEED);
    for (const auto& result : results) {
        const std::array<uint64_t, 5> fields{static_cast<uint64_t>(result.cameraEvaluated),
            static_cast<uint64_t>(result.cameraResult), static_cast<uint64_t>(result.numericUnproven),
            result.reasons, static_cast<uint64_t>(result.matchesView)};
        for (size_t field = 0; field < fields.size(); ++field)
            digest[field] = (digest[field] ^ fields[field]) * HASH_FACTOR;
    }
    return digest;
}

size_t Mismatches(const std::vector<demand::Result>& legacy, const std::vector<demand::Result>& prepared)
{
    size_t mismatches = 0;
    for (size_t index = 0; index < legacy.size(); ++index) {
        const auto& first = legacy[index];
        const auto& second = prepared[index];
        mismatches += first.cameraEvaluated != second.cameraEvaluated || first.cameraResult != second.cameraResult
            || first.numericUnproven != second.numericUnproven || first.reasons != second.reasons
            || first.matchesView != second.matchesView;
    }
    return mismatches;
}

double Percentile(std::vector<double> values, double quantile)
{
    std::sort(values.begin(), values.end());
    return values[static_cast<size_t>(std::ceil(quantile * static_cast<double>(values.size()))) - 1];
}

void WriteSeries(std::ostream& output, const char* name, const std::vector<double>& values)
{
    output << ',' << Quote(name) << ":{\"p50Us\":" << Percentile(values, 0.50)
        << ",\"p95Us\":" << Percentile(values, 0.95) << ",\"p99Us\":" << Percentile(values, 0.99) << ",\"rawUs\":[";
    for (size_t index = 0; index < values.size(); ++index) { if (index) output << ','; output << values[index]; }
    output << "]}";
}

void WriteDigest(std::ostream& output, const char* name, const Digest& digest)
{
    output << ',' << Quote(name) << ":{";
    for (size_t field = 0; field < digest.size(); ++field) {
        if (field) output << ',';
        output << Quote(FIELD_NAMES[field]) << ':' << Quote(std::to_string(digest[field]));
    }
    output << '}';
}

bool Run(const Options& options, std::ostream& output, size_t objectCount, size_t slots, size_t& executed)
{
    const std::string name = "mixed_n" + std::to_string(objectCount) + "_m" + std::to_string(slots);
    if (options.selected != "all" && options.selected != name) return true;
    const Input input(objectCount, slots);
    const size_t resultCount = objectCount * slots;
    std::vector<demand::Result> legacy(resultCount), prepared(resultCount);
    Legacy(input, legacy);
    Prepared(input, prepared);
    const auto expected = Checksums(legacy);
    Digest legacyDigest = expected, preparedDigest = Checksums(prepared);
    size_t mismatches = Mismatches(legacy, prepared);
    bool valid = mismatches == 0 && preparedDigest == expected;
    size_t visible = 0, offscreen = 0;
    for (const auto& object : input.objects) {
        const auto result = demand::Evaluate(input.view, object, input.items.front());
        visible += result.cameraEvaluated && result.cameraResult == renderer::VisibilityResult::VISIBLE;
        offscreen += result.cameraEvaluated && result.cameraResult == renderer::VisibilityResult::FRUSTUM_CULLED;
    }
    valid &= visible == objectCount / 2 && offscreen == objectCount / 2;
    std::vector<double> legacyUs, preparedUs;
    std::vector<uint8_t> legacyFirst;
    legacyUs.reserve(options.samples); preparedUs.reserve(options.samples); legacyFirst.reserve(options.samples);
    for (int iteration = 0; iteration < options.warmup + options.samples; ++iteration) {
        double oldTime = 0, newTime = 0;
        const bool oldFirst = iteration % 2 == 0;
        const auto oldOperation = [&] { oldTime = Time([&] { Legacy(input, legacy); }); };
        const auto newOperation = [&] { newTime = Time([&] { Prepared(input, prepared); }); };
        if (oldFirst) { oldOperation(); newOperation(); }
        else { newOperation(); oldOperation(); }
        legacyDigest = Checksums(legacy); preparedDigest = Checksums(prepared);
        const size_t currentMismatches = Mismatches(legacy, prepared);
        mismatches += currentMismatches;
        valid &= currentMismatches == 0 && legacyDigest == expected && preparedDigest == expected;
        if (iteration < options.warmup) continue;
        legacyUs.push_back(oldTime); preparedUs.push_back(newTime); legacyFirst.push_back(oldFirst ? 1 : 0);
    }
    output << "{\"kind\":\"case\",\"case\":" << Quote(name) << ",\"label\":" << Quote(options.label)
        << ",\"N\":" << objectCount << ",\"M\":" << slots << ",\"evaluatedSlots\":" << resultCount
        << ",\"storedSlotPrototypes\":" << input.items.size() << ",\"visibleObjects\":" << visible << ",\"offscreenObjects\":" << offscreen
        << ",\"warmup\":" << options.warmup << ",\"samples\":" << options.samples
        << ",\"valid\":" << (valid ? "true" : "false") << ",\"mismatchedResults\":" << mismatches
        << ",\"memoryBytes\":null,\"allocationCount\":null,\"legacyFirst\":[";
    for (size_t index = 0; index < legacyFirst.size(); ++index) { if (index) output << ','; output << static_cast<int>(legacyFirst[index]); }
    output << ']';
    WriteDigest(output, "expectedChecksums", expected);
    WriteDigest(output, "legacyChecksums", legacyDigest);
    WriteDigest(output, "preparedChecksums", preparedDigest);
    WriteSeries(output, "legacy", legacyUs); WriteSeries(output, "prepared", preparedUs);
    output << "}\n";
    ++executed;
    return valid;
}
} /// @note namespace
} /// @note namespace fbzz::bench

int main(int argc, char** argv)
{
    using namespace fbzz::bench;
    Options options;
    if (!Parse(argc, argv, options)) {
        std::cerr << "Usage: FBZZMaterialVisibilityBench [--case all|mixed_n1000_m1|mixed_n1000_m8|mixed_n1000_m32|mixed_n10000_m1|mixed_n10000_m8|mixed_n10000_m32] [--warmup 0..256] [--samples 1..256] [--label text] [--output path]\n";
        return 2;
    }
    std::ofstream file;
    if (!options.output.empty()) { file.open(options.output, std::ios::binary); if (!file) return 2; }
    std::ostream& output = options.output.empty() ? std::cout : file;
    output << std::setprecision(17) << "{\"kind\":\"metadata\",\"label\":" << Quote(options.label)
        << ",\"config\":" << Quote(FBZZ_MATERIAL_VISIBILITY_CONFIG)
        << ",\"clock\":\"steady_clock\",\"unit\":\"microseconds\",\"percentile\":\"nearest_rank\",\"cpuOnly\":true"
        << ",\"legacy\":\"production material_demand_detail::Evaluate\",\"prepared\":\"production PreparedView + ObjectEvaluator\""
        << ",\"labelSelectsImplementation\":false,\"order\":\"alternating_each_pair\",\"inputGenerationTimed\":false"
        << ",\"viewPreparationTimed\":true,\"objectPreparationTimed\":true,\"outputWritesTimed\":true,\"validationTimed\":false"
        << ",\"immutableInputs\":true,\"sharedSlotPrototypes\":true,\"memoryBytes\":null,\"allocationCount\":null}\n";
    bool valid = true;
    size_t executed = 0;
    for (const size_t objects : {size_t{1000}, size_t{10000}})
        for (const size_t slots : {size_t{1}, size_t{8}, size_t{32}})
            valid = Run(options, output, objects, slots, executed) && valid;
    output << "{\"kind\":\"summary\",\"cases\":" << executed << ",\"valid\":" << (valid && executed != 0 ? "true" : "false") << "}\n";
    output.flush();
    return output.good() && valid && executed != 0 ? 0 : 1;
}

