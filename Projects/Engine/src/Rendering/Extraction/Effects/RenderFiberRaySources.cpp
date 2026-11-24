/// @file    RenderFiberRaySources.cpp
/// @brief   レイトレーシングで診断対象になる繊維の素材と形状を確認する。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderFiberExtraction.hpp"
#include <Engine/Rendering/Extraction/Effects/RenderFiberExtractor.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/VFX/FiberComponent.hpp>
#include <Engine/Scene/Components/Geometry/MaterialComponent.hpp>
#include <Engine/Scene/Components/Geometry/MeshRenderer.hpp>
#include <Engine/Scene/Components/Geometry/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/Terrain/TerrainComponent.hpp>
#include <Graphics/Renderer/Geometry/FiberGeometry.hpp>
#include <Graphics/Scene/RenderScene.hpp>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>

namespace fbzz::scene {

namespace {

using namespace fiber_extraction_detail;

/// @note BuildFiberFins と BuildFiberBlades の生成条件を個数だけで評価し、展開後の形状の確保と GPU 転送を省く。
bool HasRayFiberMesh(const renderer::Mesh& mesh, const FiberComponent& fiber,
                     renderer::ResourceHandle<renderer::BufferTag> vertices)
{
    if (!vertices.IsValid() || !mesh.indexBuffer.IsValid()) return false;
    if (fiber.m_mode == FiberRenderMode::SHELL) return mesh.indexCount >= 3;
    if (fiber.m_mode == FiberRenderMode::HYBRID && mesh.indexCount >= 3) return true;
    const bool blades = fiber.m_mode == FiberRenderMode::BLADE;
    if (!blades && fiber.m_mode != FiberRenderMode::FIN && fiber.m_mode != FiberRenderMode::HYBRID) return false;
    if (mesh.cpuIndices.size() % 3 != 0 || mesh.cpuIndices.size() > 600000) return false;
    if (blades && (mesh.isSkinned || !std::isfinite(fiber.m_bladeDensity)
        || fiber.m_bladeDensity <= 0 || !std::isfinite(fiber.m_bladeWidth) || fiber.m_bladeWidth <= 0)) return false;
    const size_t vertexCount = mesh.isSkinned ? mesh.cpuSkinnedVertices.size() : mesh.cpuVertices.size();
    const auto position = [&](size_t i) { return mesh.isSkinned ? mesh.cpuSkinnedVertices[i].position : mesh.cpuVertices[i].position; };
    const auto normal = [&](size_t i) { return mesh.isSkinned ? mesh.cpuSkinnedVertices[i].normal : mesh.cpuVertices[i].normal; };
    const auto uv = [&](size_t i) { return mesh.isSkinned ? mesh.cpuSkinnedVertices[i].uv : mesh.cpuVertices[i].uv; };
    const auto finite = [](math::Vector3 vector) { return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z); };
    if (!blades)
        for (size_t i = 0; i < vertexCount; ++i)
            if (!finite(position(i)) || !finite(normal(i)) || !std::isfinite(uv(i).x) || !std::isfinite(uv(i).y)) return false;
    std::map<std::array<float, 6>, uint32_t> edges;
    float area = 0;
    for (size_t triangleOffset = 0; triangleOffset < mesh.cpuIndices.size(); triangleOffset += 3) {
        const uint32_t indices[]{mesh.cpuIndices[triangleOffset], mesh.cpuIndices[triangleOffset + 1], mesh.cpuIndices[triangleOffset + 2]};
        for (uint32_t index : indices)
            if (index >= vertexCount || !finite(position(index)) || !finite(normal(index))) return false;
        const auto face = math::Vector3::Cross(position(indices[1]) - position(indices[0]),
                                             position(indices[2]) - position(indices[0]));
        if (!finite(face)) return false;
        if (blades) {
            area += face.Length() * 0.5f;
            continue;
        }
        if (face.LengthSq() <= 1.0e-16f) continue;
        for (uint32_t edge = 0; edge < 3; ++edge) {
            const uint32_t firstVertex = indices[edge], secondVertex = indices[(edge + 1) % 3];
            const auto firstPosition = position(firstVertex), secondPosition = position(secondVertex);
            auto firstEndpoint = std::array{firstPosition.x, firstPosition.y, firstPosition.z}, secondEndpoint = std::array{secondPosition.x, secondPosition.y, secondPosition.z};
            if (secondEndpoint < firstEndpoint) std::swap(firstEndpoint, secondEndpoint);
            const auto deltaUv = uv(secondVertex) - uv(firstVertex);
            if (!std::isfinite((secondPosition - firstPosition).Length()) || !std::isfinite(std::sqrt(deltaUv.x * deltaUv.x + deltaUv.y * deltaUv.y))) return false;
            if (++edges[{firstEndpoint[0], firstEndpoint[1], firstEndpoint[2], secondEndpoint[0], secondEndpoint[1], secondEndpoint[2]}] > 2) return false;
        }
    }
    if (!blades) return !edges.empty();
    const float count = area * fiber.m_bladeDensity;
    return std::isfinite(area) && std::isfinite(count) && count >= 1 && count <= static_cast<float>(renderer::FIBER_MAX_BLADES);
}

}

