// FBZZ Engine
// Terrain.hlsl | Terrain
// ハイトマップ地形の頂点・ピクセルシェーダー
//
// 定数バッファスロット:
//   b0 = CameraConstants   (per-frame)
//   b1 = TerrainCB         (per-chunk: world / WVP / layer tiling / normal strength)
//   b3 = LightConstants    (per-frame)
//
// テクスチャスロット:
//   t0 = スプラットマップ  RGBA8 (R=layer0, G=layer1, B=layer2, A=layer3)
//   t1 = layer0 ディフューズ
//   t2 = layer1 ディフューズ
//   t3 = layer2 ディフューズ
//   t4 = layer3 ディフューズ
//   t5 = layer0 法線
//   t6 = layer1 法線
//   t7 = layer2 法線
//   t8 = layer3 法線
//   t9  = layer0 AO/Roughness (R=AO, G=Roughness)
//   t10 = layer1 AO/Roughness
//   t11 = layer2 AO/Roughness
//   t12 = layer3 AO/Roughness
//   t13 = shadow depth
//
// サンプラースロット:
//   s0 = WRAP_ANISOTROPIC  ディフューズ用（タイリングあり）
//   s1 = BORDER_ZERO       shadow PCF 用比較サンプラー
//   s2 = CLAMP_LINEAR      スプラットマップ用（UV を [0,1] にクランプ）
//
// 頂点フォーマット (C++ 側 TerrainVertex と同期すること):
//   POSITION  : float3  offset  0  (12 bytes)
//   NORMAL    : float3  offset 12  (12 bytes)
//   TANGENT   : float3  offset 24  (12 bytes)
//   TEXCOORD0 : float2  offset 36  ( 8 bytes)
//   stride = 44 bytes

#include "Common/Binding.hlsli"

// ============================================================================
// 定数バッファ宣言
// WHY: Common/Constants.hlsli は b1 に ObjectConstants を定義するが、
//      地形は同スロットを TerrainCB として使うためここで直接宣言する。
// ============================================================================

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

cbuffer TerrainCB : register(CB_OBJECT)
{
    float4x4 worldMatrix;           // テレインローカル → ワールド変換
    float4x4 wvpMatrix;            // テレインローカル → クリップ空間変換
    float4   layerTiling[4];       // xy = tilingX, tilingZ (per layer)
    float4   layerNormalStrength;  // xyzw = normalStrength (per layer)
    float4   layerMaterial[4];     // x=roughness, y=AO
    float4   layerTextureFlags;     // xyzw = AO/Roughness texture exists per layer
    float4   layerAutoHeight[4];    // x=minHeight, y=maxHeight, z=fade, w=enabled
    float4   layerAutoSlope[4];     // x=minSlope, y=maxSlope, z=fade, w=strength
};

#define MAX_POINT_LIGHTS 8
#define MAX_SPOT_LIGHTS  4

struct PointLightData { float3 position; float range; float3 color; float intensity; };
struct SpotLightData  {
    float3 position; float range;
    float3 direction; float innerCos;
    float3 color; float outerCos;
    float  intensity; float3 _pad;
};

cbuffer LightConstants : register(CB_LIGHT)
{
    float3         lightDir;       float _lightPad;
    float3         lightColor;     float lightIntensity;
    PointLightData pointLights[MAX_POINT_LIGHTS];
    SpotLightData  spotLights[MAX_SPOT_LIGHTS];
    int            pointLightCount;
    int            spotLightCount;
    float2         _lightPad2;
    float3         ambientColor;
    float          _ambientPad;
};

cbuffer ShadowConstants : register(CB_SHADOW)
{
    float4x4 lightViewProjection;
    float2   shadowMapTexelSize;
    float    shadowBias;
    float    shadowStrength;   // 0=影なし, 1=完全な影
    int      shadowPcfRadius;  // PCF カーネル半径: 0=ハード, 1=3x3, 2=5x5, 3=7x7
    float    _shadowPcfPad[3];
};

// WHY: LightConstants の ambientColor グローバルを参照するため cbuffer 宣言の後に include する。
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"

