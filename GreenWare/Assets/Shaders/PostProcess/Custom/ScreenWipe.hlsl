/// @file    ScreenWipe.hlsl
/// @brief   マスクテクスチャで画面を塗り潰す / 剥がすシーン遷移
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// .mat (PP_ScreenWipe.mat) から来るもの
///   albedo (t0) — マスク。R = その画素が «塗られる順» 0..1 (0 が最初、1 が最後)
/// customParameters
///   x — 覆い 0..1。0 で何も無し、1 で全面が塗られている
///   y — 縁の柔らかさ (マスク値の幅)。0.02 で硬い、0.15 で霞む
///   z — 縁の光の太さ (マスク値の幅)。0 で光らない
///   w — マスクの回転 [rad]。0 で素材のまま
/// customParameters2
///   xyz — 縁の光の色 (プレイヤー色)
///   w   — 縁の光の強さ
/// 塗る色はほぼ黒で固定 (kPaint)。遷移の «扉» は作品を通して同じ色でなければ扉に見えない。
/// customIntensity — 1 固定 (包絡は覆いが持つ)
///
/// WHY screenFadeAlpha (黒の lerp) をやめたか:
///   全画面が一様に暗くなるのは «電源が落ちた» の絵で、遊びの区切りとして何も言わない。
///   マスクで «形を持って» 塗られると、遷移そのものが作品の語彙 (装甲の破片 / 絞り) になる。
///
/// WHY 覆いの向きを 1 つの値で持つか:
///   出 (塗る) と入 (剥がす) は同じマスクの同じ式で、覆いが 0→1 か 1→0 かの違いしか無い。
///   剥がす側を別式にすると、出と入で縁の形が変わり、«同じ扉» に見えない。
///
/// WHY 縁を光らせるか:
///   塗り色と画面の境目が硬いと «貼った紙» に見える。境目に細い光の帯を走らせると
///   «切り取られていく» に変わり、暗い側が影として読める。

#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Common/Color.hlsli"
#include "Platform/Backend.hlsli"

Texture2D    texMask    : register(TEX_ALBEDO);
Texture2D    texInput   : register(TEX_GBUFFER0);
SamplerState sampLinear : register(SAMPLER_LINEAR_CLAMP);

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    const float3 orig = texInput.SampleLevel(sampLinear, p.uv, 0).rgb;

    const float cover    = saturate(customParameters.x);
    const float soft     = max(customParameters.y, 0.002f);
    const float glowW    = max(customParameters.z, 0.0f);
    const float rotation = customParameters.w;
    const float3 glowColor = customParameters2.xyz;
    const float glowGain = max(customParameters2.w, 0.0f);
    static const float3 kPaint = float3(0.02f, 0.025f, 0.03f);

    // マスクは正方形。画面比を戻して読み、回転は中心まわり。
    const float aspect = screenSize.x / max(screenSize.y, 1.0f);
    float2 m = p.uv - 0.5f;
    m.x *= aspect;
    const float c = cos(rotation), s = sin(rotation);
    m = float2(m.x * c - m.y * s, m.x * s + m.y * c);
    // 回転しても四隅が欠けないよう、少し縮めて収める。
    m = m / (max(aspect, 1.0f) * 1.12f) + 0.5f;
    const float mask = texMask.SampleLevel(sampLinear, m, 0).r;

    // 覆いを «柔らかさぶん» 外へ広げて、0 と 1 で完全に閉じる。
    const float threshold = cover * (1.0f + 2.0f * soft + glowW) - soft;
    // mask が threshold より小さい画素ほど «先に» 塗られる。
    const float covered   = 1.0f - smoothstep(threshold - soft, threshold + soft, mask);
    // 縁の光: 覆いの境目のすぐ外側 (まだ塗られていない側) に帯。
    const float ahead = saturate((mask - threshold) / max(glowW, 0.001f));   // 0 = 境目, 1 = 帯の外
    const float glow  = glowW > 0.0f ? (1.0f - ahead) * step(threshold, mask) * (1.0f - covered) : 0.0f;

    float3 result = lerp(orig, kPaint, covered);
    // 光は二乗で細く、その外に薄い裾。塗られた側 (covered) には乗らない。
    result += glowColor * (glow * glow * glowGain + glow * glowGain * 0.25f);

    return float4(lerp(orig, result, saturate(customBlend)), 1.0f);
}
