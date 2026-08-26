/// @file    Material/Skinned/SkinnedEnemyDissolve.hlsl
/// @brief   撃破された機体が «下から食われて» 消えるディゾルブ。無傷のときは SkinnedPBR と同じ絵
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// WHY 既存の Material/Skinned/SkinnedDissolve.hlsl を使わないか:
///   あちらはノイズを **UV 空間** で引き、しきい値を alphaCutoff に相乗りさせている。
///   GreenWare の敵はテクスチャを 1 枚も持たず (全部フラットカラー) UV は展開の都合で
///   決まっているだけなので、UV ノイズは «体のどこが先に消えるか» と無関係に穴が開く。
///   さらに alphaCutoff は «アルファテストのしきい値» という別の意味を既に持っていて、
///   ディゾルブ量として動かすと Inspector から意図が読めない。
///
/// WHY ノイズを «スキン後のローカル座標» で引くか:
///   ワールド座標で引くと、敵が歩いた瞬間に模様が体の上を流れる (泳ぐ)。
///   バインドポーズのローカル座標で引くと、今度は腕を振っても模様が動かず «貼り付けた
///   テクスチャ» に見える。スキン行列を掛けた後のローカル座標なら、模様は体に固定され
///   かつ機体の移動から独立する。
///
/// WHY 軸方向の掃引を混ぜるか:
///   純ノイズだけだと全身に一斉に穴が開き «溶けた» ではなく «points が抜けた» に見える。
///   dissolveAxis の向きから順に食わせると «下から崩れる» «正面から灼かれる» のような
///   出来事の向きが絵に出る。dissolveSweep で純ノイズ ↔ 純掃引を連続で選べる。
///
/// NOTE: clip() で消すので合成は Opaque のまま。半透明にすると深度書き込みが外れて
///       体の裏側が透ける。そのぶん ShadowPass (SkinnedShadowMap) はディゾルブを知らず、
///       消えかけの機体も影だけは丸ごと落とす。

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/Color.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Rendering/SpecularAA.hlsli"
#include "Rendering/Wetness.hlsli"
#include "Rendering/LodDither.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    // ── SkinnedPBR と同じ並び。無傷の絵を 1 対 1 で保つため名前も順序も変えないこと ──
    float4 albedo;
    float  metallic;
    float  roughness;
    float  normalStrength;
    float  occlusionStrength;
    float3 emissiveColor;
    float  emissiveScale;
    float2 uvTiling;
    float2 uvOffset;
    float  alphaCutoff;
    float3 _pad0;
    uint   textureMask;
    float3 _pad1;
    float  clearcoat;
    float  clearcoatRoughness;
    float  sheen;
    float  anisotropy;
    float3 sheenColor;
    float  _pad2;

    // ── ディゾルブ ──
    // 縁の色は «熱» であって素材色ではないので emissiveColor とは別に持つ。
    // 同じ機体でも、撃破は白熱・腐食は緑、と出来事ごとに変えたい。
    float3 dissolveEdgeColor;
    // 0 = 無傷、1 = 完全に消滅。スクリプトはこの 1 本だけを動かす。
    float  dissolveAmount;
    // 先に消える向き (ローカル空間)。既定の (0,-1,0) は «足元から» 。
    float3 dissolveAxis;
    // 掃引が覆うローカル距離 [m]。機体の高さを入れると端から端まで綺麗に流れる。
    float  dissolveRange;
    // 縁として光る帯の幅 (しきい値の単位)。太いほど «炙られている» 感じが強い。
    float  dissolveEdgeWidth;
    float  dissolveEdgeIntensity;
    // ノイズの細かさ [周期/m]。大きいほど «砂» 、小さいほど «塊» で崩れる。
    float  dissolveNoiseScale;
    // 0 = ノイズだけ (全身同時)、1 = 掃引だけ (端から順に)。
    float  dissolveSweep;
};

Texture2D<float>       texShadow        : register(TEX_SHADOW);
Texture2D              texAlbedo        : register(TEX_ALBEDO);
Texture2D              texNormal        : register(TEX_NORMAL);
Texture2D              texMetallicRough : register(TEX_METALLIC_ROUGH);
Texture2D              texEmissive      : register(TEX_EMISSIVE);
Texture2D              texAO            : register(TEX_AO);
SamplerState           sampDefault      : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow       : register(SAMPLER_SHADOW);
TextureCube            texIBLIrradiance : register(TEX_IBL_IRRADIANCE);
TextureCube            texIBLPrefilter  : register(TEX_IBL_PREFILTER);
Texture2D<float4>      texBRDFLut       : register(TEX_IBL_BRDF_LUT);
SamplerState           sampLinearClamp  : register(SAMPLER_LINEAR_CLAMP);

