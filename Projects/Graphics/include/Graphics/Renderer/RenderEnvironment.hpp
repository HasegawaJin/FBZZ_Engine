/// @file    RenderEnvironment.hpp
/// @brief   解決済みの空・雲・水中・集光の描画入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Renderer/ResourceHandle.hpp>
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
};
struct RenderCausticsInput {
    bool enabled = false;
    float intensity = 0.0f, tiling = 1.0f, surfaceY = 0.0f, timeOffset = 0.0f;
    float centerX = 0.0f, centerZ = 0.0f, halfExtentX = 0.0f, halfExtentZ = 0.0f;
    float waveAmp = 0.0f, waveFreq = 0.12f, waveSpeed = 1.0f;
    ResourceHandle<TextureTag> texture;
};
struct RenderEnvironmentInput {
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
