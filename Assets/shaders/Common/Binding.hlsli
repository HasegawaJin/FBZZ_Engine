// FBZZ Engine
// Binding.hlsli | Common
// レジスタ番号の一元定義。全シェーダーがこのファイルを参照する
#ifndef BINDING_HLSLI
#define BINDING_HLSLI

// ---- cbuffer --------------------------------------------------------
#define CB_CAMERA       b0  // CameraConstants   per-frame
#define CB_OBJECT       b1  // ObjectConstants   per-draw
#define CB_MATERIAL     b2  // MaterialConstants per-material
#define CB_LIGHT        b3  // LightConstants    per-frame
#define CB_SHADOW       b4  // ShadowConstants   per-frame
#define CB_POSTPROC     b5  // PostProcConstants per-pass
#define CB_SKINNING     b7  // SkinningConstants per-animated draw
#define CB_ATMOSPHERE   b6  // AtmosphereConstants per-frame (Skydome のみ)

// ---- Texture (マテリアル, per-draw) ----------------------------------
#define TEX_ALBEDO          t0
#define TEX_NORMAL          t1
#define TEX_METALLIC_ROUGH  t2  // R=metallic  G=roughness
#define TEX_EMISSIVE        t3
#define TEX_AO              t4

// ---- Texture (パスリソース, per-pass) --------------------------------
#define TEX_GBUFFER0    t5   // albedo(RGB) + roughness(A)
#define TEX_GBUFFER1    t6   // normal(RGB) + metallic(A)
#define TEX_DEPTH       t7
#define TEX_SHADOW      t8
#define TEX_SSAO        t9
#define TEX_BLOOM       t10
#define TEX_ENV_CUBE    t11  // Skybox キューブマップ / IBL
#define TEX_ENV_EQUIRECT t12 // Skydome 等緯度テクスチャ

// ---- UAV (コンピュートシェーダー出力) ---------------------------------
#define UAV_OUTPUT      u0
#define UAV_OUTPUT2     u1

// ---- Sampler ---------------------------------------------------------
#define SAMPLER_DEFAULT s0
#define SAMPLER_SHADOW  s1   // SamplerComparisonState (PCF 用)

#endif // BINDING_HLSLI
