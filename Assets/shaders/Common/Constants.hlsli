// FBZZ Engine
// Constants.hlsli | Common
// Shared constant-buffer declarations for shaders
#pragma once

#include "Binding.hlsli"

cbuffer CameraConstants : register(CB_CAMERA)
{
    float4x4 view;
    float4x4 projection;
    float4x4 viewProjection;
    float4x4 invViewProjection;
    float3   cameraPos;
    float    nearZ;
    float    farZ;
    float3   _camPad;
};

cbuffer ObjectConstants : register(CB_OBJECT)
{
    float4x4 world;
    float4x4 worldInvTranspose;
};

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;
    float  metallic;
    float  roughness;
    float  emissiveScale;
    uint   textureMask;
};

#define MAX_POINT_LIGHTS 8
#define MAX_SPOT_LIGHTS  4

struct PointLightData
{
    float3 position;
    float  range;
    float3 color;
    float  intensity;
};

struct SpotLightData
{
    float3 position;
    float  range;
    float3 direction;
    float  innerCos;
    float3 color;
    float  outerCos;
    float  intensity;
    float3 _pad;
};

cbuffer LightConstants : register(CB_LIGHT)
{
    float3 lightDir;
    float  _lightPad;
    float3 lightColor;
    float  lightIntensity;
    PointLightData pointLights[MAX_POINT_LIGHTS];
    SpotLightData  spotLights[MAX_SPOT_LIGHTS];
    int   pointLightCount;
    int   spotLightCount;
    float2 _lightPad2;
};

cbuffer ShadowConstants : register(CB_SHADOW)
{
    float4x4 lightViewProjection;
    float2   shadowMapTexelSize;
    float    shadowBias;
    float    _shadowPad;
};

cbuffer PostProcConstants : register(CB_POSTPROC)
{
    float2 texelSize;
    float2 screenSize;
    float  exposure;
    float  time;
    float  fogDensity;
    float  bloomIntensity;
    float3 fogColor;
    float  fogFar;
    float  contrast;
    float  saturation;
    float  hueShift;
    float  temperature;
    float  tint;
    float  vignetteIntensity;
    float  vignetteSmoothness;
    float  vignetteRoundness;
    float3 vignetteColor;
    float  filmGrainIntensity;
    float  filmGrainResponse;
    float  chromaticAberration;
    float  lensDistortion;
    float  customIntensity;
    float  customBlend;
    float4 customParameters;
    float2 _ppPad;
};

cbuffer AtmosphereConstants : register(CB_ATMOSPHERE)
{
    float3 rayleighScattering;
    float  mieScattering;
    float  planetRadius;
    float  atmosphereRadius;
    float  sunIntensity;
    float  mieG;
};

#define MAX_SKINNING_BONES 128
cbuffer SkinningConstants : register(CB_SKINNING)
{
    float4x4 boneMatrices[MAX_SKINNING_BONES];
};
