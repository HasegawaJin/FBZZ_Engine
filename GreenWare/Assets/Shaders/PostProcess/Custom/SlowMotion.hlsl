/// @file    SlowMotion.hlsl
/// @brief   世界がゆっくりになっている間の «引き伸ばされた時間» の見え方
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// customParameters
///   x — 彩度の落ち 0..1。中央は保ち、縁ほど色が抜ける
///   y — 放射ぼかしの長さ (画面比)。縁の画素が中心から外へ流れる
///   z — 色収差 [px]。縁でこの量
///   w — 縁の暗さ 0..1
/// customParameters2
///   xyz — 中央に残す «熱» の色 (プレイヤー色をごく薄く乗せる)
///   w   — 縁の脈の速さ [Hz]。0 で止まる
/// customIntensity — スローの深さ 0..1 (TimeManager::Slow01)。スクリプトが毎フレーム写す
///
/// WHY 止め (HitstopFreeze) と別のパスか:
///   止めは «時間が止まった» を言う絵 (階調を落とし、粒も固める)。スローは
///   «時間が伸びている» を言う絵で、画面は動き続けていなければならない。
///   同じ絵を使うと、回避のスローが «殴られて固まった» に読める (2026-09-07 の指摘)。
///
/// WHY 中央を触らないか:
///   スローの間に見たいのは «かわした自分と、外れていく攻撃» で、どちらも画面の
///   中央に居る。縁だけが流れ、色を失い、暗くなることで、中央が «時間の外» に
///   立っているように見える。中央まで加工すると絵が全体に濁って何も読めない。
///
/// WHY 放射ぼかしを «外へ» 流すか:
///   内へ流す (集束 / 被弾) は «何かが入ってくる» 向き。スローは自分から世界が
///   離れていく向きなので、逆に流す。

#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Common/Color.hlsli"
#include "Common/Random.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(texInput, TEX_GBUFFER0_SLOT);
SamplerState sampLinear : register(SAMPLER_LINEAR_CLAMP);

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    const float3 orig = texInput.SampleLevel(sampLinear, p.uv, 0).rgb;

    const float depth      = saturate(customIntensity);
    const float desat      = saturate(customParameters.x);
    const float streak     = max(customParameters.y, 0.0f);
    const float aberration = max(customParameters.z, 0.0f);
    const float darkness   = saturate(customParameters.w);
    const float3 heat      = customParameters2.xyz;
    const float pulseHz    = max(customParameters2.w, 0.0f);

    // 縁の深さ。中央 (半径 0.35) までは 0、画面の端で 1。横長の画面比を戻す。
    const float aspect = screenSize.x / max(screenSize.y, 1.0f);
    float2 centered = (p.uv - 0.5f) * 2.0f;
    const float2 dir  = centered;                       // 中心から外向き (UV 空間)
    centered.x *= aspect / max(aspect, 1.0f);
    const float dist = length(centered);
    float edge = saturate((dist - 0.35f) / 0.75f);
    edge = edge * edge * (3.0f - 2.0f * edge);

    // 脈。実時間で数えたいが b5 の time はスケール後なので、遅いぶん自動的にゆっくり
    // 打つ ─ それ自体が «時間が伸びている» の証拠になるので、そのまま使う。
    const float pulse = pulseHz > 0.0f ? 0.5f + 0.5f * sin(time * pulseHz * 6.2831853f) : 1.0f;

    // 放射ぼかし: 中心から外へ、縁ほど長く。8 タップ。
    const float2 stride = dir * streak * edge * depth / 8.0f;
    float3 blurred = orig;
    [unroll]
    for (int i = 1; i <= 8; ++i)
        blurred += texInput.SampleLevel(sampLinear, p.uv - stride * i, 0).rgb;
    blurred /= 9.0f;

    // 色収差: 縁で開く。ぼかした結果に掛ける。
    const float2 shift = dir * aberration * texelSize * edge * depth;
    float3 result;
    result.r = texInput.SampleLevel(sampLinear, p.uv + shift, 0).r;
    result.g = blurred.g;
    result.b = texInput.SampleLevel(sampLinear, p.uv - shift, 0).b;
    result   = lerp(blurred, result, saturate(aberration * 0.25f));

    // 彩度: 縁ほど抜ける。中央は元のまま (極の赤青を数える場所)。
    const float luma = Luminance(result);
    result = lerp(result, luma.xxx, desat * edge * depth);

    // 暗さ: 縁を締める。脈で少しだけ呼吸する。
    result *= 1.0f - darkness * edge * depth * (0.85f + 0.15f * pulse);

    // 中央の熱: ごく薄い色を «縁の逆» に乗せる。深いスローほど中央が浮く。
    const float core = (1.0f - edge) * depth * 0.12f;
    result += heat * core * luma;

    const float3 effected = lerp(orig, result, depth);
    return float4(lerp(orig, effected, saturate(customBlend)), 1.0f);
}
