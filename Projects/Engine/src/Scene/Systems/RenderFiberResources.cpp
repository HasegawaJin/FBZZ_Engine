/// @file    RenderFiberResources.cpp
/// @brief   繊維の GPU ジオメトリ・流れのバッファとキャッシュの寿命。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderFiberExtraction.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/FiberGeometry.hpp>
#include "RenderPasses/Geometry/FlowFieldGpu.hpp"
#include <Graphics/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <bit>
#include <utility>

namespace fbzz::scene::fiber_extraction_detail {

namespace {

/// @note 同じ面の形状設定違いも一緒に返す。先頭 4 語の範囲検索で、全パッチの根元を毎回走査しない。
void ReleaseFiberTerrainMesh(FiberResources& cache, renderer::ResourceManager& resources, const renderer::Mesh& mesh)
{
    const std::array<uint32_t, 8> first{mesh.vertexBuffer.id, mesh.vertexBuffer.gen,
        mesh.indexBuffer.id, mesh.indexBuffer.gen, 0, 0, 0, 0};
    for (auto it = cache.m_fins.lower_bound(first); it != cache.m_fins.end();) {
        if (!std::equal(first.begin(), first.begin() + 4, it->first.begin())) break;
        resources.Release(it->second.m_vertices);
        resources.Release(it->second.m_indices);
        resources.Release(it->second.m_blades);
        it = cache.m_fins.erase(it);
    }
    resources.Release(mesh.vertexBuffer);
    resources.Release(mesh.indexBuffer);
}

}

FiberResources& GetFiberResources(renderer::ResourceManager& resources)
{
    /// @note 複数の Renderer を交互に抽出しても、既に返した別所有者の非所有ハンドルを失効させない。
    static std::map<uint64_t, FiberResources> resourceCaches;
    std::erase_if(resourceCaches, [](const auto& owner) { return !renderer::ResourceManager::FindOwner(owner.first); });
    auto& cache = resourceCaches[resources.GetInstanceId()];
    if (cache.m_ownerIdentity != resources.GetInstanceId() || cache.m_resetVersion != resources.GetResetVersion()) {
        /// @note Reset が旧 GPU 実体を返却済みなので、失効した参照を新しい世代へ持ち越さない。
        cache = FiberResources{};
        cache.m_owner = &resources;
        cache.m_ownerIdentity = resources.GetInstanceId();
        cache.m_resetVersion = resources.GetResetVersion();
    }
    /// @note 世代切れの掃除は 1 フレーム 1 回。影のカスケード・面ごとに繰り返さない。
    const uint64_t frame = resources.FrameStamp();
    if (cache.m_sweptFrame == frame) return cache;
    cache.m_sweptFrame = frame;
    for (auto it = cache.m_placements.begin(); it != cache.m_placements.end();) {
        if (Time::frameCount > it->second.seen && Time::frameCount-it->second.seen > 120) {
            ReleaseFiberPlacement(it->second,resources);
            it = cache.m_placements.erase(it);
        } else ++it;
    }
    for (auto it = cache.m_materials.begin(); it != cache.m_materials.end();) {
        if (Time::frameCount > it->second.m_frame && Time::frameCount - it->second.m_frame > 120) {
            auto& buffer = it->second.m_appearance.material.paramsBuffer;
            if (buffer.IsValid()) resources.Release(buffer);
            buffer = {};
            it = cache.m_materials.erase(it);
        }
        else ++it;
    }
    for (auto it=cache.m_terrains.begin();it!=cache.m_terrains.end();) {
        SweepFiberTerrain(it->second, frame, [&](const renderer::Mesh& mesh) { ReleaseFiberTerrainMesh(cache, resources, mesh); });
        if (FiberTerrainExpired(frame, it->second.m_seen)) it=cache.m_terrains.erase(it);
        else ++it;
    }
    for (auto it = cache.m_flows.begin(); it != cache.m_flows.end();) {
        if (Time::frameCount > it->second.m_frame && Time::frameCount - it->second.m_frame > 120) it = cache.m_flows.erase(it);
        else ++it;
    }
    for (auto it=cache.m_contactStates.begin();it!=cache.m_contactStates.end();) {
        if (Time::frameCount>it->second.m_frame && Time::frameCount-it->second.m_frame>120)
            it=cache.m_contactStates.erase(it);
        else ++it;
    }
    for (auto it = cache.m_fins.begin(); it != cache.m_fins.end();) {
        const auto& entry = it->second;
        const auto& key = it->first;
        const bool expired = frame > entry.m_seen && frame - entry.m_seen > (entry.m_terrain ? 2u : 120u);
        /// @note 地形パッチが元バッファを所有し、ReleaseFiberTerrainMesh は返却前にフィンを登録解除する。ここで照会すると保留中の描画処理に同期してしまう。
        const bool staleSource = !expired && !entry.m_terrain
            && (!resources.Get(renderer::ResourceHandle<renderer::BufferTag>{key[0], key[1], resources.GetInstanceId()})
                || !resources.Get(renderer::ResourceHandle<renderer::BufferTag>{key[2], key[3], resources.GetInstanceId()}));
        if (expired || staleSource) {
            resources.Release(it->second.m_vertices);
            resources.Release(it->second.m_indices);
            resources.Release(it->second.m_blades);
            it = cache.m_fins.erase(it);
        } else ++it;
    }
    return cache;
}

const FiberFinBuffers& GetFiberFinBuffers(FiberResources& cache, renderer::ResourceManager& resources,
                                          const renderer::Mesh& mesh, const FiberComponent& fiber, float bladeDensity,
                                          bool blades, bool densityFromAlpha, bool terrain)
{
    const std::array<uint32_t,8> key{mesh.vertexBuffer.id,mesh.vertexBuffer.gen,mesh.indexBuffer.id,mesh.indexBuffer.gen,
        blades ? 1u : 0u, blades ? std::bit_cast<uint32_t>(bladeDensity) : 0u,
        blades ? std::bit_cast<uint32_t>(fiber.m_bladeWidth) : 0u, blades ? static_cast<uint32_t>(fiber.m_seed) : 0u};
    const auto [it, inserted] = cache.m_fins.try_emplace(key);
    auto& meshBuffers = it->second;
    meshBuffers.m_seen=resources.FrameStamp();
    meshBuffers.m_terrain = terrain;
    if (!inserted) return meshBuffers;
    if (blades) {
        /// @note 頂点は GPU で SV_VertexID から組み立てる。CPU で作って送るのは 1 葉 64 バイトの根元だけ。
        std::vector<renderer::FiberBladeRoot> roots;
        if (!renderer::BuildFiberBlades(mesh, bladeDensity, fiber.m_bladeWidth, static_cast<uint32_t>(fiber.m_seed), roots,
                densityFromAlpha)) {
            core::Logger::Warn("Fiber: blade roots rejected invalid input or budget above 32768; reduce density/patch size");
            return meshBuffers;
        }
        /// @note 面積×密度が 1 未満なら葉は 0 本。黙って描かない。
        if (roots.empty()) return meshBuffers;
        meshBuffers.m_blades = resources.CreateGpuLocalStructuredBuffer(roots.data(), static_cast<uint32_t>(roots.size()),
            sizeof(renderer::FiberBladeRoot));
        meshBuffers.m_bladeCount = meshBuffers.m_blades.IsValid() ? static_cast<uint32_t>(roots.size()) : 0;
        return meshBuffers;
    }
    renderer::FiberFinMesh fins;
    if (!renderer::BuildFiberFins(mesh, fins)) {
        core::Logger::Warn("Fiber: geometry rejected invalid input, unsupported surface or blade budget above 32768; reduce density/patch size");
        return meshBuffers;
    }
    if (fins.m_indices.empty()) {
        core::Logger::Warn("Fiber: Fin requires CPU triangle data; this mesh has no usable triangles");
        return meshBuffers;
    }
    meshBuffers.m_vertices = resources.CreateVertexBuffer(fins.m_vertices.data(),
        fins.m_vertices.size() * sizeof(renderer::FiberFinVertex), sizeof(renderer::FiberFinVertex));
    meshBuffers.m_indices = resources.CreateIndexBuffer(fins.m_indices.data(), static_cast<uint32_t>(fins.m_indices.size()));
    meshBuffers.m_indexCount = static_cast<uint32_t>(fins.m_indices.size());
    return meshBuffers;
}

/// @brief シーンの FlowField を今フレームぶん詰め、前フレームと連結して VS 用 StructuredBuffer へ送る。
/// @note 連続しないフレーム (初回・描画の空白) は前フレーム = 今フレームとし、流れの変化を速度に出さない。
FiberFlowCache& UpdateFiberFlows(FiberResources& cache, Scene& scene, renderer::ResourceManager& resources)
{
    auto& flowCache = cache.m_flows[scene.GetRenderSceneGeneration()];
    if (flowCache.m_frame == Time::frameCount) return flowCache;
    const bool hasPreviousFrame = flowCache.m_frame != (std::numeric_limits<uint64_t>::max)() && Time::frameCount == flowCache.m_frame + 1;
    flowCache.m_frame = Time::frameCount;
    std::swap(flowCache.m_previous, flowCache.m_current);
    std::swap(flowCache.m_previousPacked, flowCache.m_currentPacked);
    const auto& flowFrame = scene.FlowFrame();
    const auto fields = SelectFlowFields(flowFrame, false);
    flowCache.m_current.assign(fields.begin(), fields.end());
    flowCache.m_currentPacked.clear();
    for (const auto& field : flowCache.m_current) flowCache.m_currentPacked.push_back(PackGpuFlowField(field, resources));
    for (auto& field : flowCache.m_current) field.vectorField = nullptr;
    if (!hasPreviousFrame) {
        flowCache.m_previous = flowCache.m_current;
        flowCache.m_previousPacked = flowCache.m_currentPacked;
    }
    flowCache.m_upload = flowCache.m_currentPacked;
    flowCache.m_upload.insert(flowCache.m_upload.end(), flowCache.m_previousPacked.begin(), flowCache.m_previousPacked.end());
    /// @note 0 本でも 1 要素は借りる。未束縛の SRV を VS が静的に参照しないようにする。
    flowCache.m_buffer = cache.m_flowBuffers.Acquire(resources, (std::max)(flowCache.m_upload.size(), size_t{ 1 }),
                                                static_cast<uint32_t>(sizeof(GpuFlowField)));
    if (flowCache.m_buffer.IsValid() && !flowCache.m_upload.empty())
        resources.Update(flowCache.m_buffer, flowCache.m_upload.data(), flowCache.m_upload.size() * sizeof(GpuFlowField));
    return flowCache;
}

/// @return 球 (ワールド [m]) に届き、channels が重なる場の本数。0 本なら VS で場を評価しない。
/// @note radius < 0 は境界不明 (スキン) として全本数を返す。
uint32_t CountFiberFlows(const std::vector<ActiveFlowField>& fields, uint32_t channels,
                         const math::Vector3& center, float radius)
{
    if (channels == 0u) return 0;
    for (const auto& field : fields)
        if (FlowReceiver{true, channels}.Intersects(field, center, radius))
            return static_cast<uint32_t>(fields.size());
    return 0;
}

}
