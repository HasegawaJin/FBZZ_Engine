/// @file    PhysicsBroadPhaseMeasure.cpp
/// @brief   BroadPhaseとWorld同期の固定入力を画面なしで比較し、生標本を保存する。
/// @author  Hasegawa Jin
/// @date    2026-10-22
#include <Physics/PhysicsSolver.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/World.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::bench {
namespace {
struct Input {
    std::vector<std::unique_ptr<physics::SphereCollider>> shapes;
    std::vector<std::unique_ptr<physics::RigidBody>> bodies;
    std::vector<physics::ColliderInstance> colliders;
    std::vector<math::Vector3> positions;
};

Input MakeInput(int count, const std::string& mode)
{
    Input input;
    input.colliders.reserve(count);
    for (int index = 0; index < count; ++index) {
        const int group = index / 2;
        const math::Vector3 position{static_cast<float>(group % 64) * 4.0f + (index % 2) * 0.75f,
            static_cast<float>(group / 64) * 4.0f, 0.0f};
        auto shape = std::make_unique<physics::SphereCollider>(0.5f);
        shape->Update(position, math::Quaternion::Identity());
        auto body = std::make_unique<physics::RigidBody>();
        body->SetMass(1.0f);
        body->SetPosition(position);
        if (mode == "static") body->m_isStatic = true;
        if (mode == "sleep") body->Sleep();
        physics::ColliderInstance collider;
        collider.collider = shape.get();
        collider.body = body.get();
        input.colliders.push_back(collider);
        input.positions.push_back(position);
        input.shapes.push_back(std::move(shape));
        input.bodies.push_back(std::move(body));
    }
    return input;
}

void MoveInput(Input& input, const std::string& mode, int step)
{
    const float offset = step % 2 == 0 ? 0.1f : -0.1f;
    const std::size_t count = input.shapes.size();
    std::size_t moving = 0;
    if (mode == "one") moving = 1;
    if (mode == "one-percent") moving = (std::max)(std::size_t{1}, count / 100);
    if (mode == "all" || mode == "shuffle") moving = count;
    for (std::size_t index = 0; index < moving; ++index) {
        auto position = input.positions[index];
        if (mode == "shuffle") {
            const auto group = (index / 2 + static_cast<std::size_t>(step) * 37) % (count / 2);
            position = {static_cast<float>(group % 64) * 4.0f + (index % 2) * 0.75f,
                static_cast<float>(group / 64) * 4.0f, 0.0f};
        } else {
            position.z += offset;
        }
        input.shapes[index]->Update(position, math::Quaternion::Identity());
    }
    if (mode == "churn") input.colliders.back().collider = step % 2 == 0 ? nullptr : input.shapes.back().get();
}

bool ValidatePairs(const Input& input, const std::vector<physics::CollisionPair>& pairs,
                   const std::string& mode, int step)
{
    const auto expected = mode == "static" || mode == "sleep" ? 0u : input.shapes.size() / 2
        - (mode == "churn" && step % 2 == 0 ? 1u : 0u);
    if (pairs.size() != expected) return false;
    std::vector<unsigned char> seen(input.shapes.size() / 2, 0);
    for (const auto& pair : pairs) {
        const auto first = static_cast<std::size_t>(pair.colliderA - input.colliders.data());
        const auto second = static_cast<std::size_t>(pair.colliderB - input.colliders.data());
        if (first >= input.colliders.size() || second >= input.colliders.size() || first == second
            || first / 2 != second / 2 || seen[first / 2]++) return false;
    }
    return true;
}

struct Result {
    std::string name;
    std::vector<double> samples;
    std::size_t lastPairCount = 0;
};

bool MeasureBroadPhase(int count, const std::string& mode, int warmup, int steps, int repeats,
                       Result& result)
{
    result.name = "broadphase/" + std::to_string(count) + "/" + mode;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        auto input = MakeInput(count, mode);
        physics::PhysicsSolver solver;
        std::vector<physics::CollisionPair> pairs;
        pairs.reserve(count);
        for (int step = 0; step < warmup + steps; ++step) {
            MoveInput(input, mode, step);
            pairs.clear();
            if (mode == "cold") solver = physics::PhysicsSolver{};
            const auto begin = std::chrono::steady_clock::now();
            solver.BroadPhase(input.colliders, pairs, {});
            const auto end = std::chrono::steady_clock::now();
            if (!ValidatePairs(input, pairs, mode, step)) return false;
            if (step >= warmup) result.samples.push_back(
                std::chrono::duration<double, std::micro>(end - begin).count());
        }
        result.lastPairCount = pairs.size();
    }
    return true;
}

