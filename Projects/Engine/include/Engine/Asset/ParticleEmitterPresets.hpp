/// @file    ParticleEmitterPresets.hpp
/// @brief   Complete particle starters shared by the Inspector and authoring fixtures.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#pragma once

#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <span>
#include <string_view>

namespace fbzz::asset {

enum class ParticleEmitterPreset {
    FIRE, SMOKE, SPARKS, EXPLOSION, BUBBLES, SAND, LIGHT_TRACE, WATER_FLOW
};

struct ParticleEmitterPresetInfo {
    ParticleEmitterPreset id;
    std::string_view name;
    std::string_view description;
};

[[nodiscard]] inline std::span<const ParticleEmitterPresetInfo> ParticleEmitterPresets()
{
    static constexpr ParticleEmitterPresetInfo kPresets[] = {
        {ParticleEmitterPreset::FIRE, "Fire", "Warm cone flame with a soft lifetime fade. 192 particle budget."},
        {ParticleEmitterPreset::SMOKE, "Smoke", "Sorted, prewarmed smoke with a gentle fade. CPU; 128 particle budget."},
        {ParticleEmitterPreset::SPARKS, "Sparks", "Short stretched sparks with gravity. 128 particle budget."},
        {ParticleEmitterPreset::EXPLOSION, "Explosion", "A single 60-particle burst. Replay to trigger it again."},
        {ParticleEmitterPreset::BUBBLES, "Bubbles", "Small ring bubbles rising from a box, fading at birth and death. 96 particle budget."},
        {ParticleEmitterPreset::SAND, "Sand", "One 18-particle dust burst with restrained drift. Replay to trigger; 48 particle budget."},
        {ParticleEmitterPreset::LIGHT_TRACE, "Light Trace", "Cyan and violet light with continuous ribbons. CPU; 64 particle budget."},
        {ParticleEmitterPreset::WATER_FLOW, "Water Flow", "Thin stretched blue cards show a lateral current. GPU eligible; 80 particle budget."},
    };
    return kPresets;
}

/// @note Explicitly replaces effect settings/material while retaining enabled state, seed and runtime VFX scope.
/// @note Existing assets are untouched; all references use the material GUIDs shipped with the engine/templates.
inline void ApplyParticleEmitterPreset(scene::ParticleEmitterSettings& settings, ParticleEmitterPreset preset)
{
    const bool enabled = settings.enabled;
    const uint32_t seed = settings.randomSeed;
    const scene::EntityID scope = settings.subEmitterScopeRoot;
    settings = scene::ParticleEmitterSettings{};
    settings.enabled = enabled;
    settings.randomSeed = seed;
    settings.subEmitterScopeRoot = scope;
    settings.receiveFlowFields = false;
    settings.gravity = {};
    settings.flowCoupling = 0.2f;
    settings.duration = 4.0f;
    settings.lifetimeRandom = 0.15f;
    settings.randomStartRotation = false;
    settings.culling.lodNearDistance = 8.0f;
    settings.culling.lodFarDistance = 28.0f;
    settings.culling.lodFarRateScale = 0.2f;
    settings.colorVariation = 0.08f;
    settings.useSizeCurve = true;
    settings.sizeCurve.keyCount = 4;
    settings.sizeCurve.keys[0] = {0.0f, 0.0f};
    settings.sizeCurve.keys[1] = {0.15f, 0.06f};
    settings.sizeCurve.keys[2] = {0.65f, 0.45f};
    settings.sizeCurve.keys[3] = {1.0f, 1.0f};
    settings.sizeCurve.interpolation = scene::ParticleCurveInterpolation::Smooth;
    settings.materialPath = "guid:ccbabd52040442c29349f96d6d81c5f9";

    switch (preset) {
    case ParticleEmitterPreset::FIRE:
        settings.materialPath = "guid:89ba17527c56410a8522fb63f9992d76";
        settings.emitRate = 64.0f;
        settings.maxParticles = 192;
        settings.lifetime = 1.2f;
        settings.shape = scene::ParticleEmitterShape::Cone;
        settings.coneRadius = 0.15f;
        settings.coneAngleDegrees = 12.0f;
        settings.emitVelocity = {0.0f, 1.4f, 0.0f};
        settings.velocitySpread = 0.2f;
        settings.gravity = {0.0f, 0.5f, 0.0f};
        settings.sizeStart = 0.22f;
        settings.sizeEnd = 0.06f;
        settings.colorStart = {1.0f, 0.5f, 0.08f, 0.85f};
        settings.colorEnd = {0.8f, 0.07f, 0.01f, 0.0f};
        break;
    case ParticleEmitterPreset::SMOKE:
        settings.materialPath = "guid:b62397602041439da875882963468cfc";
        settings.emitRate = 24.0f;
        settings.maxParticles = 128;
        settings.lifetime = 3.0f;
        settings.prewarm = true;
        settings.shape = scene::ParticleEmitterShape::Sphere;
        settings.sphereRadius = 0.2f;
        settings.emitVelocity = {0.0f, 0.6f, 0.0f};
        settings.velocitySpread = 0.2f;
        settings.sizeStart = 0.25f;
        settings.sizeEnd = 0.9f;
        settings.colorStart = {0.65f, 0.7f, 0.75f, 0.28f};
        settings.colorEnd = {0.35f, 0.4f, 0.45f, 0.0f};
        settings.sortMode = scene::ParticleSortMode::BackToFront;
        break;
    case ParticleEmitterPreset::SPARKS:
        settings.materialPath = "guid:5c17544005bb4d1a8207a45509966925";
        settings.emitRate = 100.0f;
        settings.maxParticles = 128;
        settings.lifetime = 0.65f;
        settings.emitVelocity = {0.0f, 2.8f, 0.0f};
        settings.velocitySpread = 2.0f;
        settings.gravity = {0.0f, -9.8f, 0.0f};
        settings.sizeStart = 0.045f;
        settings.sizeEnd = 0.01f;
        settings.colorStart = {1.0f, 0.8f, 0.25f, 1.0f};
        settings.colorEnd = {1.0f, 0.1f, 0.01f, 0.0f};
        settings.renderMode = scene::ParticleRenderMode::StretchedBillboard;
        break;
    case ParticleEmitterPreset::EXPLOSION:
        settings.emitRate = 0.0f;
        settings.maxParticles = 96;
        settings.lifetime = 0.9f;
        settings.loop = false;
        settings.duration = 1.5f;
        settings.bursts = {{0.0f, 60, 1, 0.1f, 1.0f}};
        settings.shape = scene::ParticleEmitterShape::Sphere;
        settings.sphereRadius = 0.15f;
        settings.emitVelocity = {};
        settings.velocitySpread = 4.0f;
        settings.gravity = {0.0f, -3.0f, 0.0f};
        settings.sizeStart = 0.2f;
        settings.sizeEnd = 0.02f;
        settings.colorStart = {1.0f, 0.6f, 0.15f, 1.0f};
        settings.colorEnd = {0.4f, 0.05f, 0.0f, 0.0f};
        break;
    case ParticleEmitterPreset::BUBBLES:
        settings.materialPath = "guid:4a32cfb5ec7a4a59a89b22f471f6d481";
        settings.emitRate = 12.0f;
        settings.maxParticles = 96;
        settings.lifetime = 2.6f;
        settings.shape = scene::ParticleEmitterShape::Box;
        settings.boxExtents = {0.45f, 0.1f, 0.45f};
        settings.emitVelocity = {0.0f, 0.75f, 0.0f};
        settings.velocitySpread = 0.12f;
        settings.gravity = {0.0f, 0.08f, 0.0f};
        settings.sizeStart = 0.04f;
        settings.sizeEnd = 0.11f;
        settings.colorStart = {0.5f, 0.88f, 1.0f, 0.45f};
        settings.colorEnd = {0.85f, 0.98f, 1.0f, 0.0f};
        settings.sortMode = scene::ParticleSortMode::BackToFront;
        break;
    case ParticleEmitterPreset::SAND:
        settings.materialPath = "guid:d3c8464b8ec4408db538efde0e760ca7";
        settings.emitRate = 0.0f;
        settings.maxParticles = 48;
        settings.lifetime = 1.2f;
        settings.loop = false;
        settings.duration = 0.65f;
        settings.bursts = {{0.0f, 18, 1, 0.1f, 1.0f}};
        settings.shape = scene::ParticleEmitterShape::Box;
        settings.boxExtents = {0.22f, 0.03f, 0.22f};
        settings.emitVelocity = {0.0f, 0.35f, 0.0f};
        settings.velocitySpread = 0.45f;
        settings.gravity = {0.0f, -0.55f, 0.0f};
        settings.flowCoupling = 1.2f;
        settings.sizeStart = 0.045f;
        settings.sizeEnd = 0.15f;
        settings.colorStart = {0.72f, 0.58f, 0.36f, 0.4f};
        settings.colorEnd = {0.55f, 0.45f, 0.3f, 0.0f};
        settings.randomStartRotation = true;
        settings.sortMode = scene::ParticleSortMode::BackToFront;
        break;
    case ParticleEmitterPreset::LIGHT_TRACE:
        settings.emitRate = 20.0f;
        settings.maxParticles = 64;
        settings.lifetime = 0.7f;
        settings.rateOverDistance = 8.0f;
        settings.inheritVelocity = 0.25f;
        settings.emitVelocity = {0.0f, 0.0f, 1.6f};
        settings.velocitySpread = 0.18f;
        settings.sizeStart = 0.055f;
        settings.sizeEnd = 0.005f;
        settings.colorStart = {0.15f, 1.0f, 1.5f, 0.8f};
        settings.colorEnd = {0.65f, 0.15f, 1.0f, 0.0f};
        settings.trail.trailEnabled = true;
        settings.trail.trailRibbon = true;
        settings.trail.trailPointCount = 8;
        settings.trail.trailSampleInterval = 0.025f;
        settings.trail.trailRibbonWidth = 0.035f;
        break;
    case ParticleEmitterPreset::WATER_FLOW:
        settings.emitRate = 22.0f;
        settings.maxParticles = 80;
        settings.lifetime = 1.35f;
        settings.shape = scene::ParticleEmitterShape::Box;
        settings.boxExtents = {0.08f, 0.25f, 0.3f};
        settings.emitVelocity = {1.4f, 0.15f, 0.0f};
        settings.velocitySpread = 0.15f;
        settings.sizeStart = 0.035f;
        settings.sizeEnd = 0.07f;
        settings.sizeAxisScale = {0.45f, 1.6f, 1.0f};
        settings.colorStart = {0.15f, 0.7f, 1.0f, 0.42f};
        settings.colorEnd = {0.35f, 0.8f, 1.0f, 0.0f};
        settings.renderMode = scene::ParticleRenderMode::StretchedBillboard;
        settings.stretchedVelocityScale = 0.3f;
        settings.stretchedLengthScale = 1.5f;
        settings.simulationMode = scene::ParticleSimulationMode::Gpu;
        break;
    }

    settings.useColorGradient = true;
    settings.colorGradient.keyCount = 4;
    settings.colorGradient.colorSpace = scene::ParticleColorSpace::Linear;
    settings.colorGradient.keys[0] = {0.0f, {settings.colorStart.x, settings.colorStart.y, settings.colorStart.z, 0.0f}};
    settings.colorGradient.keys[1] = {0.12f, settings.colorStart};
    settings.colorGradient.keys[2] = {0.65f, {settings.colorEnd.x, settings.colorEnd.y, settings.colorEnd.z, settings.colorStart.w * 0.6f}};
    settings.colorGradient.keys[3] = {1.0f, settings.colorEnd};
    settings.colorGradient.interpolation = scene::ParticleCurveInterpolation::Smooth;
}

} /// @note namespace fbzz::asset
