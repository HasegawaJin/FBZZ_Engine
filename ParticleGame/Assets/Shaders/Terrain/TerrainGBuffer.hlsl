// FBZZ Engine
// Terrain/TerrainGBuffer.hlsl | Terrain (Deferred)
// Terrain を Deferred GBuffer (MRT) へ書き出す。スプラット 4 レイヤーをブレンドし、
// albedo / roughness / worldNormal / metallic を出力する。
//
// WHY: ライティングを DeferredLighting に委ねることで、GTAO/SSAO/ContactShadows/SSR/IBL が
//      他の不透明オブジェクトと同じように地形へも効く。Forward の Terrain.hlsl は前方互換のため残す。
//
// MRT レイアウト (Pipeline/Deferred/GBuffer.hlsl と一致させること):
//   SV_Target0 (RGBA16F): RGB=albedo(linear), A=roughness
//   SV_Target1 (RGBA16F): RGB=worldNormal*0.5+0.5, A=metallic
#include "Common/Binding.hlsli"
#include "Common/Color.hlsli"

cbuffer TerrainCB : register(CB_OBJECT)
{
    float4x4 worldMatrix;           // テレインローカル → ワールド変換
    float4x4 wvpMatrix;             // テレインローカル → クリップ空間変換
    float4   layerTiling[4];        // xy = tilingX, tilingZ (per layer)
    float4   layerNormalStrength;   // xyzw = normalStrength (per layer)
    float4   layerMaterial[4];      // x=roughness, y=AO
    float4   layerTextureFlags;     // xyzw = AO/Roughness texture exists per layer
    float4   layerAutoHeight[4];    // x=minHeight, y=maxHeight, z=fade, w=enabled
    float4   layerAutoSlope[4];     // x=minSlope, y=maxSlope, z=fade, w=strength
};

// テクスチャ・サンプラーは Terrain.hlsl と同一スロット (C++ TerrainRenderPass のバインドと一致)。
Texture2D    g_splatmap     : register(t0);
Texture2D    g_diffuse[4]   : register(t1);  // t1..t4
Texture2D    g_normal[4]    : register(t5);  // t5..t8
Texture2D    g_aoRoughness[4] : register(t9); // t9..t12 (R=AO, G=Roughness)
SamplerState g_sampler      : register(s0);  // Wrap Anisotropic (タイリング)
SamplerState g_samplerClamp : register(s2);  // Clamp Linear (スプラットマップ)

struct TerrainVSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float3 tangent  : TANGENT;
    float2 uv       : TEXCOORD0;
};

struct TerrainPSInput
{
    float4 svPosition   : SV_POSITION;
    float3 worldPos     : TEXCOORD0;
    float3 worldNormal  : TEXCOORD1;
    float2 uv           : TEXCOORD2;
    float  localHeight  : TEXCOORD3;
    float3 worldTangent : TEXCOORD4;
};

// GBuffer.hlsl の GBufferOut と同一レイアウト。
struct GBufferOut
{
    float4 albedoRoughness : SV_Target0;
    float4 normalMetallic  : SV_Target1;
};

TerrainPSInput VSMain(TerrainVSInput v)
{
    TerrainPSInput o;
    o.svPosition   = mul(float4(v.position, 1.0f), wvpMatrix);
    float4 wpos4   = mul(float4(v.position, 1.0f), worldMatrix);
    o.worldPos     = wpos4.xyz;
    o.worldNormal  = normalize(mul(v.normal,  (float3x3)worldMatrix));
    o.worldTangent = normalize(mul(v.tangent, (float3x3)worldMatrix));
    o.uv           = v.uv;
    o.localHeight  = v.position.y;
    return o;
}

// Terrain.hlsl と同一: タンジェント空間法線マップを 4 レイヤーぶんブレンドする。
float3 BlendTerrainNormal(float3 worldTangent, float3 geometricNormal, float2 uv, float4 splat)
{
    float3 Ng = normalize(geometricNormal);
    float3 T  = normalize(worldTangent - Ng * dot(Ng, worldTangent));
    float3 B  = normalize(cross(Ng, T));

    float3 blended = float3(0.0f, 0.0f, 0.0f);
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float2 tiledUV = uv * layerTiling[i].xy;
        float3 tn = g_normal[i].Sample(g_sampler, tiledUV).xyz * 2.0f - 1.0f;
        tn.xy *= layerNormalStrength[i];
        tn = normalize(tn);
        blended += normalize(T * tn.x + B * tn.y + Ng * tn.z) * splat[i];
    }
    return normalize(blended);
}

