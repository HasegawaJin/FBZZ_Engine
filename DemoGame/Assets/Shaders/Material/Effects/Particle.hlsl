// FBZZ Engine
// Material/Effects/Particle.hlsl | Material
// CPU パーティクル用ビルボードシェーダー
// PSO: SOLID_NOCULL + ADDITIVE/ALPHA_BLEND + DEPTH_READ

#include "Common/Binding.hlsli"
#include "Common/Constants.hlsli"
#include "Platform/Backend.hlsli"

Texture2D    gParticleTex : register(TEX_ALBEDO);
SamplerState gSampler     : register(SAMPLER_DEFAULT);

struct ParticleVSIn
{
    float3 center : POSITION;   // ワールド空間パーティクル中心
    float2 uv     : TEXCOORD0;  // クワッドコーナー UV [0,1]
    float4 color  : COLOR;      // RGBA (alpha = フェード乗数)
    float  size   : TEXCOORD1;  // ビルボードの一辺サイズ (ワールド単位)
    float  rotation : TEXCOORD2;
    float4 uvRect   : TEXCOORD3; // xy=min, zw=max
};

struct ParticlePSIn
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
    float2 localUv    : TEXCOORD1;
    float4 color      : COLOR;
};

ParticlePSIn VSMain(ParticleVSIn v)
{
    // row-major view 行列の列 0, 1 = カメラ空間 X / Y 軸のワールド向き
    float3 right = float3(view[0][0], view[1][0], view[2][0]);
    float3 up    = float3(view[0][1], view[1][1], view[2][1]);

    // UV [0,1] → corner オフセット [-1, +1]
    float2 corner   = v.uv * 2.0f - 1.0f;
    float  s = sin(v.rotation);
    float  c = cos(v.rotation);
    corner = float2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);
    float3 worldPos = v.center
                    + right * corner.x * v.size * 0.5f
                    + up    * corner.y * v.size * 0.5f;

    ParticlePSIn o;
    o.svPosition = mul(float4(worldPos, 1.0f), viewProjection);
    o.uv         = lerp(v.uvRect.xy, v.uvRect.zw, v.uv);
    o.localUv    = v.uv;
    o.color      = v.color;
    return o;
}

float4 PSMain(ParticlePSIn p) : SV_Target0
{
    // 中心から外側にかけてソフトフェード (加算合成なので alpha で輝度調整)
    float2 d    = p.localUv * 2.0f - 1.0f;
    float  fade = saturate(1.0f - dot(d, d));
    fade *= fade;
    float4 tex = gParticleTex.Sample(gSampler, p.uv);
    return tex * float4(p.color.rgb, p.color.a * fade);
}
