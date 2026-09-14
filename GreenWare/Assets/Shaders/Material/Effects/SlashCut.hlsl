/// @file    SlashCut.hlsl
/// @brief   斬撃が当たった瞬間の «一閃»。当たり点を斬った向きに横切る 1 本の光の線
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// PSO: SOLID_NOCULL + PREMULTIPLIED + DEPTH_READ
///
/// 頂点は LineRendererComponent が組む帯 (SlashCutFxComponent → BeamTrailRendererComponent)。
/// uv.x = 線の端から端 [0,1]、uv.y = 帯の横断 [0,1]。
///
/// WHY Beam.hlsl を流用しないか:
///   撚り・粒・リング・明滅は «流れ続けるエネルギー» の語彙で、0.3 秒で消える «切れた線» には
///   要らない。どれも 0 にすれば消せるが、そのぶん «切れ目がどう見えるか» を決める式が
///   ビームの式の奥へ埋もれる。断面だけを持つ短い 1 本に分けた。
///
/// WHY 定数の名前を Beam.hlsl と揃えるか (coreWidth / edgeFalloff / coreBoost / tiling /
///     scroll / muzzleFade / tipFade):
///   BeamTrailRendererComponent::PushMaterial がこの名前で per-instance に書く。揃えておけば
///   帯を張る部品に手を入れずに済む。それ以外 (taper / detail) は .mat の値がそのまま効く。
///
/// WHY 両端を «尖らせる» か:
///   帯の幅は長さ方向に一定なので、そのままだと角の丸い棒になる。刃が通った跡は中央が
///   最も深く両端で抜ける ─ 芯と裾の太さを sin(π·along)^taper で細らせ、両端が針のように
///   消える «レンズ形» にする。
///
/// WHY 芯を白へ揃えるか (Beam.hlsl は揃え «切らない» のに):
///   ビームは «どちらの色か» を遠くから読ませる線で、白へ寄せると色が消える。一閃は
///   «斬れた» の 1 コマで、色は裾と冷めた後の切れ目が持てば足りる。芯が白く抜けるほど鋭く見える。

#include "Common/Binding.hlsli"

#define FBZZ_MATERIAL_CONSTANTS
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    /// 線の色 (HDR)。LineRenderer が startColor から書く。a は «芯がどれだけ背景を隠すか»。
    float4 albedo;

    /// 芯の太さ。帯の半幅に対する割合 [0,1]。
    float  coreWidth;
    /// 裾の減衰指数。大きいほど裾が短く、線が細く見える。
    float  edgeFalloff;
    /// 芯の明るさ倍率。
    float  coreBoost;
    /// 全体の明るさ倍率 (HDR)。
    float  intensity;

    /// 素材 (T_Cut_Line) を長さ方向へ何回繰り返すか。
    float  tiling;
    /// uv.x のずらし。スクリプトが経過で進めて、切れ目の揺らぎを «燻らせる»。
    float  scroll;
    /// 両端の抜け [0,1]。
    float  muzzleFade;
    float  tipFade;

    /// 両端の尖り。大きいほど中央だけが太い針になる。
    float  taper;
    /// 素材の揺らぎを裾へどれだけ乗せるか [0,1]。0 で滑らかな光の線。
    float  detail;
    float2 _cutPad;
};

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"

Texture2D    texAlbedo   : register(TEX_ALBEDO);
SamplerState sampDefault : register(SAMPLER_DEFAULT);

struct SlashCutPSIn
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

SlashCutPSIn VSMain(VSInput v)
{
    SlashCutPSIn o;
    const float4 worldPos = mul(float4(v.position, 1.0f), world);
    o.svPosition = mul(worldPos, viewProjection);
    o.uv         = v.uv;
    return o;
}

float4 PSMain(SlashCutPSIn input) : SV_Target0
{
    const float along  = saturate(input.uv.x);
    const float across = saturate(abs(input.uv.y - 0.5f) * 2.0f);

    // レンズ形: 中央で 1、両端で 0。
    const float lens = pow(saturate(sin(along * 3.14159265f)), max(taper, 0.01f));

    // 芯は硬い縁で切る。柔らかく落とすと «光の筋» にはなるが «切れ目» の鋭さが出ない。
    const float coreHalf = max(coreWidth * lens, 1.0e-4f);
    const float aa = max(fwidth(across), 1.0e-4f);
    const float core = (1.0f - smoothstep(max(coreHalf - aa, 0.0f), coreHalf + aa, across))
                     * saturate(coreHalf / aa);
    // 裾も両端で細る。帯いっぱいに残すと、両端だけ四角い光の板が見える。
    const float glow = pow(saturate(1.0f - across / max(lens, 1.0e-4f)), max(edgeFalloff, 0.01f));

    // 素材の揺らぎは裾にだけ乗せる。芯まで揺らすと線そのものが波打ち、鋭さが鈍る。
    const float fiber = texAlbedo.Sample(sampDefault,
                                         float2(along * max(tiling, 0.0f) + scroll, input.uv.y)).r;
    const float glowDetail = lerp(1.0f, smoothstep(0.12f, 0.8f, fiber), saturate(detail));

    const float muzzle = smoothstep(0.0f, max(muzzleFade, 1.0e-4f), along);
    // NOTE: 下限は smoothstep の上下端が一致して 0 除算になるのを避けるため (Beam.hlsl と同じ)。
    const float tip  = 1.0f - smoothstep(1.0f - max(tipFade, 1.0e-4f), 1.0f, along);
    const float ends = muzzle * tip;

    const float3 tint = albedo.rgb;
    const float  peak = max(max(tint.r, tint.g), tint.b);
    const float3 hot  = float3(peak, peak, peak);

    // 裾と芯を加算し続けると中央が太い白帯になる。芯の外でだけ質感を残す。
    float3 emit = tint * glow * glowDetail * (1.0f - core * 0.75f)
                + hot * core * max(coreBoost, 0.0f);
    emit *= max(intensity, 0.0f) * ends;

    // a = «隠す量»。芯は背景を隠して光り、裾はほぼ純粋な加算グロー。
    const float occlusion = saturate(core * 0.9f + glow * 0.15f) * albedo.a * ends;

    clip(max(occlusion, dot(emit, float3(1.0f, 1.0f, 1.0f))) - 0.002f);
    return float4(emit, occlusion);
}
