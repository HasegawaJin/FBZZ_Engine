/// @file    RayGeometryCache.cpp
/// @brief   版付き形状の AS 構築と TLAS・ヒット表の不可分な交換。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#include <Graphics/RayTracing/RayGeometryCache.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <algorithm>

namespace fbzz::renderer {
namespace {
bool SameInstances(const std::vector<RayInstance>& left, const std::vector<RayInstance>& right)
{
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        const auto& a = left[i];
        const auto& b = right[i];
        if (a.bottomLevel != b.bottomLevel || a.instanceId != b.instanceId || a.mask != b.mask
            || a.doubleSided != b.doubleSided || a.frontCounterClockwise != b.frontCounterClockwise)
            return false;
        for (uint32_t row = 0; row < 4; ++row)
            for (uint32_t col = 0; col < 4; ++col)
                if (a.transform.m[row][col] != b.transform.m[row][col]) return false;
    }
    return true;
}
RenderGraph::ResourceDesc BufferDescription(const IBuffer& buffer)
{
    RenderGraph::ResourceDesc desc;
    desc.kind = RenderGraph::ResourceKind::Buffer;
    desc.external = true;
    desc.transient = false;
    desc.allowAliasing = false;
    desc.byteSize = buffer.GetSize();
    desc.stride = buffer.GetStride();
    return desc;
}
RenderGraph::ResourceDesc AsDescription(bool external)
{
    RenderGraph::ResourceDesc desc;
    desc.kind = RenderGraph::ResourceKind::AccelerationStructure;
    desc.external = external;
    desc.transient = false;
    desc.allowAliasing = false;
    return desc;
}
} /// @note namespace