// 共有 PSInput にローカル座標の口が無いので、このシェーダー専用の補間子を持つ。
struct DissolvePSInput
{
    float4 svPosition : SV_POSITION;
    float3 worldPos   : TEXCOORD0;
    float3 normal     : TEXCOORD1;
    float3 tangent    : TEXCOORD2;
    float2 uv         : TEXCOORD3;
    float3 localPos   : TEXCOORD4;
};

float4x4 BlendSkinMatrix(SkinnedVSInput v)
{
    return boneMatrices[v.boneIndices.x] * v.boneWeights.x
         + boneMatrices[v.boneIndices.y] * v.boneWeights.y
         + boneMatrices[v.boneIndices.z] * v.boneWeights.z
         + boneMatrices[v.boneIndices.w] * v.boneWeights.w;
}

DissolvePSInput VSMain(SkinnedVSInput v)
{
    DissolvePSInput o;
    float4x4 skin    = BlendSkinMatrix(v);
    float4 localPos  = mul(float4(v.position, 1.0f), skin);
    float3 localN    = SafeNormalize(mul(v.normal,  (float3x3)skin), float3(0.0f, 1.0f, 0.0f));
    float3 localT    = SafeNormalize(mul(v.tangent, (float3x3)skin), float3(1.0f, 0.0f, 0.0f));
    float4 worldPos4 = mul(localPos, world);
    o.worldPos   = worldPos4.xyz;
    o.svPosition = mul(worldPos4, viewProjection);
    o.normal     = SafeNormalize(mul(localN, (float3x3)worldInvTranspose), float3(0.0f, 1.0f, 0.0f));
    o.tangent    = SafeNormalize(mul(localT, (float3x3)world), float3(1.0f, 0.0f, 0.0f));
    o.uv         = v.uv;
    o.localPos   = localPos.xyz;
    return o;
}

float DissolveHash(float3 p)
{
    p = frac(p * 0.1031f);
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}

float DissolveValueNoise(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0f - 2.0f * f);

    float n000 = DissolveHash(i + float3(0.0f, 0.0f, 0.0f));
    float n100 = DissolveHash(i + float3(1.0f, 0.0f, 0.0f));
    float n010 = DissolveHash(i + float3(0.0f, 1.0f, 0.0f));
    float n110 = DissolveHash(i + float3(1.0f, 1.0f, 0.0f));
    float n001 = DissolveHash(i + float3(0.0f, 0.0f, 1.0f));
    float n101 = DissolveHash(i + float3(1.0f, 0.0f, 1.0f));
    float n011 = DissolveHash(i + float3(0.0f, 1.0f, 1.0f));
    float n111 = DissolveHash(i + float3(1.0f, 1.0f, 1.0f));

    return lerp(lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y),
                lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y), f.z);
}

// 崩れる «順番» を 0..1 で返す。小さい画素ほど先に消える。
float DissolveField(float3 localPos)
{
    const float scale = max(dissolveNoiseScale, 0.0001f);
    float noise  = DissolveValueNoise(localPos * scale)        * 0.60f;
          noise += DissolveValueNoise(localPos * scale * 2.1f) * 0.27f;
          noise += DissolveValueNoise(localPos * scale * 4.3f) * 0.13f;

    // 軸に沿った位置を 0..1 へ。dissolveAxis の «向いている先» が先に消える。
    const float3 axis = SafeNormalize(dissolveAxis, float3(0.0f, -1.0f, 0.0f));
    const float  span = max(dissolveRange, 0.0001f);
    const float  sweep = saturate(dot(localPos, axis) / span + 0.5f);

    return saturate(lerp(saturate(noise), sweep, saturate(dissolveSweep)));
}

