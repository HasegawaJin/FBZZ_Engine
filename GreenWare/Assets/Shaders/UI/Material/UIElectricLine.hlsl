// FBZZ Engine
// Material/UIElectricLine.hlsl | UI
// ロゴの下線に沿って走る放電。矩形の左端 (＋) から右端 (−) へ、細い稲妻が
// ときどき飛ぶ。テクスチャを読まず、矩形の中で式だけで描く。
//
// WHY ロゴに «装飾» として放電を足すか:
//   タイトルの盤面では電極が火花を散らしている。題字の下線が «ただの線» だと、
//   題字が盤面と別の世界の紙に見える。同じ線の上を放電が走れば、題字が 2 極の
//   あいだに «張られている» ことが絵で言える (ロゴの ＋ − の記号と対応する)。
//
// WHY 常時ではなく burst で出すか:
//   常時走っていると «壊れた蛍光灯» になり、目が休まらない。数秒に 1 回、
//   0.3 秒だけ飛べば、静止している時間の方が長く «ときどき火花を拾う» になる。
//   出すタイミングは ScreenDressingComponent が持つ (ロゴの縁の放電と同時)。
//
// WHY 稲妻を 3 本重ねるか:
//   1 本だと «線が揺れている» にしか見えない。太さと揺れの違う 3 本を重ね、
//   芯 1 本だけ白く、脇の 2 本を極の色にすると、放電の «束» に見える。
#include "UI/UICommon.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 plusColor;     // 左端の色                                offset  0
    float4 minusColor;    // 右端の色                                offset 16
    float4 coreColor;     // 芯の色 (ほぼ白)                         offset 32
    float  burst;         // 放電の強さ 0..1。script が叩く          offset 48
    float  phase;         // 位相 [秒]。script が進める              offset 52
    float  lineY;         // 下線の位置 [0,1] (矩形の上からの割合)   offset 56
    float  amplitude;     // 揺れの幅 [矩形の高さに対する割合]       offset 60
    float  thickness;     // 芯の太さ [px]                           offset 64
    float  glowWidth;     // 光の裾 [px]                             offset 68
    float  segments;      // 折れの数 (横方向のノイズの周期)         offset 72
    float  _pad0;         //                                         offset 76
};

UIPixelInput VSMain(UIVertexInput input)
{
    return UIVertexMain(input);
}

float Hash21(float2 p)
{
    return frac(sin(dot(p, float2(12.9898f, 78.233f))) * 43758.5453f);
}

float ValueNoise(float2 p)
{
    const float2 i = floor(p);
    const float2 f = frac(p);
    const float2 u = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(Hash21(i), Hash21(i + float2(1, 0)), u.x),
                lerp(Hash21(i + float2(0, 1)), Hash21(i + float2(1, 1)), u.x), u.y);
}

// 1 本の稲妻。x で横位置、seed で本ごとの揺れ、返り値は (芯, 裾) の明るさ。
float2 Bolt(float2 px, float2 rect, float seed, float amp, float thick, float soft, float t)
{
    const float x = px.x / max(rect.x, 1.0f);
    // 折れ線。粗い揺れ + 細かい揺れ。時間で形が入れ替わる (同じ形が続くと «描いた線»)。
    const float n1 = ValueNoise(float2(x * segments, seed + floor(t * 24.0f) * 0.37f)) - 0.5f;
    const float n2 = ValueNoise(float2(x * segments * 3.7f, seed * 1.7f + floor(t * 24.0f) * 0.53f)) - 0.5f;
    // 両端は下線に固定する (端が浮くと «どこから来た線か» が判らない)。
    const float pin = sin(x * 3.14159265f);
    const float y   = (lineY + (n1 * 0.8f + n2 * 0.35f) * amp * pin) * rect.y;
    const float d   = abs(px.y - y);
    const float core = 1.0f - smoothstep(0.0f, thick, d);
    const float halo = 1.0f - smoothstep(0.0f, soft, d);
    return float2(core, halo * halo);
}

float4 PSMain(UIPixelInput input) : SV_TARGET
{
    const float2 rect = g_Rect.xy;
    const float2 px   = input.localUv * rect;
    const float  b    = saturate(burst);

    // 放電は左から右へ «伸びる». burst の立ち上がりで先頭が走り、抜けは全体が薄れる。
    const float x     = input.localUv.x;
    const float reach = smoothstep(x - 0.08f, x + 0.02f, b * 1.3f);

    const float2 b0 = Bolt(px, rect, 1.3f, amplitude,        thickness,        glowWidth,        phase);
    const float2 b1 = Bolt(px, rect, 7.1f, amplitude * 1.6f, thickness * 0.6f, glowWidth * 0.7f, phase);
    const float2 b2 = Bolt(px, rect, 4.4f, amplitude * 1.2f, thickness * 0.5f, glowWidth * 0.6f, phase);

    // 色は左 ＋ → 右 −。芯は白、脇の 2 本は極の色。
    const float3 pole = lerp(plusColor.rgb, minusColor.rgb, x);
    float3 rgb   = coreColor.rgb * b0.x * coreColor.a
                 + pole * (b0.y * 0.9f + b1.x * 0.8f + b2.x * 0.7f + (b1.y + b2.y) * 0.35f);
    float  alpha = saturate(b0.x + b0.y * 0.8f + b1.x * 0.8f + b2.x * 0.7f + (b1.y + b2.y) * 0.3f);

    // 明滅。放電はちらつくもの ─ ただし周期を速くしすぎると «点滅» になる。
    const float flicker = 0.75f + 0.25f * ValueNoise(float2(phase * 37.0f, 3.0f));
    alpha *= reach * b * flicker;

    float4 result = float4(rgb, alpha) * UITint(input);
    clip(result.a - 0.002f);
    return result;
}
