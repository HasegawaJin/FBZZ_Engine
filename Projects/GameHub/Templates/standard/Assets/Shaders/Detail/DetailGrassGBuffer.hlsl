// FBZZ Engine
// Detail/DetailGrassGBuffer.hlsl | VS + PS (Deferred)
// プロシージャル草ブレードを Deferred GBuffer (MRT) へ書き出す変種。
// VS は DetailGrass.hlsl と同一（手続き生成・風・カメラ向きビルボード）。PS だけ GBuffer 出力に差し替える。
//
// WHY: forward の DetailGrass.hlsl は abs(dot) の簡易両面ライティングだった。Deferred では片面
//      N·L になるため、カメラ向きの生の法線だと太陽光で暗くなりやすい。法線を上向きへ寄せて
//      地面と同じように陽を受けるようにし、AO/接触影の恩恵だけを得る。
//
// MRT: SV_Target0=albedo(linear)+roughness / SV_Target1=worldNormal*0.5+0.5+metallic
#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Color.hlsli"
#include "Platform/DX11.hlsli"

cbuffer DetailGrassCB : register(CB_MATERIAL)
{
    float3 windDir;
    float  gTime;
    float  windStrength;
    float  windFrequency;
    float  bladeHeight;
    float  bladeWidth;
    int    bladeSegments;
    float  alphaCutoff;
    int    hasAlbedoTex;
    float  _gPad;
};

struct GrassInstance
{
    float3 pos;
    float  rotY;
    float  scale;
    float  windPhase;
};
StructuredBuffer<GrassInstance> g_Instances : register(t0);

Texture2D    gAlbedo  : register(TEX_ALBEDO);
SamplerState gSampler : register(SAMPLER_DEFAULT);

struct PsIn
{
    float4 svPos   : SV_POSITION;
    float2 uv      : TEXCOORD0;
    float  bladetT : TEXCOORD1;
    float3 normal  : NORMAL;
};

struct GBufferOut
{
    float4 albedoRoughness : SV_Target0;
    float4 normalMetallic  : SV_Target1;
};

static const float kSideU[6] = { -0.5f,  0.5f, -0.5f,  0.5f,  0.5f, -0.5f };
static const float kIsTop[6] = {  0.0f,  0.0f,  1.0f,  0.0f,  1.0f,  1.0f };

// VS は DetailGrass.hlsl と同一。
PsIn VSMain(uint vertId : SV_VertexID, uint instId : SV_InstanceID)
{
    GrassInstance inst = g_Instances[instId];

    int   segIdx  = (int)(vertId / 6);
    int   localV  = (int)(vertId % 6);
    float sideU   = kSideU[localV];
    float isTop   = kIsTop[localV];

    float t = (segIdx + isTop) / (float)bladeSegments;

    float3 viewDir = cameraPos - inst.pos;
    viewDir.y = 0.0f;
    float viewLenSq = dot(viewDir, viewDir);
    viewDir = (viewLenSq > 1e-6f)
        ? viewDir * rsqrt(viewLenSq)
        : float3(0.0f, 0.0f, 1.0f);
    float3 bladeRight   = normalize(cross(float3(0.0f, 1.0f, 0.0f), viewDir));
    float3 bladeForward = normalize(cross(bladeRight, float3(0.0f, 1.0f, 0.0f)));

    float width = bladeWidth * inst.scale * (1.0f - t * 0.8f);

    float windAmount = sin(gTime * windFrequency + inst.windPhase)
                     * windStrength * inst.scale * t * t;
    float3 windOffset = float3(windDir.x, 0.0f, windDir.z) * windAmount;

    float3 basePos  = inst.pos;
    float3 worldPos = basePos
                    + bladeRight * sideU * width
                    + float3(0.0f, bladeHeight * inst.scale * t, 0.0f)
                    + windOffset;

    float3 normal = normalize(lerp(bladeForward, float3(0.0f, 1.0f, 0.0f), t * 0.5f));

    float2 uv = float2(sideU + 0.5f, 1.0f - t);

    PsIn o;
    o.svPos   = mul(float4(worldPos, 1.0f), viewProjection);
    o.uv      = uv;
    o.bladetT = t;
    o.normal  = normal;
    return o;
}

GBufferOut PSMain(PsIn p)
{
    float4 col;
    if (hasAlbedoTex)
    {
        col = gAlbedo.Sample(gSampler, p.uv);
    }
    else
    {
        float3 baseColor = float3(0.10f, 0.30f, 0.04f);
        float3 tipColor  = float3(0.30f, 0.52f, 0.08f);
        col = float4(lerp(baseColor, tipColor, p.bladetT), 1.0f);
    }

    if (alphaCutoff > 0.0f)
        clip(col.a - alphaCutoff);

    // 草はカメラ向きで生法線が水平気味のため、片面ライティングだと暗くなる。
    // 上向きへ強めに寄せて、地面と同様に空・太陽光を受けるようにする。
    float3 N = normalize(lerp(normalize(p.normal), float3(0.0f, 1.0f, 0.0f), 0.6f));

    GBufferOut o;
    o.albedoRoughness = float4(SRGBToLinear(col.rgb), 0.8f);
    o.normalMetallic  = float4(N * 0.5f + 0.5f, 0.0f);
    return o;
}
