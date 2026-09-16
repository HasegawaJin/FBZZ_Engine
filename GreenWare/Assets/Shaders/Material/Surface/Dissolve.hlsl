// FBZZ Engine
// Material/Surface/Dissolve.hlsl | Material
// ディゾルブ (消滅) エフェクト — プロシージャルノイズで面をカットし、
// 消滅エッジを emissiveColor で発光させる PBR ベースシェーダー。
//
// alphaCutoff : ディゾルブ進行量 (0 = 完全表示, 1 = 完全消滅)
// emissiveColor * emissiveScale : エッジ発光色と強度

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Rendering/SpecularAA.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/MaterialTextures.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;             // RGBA ベースカラー              offset  0
    float  metallic;           // 金属度 [0,1]                   offset 16
    float  roughness;          // 粗さ   [0,1]                   offset 20
    float  normalStrength;     // 法線マップ強度                  offset 24
    float  occlusionStrength;  // AO 強度 [0,1]                  offset 28
    float3 emissiveColor;      // エッジ発光色                   offset 32
    float  emissiveScale;      // エッジ発光強度                 offset 44
    float2 uvTiling;           // UV タイリング                  offset 48
    float2 uvOffset;           // UV オフセット                  offset 56
    float  alphaCutoff;        // ディゾルブ進行量 (0=表示, 1=消滅) offset 64
    float3 _pad0;              //                                offset 68
    uint   textureMask;        // テクスチャフラグ                offset 80
    float3 _pad1;              //                                offset 84

    // bindless のテクスチャ添字。Material::Upload が毎フレーム書き込む。
    // ここに宣言した枠だけが Inspector に出る (Common/MaterialTextures.hlsli)。
    uint texAlbedoIndex;
    uint texNormalIndex;
    uint texMetallicIndex;
    uint texEmissiveIndex;
    uint texAOIndex;
};

FBZZ_TEX2D_T(float, texShadow, TEX_SHADOW_SLOT);
FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);
FBZZ_MATERIAL_TEX(texNormal, texNormalIndex);
FBZZ_MATERIAL_TEX(texMetallicRough, texMetallicIndex);
FBZZ_MATERIAL_TEX(texEmissive, texEmissiveIndex);
FBZZ_MATERIAL_TEX(texAO, texAOIndex);
SamplerState           sampDefault      : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow       : register(SAMPLER_SHADOW);

// --- ノイズ -----------------------------------------------------------
// 格子点ハッシュ: 2D → [0,1]
float Hash21(float2 p)
{
    p = frac(p * float2(127.1f, 311.7f));
    p += dot(p, p + 19.19f);
    return frac(p.x * p.y);
}

// バイキュービックスムーズ補間によるバリューノイズ
float ValueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(Hash21(i),                Hash21(i + float2(1.0f, 0.0f)), u.x),
                lerp(Hash21(i + float2(0.0f, 1.0f)), Hash21(i + float2(1.0f, 1.0f)), u.x), u.y);
}

// 4 オクターブ fBm — 細かい凹凸入り繊維状パターン
float DissolveMask(float2 uv)
{
    float n  = ValueNoise(uv * 4.0f)         * 0.500f;
          n += ValueNoise(uv * 8.0f)         * 0.250f;
          n += ValueNoise(uv * 16.0f)        * 0.125f;
          n += ValueNoise(uv * 32.0f)        * 0.125f;
    return saturate(n);
}
// ----------------------------------------------------------------------

PSInput VSMain(VSInput v)
{
    PSInput o;
    float4 worldPos4 = mul(float4(v.position, 1.0f), world);
    o.worldPos   = worldPos4.xyz;
    o.svPosition = mul(worldPos4, viewProjection);
    o.normal     = normalize(mul(v.normal,  (float3x3)worldInvTranspose));
    o.tangent    = normalize(mul(v.tangent, (float3x3)world));
    o.uv         = v.uv;
    return o;
}

float4 PSMain(PSInput p) : SV_Target0
{
    float2 uv = p.uv * uvTiling + uvOffset;

    // Albedo
    float4 albedoSample = (textureMask & (1u << 0))
        ? texAlbedo.Sample(sampDefault, uv)
        : albedo;
    float3 col   = albedoSample.rgb * albedo.rgb;
    float  alpha = albedoSample.a  * albedo.a;

    // ディゾルブ: alphaCutoff をしきい値にノイズでカット。
    // エッジ幅 0.06 のバンドで発光強度を計算する。
    float  mask      = DissolveMask(uv);
    float  edgeWidth = 0.06f;
    clip(mask - alphaCutoff);
    float  edgeFactor = saturate((mask - alphaCutoff) / edgeWidth);

    // Normal
    float3 N = normalize(p.normal);
    if (textureMask & (1u << 1))
    {
        float3 ns = texNormal.Sample(sampDefault, uv).rgb;
        float3 nm = ApplyNormalMap(ns, N, normalize(p.tangent));
        N = normalize(lerp(N, nm, normalStrength));
    }

    // Metallic / Roughness
    float met   = metallic;
    float rough = roughness;
    if (textureMask & (1u << 2))
    {
        float2 mr = texMetallicRough.Sample(sampDefault, uv).gb;
        rough = mr.x;
        met   = mr.y;
    }
    rough = FilterSpecularRoughness(N, saturate(rough));

    // AO
    float ao = 1.0f;
    if (textureMask & (1u << 4))
        ao = lerp(1.0f, texAO.Sample(sampDefault, uv).r, occlusionStrength);

    float3 V      = normalize(cameraPos - p.worldPos);
    float3 L      = normalize(-lightDir);
    float  shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    // Forward の画面空間 AO / 接触影。Deferred では b8 が 0 なので素通りする。
    shadow *= FBZZ_ScreenContactShadow(p.svPosition.xy);
    ao *= FBZZ_ScreenAO(p.svPosition.xy);
    float3 result = Lighting_PBR(N, V, L, col, met, rough,
                                 lightColor, lightIntensity, shadow, ao);

    // 点光源 / スポットライト — 走査元は clusterLightMode が決める
    // (b3 の固定長配列 / StructuredBuffer / クラスタリスト)。
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        result += Lighting_PBR_Direct(N, V, ps.L, col, met, saturate(rough + ps.roughnessBias),
            ps.color, ps.intensity);
    FBZZ_PUNCTUAL_END

    // 通常エミッシブ
    float3 emissiveTex = (textureMask & (1u << 3))
        ? texEmissive.Sample(sampDefault, uv).rgb
        : float3(1.0f, 1.0f, 1.0f);
    result += emissiveTex * emissiveColor * emissiveScale;

    // エッジ発光: しきい値直上の帯が通常エミッシブより 2 倍明るく燃える。
    // edgeFactor=0 がしきい値エッジ (最大発光)、1 が帯の外側 (消灯)。
    // WHY: emissiveScale だけでは通常エミッシブと同強度になり「燃える」印象が出ないため、
    //      定数 2.0 を掛けてエッジ帯を明示的に強調する。
    result += emissiveColor * emissiveScale * 2.0f * (1.0f - edgeFactor);

    return float4(result, alpha);
}