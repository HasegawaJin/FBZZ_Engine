// FBZZ Engine
// Foliage/Foliage.hlsl | VS + PS
// 複数SubMesh樹木・大型植生向けGPU Instancingシェーダー
//
// WHY: LightConstants の ambientColor を参照するため Common/Constants.hlsli を使用する。
//      Unlit モードで RenderSystem が ambientColor={1,1,1}/lightIntensity=0 を設定するため、
//      ambientColor を乗算するだけで分岐なしに Unlit が自然に機能する。
#define FBZZ_MATERIAL_CONSTANTS  // FoliageMaterialCB で MaterialConstants を上書きするため
#include "Common/Constants.hlsli"
#include "Platform/Backend.hlsli"

cbuffer FoliageMaterialCB : register(CB_MATERIAL)
{
    float4 baseColor;
    uint hasAlbedo;
    float alphaCutoff;
    float2 _foliagePad;
    // ── 風スウェイ (WindZone) ── C++ FoliageMaterialCB と一致させること
    float3 windDir;       // ワールド風向き (正規化済み)
    float  windTime;      // 累積時間
    float  windStrength;  // 揺れ振幅スケール。0 で無効
    float  windFrequency; // 揺れ周波数
    float2 _windPad;
};

// 風スウェイ: 根元 (ローカル Y=0) を固定し、高い頂点ほど大きく風下へ振る (高さ二乗重み)。
// WHY: 位相をインスタンス位置でずらして全樹木の同期揺れを避け、
//      周波数の異なる 2 波を合成して単調な sin 揺れに見えないようにする。
float3 WindSway(float3 worldPosition, float localHeight, float2 instanceXZ)
{
    if (windStrength <= 0.0f)
        return worldPosition;
    const float height = max(localHeight, 0.0f);
    const float phase  = dot(instanceXZ, float2(0.37f, 0.71f));
    const float wave   = sin(windTime * windFrequency + phase) * 0.7f
                       + sin(windTime * windFrequency * 2.33f + phase * 1.7f) * 0.3f;
    return worldPosition + windDir * (wave * windStrength * 0.02f * height * height);
}

struct FoliageInstance
{
    float3 pos;
    float rotY;
    float scale;
};
StructuredBuffer<FoliageInstance> g_Instances : register(t0);

Texture2D gAlbedo : register(TEX_ALBEDO);
SamplerState gSampler : register(SAMPLER_DEFAULT);

struct VsIn
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float2 uv : TEXCOORD0;
};

struct PsIn
{
    float4 svPos : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

PsIn VSMain(VsIn input, uint instanceId : SV_InstanceID)
{
    FoliageInstance instance = g_Instances[instanceId];
    const float s = sin(instance.rotY);
    const float c = cos(instance.rotY);
    const float3x3 rotation = float3x3(
         c, 0.0f, s,
         0.0f, 1.0f, 0.0f,
        -s, 0.0f, c);

    float3 worldPosition =
        mul(rotation, input.position * instance.scale) + instance.pos;
    worldPosition = WindSway(worldPosition, input.position.y * instance.scale, instance.pos.xz);

    PsIn output;
    output.svPos = mul(float4(worldPosition, 1.0f), viewProjection);
    output.normal = normalize(mul(rotation, input.normal));
    output.uv = input.uv;
    return output;
}

float4 PSMain(PsIn input) : SV_Target0
{
    float4 color = baseColor;
    if (hasAlbedo)
        color *= gAlbedo.Sample(gSampler, input.uv);
    if (alphaCutoff > 0.0f)
        clip(color.a - alphaCutoff);

    const float ndotl = saturate(dot(normalize(input.normal), normalize(-lightDir)));
    // ambientColor は Lit モードで {0.08,...}、Unlit モードで RenderSystem が {1,1,1} に設定する。
    // lightIntensity は Unlit モードで 0 になるため、Unlit 時は ambientColor のみが乗算される。
    color.rgb *= ambientColor + lightColor * (ndotl * lightIntensity);
    return color;
}
