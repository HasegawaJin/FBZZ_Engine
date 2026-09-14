/// @file    VFXLine.hlsl
/// @brief   VFX Line (雷・ビーム) の帯。芯とグロー・太さのムラ・流れる輝点を 1 枚で描く
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// PSO: SOLID_NOCULL + (.mat の blend_mode) + DEPTH_READ
///
/// 頂点は VFXLineSystem が組む «中心線» で、帯へ広げるのは VS。
///   POSITION = 中心線の点 / NORMAL = 線の向き / TANGENT.x = 半幅 [m]
///   TEXCOORD = (線に沿った進み [0,1], 帯の左右 0 / 1) / COLOR = 本流と枝の明るさ
/// WHY VS で広げるか: CPU で広げるとゲームのメインカメラへ向けるしかなく、Scene ビューや
///     別のカメラからは帯が紙のように潰れて見える。VS なら描いているビューのカメラへ正対する。
///
/// WHY cbuffer の名前を MaterialConstants にするか: .mat の [params] と per-instance の上書き
///     (VFXLineSystem が書く intensity / phase) はリフレクションでこの名前の変数表を引く。
#include "Common/Binding.hlsli"

#define FBZZ_MATERIAL_CONSTANTS
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;        // グローの色 × 不透明度 (VFXLineSystem が毎フレーム書く)
    float4 coreColor;     // 芯の色 (HDR)
    float  coreWidth;     // 芯の太さ [0,1]
    float  glowFalloff;   // 縁の減衰指数。大きいほどグローが細い
    float  intensity;     // 明滅と出入りを含む明るさ (毎フレーム書く)
    float  phase;         // 経過時間 [秒] (毎フレーム書く)
    float  breakup;       // 芯の太さのムラ [0,1]
    float  pulseSpeed;    // 流れる輝点の速さ
    float  pulseDensity;  // 輝点の数 (0 で流さない)
    float  endFade;       // 両端の絞りの長さ [0,0.5]
};

#include "Common/Constants.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/ParticleNoise.hlsli" // ValueNoise3D

// 宣言順は renderer::Vertex と同じにすること (入力レイアウトは宣言順に詰めて作られる)。
struct LineVSIn
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float3 tangent  : TANGENT;
    float2 uv       : TEXCOORD;
    float4 color    : COLOR;
};

struct LinePSIn
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
    float4 color      : COLOR;
};

LinePSIn VSMain(LineVSIn v)
{
    const float3 center = mul(float4(v.position, 1.0f), world).xyz;
    const float3 along = mul(v.normal, (float3x3)world);
    const float3 toCamera = cameraPos - center;
    float3 side = cross(along, toCamera);
    // 線をちょうど軸方向から見ると外積が消える。そのときは上方向で代用する (一瞬点に潰れる方がまし)。
    if (dot(side, side) < 1.0e-12f) side = cross(along, float3(0.0f, 1.0f, 0.0f));
    side = normalize(side);
    const float3 worldPos = center + side * (v.tangent.x * (v.uv.y * 2.0f - 1.0f));

    LinePSIn o;
    o.svPosition = mul(float4(worldPos, 1.0f), viewProjection);
    o.uv = v.uv;
    o.color = v.color;
    return o;
}

float4 PSMain(LinePSIn input) : SV_Target0
{
    const float along = saturate(input.uv.x);
    // 帯の中心を 0、両縁を 1 にした横断座標。断面の形はすべてこの値から作る。
    const float across = saturate(abs(input.uv.y - 0.5f) * 2.0f);

    // 芯の太さのムラ。時間でずらすと、同じ形でも毎フレーム違う放電に見える。
    const float grain = ValueNoise3D(float3(along * 14.0f + phase * 3.7f, 0.5f, 0.0f)) * 0.5f + 0.5f;
    const float thickness = lerp(1.0f, grain, saturate(breakup));
    const float core = 1.0f - smoothstep(0.0f, max(coreWidth * thickness, 1.0e-4f), across);
    const float glow = pow(saturate(1.0f - across), max(glowFalloff, 0.01f));

    // 両端を絞る。絞らないと帯の切り口が四角く残り、手や電極から板が生えているように見える。
    const float fade = max(endFade, 1.0e-4f);
    const float ends = smoothstep(0.0f, fade, along) * smoothstep(0.0f, fade, 1.0f - along);

    // 帯に沿って流れる輝点。向きのある線 (吸い取る・撃ち出す) の «どちらへ» を見せる。
    float bead = 0.0f;
    if (pulseDensity > 0.0f)
    {
        const float lane = along * pulseDensity - phase * pulseSpeed;
        const float distance = abs(frac(lane) - 0.5f) * 2.0f;
        bead = pow(saturate(1.0f - distance), 8.0f) * pow(saturate(1.0f - across), 1.6f);
    }

    float3 color = albedo.rgb * glow + coreColor.rgb * core + lerp(albedo.rgb, coreColor.rgb, 0.4f) * bead * 1.5f;
    color *= max(intensity, 0.0f) * input.color.rgb;
    float alpha = saturate(glow * 0.85f + core + bead * 0.5f) * ends * albedo.a * input.color.a;
    // 太さのムラは明るさよりアルファに強く効かせる (明るさだけ落とすと «灰色の帯» になる)。
    alpha *= lerp(1.0f, thickness, saturate(breakup) * 0.75f);
    clip(alpha - 0.002f);
    return float4(color, alpha);
}
