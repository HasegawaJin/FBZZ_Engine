/// @file    RenderParticleExtraction.hpp
/// @brief   パーティクルの材質・描画定数・GPU シミュレーションの抽出境界。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Graphics/Effects/RenderParticleInput.hpp>
#include <Graphics/Pipeline/RenderConstants.hpp>
#include <vector>

namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::scene {
class Scene;
struct ParticleEmitter;
struct AnimatorComponent;
struct Transform;
struct ActiveFlowField;
struct MeshRenderer;
struct RenderPassContext;
}

namespace fbzz::scene::particle_extraction_detail {

struct ParticleMaterialCache;
ParticleMaterialCache& BeginParticleMaterialPass(Scene& scene, renderer::ResourceManager& resources);

void PrepareParticleAppearance(ParticleEmitter& emitter, renderer::ResourceManager& resources, ParticleMaterialCache& cache);
bool ShouldSortGpuParticles(const ParticleEmitter& emitter, const renderer::RenderPassHandles& handles);
renderer::ParticleRenderCB BuildParticleRenderConstants(ParticleEmitter& emitter, renderer::ResourceManager& resources,
    int maxParticles, uint32_t screenWidth, uint32_t screenHeight, bool gpuSortEnabled = false);
void PrepareGpuEmitter(ParticleEmitter& emitter, const Transform& transform, const AnimatorComponent* animator,
    float deltaTime, float time, bool canEmit, const std::vector<ActiveFlowField>& flowFields,
    const MeshRenderer* meshParticleRenderer, RenderPassContext& context, renderer::RenderParticleInput& input);

} /// @note namespace fbzz::scene::particle_extraction_detail