RaySceneGpu RayGeometryCache::Prepare(const RayScene& scene, RenderPassContext& context)
{
    RaySceneGpu result;
    auto& resources = context.resources;
    auto& renderer = context.renderer;
    if (!renderer.GetCapabilities().inlineRayQuery || !scene.sceneGeneration) return result;
    const auto frameStamp = context.frameStamp;
    Prune(resources, frameStamp);
    if (scene.instances.empty()) { result.ready = true; return result; }
    if (scene.instances.size() > 0x1000000u) return result;

    m_preparation.BeginBuild();
    using Usage = RenderGraph::ResourceUsage;
    using Purpose = RenderGraph::ResourceAccessPurpose;
    std::vector<ResourceHandle<AccelerationStructureTag>> bottomLevels;
    std::vector<std::string> bottomNames;
    std::vector<size_t> createdGeometry;
    bottomLevels.reserve(scene.geometries.size());
    bottomNames.reserve(scene.geometries.size());
    createdGeometry.reserve(scene.geometries.size());
    bool buildSucceeded = true;
    bool createdTopLevel = false;
    bool createdHitRecords = false;
    bool createdSurfaceMaterials = false;
    const auto fail = [&]() {
        /// @note Reused immutable tables still belong to the prior entry; only this attempt's allocations may be retired on failure.
        if (createdTopLevel) resources.Release(result.topLevel);
        if (createdHitRecords) resources.Release(result.hitRecords);
        if (createdSurfaceMaterials) resources.Release(result.surfaceMaterials);
        for (auto index : createdGeometry) resources.Release(m_geometry[index].handle);
        std::erase_if(m_geometry, [&](const GeometryEntry& entry) { return !resources.Get(entry.handle); });
        m_preparation.BeginBuild();
        return RaySceneGpu{};
    };
    for (size_t i = 0; i < scene.geometries.size(); ++i) {
        const auto& geometry = scene.geometries[i];
        const auto& key = geometry.key;
        const auto& triangles = geometry.triangles;
        if (triangles.vertices != key.vertices || triangles.indices != key.indices
            || triangles.firstVertex != key.firstVertex || triangles.vertexCount != key.vertexCount
            || triangles.positionOffset != key.positionOffset || triangles.firstIndex != key.firstIndex
            || triangles.indexCount != key.indexCount || triangles.opaque != key.opaque) return fail();
        auto* vertices = resources.Get(key.vertices);
        auto* indices = resources.Get(key.indices);
        if (!vertices || vertices->GetContentVersion() != key.vertexContentVersion
            || vertices->GetStride() != key.vertexStride
            || vertices->GetSize() > UINT32_MAX || (indices && indices->GetSize() > UINT32_MAX)
            || vertices->GetBindlessSrvIndex() == INVALID_BINDLESS_INDEX
            || (key.indexCount && (!indices || indices->GetContentVersion() != key.indexContentVersion
                || indices->GetBindlessSrvIndex() == INVALID_BINDLESS_INDEX))) return fail();
        const auto vertexName = "RayVertex" + std::to_string(i);
        const auto indexName = "RayIndex" + std::to_string(i);
        const auto bottomName = "RayBLAS" + std::to_string(i);
        m_preparation.DeclareBuffer(vertexName, key.vertices, BufferDescription(*vertices));
        if (key.indexCount) m_preparation.DeclareBuffer(indexName, key.indices, BufferDescription(*indices));
        auto found = std::find_if(m_geometry.begin(), m_geometry.end(), [&](const GeometryEntry& entry) {
            return entry.key == key && resources.Get(entry.handle);
        });
        if (found == m_geometry.end()) {
            AccelerationStructureDesc desc;
            desc.geometries.push_back(geometry.triangles);
            const auto handle = resources.CreateAccelerationStructure(desc);
            if (!handle.IsValid()) return fail();
            createdGeometry.push_back(m_geometry.size());
            m_geometry.push_back({key, handle, frameStamp});
            found = m_geometry.end() - 1;
            m_preparation.DeclareAccelerationStructure(bottomName, handle, AsDescription(false));
            std::vector<RenderGraph::ResourceAccess> accesses = {
                {vertexName, Usage::Read, Purpose::BUILD_INPUT},
                {bottomName, Usage::Write, Purpose::AS_WRITE}};
            if (key.indexCount) accesses.push_back({indexName, Usage::Read, Purpose::BUILD_INPUT});
            m_preparation.AddRawPass("Build" + bottomName, std::move(accesses),
                [&, bottomName](PassResources& inputs) {
                    if (buildSucceeded)
                        buildSucceeded = renderer.BuildAccelerationStructure(inputs.AccelerationStructure(bottomName), resources);
                });
            ++result.builtBottomLevels;
        } else {
            found->lastUse = frameStamp;
            m_preparation.DeclareAccelerationStructure(bottomName, found->handle, AsDescription(true));
        }
        bottomLevels.push_back(found->handle);
        bottomNames.push_back(bottomName);
        if (std::find(result.readBuffers.begin(), result.readBuffers.end(), key.vertices) == result.readBuffers.end())
            result.readBuffers.push_back(key.vertices);
        if (key.indexCount && std::find(result.readBuffers.begin(), result.readBuffers.end(), key.indices) == result.readBuffers.end())
            result.readBuffers.push_back(key.indices);
    }

    std::vector<RayInstance> instances;
    std::vector<RayHitRecord> records;
    std::vector<RaySurfaceRecord> surfaces;
    instances.reserve(scene.instances.size());
    records.reserve(scene.instances.size());
    surfaces.reserve(scene.instances.size());
    for (size_t i = 0; i < scene.instances.size(); ++i) {
        const auto& instance = scene.instances[i];
        if (instance.geometryIndex >= scene.geometries.size() || instance.denseInstanceId != i) return fail();
        const auto& key = scene.geometries[instance.geometryIndex].key;
        instances.push_back({bottomLevels[instance.geometryIndex], instance.world,
            instance.denseInstanceId, instance.mask, instance.doubleSided, false});
        RayHitRecord record;
        record.vertexSrv = resources.Get(key.vertices)->GetBindlessSrvIndex();
        if (key.indexCount) record.indexSrv = resources.Get(key.indices)->GetBindlessSrvIndex();
        record.vertexStride = key.vertexStride;
        record.positionOffset = key.positionOffset;
        record.firstVertex = key.firstVertex;
        record.firstIndex = key.firstIndex;
        record.indexCount = key.indexCount;
        record.vertexCount = key.vertexCount;
        record.objectIndex = instance.objectId.index;
        record.objectGeneration = instance.objectId.generation;
        record.sceneGenerationLow = static_cast<uint32_t>(instance.objectId.sceneGeneration);
        record.sceneGenerationHigh = static_cast<uint32_t>(instance.objectId.sceneGeneration >> 32);
        records.push_back(record);
        auto surface = MakeRaySurfaceRecord(instance.surface);
        if (surface.supported) {
            for (uint32_t slot = 0; slot < instance.surface.textures.size(); ++slot) {
                if ((surface.textureMask & (1u << slot)) == 0) continue;
                const auto& binding = instance.surface.textures[slot];
                const auto* texture = resources.Get(binding.texture);
                if (!texture || !texture->IsRayMaterialTexture() || binding.contentVersion == 0
                    || texture->GetContentVersion() != binding.contentVersion
                    || texture->GetBindlessIndex() == INVALID_BINDLESS_INDEX) return fail();
                surface.textureSrv[slot] = texture->GetBindlessIndex();
                if (std::find(result.readTextures.begin(), result.readTextures.end(), binding.texture) == result.readTextures.end())
                    result.readTextures.push_back(binding.texture);
            }
        }
        surfaces.push_back(surface);
    }
    auto existing = std::find_if(m_scenes.begin(), m_scenes.end(), [&](const SceneEntry& entry) {
        return entry.sceneGeneration == scene.sceneGeneration && entry.layerMask == scene.layerMask;
    });
    const bool hasExisting = existing != m_scenes.end();
    const auto tableReady = [&](ResourceHandle<StructuredBufferTag> handle) {
        const auto* table = resources.Get(handle);
        return table && table->GetBindlessIndex() != INVALID_BINDLESS_INDEX;
    };
    const bool reuseTopLevel = hasExisting && createdGeometry.empty()
        && SameInstances(existing->instances, instances) && resources.Get(existing->gpu.topLevel);
    const bool reuseHitRecords = hasExisting && existing->records == records && tableReady(existing->gpu.hitRecords);
    const bool reuseSurfaceMaterials = hasExisting && existing->surfaces == surfaces
        && tableReady(existing->gpu.surfaceMaterials);
    /// @note Instance transforms affect only TLAS; table reuse requires exact current content and a live descriptor after all source-version validation.
    /// @note フレーム記録中の DEFAULT upload は描画フェンスを消費するため、変更した表だけ immutable UPLOAD の新しい版にする。
    if (reuseHitRecords) result.hitRecords = existing->gpu.hitRecords;
    else {
        result.hitRecords = resources.CreateStructuredBuffer(records.data(),
            static_cast<uint32_t>(records.size()), sizeof(RayHitRecord));
        createdHitRecords = true;
    }
    if (reuseSurfaceMaterials) result.surfaceMaterials = existing->gpu.surfaceMaterials;
    else {
        result.surfaceMaterials = resources.CreateStructuredBuffer(surfaces.data(),
            static_cast<uint32_t>(surfaces.size()), sizeof(RaySurfaceRecord));
        createdSurfaceMaterials = true;
    }
    if (!tableReady(result.hitRecords) || !tableReady(result.surfaceMaterials)) return fail();
    if (reuseTopLevel) result.topLevel = existing->gpu.topLevel;
    else {
        AccelerationStructureDesc topDesc;
        topDesc.kind = AccelerationStructureKind::TOP_LEVEL;
        topDesc.instances = instances;
        result.topLevel = resources.CreateAccelerationStructure(topDesc);
        createdTopLevel = true;
        if (!resources.Get(result.topLevel)) return fail();
        m_preparation.DeclareAccelerationStructure("RayTLAS", result.topLevel, AsDescription(false));
        std::vector<RenderGraph::ResourceAccess> topAccesses;
        topAccesses.reserve(bottomNames.size() + 1);
        topAccesses.push_back({"RayTLAS", Usage::Write, Purpose::AS_WRITE});
        for (const auto& name : bottomNames) topAccesses.push_back({name, Usage::Read, Purpose::BUILD_INPUT});
        m_preparation.AddRawPass("BuildRayTLAS", std::move(topAccesses), [&](PassResources& inputs) {
            if (buildSucceeded)
                buildSucceeded = renderer.BuildAccelerationStructure(inputs.AccelerationStructure("RayTLAS"), resources);
        });
        /// @note ビュー graph の import は準備 graph の生存判定に伝わらないため出力を明示する。
        m_preparation.SetOutputs({"RayTLAS"});
        auto savedRegistry = std::move(context.resourceRegistry);
        const bool executed = m_preparation.Execute(context);
        context.resourceRegistry = std::move(savedRegistry);
        if (!executed || !buildSucceeded) return fail();
        result.builtTopLevel = true;
    }
    m_preparation.BeginBuild();
    result.instanceCount = static_cast<uint32_t>(instances.size());
    result.ready = true;
    if (hasExisting) {
        /// @note Publish only after all new allocations/builds succeed; unchanged handles keep their prior GPU lifetime and are never retired here.
        if (!reuseTopLevel) resources.Release(existing->gpu.topLevel);
        if (!reuseHitRecords) resources.Release(existing->gpu.hitRecords);
        if (!reuseSurfaceMaterials) resources.Release(existing->gpu.surfaceMaterials);
        *existing = {scene.sceneGeneration, scene.layerMask, std::move(instances), std::move(records),
            std::move(surfaces), result, frameStamp};
    } else {
        m_scenes.push_back({scene.sceneGeneration, scene.layerMask, std::move(instances), std::move(records),
            std::move(surfaces), result, frameStamp});
    }
    return result;
}

