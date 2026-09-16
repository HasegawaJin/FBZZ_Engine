// FBZZ Engine
// Material/Effects/Trail.hlsl | Material
// TrailComponent 用リボンメッシュシェーダー
// PSO: SOLID_NOCULL + ALPHA_BLEND + DEPTH_READ

#include "Common/Binding.hlsli"

#define FBZZ_MATERIAL_CONSTANTS
cbuffer TrailConstants : register(CB_MATERIAL)
{
    // colorStart / colorEnd は CPU 側でリニア化済み (GeometryPasses.hpp の TrailCB を参照)。
    float4 colorStart;
    float4 colorEnd;
    float  uvScrollSpeed;
    float  uvTiling;
    float  trailTime;
    // bit0 = テクスチャが sRGB エンコード。
    uint   gTrailFlags;
    // 多キー色 (GeometryPasses.hpp の TrailCB と一致させること)。
    // gGradientKeyCount = 0 なら colorStart / colorEnd の 2 点で描く。
    float4 gGradientColors[8]; // リニア化済み
    float4 gGradientTimes[2];  // 8 個のキー時刻
    uint   gGradientKeyCount;
    uint   gGradientInterp;    // 0=Linear / 1=Step / 2=Smooth
    uint2  _gGradientPad;
};

#include "Common/Color.hlsli"
#include "Common/Constants.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

#define FBZZ_TRAIL_SRGB_TEXTURE 1u

FBZZ_TEX2D(gTrailTex, TEX_ALBEDO_SLOT);
SamplerState gSampler  : register(SAMPLER_DEFAULT);

struct TrailVSIn
{
    float3 position : POSITION;
    float  age      : TEXCOORD0;
    float  v        : TEXCOORD1;
    float  u        : TEXCOORD2;
};

struct TrailPSIn
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
    float  age        : TEXCOORD1;
};

TrailPSIn VSMain(TrailVSIn input)
{
    TrailPSIn output;
    output.svPosition = mul(float4(input.position, 1.0f), viewProjection);
    output.uv = float2(input.u, input.v);
    output.age = input.age;
    return output;
}

// 補間係数へ曲線モードを適用する。
// LAYOUT: ParticleCurve.hpp の ApplyCurveInterpolation と同じ式にすること。
float ApplyTrailCurveAlpha(float alpha, uint mode)
{
    if (mode == 1u) return alpha >= 1.0f ? 1.0f : 0.0f;       // Step
    if (mode == 2u) return alpha * alpha * (3.0f - 2.0f * alpha); // Smooth
    return alpha;
}

// キー i の時刻。float4 × 2 に詰めてあるので、成分の取り出しは分岐で書く。
// WHY 動的な成分添字 (packed[lane]) にしないか: ベクトルの添字アクセスは
//     コンパイラによってスカラー配列へ降ろされ、定数バッファの読み方が変わる。
float TrailGradientTime(uint index)
{
    const float4 packed = gGradientTimes[index >> 2u];
    const uint lane = index & 3u;
    if (lane == 0u) return packed.x;
    if (lane == 1u) return packed.y;
    if (lane == 2u) return packed.z;
    return packed.w;
}

// 多キー色の評価。t=0 が帯の先端 (colorStart 側)、t=1 が消え際。
//
// WHY リニア空間で混ぜるか: キーは CPU 側でリニア化して届く。ここでオーサリング
//     空間へ戻して混ぜ直すと、Oklab まで含めた 3 種類の混ぜ方を帯のためだけに
//     シェーダーへ持ち込むことになる。キーそのものの色は一致する。
float4 EvaluateTrailGradient(float t)
{
    const uint count = min(max(gGradientKeyCount, 1u), 8u);
    if (t <= TrailGradientTime(0u)) return gGradientColors[0];

    float4 result = gGradientColors[count - 1u];
    bool resolved = false;
    [unroll]
    for (uint i = 1u; i < 8u; ++i)
    {
        if (resolved || i >= count) continue;
        const float keyTime  = TrailGradientTime(i);
        const float prevTime = TrailGradientTime(i - 1u);
        if (t > keyTime) continue;

        const float span  = max(keyTime - prevTime, 0.0001f);
        const float alpha = ApplyTrailCurveAlpha(saturate((t - prevTime) / span), gGradientInterp);
        result = lerp(gGradientColors[i - 1u], gGradientColors[i], alpha);
        resolved = true;
    }
    return result;
}

float4 PSMain(TrailPSIn input) : SV_Target0
{
    // WHY: uvTiling を先に掛けることで、テクスチャの繰り返し回数とスクロール速度を独立して調整できる。
    float2 uv = float2(input.uv.x * uvTiling + trailTime * uvScrollSpeed, input.uv.y);
    float4 tex = gTrailTex.Sample(gSampler, uv);
    // 素材は他のマテリアルと同じ規約でシェーダー側がリニア化する
    // (このエンジンは _SRGB フォーマットの SRV を作らない)。
    if ((gTrailFlags & FBZZ_TRAIL_SRGB_TEXTURE) != 0u) tex.rgb = SRGBToLinear(tex.rgb);
    // age は 0 が最古の点、1 が最新の点。グラデーションの時刻は先端から数えるので反転する。
    const float age = saturate(input.age);
    float4 color = lerp(colorEnd, colorStart, age);
    // 三項演算子にすると両辺が評価され、キーが無い帯でも 8 段の探索を毎画素で払う。
    [branch]
    if (gGradientKeyCount >= 2u) color = EvaluateTrailGradient(1.0f - age);
    return tex * color;
}
