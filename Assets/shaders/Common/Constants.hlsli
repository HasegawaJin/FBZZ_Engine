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

// シェーダーごとに独自の MaterialConstants を宣言したい場合は
// #include より前に #define FBZZ_MATERIAL_CONSTANTS を定義する。
// 全パラメータが必要な PBR 系シェーダーはこのデフォルト定義を使う。
#ifndef FBZZ_MATERIAL_CONSTANTS
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;                // RGBA ベースカラー               offset  0
    float  metallic;              // 金属度 [0, 1]                   offset 16
    float  roughness;             // 粗さ   [0, 1]                   offset 20
    float  normalStrength;        // 法線マップ強度                   offset 24
    float  occlusionStrength;     // AO 強度 [0, 1]                  offset 28
    float3 emissiveColor;         // エミッシブ色 (emissiveScale と乗算) offset 32
    float  emissiveScale;         // エミッシブ強度                   offset 44
    float2 uvTiling;              // UV タイリング (X, Y)             offset 48
    float2 uvOffset;              // UV オフセット (X, Y)             offset 56
    float  alphaCutoff;           // アルファカットオフしきい値         offset 64
    float3 _matPad0;              //                                  offset 68
    uint   textureMask;           // テクスチャ存在フラグ (bit 0-4)    offset 80
    float3 _matPad1;              //                                  offset 84
};
#endif // FBZZ_MATERIAL_CONSTANTS

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
    float  ssaoIntensity;
    float  customIntensity;
    float  customBlend;
    float2 _customPad;
    float4 customParameters;
    float  underwaterStrength;
    float  underwaterDepth;
    float2 _underwaterPad;
    float3 underwaterColor;
    float  underwaterFogDensity;
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
