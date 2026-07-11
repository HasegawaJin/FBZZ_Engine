// FBZZ Engine
// PostProcess/Cloud/CloudUpscale.hlsl | PostProcess
// ハーフ解像度で描いたボリューメトリック雲 (scatter.rgb + alpha) をフル解像度 HDR へ
// バイリニアでアップスケールして ALPHA_BLEND 合成する。
//
// WHY: 雲はレイマーチが重く低周波なので 1/2 解像度で描けば描画ピクセルが 1/4 になる。
//      合成側はサンプル 1 回のみで安価。scatter/alpha は雲なし画素で共に 0 のため、
//      バイリニア補間しても比率が保たれ、元のフル解像度合成と同じ見た目になる。
#include "Common/Binding.hlsli"

Texture2D<float4> g_cloudHalf : register(t0);
SamplerState      sampLinear  : register(SAMPLER_DEFAULT); // s0: CLAMP_LINEAR

struct FSTriVSOut
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

FSTriVSOut VSMain(uint id : SV_VertexID)
{
    FSTriVSOut o;
    o.uv = float2((id & 1u) ? 2.0f : 0.0f,
                  (id & 2u) ? 2.0f : 0.0f);
    o.svPosition = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

float4 PSMain(FSTriVSOut p) : SV_Target0
{
    return g_cloudHalf.Sample(sampLinear, p.uv);
}
