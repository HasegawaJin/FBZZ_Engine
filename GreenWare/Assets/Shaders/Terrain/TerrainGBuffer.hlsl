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
#include "Rendering/SpecularAA.hlsli"
#include "Rendering/Wetness.hlsli"
#include "Common/BindlessIndices.hlsli"

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
    float4   weather;               // x=wetness, y=darkening, z=puddleAmount
};

// テクスチャ・サンプラーは Terrain.hlsl と同一スロット (C++ TerrainRenderPass のバインドと一致)。
FBZZ_TEX2D(g_splatmap, 0);
/// @brief レイヤー i のテクスチャ。枠は旧 t1..t4 / t5..t8 / t9..t12 (TerrainRenderPass が textures[1..12] へ差す)。
/// @note テーブル撤去後は register(tN) の配列を束縛できないため、枠番号から添字を引く。
Texture2D TerrainDiffuse(uint i)     { return ResourceDescriptorHeap[NonUniformResourceIndex(FbzzPixelSlot(1u + i))]; }
Texture2D TerrainNormal(uint i)      { return ResourceDescriptorHeap[NonUniformResourceIndex(FbzzPixelSlot(5u + i))]; }
Texture2D TerrainAoRoughness(uint i) { return ResourceDescriptorHeap[NonUniformResourceIndex(FbzzPixelSlot(9u + i))]; }
// 地形レイヤーはワールド座標でタイリングするので wrap。異方性は x4。
// WHY x16 の s0 を使わないか: 地形は画面を広く覆い、レイヤーごとに 3 枚を引くので
//     x16 のコストが枚数ぶん乗る。x4 で見た目はほぼ変わらず約 1/3 のコストで済む。
SamplerState g_sampler      : register(SAMPLER_WRAP_ANISO4);
// スプラットマップは地形 1 枚に 1:1 で貼るので clamp。
SamplerState g_samplerClamp : register(SAMPLER_LINEAR_CLAMP);

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

    // 法線マップの凹凸を強く見せるためのグローバル増幅係数。
    // WHY: Deferred の地形はマット面 (高 roughness) で鏡面反射が弱く、法線マップの陰影が
    //      Forward より地味になりがち。タンジェント法線の XY を増幅すると、鏡面に頼らず
    //      ディフューズ陰影 (N·L) だけでも凹凸を強く出せる。各レイヤー .mat の Normal Strength に
    //      比例するため、マテリアル側でさらに強弱を調整できる。
    const float kTerrainNormalBoost = 3.0f;

    float3 blended = float3(0.0f, 0.0f, 0.0f);
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float2 tiledUV = uv * layerTiling[i].xy;
        float3 tn = TerrainNormal(i).Sample(g_sampler, tiledUV).xyz * 2.0f - 1.0f;
        tn.xy *= layerNormalStrength[i] * kTerrainNormalBoost;
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
        float3 d       = SRGBToLinear(TerrainDiffuse(i).Sample(g_sampler, tiledUV).rgb);
        float2 aoRough = TerrainAoRoughness(i).Sample(g_sampler, tiledUV).rg;
        float  hasTex  = saturate(layerTextureFlags[i]);
        float  layerRough = lerp(saturate(layerMaterial[i].x), aoRough.g, hasTex);
        albedo    += d * splat[i];
        roughness += layerRough * splat[i];
    }

    float3 N = BlendTerrainNormal(p.worldTangent, Ng, p.uv, splat);

    // 地形は土・草・岩などのマット面だが、roughness を高く固定しすぎると法線マップを浮き立たせる
    // スペキュラ（特に太陽光の直接反射）まで消えてしまう。
    // WHY: 以前は青空 IBL の鏡面艶を消す目的で 0.97 に固定していたが、これは Forward
    //      (Terrain.hlsl / Blinn-Phong, material roughness 0.8) では出ていた法線由来のスペキュラを
    //      Deferred で丸ごと潰し、「Deferred だと地形の法線マップが効いていない」原因になっていた。
    //      そこでレイヤーマテリアルの roughness をそのまま尊重し（Forward と同じ挙動）、鏡のように
    //      なるのを防ぐ控えめな下限だけを設ける。青空の鏡面反射が気になる場合は各レイヤー .mat の
    //      Roughness を上げる（= マテリアル側で調整可能）。IBL 全体の鏡面量は iblSpecularScale で別途調整できる。
    const float kTerrainMinRoughness = 0.6f;
    float terrainRoughness = max(saturate(roughness), kTerrainMinRoughness);

    // 天候。地形は b8 を宣言できないので TerrainCB から値を手渡す。
    const WetSurface wet = ApplyWetness(albedo, terrainRoughness, N,
                                        weather.x, weather.y, weather.z);
    albedo           = wet.albedo;
    terrainRoughness = wet.roughness;

    // kTerrainNormalBoost で XY を 3 倍した法線は遠景で 1 ピクセルあたりの振れが大きく、
    // 太陽の反射がタイル模様に明滅する。増幅した分はここで粗さへ返す。
    terrainRoughness = FilterSpecularRoughness(N, terrainRoughness);

    GBufferOut o;
    o.albedoRoughness = float4(albedo, terrainRoughness);
    o.normalMetallic  = float4(N * 0.5f + 0.5f, 0.0f); // 地形は非金属
    return o;
}