// Terrain.hlsl と同一: 高さ/傾斜の自動ブレンドマスク。
float TerrainBandMask(float value, float minValue, float maxValue, float fade)
{
    if (maxValue <= minValue)
        return 1.0f;
    float f = max(fade, 0.0001f);
    float lower = smoothstep(minValue, minValue + f, value);
    float upper = 1.0f - smoothstep(maxValue - f, maxValue, value);
    return saturate(lower * upper);
}

float4 ApplyAutoBlend(float4 paintedSplat, float localHeight, float slope)
{
    float4 autoWeight   = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 autoStrength = float4(0.0f, 0.0f, 0.0f, 0.0f);

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float heightMask = TerrainBandMask(localHeight,
            layerAutoHeight[i].x, layerAutoHeight[i].y, layerAutoHeight[i].z);
        float slopeMask = TerrainBandMask(slope,
            layerAutoSlope[i].x, layerAutoSlope[i].y, layerAutoSlope[i].z);
        float enabled = saturate(layerAutoHeight[i].w);
        autoStrength[i] = saturate(layerAutoSlope[i].w) * enabled;
        autoWeight[i] = heightMask * slopeMask * autoStrength[i];
    }

    float autoSum = autoWeight.r + autoWeight.g + autoWeight.b + autoWeight.a;
    if (autoSum <= 0.001f)
        return paintedSplat;

    autoWeight /= autoSum;
    float blend = saturate(max(max(autoStrength.r, autoStrength.g), max(autoStrength.b, autoStrength.a)));
    float4 result = lerp(paintedSplat, autoWeight, blend);
    float sum = result.r + result.g + result.b + result.a;
    return sum > 0.001f ? result / sum : paintedSplat;
}

GBufferOut PSMain(TerrainPSInput p)
{
    // 幾何法線（傾斜計算と法線マップの基底に使う）。
    float3 Ng = normalize(p.worldNormal);

    // スプラットマップ（Clamp）→ 正規化 → 高さ/傾斜の自動ブレンド。
    float4 splat = g_splatmap.Sample(g_samplerClamp, p.uv);
    float wsum = splat.r + splat.g + splat.b + splat.a;
    if (wsum > 0.001f) splat /= wsum;
    float slope = saturate(1.0f - abs(Ng.y));
    splat = ApplyAutoBlend(splat, p.localHeight, slope);

    // 4 レイヤーの albedo / roughness をウェイトブレンド。
    // WHY: sRGB テクスチャを線形化して出力する（GBuffer.hlsl と同じ規約）。AO は GBuffer に
    //      チャンネルが無く、画面空間 GTAO/SSAO が担うため、ここでは出力しない（メッシュと同じ扱い）。
    float3 albedo    = float3(0.0f, 0.0f, 0.0f);
    float  roughness = 0.0f;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float2 tiledUV = p.uv * layerTiling[i].xy;
        float3 d       = SRGBToLinear(g_diffuse[i].Sample(g_sampler, tiledUV).rgb);
        float2 aoRough = g_aoRoughness[i].Sample(g_sampler, tiledUV).rg;
        float  hasTex  = saturate(layerTextureFlags[i]);
        float  layerRough = lerp(saturate(layerMaterial[i].x), aoRough.g, hasTex);
        albedo    += d * splat[i];
        roughness += layerRough * splat[i];
    }

    float3 N = BlendTerrainNormal(p.worldTangent, Ng, p.uv, splat);

    // 地形は土・草・岩などのマット面。鏡面 IBL（青空の反射）は不自然な青い艶として残るため、
    // roughness を高く固定して鏡面反射をほぼ消す。WHY: 地形に「そもそも」空の鏡面反射は要らない。
    //      grazing 角で出ていた青い縁/部分の主因がこれ。粗くするほど prefilter は最も鈍い mip を
    //      サンプルし brdf bias も下がるため、鏡面 IBL がほぼ無視できる量まで落ちる。
    const float kTerrainMinRoughness = 0.97f;
    float terrainRoughness = max(saturate(roughness), kTerrainMinRoughness);

    GBufferOut o;
    o.albedoRoughness = float4(albedo, terrainRoughness);
    o.normalMetallic  = float4(N * 0.5f + 0.5f, 0.0f); // 地形は非金属
    return o;
}
