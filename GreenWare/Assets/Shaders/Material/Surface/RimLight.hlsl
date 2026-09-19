// FBZZ Engine
// Material/Surface/RimLight.hlsl | Material
// Blinn-Phong + リムライト(フレネル輪郭発光) + 法線マップ + PCF シャドウ

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/MaterialTextures.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;        // RGBA ベースカラー                 offset 0
    float  roughness;     // スペキュラの広がり [0,1]          offset 16
    float  rimPower;      // リム輪郭の鋭さ [1,16]             offset 20
    float  rimIntensity;  // リム輝度スケール                  offset 24
    uint   textureMask;   // テクスチャ有効フラグ              offset 28
    float3 rimColor;      // リムライトの色                    offset 32
    float  _pad;          //                                  offset 44

    // bindless のテクスチャ添字。Material::Upload が毎フレーム書き込む。
    // ここに宣言した枠だけが Inspector に出る (Common/MaterialTextures.hlsli)。
    uint texAlbedoIndex;
    uint texNormalIndex;
};

FBZZ_TEX2D_T(float, texShadow, TEX_SHADOW_SLOT);
FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);
FBZZ_MATERIAL_TEX(texNormal, texNormalIndex);
SamplerState           sampDefault : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow  : register(SAMPLER_SHADOW);

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
    float3 col = (textureMask & 1u)
        ? texAlbedo.Sample(sampDefault, p.uv).rgb
        : albedo.rgb;

    float3 Ngeo = normalize(p.normal);
    float3 N    = Ngeo;
    if (textureMask & (1u << 1))
    {
        float3 normalSample = texNormal.Sample(sampDefault, p.uv).rgb;
        N = ApplyNormalMap(normalSample, N, normalize(p.tangent));
    }

    float3 V      = normalize(cameraPos - p.worldPos);
    float3 L      = normalize(-lightDir);
    float  shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    // Forward の画面空間 AO / 接触影。Deferred では b8 が 0 なので素通りする。
    shadow *= FBZZ_ScreenContactShadow(p.svPosition.xy);
    float3 result = Lighting_BlinnPhong(N, V, L, col, roughness,
                                        lightColor, lightIntensity, shadow);

    // 点光源 / スポットライト — 走査元は clusterLightMode が決める
    // (b3 の固定長配列 / StructuredBuffer / クラスタリスト)。
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        result += Lighting_BlinnPhong_Direct(N, V, ps.L, col, saturate(roughness + ps.roughnessBias),
            ps.color, ps.intensity);
    FBZZ_PUNCTUAL_END

    // リムライト: Ngeo でシルエットを安定させ、rimPower=0 の pow(x,0)=1 フラッシュを防ぐ
    float rimFactor = pow(1.0f - saturate(dot(Ngeo, V)), max(rimPower, 0.01f));
    result += rimColor * rimIntensity * rimFactor;

    return float4(result, 1.0f);
}