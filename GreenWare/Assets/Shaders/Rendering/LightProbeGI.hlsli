/// @file    LightProbeGI.hlsli
/// @brief   Light Probe Volume の SH 係数を画素のワールド座標で三線形補間し、拡散放射照度を返す。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @note 係数は IBL/LightProbeProject.cs.hlsl が焼き、IBL/LightProbeDilate.cs.hlsl が無効なプローブを埋めたもの。並びと定数の正本はそちら。
/// @note 同時に引けるのは 2 つ。[0] (内側, t22) を [1] (外側, t21) の上に重ね、どちらも外れた画素は呼び出し側の値 (IBL キューブ) のまま。
/// @see Docs/design/light-probe-gi.md
#ifndef LIGHT_PROBE_GI_HLSLI
#define LIGHT_PROBE_GI_HLSLI

#include "Common/Binding.hlsli"
#include "Common/BindlessIndices.hlsli"

/// @note 1 プローブ = 7 texel。Z 方向へ [SHAr, SHAg, SHAb, SHBr, SHBg, SHBb, SHC] の順に gridZ 枚ずつ積む。
FBZZ_TEX3D(gLightProbeSH, TEX_LIGHT_PROBE_SH_SLOT);
FBZZ_TEX3D(gLightProbeSHOuter, TEX_LIGHT_PROBE_SH_OUTER_SLOT);
static const uint kLightProbeSHSlices = 7u;

/// @brief 多項式形の L2 SH から、法線 n の拡散放射照度 / π を求める。
/// @param n 正規化済みのワールド法線。
/// @note 係数には余弦ローブの畳み込み (Â_l / π) が焼き込み済みなので、一様な放射輝度 L は L を返す。
/// @see https://www.ppsloan.org/publications/StupidSH36.pdf Sloan, "Stupid Spherical Harmonics (SH) Tricks", Appendix A10 (多項式形の評価)
float3 FBZZ_EvaluateSHIrradiance(float4 shAr, float4 shAg, float4 shAb,
                                 float4 shBr, float4 shBg, float4 shBb,
                                 float3 shC, float3 n)
{
    const float4 linearTerm = float4(n, 1.0f);
    const float4 quadTerm   = n.xyzz * n.yzzx;
    float3 result;
    result.r = dot(shAr, linearTerm) + dot(shBr, quadTerm);
    result.g = dot(shAg, linearTerm) + dot(shBg, quadTerm);
    result.b = dot(shAb, linearTerm) + dot(shBb, quadTerm);
    result  += shC * (n.x * n.x - n.y * n.y);
    return max(result, 0.0f);
}

/// @brief ボリューム 1 つで拡散放射照度 / π を引く。
/// @param volume b8 の probeVolumes[i]。
/// @param sh そのボリュームの SH テクスチャ。
/// @param linearClamp Linear clamp サンプラー (各シェーダーが s2 を自前の名前で持つので呼び出し側から渡す)。
/// @param irradiance 出力。IBL キューブの irradiance と同じ単位。
/// @return 混合率 [0,1]。箱の縁で 0 へ落ちる。無効または箱の外なら 0 (出力も 0)。
/// @note 点を法線方向へ normalBias だけ押し出して引く。壁に埋まったプローブは焼いた後に周りの有効なプローブで埋めてあるが、補間の裾はなお壁の向こうへ届くため。
float FBZZ_SampleProbeVolume(ProbeVolumeParams volume, Texture3D sh, float3 worldPos, float3 N,
                             SamplerState linearClamp, out float3 irradiance)
{
    irradiance = 0.0f;
    if (volume.intensity <= 0.0f) return 0.0f;

    const float3 t = (worldPos + N * volume.normalBias - volume.boxMin) * volume.invSize;
    if (any(t < 0.0f) || any(t > 1.0f)) return 0.0f;

    const float3 size   = 1.0f / volume.invSize;
    const float3 toEdge = min(t, 1.0f - t) * size;
    const float  edge   = min(toEdge.x, min(toEdge.y, toEdge.z));
    const float  weight = volume.fade > 0.0f ? saturate(edge / volume.fade) : 1.0f;

    /// @note 箱を格子に割った各セルの中心にプローブがある (texel 中心 = プローブ)。縁の半セルは最寄りのプローブで埋まる。
    /// @note 角に置くと壁面上のプローブが壁の裏 (空) を見て、壁際だけ明るく漏れる。
    const float3 grid   = float3(volume.grid);
    const float2 uv     = t.xy;
    /// @note Z は係数ごとの区画の中だけで補間する。[0.5, gridZ - 0.5] へ留めて隣の係数の区画と混ぜない。
    const float  texelZ = clamp(t.z * grid.z, 0.5f, grid.z - 0.5f);
    const float  depth  = grid.z * (float)kLightProbeSHSlices;
    float4 c[kLightProbeSHSlices];
    [unroll]
    for (uint k = 0u; k < kLightProbeSHSlices; ++k)
        c[k] = sh.SampleLevel(linearClamp, float3(uv, (texelZ + grid.z * k) / depth), 0.0f);

    irradiance = FBZZ_EvaluateSHIrradiance(c[0], c[1], c[2], c[3], c[4], c[5], c[6].rgb, N) * volume.intensity;
    return weight;
}

/// @brief ボリュームの中なら拡散放射照度 / π を差し替える。
/// @param fallback ボリュームの外で使う値 (IBL キューブの irradiance / 地形の定数環境光)。
/// @param coverage 出力。どれだけプローブに置き換わったか [0,1]。
/// @return 外側 → 内側の順に重ねた放射照度。
float3 FBZZ_ApplyLightProbes(float3 worldPos, float3 N, SamplerState linearClamp, float3 fallback,
                             out float coverage)
{
    float3 outer, inner;
    const float outerWeight = FBZZ_SampleProbeVolume(probeVolumes[1], gLightProbeSHOuter, worldPos, N, linearClamp, outer);
    const float innerWeight = FBZZ_SampleProbeVolume(probeVolumes[0], gLightProbeSH, worldPos, N, linearClamp, inner);
    coverage = 1.0f - (1.0f - outerWeight) * (1.0f - innerWeight);
    return lerp(lerp(fallback, outer, outerWeight), inner, innerWeight);
}

/// @brief プローブが «空よりどれだけ暗いか» を鏡面 IBL の遮蔽に使う。
/// @param probeIrradiance FBZZ_ApplyLightProbes の結果。
/// @param skyIrradiance 同じ法線で引いた IBL キューブの irradiance。
/// @return 鏡面 IBL に掛ける倍率 [0,1]。ボリュームの外や probeSpecularOcclusion = 0 では 1。
/// @note 鏡面 IBL は空のキューブのままなので、屋根の下でも空の青い映り込みが残り «室内が浮く»。拡散で分かった遮蔽の比をそのまま鏡面にも掛ける (明るくはしない)。
/// @see https://seblagarde.files.wordpress.com/2015/07/course_notes_moving_frostbite_to_pbr_v32.pdf Lagarde, "Moving Frostbite to PBR 3.0", §4.10.2 Specular occlusion
float FBZZ_ProbeSpecularOcclusion(float3 probeIrradiance, float3 skyIrradiance)
{
    const float3 lum = float3(0.2126f, 0.7152f, 0.0722f);
    const float ratio = saturate(dot(probeIrradiance, lum) / max(dot(skyIrradiance, lum), 1e-4f));
    return lerp(1.0f, ratio, saturate(probeSpecularOcclusion));
}

#endif // LIGHT_PROBE_GI_HLSLI
