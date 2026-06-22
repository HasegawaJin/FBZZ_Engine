// FBZZ Engine
// Foliage/Foliage.hlsl | VS + PS
// 複数SubMesh樹木・大型植生向けGPU Instancingシェーダー
#include "Common/Binding.hlsli"
#include "Platform/DX11.hlsli"

cbuffer CameraConstants : register(CB_CAMERA)
{
    float4x4 view;
    float4x4 projection;
    float4x4 viewProjection;
    float4x4 invViewProjection;
    float3 cameraPos;
    float nearZ;
    float farZ;
    float3 _camPad;
};

#define FBZZ_MATERIAL_CONSTANTS
cbuffer FoliageMaterialCB : register(CB_MATERIAL)
{
    float4 baseColor;
    uint hasAlbedo;
    float alphaCutoff;
    float2 _foliagePad;
};

cbuffer LightConstants : register(CB_LIGHT)
{
    float3 lightDir;
    float _lightPad0;
    float3 lightColor;
    float lightIntensity;
};

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

    const float3 worldPosition =
        mul(rotation, input.position * instance.scale) + instance.pos;

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
    color.rgb *= (0.30f + ndotl * lightIntensity * 0.70f) * lightColor;
    return color;
}
