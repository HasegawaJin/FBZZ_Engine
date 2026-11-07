/// @file    RenderEnvironment.hpp
/// @brief   解決済みの空・雲・水中・集光の描画入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <Graphics/RayTracing/RayEnvironment.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <optional>
namespace fbzz::renderer {
struct RenderSkyInput {
    math::Vector3 rayleighScattering{};
    float mieScattering = 0.0f;
    float planetRadius = 0.0f;
    float atmosphereRadius = 0.0f;
    float skyScatterIntensity = 0.0f;
    float mieG = 0.0f;
};
struct RenderSunMoonInput {
    bool sunEnabled = false;
    float sunDiskIntensity = 0.0f;
    bool moonEnabled = false;
    float moonSize = 0.0f;
    float moonBrightness = 0.0f;
    math::Vector3 moonColor{};
    float starBrightness = 0.0f;
};
/// @note CloudVolume.hlsli の VolumetricCloudConstants と同じ配列順。
struct RenderCloudConstants {
    math::Vector4 cloudLayer, cloudNoise, cloudWind, cloudLighting, cloudAlbedo;
    math::Vector4 cloudWeather, cloudShading, cloudProfile, cloudRange, cloudSunTint, cloudAmbTint;
};
static_assert(sizeof(RenderCloudConstants) == 176);
struct RenderUnderwaterInput {
    bool enabled = false;
    float strength = 0.0f;
    float depth = 0.0f;
    float fogDensity = 0.0f;
    math::Vector3 color{};
    bool opticalEnabled = false;
    math::Vector3 absorption{};
    math::Vector3 scattering{};
    float phaseG = 0.0f;
    float surfaceY = 0.0f;
    float bottomY = 0.0f;
    float maxDistance = 120.0f;
    float lightStrength = 1.0f;
    float waveAmplitude = 0.0f;
    float waveFrequency = 0.12f;
    float waveSpeed = 1.0f;
    float distortion = 0.001f;
    math::Vector2 center{};
    math::Vector2 halfExtent{};
    float cosYaw = 1.0f;
    float sinYaw = 0.0f;
    int lightSteps = 48;
};
struct RenderCausticsInput {
    bool enabled = false;
    float maxDistance = 120.0f;
    float intensity = 0.0f, tiling = 1.0f, surfaceY = 0.0f, timeOffset = 0.0f;
    float centerX = 0.0f, centerZ = 0.0f, halfExtentX = 0.0f, halfExtentZ = 0.0f;
    float waveAmp = 0.0f, waveFreq = 0.12f, waveSpeed = 1.0f;
    bool opticalEnabled = false;
    math::Vector3 absorption{};
    math::Vector3 scattering{};
    float phaseG = 0.0f;
    float cosYaw = 1.0f;
    float sinYaw = 0.0f;
    ResourceHandle<TextureTag> texture;
};
struct RenderEnvironmentInput {
    RayEnvironmentInput rayEnvironment;
    std::optional<RenderSkyInput> sky;
    std::optional<RenderSunMoonInput> sunMoon;
    RenderCloudConstants cloud{};
    bool cloudEnabled = false;
    bool cloudHalfResolution = false;
    bool cameraUnderwater = false;
    RenderUnderwaterInput underwater;
    RenderCausticsInput caustics;
};
}