void RayGeometryCache::Prune(ResourceManager& resources, uint64_t frameStamp)
{
    /// @note 交互に描くビューを保持しつつ、閉じた Scene と旧 geometry 版を無期限に残さない。
    const auto expired = [&](uint64_t use) { return frameStamp > use && frameStamp - use > 2; };
    std::erase_if(m_scenes, [&](const SceneEntry& entry) {
        if (!expired(entry.lastUse)) return false;
        resources.Release(entry.gpu.topLevel);
        resources.Release(entry.gpu.hitRecords);
        resources.Release(entry.gpu.surfaceMaterials);
        return true;
    });
    std::erase_if(m_geometry, [&](const GeometryEntry& entry) {
        if (!expired(entry.lastUse)) return false;
        resources.Release(entry.handle);
        return true;
    });
}

void RayGeometryCache::Release(ResourceManager& resources)
{
    for (const auto& scene : m_scenes) {
        resources.Release(scene.gpu.topLevel);
        resources.Release(scene.gpu.hitRecords);
        resources.Release(scene.gpu.surfaceMaterials);
    }
    for (const auto& geometry : m_geometry) resources.Release(geometry.handle);
    m_scenes.clear();
    m_geometry.clear();
    m_preparation.BeginBuild();
}
} /// @note namespace fbzz::renderer
