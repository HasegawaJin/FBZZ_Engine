/// @file    RenderParticleInput.hpp
/// @brief   パーティクル描画と GPU 更新要求の不変入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/RenderConstants.hpp>
#include <Graphics/Effects/ParticleDrawTypes.hpp>
namespace fbzz::renderer {
struct ParticleDrawSettings {
    ParticleSortMode sortMode = ParticleSortMode::None;
    ParticleSimulationSpace simulationSpace = ParticleSimulationSpace::World;
    ParticleTrailSettings trail;
    math::Vector4 colorStart{}, colorEnd{};
    bool meshParticle = false;
    int maxParticles = 1;
};
struct ParticleDrawMaterial {
    bool distortion = false;
    bool punctualLighting = false;
    float selfShadowStrength = 0;
};
struct ParticleDrawResources {
    ResourceHandle<TextureTag> texture, distortionTexture, motionVectorTexture, sixWayNegativeTexture;
    ResourceHandle<ShaderTag> customShader;
    ResourceHandle<ConstantBufferTag> renderCB, materialParamsCB, trailRibbonCB, gpuEmitterCB, gpuSortCB;
    ResourceHandle<StructuredBufferTag> gpuParticleBuffer, gpuSpawnBuffer, gpuForceBuffer, gpuSortBuffer;
    uint32_t gpuSortCapacity = 0;
    ParticleDrawMaterial material;
    ParticleBlendMode resolvedBlend = ParticleBlendMode::Additive;
    bool textureIsSrgb = true;
    std::vector<Particle> particles;
};
struct RenderParticleInput {
    uint32_t layer = 0;
    int priority = 0, drawCount = 0;
    math::Vector3 position;
    bool gpu = false, simulate = false;
    ParticleDrawSettings settings;
    ParticleDrawResources runtime;
    ParticleRenderCB constants{};
    GpuParticleEmitterCB simulation{};
    ResourceHandle<TextureTag> velocityField;
    ResourceHandle<BufferTag> meshVertices, meshIndices;
    uint32_t meshIndexCount = 0, meshVertexCount = 0;
};
}