float4 PSMain(DissolvePSInput p) : SV_Target0
{
    ApplyLodDither(p.svPosition.xy, objectParams.x);

    // dissolveAmount = 0 で 1 画素も欠けず、1 で 1 画素も残らないよう、
    // しきい値を field の範囲より僅かに外へ振る (両端がちょうど境界に乗ると
    // «消え切らずに点が残る» / «開始と同時に穴が開く» のどちらかが必ず出る)。
    const float field     = DissolveField(p.localPos);
    const float threshold = lerp(-0.001f, 1.001f, saturate(dissolveAmount));
    clip(field - threshold);

    float2 uv = p.uv * uvTiling + uvOffset;

    float4 rawAlbedo = (textureMask & (1u << 0))
        ? texAlbedo.Sample(sampDefault, uv)
        : float4(1.0f, 1.0f, 1.0f, 1.0f);
    float3 col   = SRGBToLinear(rawAlbedo.rgb) * albedo.rgb;
    float  alpha = rawAlbedo.a * albedo.a;
    clip(alpha - alphaCutoff);

    float3 N = SafeNormalize(p.normal, float3(0.0f, 1.0f, 0.0f));
    if (textureMask & (1u << 1))
    {
        float3 ns = texNormal.Sample(sampDefault, uv).rgb;
        float3 nm = ApplyNormalMap(ns, N, SafeNormalize(p.tangent, float3(1.0f, 0.0f, 0.0f)));
        N = SafeNormalize(lerp(N, nm, saturate(normalStrength)), N);
    }

    float met   = metallic;
    float rough = roughness;
    if (textureMask & (1u << 2))
    {
        float2 mr = texMetallicRough.Sample(sampDefault, uv).gb;
        rough = mr.x;
        met   = mr.y;
    }

    met   = saturate(met);
    rough = max(saturate(rough), 0.045f);
    const WetSurface wet = ApplyWetness(col, rough, N);
    col   = wet.albedo;
    rough = max(wet.roughness, 0.045f);
    rough = FilterSpecularRoughness(N, rough);
    const float3 tangent = SafeNormalize(p.tangent, float3(1.0f, 0.0f, 0.0f));
    float3 T = SafeNormalize(tangent - N * dot(N, tangent),
                             abs(N.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f));
    T = SafeNormalize(T - N * dot(N, T), T);
    float3 B = SafeNormalize(cross(N, T), float3(0.0f, 0.0f, 1.0f));

    float ao = 1.0f;
    if (textureMask & (1u << 4))
        ao = lerp(1.0f, texAO.Sample(sampDefault, uv).r, occlusionStrength);

    float3 V      = SafeNormalize(cameraPos - p.worldPos, N);
    float3 L      = SafeNormalize(-lightDir, N);
    float  shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    float3 result = iblIntensity > 0.0f
        ? Lighting_PBR_IBL_Advanced(N, V, L, T, B, col, met, rough,
              clearcoat, clearcoatRoughness, sheen, anisotropy, sheenColor,
              lightColor, lightIntensity, shadow, ao,
              texIBLIrradiance, texIBLPrefilter, texBRDFLut, iblMaxMipLevel,
              iblIntensity, iblDiffuseScale, iblSpecularScale,
              sampDefault, sampLinearClamp)
        : Lighting_PBR_Advanced(N, V, L, T, B, col, met, rough,
              clearcoat, clearcoatRoughness, sheen, anisotropy, sheenColor,
              lightColor, lightIntensity, shadow);

    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        result += Lighting_PBR_Advanced(N, V, ps.L, T, B, col, met, saturate(rough + ps.roughnessBias),
            clearcoat, clearcoatRoughness, sheen, anisotropy, sheenColor,
            ps.color, ps.intensity, 1.0f);
    FBZZ_PUNCTUAL_END

    float3 emissiveTex = (textureMask & (1u << 3))
        ? SRGBToLinear(texEmissive.Sample(sampDefault, uv).rgb)
        : float3(1.0f, 1.0f, 1.0f);
    result += emissiveTex * emissiveColor * emissiveScale;

    // 消える直前の «食われている縁» 。しきい値のすぐ上に居る画素ほど強い。
    // WHY dissolveAmount で伏せるか: 無傷 (0) のときに縁が出ると、まだ何も起きていない
    //     機体の足元だけが常時光る。撃破が始まってから灯る必要がある。
    const float edge = (1.0f - saturate((field - threshold) / max(dissolveEdgeWidth, 0.0001f)))
                     * step(0.0001f, dissolveAmount);
    result += dissolveEdgeColor * (dissolveEdgeIntensity * edge * edge);

    return float4(result, alpha);
}