bool MeasureWorld(int count, int warmup, int steps, int repeats, Result& result)
{
    result.name = "world-sync-step/" + std::to_string(count) + "/free-motion";
    for (int repeat = 0; repeat < repeats; ++repeat) {
        auto input = MakeInput(count, "all");
        physics::World world;
        world.SetGravity(math::Vector3::ZERO);
        std::vector<physics::BodyHandle> bodies(count);
        std::vector<physics::ColliderHandle> shapes(count);
        for (int index = 0; index < count; ++index) {
            input.bodies[index]->SetPosition(input.positions[index] * 4.0f);
            input.bodies[index]->SetVelocity({0.0f, 0.0f, 1.0f});
        }
        for (int step = 0; step < warmup + steps; ++step) {
            const auto begin = std::chrono::steady_clock::now();
            world.BeginSceneSync();
            for (int index = 0; index < count; ++index) {
                bodies[index] = world.SyncBody(bodies[index], input.bodies[index].get());
                shapes[index] = world.SyncCollider(shapes[index], input.colliders[index]);
            }
            world.EndSceneSync();
            world.Step(1.0f / 60.0f);
            const auto end = std::chrono::steady_clock::now();
            if (step >= warmup) result.samples.push_back(
                std::chrono::duration<double, std::micro>(end - begin).count());
        }
        for (int index = 0; index < count; ++index) {
            const auto position = input.bodies[index]->GetPosition();
            if (!std::isfinite(position.z) || position.z <= 0.0f) return false;
        }
    }
    return true;
}

double Percentile(std::vector<double> values, double fraction)
{
    std::sort(values.begin(), values.end());
    const auto rank = static_cast<std::size_t>(std::ceil(fraction * values.size()));
    return values[(std::max)(std::size_t{1}, rank) - 1];
}
} /// @note namespace
} /// @note namespace fbzz::bench

int main(int argc, char** argv)
{
    using namespace fbzz::bench;
    const char* output = nullptr;
    int warmup = 40;
    int steps = 120;
    int repeats = 3;
    for (int index = 1; index + 1 < argc; index += 2) {
        const std::string option = argv[index];
        if (option == "--json") output = argv[index + 1];
        else if (option == "--warmup") warmup = std::atoi(argv[index + 1]);
        else if (option == "--steps") steps = std::atoi(argv[index + 1]);
        else if (option == "--repeats") repeats = std::atoi(argv[index + 1]);
        else return 2;
    }
    if (!output || warmup < 1 || steps < 1 || repeats < 1) return 2;
    std::vector<Result> results;
    for (int count : {256, 5000}) {
        for (const std::string mode : {"unchanged", "one", "one-percent", "all", "shuffle", "churn", "static", "sleep", "cold"}) {
            Result result;
            if (!MeasureBroadPhase(count, mode, warmup, steps, repeats, result)) {
                std::fprintf(stderr, "INVALID %s\n", result.name.c_str());
                return 1;
            }
            results.push_back(std::move(result));
        }
    }
    for (int count : {256, 1024}) {
        Result result;
        if (!MeasureWorld(count, warmup, steps, repeats, result)) return 1;
        results.push_back(std::move(result));
    }
    std::FILE* file = nullptr;
    if (fopen_s(&file, output, "wb") != 0 || !file) return 2;
    std::fprintf(file, "{\"schema\":\"fbzz-broadphase-measure/1\",\"warmup\":%d,\"steps\":%d,\"repeats\":%d,\"results\":[", warmup, steps, repeats);
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& result = results[index];
        const double median = Percentile(result.samples, 0.5);
        const double p95 = Percentile(result.samples, 0.95);
        std::printf("%s median=%.3f p95=%.3f us\n", result.name.c_str(), median, p95);
        std::fprintf(file, "%s{\"name\":\"%s\",\"validated\":true,\"unit\":\"us/call\",\"median\":%.9f,\"p95\":%.9f,\"lastPairCount\":%zu,\"samples\":[",
            index ? "," : "", result.name.c_str(), median, p95, result.lastPairCount);
        for (std::size_t sample = 0; sample < result.samples.size(); ++sample)
            std::fprintf(file, "%s%.9f", sample ? "," : "", result.samples[sample]);
        std::fprintf(file, "]}");
    }
    std::fprintf(file, "]}\n");
    return std::fclose(file) == 0 ? 0 : 2;
}
