/// @file    StaticMeshReuseMeasure.cpp
/// @brief   静的Mesh抽出の無変更・部分変更・全変更・寿命変更を同じ入力で計測する。
/// @author  Hasegawa Jin
/// @date    2026-10-23
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Core/Profiler/Profiler.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Systems/RenderSceneExtractor.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/Script.hpp>
#include "../../Engine/src/Scene/Managed/ManagedRenderComponentServices.hpp"
#include "../../Engine/src/Scene/Managed/ManagedServicesBridge.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::bench {
namespace {
void HashWord(uint64_t& hash, uint32_t value) { hash = (hash ^ value) * 1099511628211ULL; }
void HashFloat(uint64_t& hash, float value) { HashWord(hash, std::bit_cast<uint32_t>(value)); }
uint64_t HashGeometry(const renderer::RenderScene& snapshot)
{
    uint64_t hash = 14695981039346656037ULL;
    for (const auto& object : snapshot.Objects()) {
        HashWord(hash, object.sourceIndex); HashWord(hash, object.sourceGeneration);
        HashWord(hash, object.firstItem); HashWord(hash, object.itemCount); HashWord(hash, object.layer);
        HashWord(hash, object.castShadows); HashWord(hash, object.lodVisible);
        HashFloat(hash, object.lodDither);
        for (const auto* matrix : {&object.world, &object.worldInvTranspose, &object.previousWorld})
            for (const auto& row : matrix->m) for (float value : row) HashFloat(hash, value);
        for (const auto& v : {object.sortPosition, object.boundsCenter}) {
            HashFloat(hash, v.x); HashFloat(hash, v.y); HashFloat(hash, v.z);
        }
        HashFloat(hash, object.boundsRadius);
    }
    for (const auto& item : snapshot.items) {
        HashWord(hash, item.objectIndex); HashWord(hash, item.vertexCount); HashWord(hash, item.indexCount);
        HashWord(hash, item.vertexBuffer.id); HashWord(hash, item.vertexBuffer.gen);
        HashWord(hash, item.indexBuffer.id); HashWord(hash, item.indexBuffer.gen);
        HashWord(hash, item.vertexStride); HashWord(hash, item.reliableOccluder);
        HashFloat(hash, item.boundsCenter.x); HashFloat(hash, item.boundsCenter.y);
        HashFloat(hash, item.boundsCenter.z); HashFloat(hash, item.boundsRadius);
    }
    return hash;
}
}
int Measure(int argc, char** argv)
{
    if (argc != 5 && argc != 6) return 2;
    const int count = std::atoi(argv[1]), samples = std::atoi(argv[3]);
    const int warmup = argc == 6 ? std::atoi(argv[5]) : 120;
    const std::string mode(argv[2]);
    const bool service = mode.starts_with("service-");
    const bool pair = mode == "initial-pair";
    if (count <= 0 || samples <= 0 || warmup < 0 || (mode != "static" && mode != "one-percent" && mode != "all"
        && mode != "churn" && mode != "cold" && !pair && mode != "service-one-percent"
        && mode != "service-all" && mode != "service-churn")) return 2;
    profiler::Profiler::SetEnabled(false);
    scene::Scene scene;
    std::array<renderer::Mesh, 8> meshes;
    for (uint32_t index = 0; index < meshes.size(); ++index) {
        auto& mesh = meshes[index];
        mesh.vertexBuffer = {10 + index, 2}; mesh.indexBuffer = {30 + index, 3};
        mesh.vertexCount = 24; mesh.indexCount = 36;
        mesh.boundsCenter = {0.25f, -0.5f, 0.75f}; mesh.boundsRadius = 1.25f;
        mesh.boundsExtents = {1, 1, 1};
    }
    std::vector<scene::EntityID> entities;
    entities.reserve(count);
    const auto create = [&](int index) {
        auto& object = scene.CreateGameObject("Static mesh measurement");
        object.AddComponent<scene::MeshRenderer>().mesh = &meshes[index % meshes.size()];
        object.transform.position = object.transform.worldPosition = {static_cast<float>(index % 100), static_cast<float>(index / 100), 0};
        object.transform.scale = object.transform.worldScale = {2, -3, 1};
        return object.GetID();
    };
    for (int index = 0; index < count; ++index) entities.push_back(create(index));
    std::vector<double> timings;
    std::vector<uint64_t> hashes;
    scene::Script serviceView;
    const bool previousPlay = scene::Script::IsInPlayMode();
    if (service) scene::Script::SetInPlayMode(true);
    scene::ManagedBridgeRequest request;
    request.op = scene::ManagedBridgeOp::SERVICE_MESH; request.index = 3;
    scene::managed_detail::ManagedServiceResult serviceResult;
    bool valid = true;
    for (int frame = 0; frame < samples + warmup; ++frame) {
        if (mode == "cold" || pair) {
            scene.Clear();
            for (int index = 0; index < count; ++index) entities[index] = create(index);
        }
        const auto updateStart = std::chrono::steady_clock::now();
        const int changed = mode == "all" || mode == "service-all" ? count
            : mode == "one-percent" || mode == "churn" || mode == "service-one-percent" || mode == "service-churn"
                ? (std::max)(1, count / 100) : 0;
        for (int index = 0; index < changed; ++index) {
            if (mode == "churn" || mode == "service-churn") {
                valid = scene.DestroyGameObject(entities[index]) && valid;
                entities[index] = create(index);
            }
            auto* object = scene.GetGameObject(entities[index]);
            object->transform.position.z = object->transform.worldPosition.z = frame % 2 == 0 ? 0.125f : -0.125f;
            if (service) {
                serviceView.SetContext(&scene, object);
                valid = scene::managed_detail::RunRenderComponentServices(scene, serviceView, *object, nullptr,
                    request, frame % 2, {}, {}, serviceResult) == scene::ManagedBridgeStatus::OK && valid;
            }
        }
        if (service) serviceView.SetContext(nullptr, nullptr);
        const auto start = std::chrono::steady_clock::now();
        const uint64_t stamp = static_cast<uint64_t>(frame + 1) * (pair ? 2 : 1);
        if (pair) { const auto first = scene::ExtractRenderSceneGeometry(scene, stamp - 1); valid = valid && first.items.size() == static_cast<size_t>(count); }
        const auto snapshot = scene::ExtractRenderSceneGeometry(scene, stamp);
        const auto end = std::chrono::steady_clock::now();
        valid = valid && snapshot.Objects().size() == static_cast<size_t>(count) && snapshot.items.size() == static_cast<size_t>(count);
        if (frame >= warmup) {
            timings.push_back(std::chrono::duration<double, std::milli>(end - (service ? updateStart : start)).count());
            hashes.push_back(HashGeometry(snapshot));
        }
    }
    if (service) scene::Script::SetInPlayMode(previousPlay);
    HMODULE engine = GetModuleHandleW(L"FBZZEngine.dll");
    wchar_t modulePath[MAX_PATH]{};
    if (!engine || !GetModuleFileNameW(engine, modulePath, MAX_PATH)) return 3;
    std::FILE* output = nullptr;
    if (fopen_s(&output, argv[4], "wb") != 0 || !output) return 4;
    const std::unique_ptr<std::FILE, decltype(&std::fclose)> ownedOutput(output, &std::fclose);
    std::fprintf(output, "{\"valid\":%s,\"objects\":%d,\"mode\":\"%s\",\"warmup\":%d,\"samples\":%d,\"boundary\":\"%s\",\"extractionsPerSample\":%d,\"enginePath\":\"",
        valid ? "true" : "false", count, mode.c_str(), warmup, samples,
        service ? "native-service-and-extraction" : "extraction", pair ? 2 : 1);
    for (wchar_t ch : std::wstring(modulePath)) {
        if (ch == L'\\') std::fputs("/", output);
        else if (ch < 128) std::fputc(static_cast<char>(ch), output);
        else valid = false;
    }
    std::fputs("\",\"frames\":[", output);
    for (size_t index = 0; index < timings.size(); ++index)
        std::fprintf(output, "%s{\"ms\":%.9f,\"hash\":\"%016llx\"}", index ? "," : "", timings[index], static_cast<unsigned long long>(hashes[index]));
    std::fputs("]}\n", output);
    return valid ? 0 : 5;
}
}
int main(int argc, char** argv) { return fbzz::bench::Measure(argc, argv); }
