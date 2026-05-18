// FBZZ Engine
// Constants.hlsli | Common
// cbuffer 宣言。全シェーダーが #include して使用する
#pragma once

#include "Binding.hlsli"

// ---- b0: CameraConstants (per-frame) ------------------------------------
cbuffer CameraConstants : register(CB_CAMERA)
{
    float4x4 view;
    float4x4 projection;
    float4x4 viewProjection;
    float4x4 invViewProjection;  // worldPos 復元用 (DeferredLighting / SSAO)
    float3   cameraPos;
    float    nearZ;
    float    farZ;
    float3   _camPad;
};

// ---- b1: ObjectConstants (per-draw) -------------------------------------
cbuffer ObjectConstants : register(CB_OBJECT)
{
    float4x4 world;
    float4x4 worldInvTranspose;  // 非一様スケール対応の法線変換行列
};

// ---- b2: MaterialConstants (per-material) --------------------------------
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;
    float  metallic;
    float  roughness;
    float  emissiveScale;
    uint   textureMask;  // bit0=albedo bit1=normal bit2=metalRough bit3=emissive
};

// ---- b3: LightConstants (per-frame) -------------------------------------
cbuffer LightConstants : register(CB_LIGHT)
{
    float3 lightDir;       // ワールド空間、正規化済み (光の進行方向)
    float  _lightPad;
    float3 lightColor;
    float  lightIntensity;
};

// ---- b4: ShadowConstants (per-frame) ------------------------------------
cbuffer ShadowConstants : register(CB_SHADOW)
{
    float4x4 lightViewProjection;
    float2   shadowMapTexelSize;  // 1.0 / shadowMapResolution (PCF オフセット計算用)
    float    shadowBias;
    float    _shadowPad;
};

// ---- b5: PostProcConstants (per-pass) -----------------------------------
cbuffer PostProcConstants : register(CB_POSTPROC)
{
    float2 texelSize;    // 1.0 / screenSize
    float2 screenSize;
    float  exposure;
    float  time;
    float2 _ppPad;
};

// ---- b6: AtmosphereConstants (per-frame, Skydome のみ) ------------------
cbuffer AtmosphereConstants : register(CB_ATMOSPHERE)
{
    float3 rayleighScattering;  // 波長ごとの散乱係数 (RGB)
    float  mieScattering;
    float  planetRadius;        // 地球半径 (km)
    float  atmosphereRadius;    // 大気圏上端 (km)
    float  sunIntensity;
    float  mieG;                // Mie 位相関数の非対称パラメータ (-1〜1)
};
