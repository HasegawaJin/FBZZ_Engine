/// @file    RaySceneBuilder.cpp
/// @brief   全カメラ共通の描画候補から用途 mask 付きの静的レイ入力を収集する。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#include <Graphics/RayTracing/RayScene.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <cmath>
#include <functional>
#include <unordered_map>

namespace fbzz::renderer {
namespace {

bool MatchesLayer(uint32_t layer, uint32_t mask)
{
    return layer < 32 && (mask & (uint32_t{1} << layer)) != 0;
}

bool ValidTransform(const math::Matrix4& transform)
{
    for (const auto& row : transform.m)
        for (float value : row)
            if (!std::isfinite(value)) return false;
    if (transform.m[3][0] != 0 || transform.m[3][1] != 0
        || transform.m[3][2] != 0 || transform.m[3][3] != 1)
        return false;
    const auto& m = transform.m;
    const double determinant = static_cast<double>(m[0][0]) * (static_cast<double>(m[1][1]) * m[2][2] - static_cast<double>(m[1][2]) * m[2][1])
        - static_cast<double>(m[0][1]) * (static_cast<double>(m[1][0]) * m[2][2] - static_cast<double>(m[1][2]) * m[2][0])
        + static_cast<double>(m[0][2]) * (static_cast<double>(m[1][0]) * m[2][1] - static_cast<double>(m[1][1]) * m[2][0]);
    return std::isfinite(determinant) && determinant != 0;
}

template<typename T>
void DiagnoseUnsupported(const std::vector<T>& input, RayScene& output)
{
    for (const auto& value : input)
        if (MatchesLayer(value.layer, output.layerMask))
            output.diagnostics.push_back({{output.sceneGeneration, 0, 0}, UINT32_MAX,
                RaySceneIssue::NONMESH_GEOMETRY_UNSUPPORTED});
}

/// @note Only resolved empty counts prove absence; camera bounds, distance, LOD ratios and unknown modes do not.
bool HasResolvedFiberGeometry(const RenderFiberInput& fiber)
{
    if (!std::isfinite(fiber.material.m_length) || !std::isfinite(fiber.material.m_density)) return true;
    if (fiber.material.m_length <= 0 || fiber.material.m_density <= 0) return false;
    const bool shell = (fiber.vertices.IsValid() || (fiber.skinned && fiber.skinnedVertices.IsValid()))
        && fiber.indices.IsValid() && fiber.indexCount >= 3;
    const bool fin = fiber.fins.m_vertices.IsValid() && fiber.fins.m_indices.IsValid() && fiber.fins.m_indexCount > 0;
    switch (fiber.settings.m_mode) {
    case FiberRenderMode::SHELL: return shell;
    case FiberRenderMode::FIN: return fin;
    case FiberRenderMode::HYBRID: return shell || fin;
    case FiberRenderMode::BLADE: return !fiber.skinned && fiber.blades.m_blades.IsValid() && fiber.blades.m_bladeCount > 0;
    }
    return true;
}

RaySceneIssue GeometryIssue(const RenderObject& object, const RenderMeshItem& item, const RaySceneBuildOptions& options)
{
    if (object.skinned && (!item.deformedVertexBuffer.IsValid() || item.deformedContentVersion == 0))
        return RaySceneIssue::SKINNED_UNSUPPORTED;
    if (!item.material.valid) return RaySceneIssue::MATERIAL_UNRESOLVED;
    if (!item.material.rayCapabilities.staticGeometry) return RaySceneIssue::CUSTOM_GEOMETRY_UNSUPPORTED;
    if (item.material.capabilities.blend != BlendMode::OPAQUE_BLEND) return RaySceneIssue::TRANSPARENT_UNSUPPORTED;
    if (item.material.rayCapabilities.opacity != RayOpacity::OPAQUE_SURFACE
        && !(item.material.rayCapabilities.opacity == RayOpacity::ALPHA_TEST && item.material.surface.standardSurfaceSupported))
        return RaySceneIssue::ALPHA_TEST_UNSUPPORTED;
    if (item.material.surface.dielectric.transmission != 0
        && (!options.allowSolidDielectrics || !item.material.surface.solidDielectricSupported))
        return RaySceneIssue::SOLID_DIELECTRIC_UNSUPPORTED;
    if (item.vertexContentVersion == 0 || (item.indexBuffer.IsValid() && item.indexContentVersion == 0))
        return RaySceneIssue::CONTENT_VERSION_UNAVAILABLE;
    return RaySceneIssue::INVALID_GEOMETRY;
}

bool CanCollect(const RenderObject& object, const RenderMeshItem& item, const RaySceneBuildOptions& options)
{
    const bool deformed = object.skinned && item.deformedVertexBuffer.IsValid() && item.deformedContentVersion != 0;
    const auto vertices = object.skinned ? item.deformedVertexBuffer : item.vertexBuffer;
    const auto version = object.skinned ? item.deformedContentVersion : item.vertexContentVersion;
    const auto stride = object.skinned ? sizeof(Vertex) : item.vertexStride;
    return (!object.skinned || deformed) && item.material.valid && item.material.rayCapabilities.staticGeometry
        && item.material.capabilities.blend == BlendMode::OPAQUE_BLEND
        && (item.material.rayCapabilities.opacity == RayOpacity::OPAQUE_SURFACE
            || (item.material.rayCapabilities.opacity == RayOpacity::ALPHA_TEST && item.material.surface.standardSurfaceSupported))
        && (item.material.surface.dielectric.transmission == 0
            || (options.allowSolidDielectrics && item.material.surface.solidDielectricSupported))
        && vertices.IsValid() && version != 0
        && stride >= 12 && stride % 4 == 0
        && item.vertexPositionOffset % 4 == 0 && item.vertexPositionOffset <= stride - 12
        && item.vertexCount != 0 && (item.indexBuffer.IsValid()
            ? item.indexContentVersion != 0 && item.indexCount != 0 && item.indexCount % 3 == 0
            : item.indexCount == 0 && item.vertexCount % 3 == 0);
}

} /// @note namespace

size_t RayGeometryKeyHash::operator()(const RayGeometryKey& key) const
{
    size_t hash = 0;
    const auto add = [&](uint64_t value) { hash = hash * 131u + std::hash<uint64_t>{}(value); };
    add(key.vertices.id); add(key.vertices.gen); add(key.indices.id); add(key.indices.gen);
    add(key.vertexContentVersion); add(key.indexContentVersion); add(key.vertexStride);
    add(key.firstVertex); add(key.vertexCount); add(key.positionOffset); add(key.firstIndex);
    add(key.indexCount); add(key.opaque); add(key.doubleSided);
    return hash;
}

RayScene BuildRayScene(const RenderScene& scene, RaySceneBuildOptions options)
{
    RayScene output;
    output.sceneGeneration = scene.sceneGeneration;
    output.snapshotSerial = scene.snapshotSerial;
    output.frameStamp = scene.frameStamp;
    output.layerMask = options.layerMask;
    output.hybridCandidatePolicy = options.hybridCandidatePolicy;
    if (scene.sceneGeneration == 0) {
        output.diagnostics.push_back({{}, UINT32_MAX, RaySceneIssue::SCENE_ID_UNAVAILABLE});
        return output;
    }
    std::unordered_map<RayGeometryKey, uint32_t, RayGeometryKeyHash> geometryIndices;
    for (const auto& diagnostic : scene.rayLodDiagnostics)
        if ((diagnostic.layerMask & options.layerMask) != 0)
            output.diagnostics.push_back({{scene.sceneGeneration, diagnostic.sourceIndex,
                diagnostic.sourceGeneration}, UINT32_MAX, RaySceneIssue::LOD_SELECTION_UNRESOLVED});
    for (uint32_t objectIndex = 0; objectIndex < scene.objects.size(); ++objectIndex) {
        const auto& object = scene.objects[objectIndex];
        if (!MatchesLayer(object.layer, options.layerMask)) continue;
        const RayObjectId objectId{scene.sceneGeneration, object.sourceIndex, object.sourceGeneration};
        if (object.rayLodSelectionRequired) {
            output.diagnostics.push_back({objectId, UINT32_MAX, RaySceneIssue::LOD_SELECTION_UNRESOLVED});
            continue;
        }
        if (!object.rayVisible) continue;
        if (object.firstItem > scene.items.size() || object.itemCount > scene.items.size() - object.firstItem) {
            output.diagnostics.push_back({objectId, UINT32_MAX, RaySceneIssue::INVALID_OBJECT_RANGE});
            continue;
        }
        if (!ValidTransform(object.world)) {
            output.diagnostics.push_back({objectId, UINT32_MAX, RaySceneIssue::INVALID_TRANSFORM});
            continue;
        }
        for (uint32_t offset = 0; offset < object.itemCount; ++offset) {
            const uint32_t sourceItem = object.firstItem + offset;
            const auto& item = scene.items[sourceItem];
            if (!item.slotVisible || item.material.rayCapabilities.opacity == RayOpacity::EMPTY) continue;
            if (!item.material.surface.standardSurfaceSupported
                && !(options.allowSolidDielectrics && item.material.surface.solidDielectricSupported))
                output.surfaceDiagnostics.push_back({objectId, sourceItem,
                    item.material.surface.solidDielectricSupported
                        ? SurfaceMaterialIssue::SOLID_DIELECTRIC_UNSUPPORTED : item.material.surface.issue});
            if (item.objectIndex != objectIndex || !CanCollect(object, item, options)) {
                output.diagnostics.push_back({objectId, sourceItem, item.objectIndex != objectIndex
                    ? RaySceneIssue::INVALID_OBJECT_RANGE : GeometryIssue(object, item, options)});
                continue;
            }
            if (output.instances.size() >= (1u << 24)) {
                output.diagnostics.push_back({objectId, sourceItem, RaySceneIssue::INSTANCE_LIMIT_EXCEEDED});
                continue;
            }
            const bool dielectric = item.material.surface.dielectric.transmission != 0.0f;
            const bool opaque = item.material.rayCapabilities.opacity == RayOpacity::OPAQUE_SURFACE
                && !(options.hybridCandidatePolicy && dielectric);
            const bool doubleSided = options.hybridCandidatePolicy ? dielectric : item.material.doubleSided;
            RayGeometryKey key{object.skinned ? item.deformedVertexBuffer : item.vertexBuffer, item.indexBuffer,
                object.skinned ? item.deformedContentVersion : item.vertexContentVersion,
                item.indexContentVersion, object.skinned ? static_cast<uint32_t>(sizeof(Vertex)) : item.vertexStride, 0, item.vertexCount,
                item.vertexPositionOffset, 0, item.indexCount,
                opaque, doubleSided};
            const auto [geometry, inserted] = geometryIndices.emplace(key,
                static_cast<uint32_t>(output.geometries.size()));
            if (inserted) {
                output.geometries.push_back({key, {key.vertices, key.indices, key.firstVertex,
                    key.vertexCount, key.positionOffset, key.firstIndex, key.indexCount, key.opaque}});
            }
            RaySceneInstance instance;
            instance.objectId = objectId;
            instance.geometryIndex = geometry->second;
            instance.denseInstanceId = static_cast<uint32_t>(output.instances.size());
            instance.sourceItem = sourceItem;
            instance.sourceSubmesh = item.sourceSubmesh;
            instance.materialSlot = item.materialSlot;
            instance.surface = item.material.surface;
            instance.world = object.world;
            instance.previousWorld = object.previousWorld;
            instance.doubleSided = doubleSided;
            if (!object.castShadows) instance.mask &= static_cast<uint8_t>(~RAY_SHADOW_MASK);
            output.instances.push_back(instance);
        }
    }
    DiagnoseUnsupported(scene.terrains, output);
    DiagnoseUnsupported(scene.water, output);
    DiagnoseUnsupported(scene.trails, output);
    DiagnoseUnsupported(scene.meshTrails, output);
    for (const auto& source : scene.rayUnsupportedEffects) {
        if ((source.layerMask & output.layerMask) != 0)
            output.diagnostics.push_back({{output.sceneGeneration, source.sourceIndex, source.sourceGeneration},
                UINT32_MAX, RaySceneIssue::NONMESH_GEOMETRY_UNSUPPORTED});
    }
    /// @note Partial metadata cannot prove that another legacy particle owner or layer is absent; duplicate diagnostics are conservative.
    DiagnoseUnsupported(scene.particles, output);
    /// @note Fiber draw lists can include terrain sources absent from mesh metadata; retaining their conservative diagnosis cannot hide another owner or layer.
    for (const auto& fiber : scene.fibers)
        if (MatchesLayer(fiber.layer, output.layerMask) && HasResolvedFiberGeometry(fiber))
            output.diagnostics.push_back({{output.sceneGeneration, 0, 0}, UINT32_MAX,
                RaySceneIssue::NONMESH_GEOMETRY_UNSUPPORTED});
    DiagnoseUnsupported(scene.decals, output);
    return output;
}

const char* DescribeRaySceneIssue(RaySceneIssue issue)
{
    switch (issue) {
    case RaySceneIssue::SCENE_ID_UNAVAILABLE: return "Scene generation unavailable";
    case RaySceneIssue::INVALID_OBJECT_RANGE: return "Invalid object / submesh association";
    case RaySceneIssue::INVALID_TRANSFORM: return "Nonfinite, projective or singular transform";
    case RaySceneIssue::INVALID_GEOMETRY: return "Invalid vertex format or triangle range";
    case RaySceneIssue::CONTENT_VERSION_UNAVAILABLE: return "Buffer content version unavailable";
    case RaySceneIssue::MATERIAL_UNRESOLVED: return "Material unresolved";
    case RaySceneIssue::SKINNED_UNSUPPORTED: return "Skinned / morph geometry unsupported";
    case RaySceneIssue::CUSTOM_GEOMETRY_UNSUPPORTED: return "Custom shader geometry unsupported";
    case RaySceneIssue::ALPHA_TEST_UNSUPPORTED: return "Alpha / opacity evaluation unsupported";
    case RaySceneIssue::TRANSPARENT_UNSUPPORTED: return "Transparent surface unsupported";
    case RaySceneIssue::NONMESH_GEOMETRY_UNSUPPORTED: return "Terrain / water / VFX / decal unsupported";
    case RaySceneIssue::INSTANCE_LIMIT_EXCEEDED: return "DXR 24-bit instance ID capacity exceeded";
    case RaySceneIssue::SOLID_DIELECTRIC_UNSUPPORTED: return "Solid dielectric requires Path opt-in and supported material";
    case RaySceneIssue::LOD_SELECTION_UNRESOLVED: return "Ray LOD0 selection is ambiguous";
    }
    return "Unknown ray scene issue";
}

} /// @note namespace fbzz::renderer