void ExtractRayFiberSources(Scene& scene, renderer::ResourceManager& resources, renderer::RenderScene& output,
    bool experimentalRayTracingEnabled)
{
    if (!experimentalRayTracingEnabled) return;
    auto& cache = GetFiberResources(resources);
    constexpr uint8_t SHELL = 1u << 0;
    constexpr uint8_t FIN = 1u << 1;
    constexpr uint8_t BLADE = 1u << 2;
    constexpr std::array<uint8_t, 4> MODE_MASKS{SHELL, FIN, SHELL | FIN, BLADE};
    std::optional<std::unordered_map<uint64_t, uint8_t>> terrainModes;
    for (EntityID entity : scene.GetEntities<FiberComponent>()) {
        auto* ownerObject = scene.GetGameObject(entity);
        const auto* fiber = ownerObject ? ownerObject->GetComponent<FiberComponent>() : nullptr;
        if (!ownerObject || !ownerObject->activeInHierarchy() || !fiber || !fiber->m_enabled || fiber->m_materialPath.empty()) continue;
        const auto& material = GetFiberMaterial(cache, resources, fiber->m_materialPath);
        if (!material.m_valid) continue;
        const auto settings = ApplyFiberOverrides(material.m_settings, *fiber, cache, resources);
        if (settings.m_length <= 0 || settings.m_density <= 0) continue;
        bool hasRenderableFiber = false;
        if (const auto* mesh = ownerObject->GetComponent<MeshRenderer>(); mesh && mesh->enabled && mesh->mesh && !mesh->mesh->isSkinned)
            hasRenderableFiber = HasRayFiberMesh(*mesh->mesh, *fiber, mesh->mesh->vertexBuffer);
        if (const auto* skinnedRenderer = ownerObject->GetComponent<SkinnedMeshRenderer>(); !hasRenderableFiber && skinnedRenderer && skinnedRenderer->enabled && skinnedRenderer->model) {
            const auto* baseMaterial = ownerObject->GetComponent<MaterialComponent>();
            for (size_t slot = 0; slot < skinnedRenderer->SubmeshCount() && !hasRenderableFiber; ++slot) {
                const auto* mesh = skinnedRenderer->SubmeshMesh(slot);
                if (!mesh || (baseMaterial && !baseMaterial->SlotAt(slot).visible)) continue;
                hasRenderableFiber = HasRayFiberMesh(*mesh, *fiber, skinnedRenderer->ResolveSlotVertexBuffer(slot, mesh->vertexBuffer));
            }
        }
        if (const auto* terrain = ownerObject->GetComponent<TerrainComponent>();
            !hasRenderableFiber && terrain && terrain->enabled && fiber->m_renderIdentity) {
            /// @note 頂点アルファとシードによる除外を反映した常駐地形の描画件数を再利用し、診断用のパッチ生成を省く。
            if (!terrainModes) {
                terrainModes.emplace();
                for (const auto& surface : output.fibers) {
                    if (!surface.identity) continue;
                    uint8_t mask = 0;
                    if (surface.vertices.IsValid() && surface.indices.IsValid() && surface.indexCount >= 3)
                        mask |= SHELL;
                    if (surface.fins.m_vertices.IsValid() && surface.fins.m_indices.IsValid() && surface.fins.m_indexCount > 0)
                        mask |= FIN;
                    if (surface.blades.m_blades.IsValid() && surface.blades.m_bladeCount > 0)
                        mask |= BLADE;
                    if (mask) (*terrainModes)[surface.identity] |= mask;
                }
            }
            const auto modes = terrainModes->find(fiber->m_renderIdentity);
            const auto mode = static_cast<size_t>(fiber->m_mode);
            hasRenderableFiber = modes != terrainModes->end() && mode < MODE_MASKS.size()
                && (modes->second & MODE_MASKS[mode]) != 0;
        }
        if (hasRenderableFiber) {
            const uint32_t layerMask = ownerObject->layer >= 0 && ownerObject->layer < 32 ? uint32_t{1} << ownerObject->layer : 0;
            output.rayUnsupportedEffects.push_back({entity.index, entity.generation, layerMask,
                renderer::RenderRayUnsupportedEffect::FIBER});
        }
    }
}

}
