/// @file    HitstopFreeze.hlsl
/// @brief   ヒットストップの数フレームだけ画面を固める。12.6 の衝突を絵で言い切る
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// customParameters
///   x — 階調の段数。小さいほど硬い
///   y — 色収差の開き [px]。画面の縁でこの量になる
///   z — 粒の強さ
///   w — コントラストの持ち上げ
/// customIntensity — 0..1 の包絡。止まっている間は 1 のまま保ち、解除で落とす
///
/// WHY 立ち上がりを作らないか:
///   止めは最長でも 0.09 秒 = 60fps で 5 フレームしかない (HitstopManagerComponent の
///   maxSeconds)。そこへ «だんだん掛かる» を入れると、掛かりきる前に終わる。
///   掛かるのは 1 フレーム目、滑らかにするのは抜けるときだけ。包絡そのものは
///   ScreenEffectManagerComponent が実時間で作る。
///
/// WHY 粒に time を使わないか:
///   b5 の time は Time::time、つまりタイムスケール適用済みなので、止めている間は
///   ほとんど進まない。それ以前に、止まった画面で粒だけがちらつくと «時間はまだ
///   動いている» と目が拾ってしまう。止めの間は粒も固めておく。
///
/// WHY 階調を輝度で落とすか:
///   RGB を別々に量子化すると段の境目で色相が転ぶ。12.2 は «赤 = ＋ / 青 = −» を
///   全アセット共通の制約に置いているので、止めた瞬間だけ極の色がずれるのは通らない。
///   輝度だけを段にして、色は比のまま引き伸ばす。

#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Common/Color.hlsli"
#include "Common/Random.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

// t5 は RenderSystem が自動で束ねる LDR カラー。
FBZZ_TEX2D(texInput, TEX_GBUFFER0_SLOT);
// 全画面フェッチなので clamp 必須 (s0 は DX12 では WRAP)。
SamplerState sampLinear : register(SAMPLER_LINEAR_CLAMP);

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

/// 明るさだけを段にする。色相と彩度は触らない (ファイル冒頭の WHY を参照)。
float3 CrushLuminance(float3 color, float levels)
{
    const float steps = max(floor(levels), 2.0f);
    // 0 割りを避けるだけでなく、真っ黒な画素で比が発散するのも同時に防ぐ。
    const float luma    = max(Luminance(color), 1.0e-4f);
    const float stepped = floor(luma * steps + 0.5f) / steps;
    return color * (stepped / luma);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    const float3 orig = texInput.SampleLevel(sampLinear, p.uv, 0).rgb;

    const float levels     = customParameters.x;
    const float aberration = customParameters.y;
    const float grain      = customParameters.z;
    const float contrastUp = customParameters.w;

    // 放射状に開く色収差。中央は割らず、縁へ行くほど離す。
    //
    // WHY 一様にずらさないか: 画面全体を同じ量だけずらすと «印刷がずれた» に見える。
    //     縁ほど開く形なら、衝撃が中心から外へ抜けた跡として読める。
    const float2 centered = p.uv - 0.5f;
    const float2 shift    = centered * aberration * texelSize * 2.0f;

    float3 result;
    result.r = texInput.SampleLevel(sampLinear, p.uv + shift, 0).r;
    result.g = orig.g;
    result.b = texInput.SampleLevel(sampLinear, p.uv - shift, 0).b;

    result = CrushLuminance(result, levels);
    result = (result - 0.5f) * (1.0f + contrastUp) + 0.5f;

    // 画素の位置«だけ»から引く固定の粒 (ファイル冒頭の WHY を参照)。
    const float speck = Hash2D(floor(p.uv * screenSize)) * 2.0f - 1.0f;
    result = saturate(result + speck * grain);

    const float3 effected = lerp(orig, result, saturate(customIntensity));
    return float4(lerp(orig, effected, saturate(customBlend)), 1.0f);
}
