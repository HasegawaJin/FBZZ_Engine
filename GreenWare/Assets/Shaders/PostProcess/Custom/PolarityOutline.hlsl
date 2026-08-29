/// @file    PolarityOutline.hlsl
/// @brief   極を帯びた対象のシルエットへ、放電しているアウトラインを掛ける
/// @author  Hasegawa Jin
/// @date    2026-08-28
///
/// customParameters
///   x — 輪郭の最大の太さ [px]。マスクの A (要求ごとの太さ 0..1) がこれを縮める
///   y — ビリビリの強さ 0..1。0 で滑らかな等幅の輪郭
///   z — 明滅の速さ [Hz]
///   w — 明るさ。HDR へ加算するので 1 を超えるとブルームが拾う
/// customIntensity — 全体の強さ 0..1
///
/// stage = SceneHDR / blendMode = ADDITIVE で走らせる前提。
/// 入力の t6 は OutlineMaskPass が描いたシルエット (RGB = 極の色 / A = 太さ)。
///
/// WHY 色をここで決めないか:
///   ＋と − が同時に盤面へ並ぶ。パスの定数で 1 色に決めると、どちらの極が
///   帯電しているのかが輪郭から読めず、色に意味を持たせた 12.2 が崩れる。
///   誰が何色かはマスクが 1 フェッチで答える。
///
/// WHY マスクを point で引くか:
///   マスクの A は «被覆率» ではなく «その対象の太さ» で、linear で混ぜると
///   シルエットの縁で太さが 0 へ向かって溶ける。届く距離が縁だけ短くなり、
///   細い部位 (脚・触手) の輪郭が消える。色も背景の黒と混ざって濁る。
///
/// WHY 深度を見ないか:
///   壁の裏の輪郭を出さない判定はマスクを描く側で済んでいる (OutlineMask.hlsl が
///   遮蔽された面を discard する)。ここへ持ってくると、SceneHDR 段では描き先の
///   深度を読むことになって成立しない。
///
/// WHY 方向を放射状に舐めるか (正方形の全走査ではなく):
///   SelectionOutline.hlsl は半径 16px で 1089 タップまで膨らむ。あちらは編集中の
///   1 体だけだが、こちらは戦闘中ずっと乗る。放射状なら太さに関係なくタップ数が
///   一定で、しかも «方向ごとに届く距離を変える» がそのまま放電の揺らぎになる。

#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Platform/Backend.hlsli"

// NOTE: SceneHDR + ADDITIVE で走るので、画面の色 (t5) も深度 (t7/t8) も束縛されない。
//       描き先 (hdrRT とその深度) を読みながら書くことはできない。このシェーダーは
//       «足す分» だけを返し、遮蔽の判定はマスク側で済んでいる (OutlineMask.hlsl)。
Texture2D texOutlineMask : register(TEX_GBUFFER1);

// 全画面フェッチなので clamp 必須 (s0 は DX12 では WRAP)。
SamplerState sampPoint : register(SAMPLER_POINT_CLAMP);

// 舐める方向の数と、1 方向あたりの刻み。
// 方向を増やすと輪郭が滑らかになり、刻みを増やすと «太さの段» が細かくなる。
static const int kDirections = 10;
static const int kSteps      = 3;

/// 方向ごとの «届く距離» の倍率。1 を中心に揺れ、たまに大きく跳ねる。
///
/// WHY 跳ねさせるか: 全周が同じ幅で揺れるだけだと «太さが呼吸している» に見える。
///     放電は一部だけが遠くまで伸びて消えるので、跳ねが無いと電気にならない。
float ArcReach(float angle, float phase, float crackle)
{
    const float slow = sin(angle * 3.0f + phase * 2.0f);
    const float fast = sin(angle * 11.0f - phase * 7.0f);
    const float wave = slow * 0.6f + fast * 0.4f;
    // 跳ねは «たまに» でよい。常に出ていると輪郭の形そのものが読めなくなる。
    const float spike = saturate(fast * 0.5f + 0.5f - 0.82f) * 5.5f;
    return 1.0f + crackle * (wave * 0.35f + spike * 0.7f);
}

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