// ============================================================================
// テクスチャ・サンプラー宣言 (Phase 4)
//
// g_splatmap (t0):
//   RGBA8 スプラットマップ。各チャンネルが各レイヤーのブレンドウェイトを表す。
//   4 チャンネルの合計が 1 になるよう正規化してある（ペイントツールが保証）。
//   シェーダー側でも安全のため正規化する。
//
// g_diffuse[4] (t1-t4):
//   各レイヤーのアルベドテクスチャ。未設定レイヤーは 1×1 白テクスチャで代替。
//   WHY: シェーダーは常に [unroll] 4 レイヤー固定でブレンドする。
//        未設定レイヤーの splat ウェイトは 0 なので白テクスチャの寄与は 0 になる。
//        動的分岐を排除することで GPU パイプラインの効率を上げる。
//
// s0 = ディフューズ用 Wrap Anisotropic（タイリング UV で繰り返しサンプリング）
// s1 = シャドウ用 Comparison Border Zero（ライト錐台外を非遮蔽として扱う）
// s2 = スプラットマップ用 Clamp Linear（UV が [0,1] を超えた場合に端値を維持）
// ============================================================================
Texture2D    g_splatmap      : register(t0);
Texture2D    g_diffuse[4]    : register(t1); // t1, t2, t3, t4
Texture2D    g_normal[4]     : register(t5); // t5, t6, t7, t8
Texture2D    g_aoRoughness[4] : register(t9); // R=AO, G=Roughness
Texture2D<float> g_shadowMap : register(t13);
SamplerState g_sampler       : register(s0); // Wrap Anisotropic
SamplerComparisonState g_shadowSampler : register(s1);
SamplerState g_samplerClamp  : register(s2); // Clamp Linear

// ============================================================================
// 頂点入力・補間構造体
// ============================================================================
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

// ============================================================================
// 頂点シェーダー
// ============================================================================
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

float3 BlendTerrainNormal(float3 worldTangent, float3 geometricNormal, float2 uv, float4 splat)
{
    // TBN を頂点シェーダーから受け取ったタンジェントで構築する。
    // DDX/DDY による画面空間微分を廃止し、ピクセルシェーダーの計算コストを削減する。
    float3 T = normalize(worldTangent);
    float3 B = normalize(cross(geometricNormal, T));

    float3 blended = float3(0.0f, 0.0f, 0.0f);
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float2 tiledUV = uv * layerTiling[i].xy;
        float3 tn = g_normal[i].Sample(g_sampler, tiledUV).xyz * 2.0f - 1.0f;
        tn.xy *= layerNormalStrength[i];
        tn = normalize(tn);
        blended += normalize(T * tn.x + B * tn.y + geometricNormal * tn.z) * splat[i];
    }
    return normalize(blended);
}

float TerrainBandMask(float value, float minValue, float maxValue, float fade)
{
    // WHAT: [minValue, maxValue] の範囲内を 1、範囲外を 0 に近づける滑らかなマスク。
    // WHY: 高さ・傾斜の境界を硬く切ると等高線状の境目が見えるため、fade 幅で自然に混ぜる。
    if (maxValue <= minValue)
        return 1.0f;

    float f = max(fade, 0.0001f);
    float lower = smoothstep(minValue, minValue + f, value);
    float upper = 1.0f - smoothstep(maxValue - f, maxValue, value);
    return saturate(lower * upper);
}

