// FBZZ Engine
// Debug/OverdrawHeatmap.hlsl | PostProcess
// ParticleOverdraw.hlsl が積んだ重なり枚数をヒートマップへ変換する
// PSO: postproc (フルスクリーン三角形, ブレンドなし)
//
// 入力 t5 の R チャンネル = そのピクセルを塗ったパーティクル枚数。
// 段階の境目が分かるよう連続グラデーションではなく帯で区切る
// (「何枚重なっているか」を色から読み取れるようにするため)。

#include "Common/Binding.hlsli"
#include "Common/Constants.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(gOverdraw, 5);
SamplerState gSampler  : register(SAMPLER_DEFAULT);

struct VSOut
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

VSOut VSMain(uint vertexId : SV_VertexID)
{
    // 画面を覆う 1 枚の三角形 (フルスクリーン矩形より頂点が少なく継ぎ目も出ない)
    VSOut o;
    o.uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.svPosition = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

float4 PSMain(VSOut p) : SV_Target0
{
    float layers = gOverdraw.Sample(gSampler, p.uv).r;
    if (layers < 0.5f) return float4(0.0f, 0.0f, 0.0f, 0.0f);   // Alpha Blend時はモデルを残す

    // 1〜2 枚: 青 (問題なし) / 3〜4: 緑 / 5〜8: 黄 / 9〜16: 橙 / 17+: 赤
    float3 color;
    if      (layers < 3.0f)  color = float3(0.15f, 0.30f, 0.85f);
    else if (layers < 5.0f)  color = float3(0.20f, 0.75f, 0.35f);
    else if (layers < 9.0f)  color = float3(0.90f, 0.85f, 0.20f);
    else if (layers < 17.0f) color = float3(0.95f, 0.50f, 0.15f);
    else                     color = float3(1.00f, 0.15f, 0.10f);

    // 帯の中でも枚数の増加が分かるよう、明るさに緩やかな傾きを付ける。
    const float shade = saturate(0.55f + layers * 0.03f);
    return float4(color * shade, 1.0f);
}