// 何も足さない画素。ADDITIVE は rgb に a を掛けてから足すので、alpha は 1 のまま返す
// (RenderState.hpp の BlendMode の式が正本。0 を返すと «足さない» ではなく式が壊れる)。
static const float4 kNoContribution = float4(0.0f, 0.0f, 0.0f, 1.0f);

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    const float radiusPx = max(customParameters.x, 1.0f);
    const float crackle  = saturate(customParameters.y);
    const float speed    = max(customParameters.z, 0.0f);
    const float gain     = max(customParameters.w, 0.0f);

    // シルエットの内側は素通し。中を塗ると «光っている敵» になり、輪郭で
    // 位置と数を数えるという役目から外れる。
    const float4 center = texOutlineMask.SampleLevel(sampPoint, p.uv, 0);
    if (center.a > 0.0f) return kNoContribution;

    // 方向の刻みが揃うと、輪郭に kDirections 個の «角» が出る。画素ごとに位相を
    // ずらして角を散らす。
    //
    // WHY 時間を混ぜないか: 毎フレーム散らし方が変わると、輪郭の «形» そのものが
    //     ちらつく。動いてほしいのは放電 (ArcReach の位相) であって、輪郭の
    //     滑らかさではない。ここは画素に固定する。
    const float dither = Hash2D(p.uv * screenSize);
    const float phase  = time * speed;

    // 位相を画面のブロック単位でずらす。
    //
    // WHY 場所で変えるか: 位相が時間だけで決まると、盤面の全員が同じ方向へ同じ
    //     タイミングで放電する。«それぞれが帯電している» ではなく «画面全体が
    //     1 つの効果» に見えてしまう。画素ごとにすると今度は縁がざらつくだけなので、
    //     輪郭を跨ぐ程度の粗さ (数十 px) で区切る。
    const float regionPhase = phase + Hash2D(floor(p.uv * screenSize / 24.0f)) * 8.0f;

    float  band      = 0.0f;
    float3 bandColor = float3(0.0f, 0.0f, 0.0f);

    [loop]
    for (int d = 0; d < kDirections; ++d)
    {
        const float angle = (float(d) + dither) * (TWO_PI / float(kDirections));
        const float2 dir  = float2(cos(angle), sin(angle));
        const float reach = max(ArcReach(angle, regionPhase, crackle), 0.05f);

        [loop]
        for (int s = 1; s <= kSteps; ++s)
        {
            const float t    = float(s) / float(kSteps);
            const float dist = radiusPx * t;
            const float2 uv  = p.uv + dir * dist * texelSize;

            const float4 m = texOutlineMask.SampleLevel(sampPoint, uv, 0);
            if (m.a <= 0.0f) continue;

            // 届く距離はその対象自身の太さ (m.a) が決める。1 本の共有幅にすると、
            // 大きいボスと小さい雑魚が同じ px 数で縁取られて «近さ» が読めなくなる。
            const float limit = radiusPx * m.a * reach;
            if (dist > limit) continue;

            const float strength = 1.0f - dist / limit;
            if (strength <= band) continue;

            band      = strength;
            bandColor = m.rgb;
        }
    }

    if (band <= 0.0f) return kNoContribution;

    // 明滅。ゆっくりした脈と、細かいちらつきを重ねる。前者が «帯電している»、
    // 後者が «放電している» を受け持つ。
    const float pulse  = sin(phase * TWO_PI) * 0.5f + 0.5f;
    const float jitter = Hash2D(floor(p.uv * screenSize * 0.25f) + floor(phase * 8.0f));
    const float flicker = lerp(0.72f, 1.0f, pulse) * lerp(1.0f, 1.45f, jitter * crackle);

    // 縁を «芯 + 裾» の 2 段にする。線形のままだと太さのわりに薄く、
    // 太くすると今度は板に見える。芯を細く強く出すと線として読める。
    const float core = band * band * band;
    const float glow = band * band;

    const float amount = saturate(customIntensity) * saturate(customBlend) * flicker * gain;

    // HDR へ足すので 1 を超えてよい。芯が 1 を越えた分だけブルームが拾い、
    // 露出にも乗る ─ トーンマップ後に足していた頃はここが頭打ちで、
    // どれだけ Gain を上げても «滲まない明るい線» のままだった。
    return float4(bandColor * (core * 1.6f + glow * 0.7f) * amount, 1.0f);
}