float4 ApplyAutoBlend(float4 paintedSplat, float localHeight, float slope)
{
    float4 autoWeight = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 autoStrength = float4(0.0f, 0.0f, 0.0f, 0.0f);

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float heightMask = TerrainBandMask(
            localHeight,
            layerAutoHeight[i].x,
            layerAutoHeight[i].y,
            layerAutoHeight[i].z);
        float slopeMask = TerrainBandMask(
            slope,
            layerAutoSlope[i].x,
            layerAutoSlope[i].y,
            layerAutoSlope[i].z);
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

// ============================================================================
// ピクセルシェーダー (スプラットマップ 4 レイヤーブレンド)
// ============================================================================
float4 PSMain(TerrainPSInput p) : SV_Target0
{
    float3 N = normalize(p.worldNormal);
    float3 V = normalize(cameraPos - p.worldPos);
    float3 L = normalize(-lightDir);

    // ── スプラットマップからレイヤーウェイトを取得 ─────────────────
    // スプラットマップは Clamp サンプラーでサンプリングする。
    // WHY: UV が地形端に近い頂点でわずかに [0,1] をはみ出すことがあり、
    //      Wrap だと反対端の値を参照してブレンドが壊れる。
    float4 splat = g_splatmap.Sample(g_samplerClamp, p.uv);

    // 4 チャンネルの合計で正規化（ペイントツールが保証するが数値誤差を安全に処理）
    float wsum = splat.r + splat.g + splat.b + splat.a;
    if (wsum > 0.001f) splat /= wsum;
    float slope = saturate(1.0f - abs(N.y));
    splat = ApplyAutoBlend(splat, p.localHeight, slope);

    // ── 4 レイヤーのアルベドをウェイトブレンド ───────────────────
    // [unroll] を使って静的展開する。
    // WHY: ループ変数で Texture2D 配列を動的インデックスすると SM 5.0 では
    //      テクスチャフェッチが最適化されない場合がある。
    //      [unroll] で展開することで各テクスチャフェッチが独立したコンパイル済み
    //      命令になり、GPU パイプラインが並列フェッチを行いやすくなる。
    float3 albedo = float3(0.0f, 0.0f, 0.0f);
    float  roughness = 0.0f;
    float  ao = 0.0f;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float2 tiledUV = p.uv * layerTiling[i].xy;
        float3 d       = g_diffuse[i].Sample(g_sampler, tiledUV).rgb;
        float2 aoRoughnessTex = g_aoRoughness[i].Sample(g_sampler, tiledUV).rg;
        float hasAoRoughnessTex = saturate(layerTextureFlags[i]);
        float layerAO = lerp(saturate(layerMaterial[i].y), aoRoughnessTex.r, hasAoRoughnessTex);
        float layerRoughness = lerp(saturate(layerMaterial[i].x), aoRoughnessTex.g, hasAoRoughnessTex);
        albedo        += d * splat[i];
        roughness     += layerRoughness * splat[i];
        ao            += layerAO * splat[i];
    }

    N = BlendTerrainNormal(p.worldTangent, N, p.uv, splat);

    float shadow = ComputeShadow(g_shadowMap, g_shadowSampler, p.worldPos,
        lightViewProjection, shadowMapTexelSize, shadowBias, N, L);

    float3 result = Lighting_BlinnPhong(
        N, V, L, albedo, roughness, lightColor, lightIntensity, shadow);

    [loop]
    for (int pi = 0; pi < pointLightCount; ++pi)
    {
        float3 toLight = pointLights[pi].position - p.worldPos;
        float  dist    = length(toLight);
        float  atten   = LightAttenuation(dist, pointLights[pi].range);
        result += Lighting_BlinnPhong_Direct(
            N, V, toLight / dist, albedo, roughness,
            pointLights[pi].color, pointLights[pi].intensity * atten);
    }

    [loop]
    for (int si = 0; si < spotLightCount; ++si)
    {
        float3 toLight = spotLights[si].position - p.worldPos;
        float  dist    = length(toLight);
        float3 Ls      = toLight / dist;
        float  atten   = LightAttenuation(dist, spotLights[si].range);
        float  cone    = SpotConeWeight(Ls, spotLights[si].direction,
                             spotLights[si].innerCos, spotLights[si].outerCos);
        result += Lighting_BlinnPhong_Direct(
            N, V, Ls, albedo, roughness,
            spotLights[si].color, spotLights[si].intensity * atten * cone);
    }

    // AO は直接光を完全に消さず、地形の谷・泥・岩陰の環境光成分を中心に抑える。
    result *= lerp(0.35f + ao * 0.65f, 1.0f, saturate(dot(N, L)));

    return float4(result, 1.0f);
}
