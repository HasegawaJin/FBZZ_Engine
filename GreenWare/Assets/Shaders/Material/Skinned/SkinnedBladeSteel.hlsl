/// @file SkinnedBladeSteel.hlsl
/// @brief プレイヤーの刀身。焼き入れ線 (刃文) が攻撃に連動して熱を持つ鋼
/// @author Hasegawa Jin
/// @date 2026-09-11
///
/// WHY 専用シェーダーを起こすか:
///     刀身はこれまで SkinnedPBR のただの鋼で、溜めても振っても弾いても見た目が
///     1 ミリも変わらなかった。«攻撃の状態» を刀そのもので言うには、刃のどこが
///     熱を持つかを刀身の形に沿って描き分ける必要があり、それは一様な自発光
///     (emissiveScale) では表現できない ─ 刀が丸ごと光ると刃の «長さ» も
///     «向き» も潰れて、ただの棒になる。
///
/// WHY UV を使わないか (最重要):
///     WPN_Sword_L/R の頂点 UV は全submeshで (0,0) — DCC が書き出していない。
///     テクスチャも UV 由来の手続きパターンも成立しないため、この材質は
///     テクスチャを 1 枚も取らず、刃の形はローカル座標と法線«だけ»から作る。
///     textureMask を持たないのはこのため (Material::Upload は offset が
///     見つからなければ何も書かないので、無くて問題ない)。
///
/// WHY 刃文を «法線» から出すか:
///     刃文は刃に平行な帯で、刀身の «どこからが刃か» が要る。刃の位置は
///     ローカル Y の固定値では取れない ─ 反り (sori) で切先へ行くほど下がり、
///     その落ち方は直線でない。一方 «刃へ向いた面 (鎬から刃までの斜面)» は
///     法線が刃の向きへ倒れているかどうかで、反りに関係なく判定できる。
///     帯の境目を刃長方向の波でうねらせると、それがそのまま刃文になる。
///
/// 連動する値はすべて BladeSteelComponent が GameObject 単位の override で書く。
/// .mat に書いてあるのは «何も起きていないときの刀» で、Play を止めれば必ずそこへ戻る。

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
#include "Common/BindlessIndices.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;
    float  metallic;
    float  roughness;
    float  anisotropy;
    float  clearcoat;
    float  clearcoatRoughness;
    float3 emissiveColor;
    float  emissiveScale;

    // 刃の座標系。ローカル空間で «柄から切先へ» と «刃の向き» を宣言する。
    float3 bladeAxis;
    float  bladeStart;
    float3 edgeDir;
    float  bladeLength;

    // 焼き入れ線。
    float3 hamonColor;
    float  hamonHeat;
    float  hamonWidth;
    float  hamonWave;
    float  hamonFrequency;
    float  hamonSoftness;

    // 振った瞬間に柄から切先へ抜ける帯。
    float3 sweepColor;
    float  sweepPos;
    float  sweepWidth;
    float  sweepIntensity;

    // 刀身全体の縁。溜まり切ったことを «形» でなく «輪郭» で言う。
    float3 rimColor;
    float  rimIntensity;
    float  rimPower;

    // 上の 3 つの色を白へ寄せる量。連撃が進むほど «熱い» ではなく «白い» へ。
    float  whiteHeat;
};

FBZZ_TEX2D_T(float, texShadow, TEX_SHADOW_SLOT);
SamplerState           sampDefault      : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow       : register(SAMPLER_SHADOW);
FBZZ_TEXCUBE(texIBLIrradiance, TEX_IBL_IRRADIANCE_SLOT);
FBZZ_TEXCUBE(texIBLPrefilter, TEX_IBL_PREFILTER_SLOT);
FBZZ_TEX2D_T(float4, texBRDFLut, TEX_IBL_BRDF_LUT_SLOT);
SamplerState           sampLinearClamp  : register(SAMPLER_LINEAR_CLAMP);

// スキン後のローカル位置と法線を PS まで運ぶ。既定の PSInput には枠が無い。
struct BladePSIn
{
    float4 svPosition  : SV_POSITION;
    float3 worldPos    : TEXCOORD0;
    float3 normal      : TEXCOORD1;
    float3 tangent     : TEXCOORD2;
    float2 uv          : TEXCOORD3;
    float3 localPos    : TEXCOORD4;
    float3 localNormal : TEXCOORD5;
};

float4x4 BlendSkinMatrix(SkinnedVSInput v)
{
    return boneMatrices[v.boneIndices.x] * v.boneWeights.x
         + boneMatrices[v.boneIndices.y] * v.boneWeights.y
         + boneMatrices[v.boneIndices.z] * v.boneWeights.z
         + boneMatrices[v.boneIndices.w] * v.boneWeights.w;
}

BladePSIn VSMain(SkinnedVSInput v)
{
    BladePSIn o;
    float4x4 skin    = BlendSkinMatrix(v);
    float4 localPos  = mul(float4(v.position, 1.0f), skin);
    float3 localN    = SafeNormalize(mul(v.normal,  (float3x3)skin), float3(0.0f, 1.0f, 0.0f));
    float3 localT    = SafeNormalize(mul(v.tangent, (float3x3)skin), float3(1.0f, 0.0f, 0.0f));
    float4 worldPos4 = mul(localPos, world);
    o.worldPos    = worldPos4.xyz;
    o.svPosition  = mul(worldPos4, viewProjection);
    o.normal      = SafeNormalize(mul(localN, (float3x3)worldInvTranspose), float3(0.0f, 1.0f, 0.0f));
    o.tangent     = SafeNormalize(mul(localT, (float3x3)world), float3(1.0f, 0.0f, 0.0f));
    o.uv          = v.uv;
    // ソケット追従で world は毎フレーム動くので、刃の形はここで凍らせる。
    o.localPos    = localPos.xyz;
    o.localNormal = localN;
    return o;
}

// 刃文の境目。1 本の正弦では «機械の目盛り» になるので、割り切れない比の
// 2 本目を重ねて周期を隠す。
float HamonWave(float t)
{
    return sin(t * hamonFrequency * 6.2831853f) * 0.62f
         + sin(t * hamonFrequency * 2.713f * 6.2831853f + 1.7f) * 0.38f;
}

float4 PSMain(BladePSIn p) : SV_Target0
{
    ApplyLodDither(p.svPosition.xy, objectParams.x);

    float3 col   = albedo.rgb;
    float  alpha = albedo.a;

    float3 N = SafeNormalize(p.normal, float3(0.0f, 1.0f, 0.0f));

    // ── 刃の座標 ────────────────────────────────────────────────────────────
    const float3 axis = SafeNormalize(bladeAxis, float3(0.0f, 0.0f, 1.0f));
    const float3 edge = SafeNormalize(edgeDir,   float3(0.0f, -1.0f, 0.0f));
    // t = 0 が柄、1 が切先。範囲外は帯も刃文も出さない (柄と鍔を焼かないため)。
    const float t = saturate((dot(p.localPos, axis) - bladeStart) / max(bladeLength, 1.0e-4f));
    // 刃へ向いた面ほど 1。鎬から先の斜面だけが拾われ、平地と棟は 0 に落ちる。
    const float edgeFacing = saturate(dot(SafeNormalize(p.localNormal, edge), edge));

    // 帯の «高さ» を刃長方向にうねらせる。hamonWave = 0 なら直刃。
    const float wave      = lerp(0.0f, HamonWave(t), saturate(hamonWave));
    const float threshold = saturate(1.0f - saturate(hamonWidth) * (0.75f + 0.45f * wave));
    const float soft      = max(hamonSoftness, 1.0e-3f);
    float hamon = smoothstep(threshold - soft, threshold + soft, edgeFacing);
    // 柄側の 1 割は焼かない。刃文が鍔まで届くと、刃の «始まり» が読めなくなる。
    hamon *= smoothstep(0.0f, 0.10f, t);

    const float heat = hamon * max(hamonHeat, 0.0f);

    // 熱が乗った鋼は反射色そのものが色付く。金属なので albedo = F0 で、
    // ここを動かさないと «白い光が乗っただけ» にしか見えない。
    const float3 hot = lerp(hamonColor, float3(1.0f, 1.0f, 1.0f), saturate(whiteHeat));
    col = lerp(col, hot, saturate(heat * 0.30f));

    float met   = saturate(metallic);
    float rough = max(saturate(roughness), 0.045f);
    // 焼けた面は僅かに曇る。鏡のまま光らせると発光が反射に負けて出てこない。
    rough = max(rough, saturate(heat) * 0.22f);

    const WetSurface wet = ApplyWetness(col, rough, N);
    col   = wet.albedo;
    rough = max(wet.roughness, 0.045f);
    rough = FilterSpecularRoughness(N, rough);

    const float3 tangent = SafeNormalize(p.tangent, float3(1.0f, 0.0f, 0.0f));
    float3 T = SafeNormalize(tangent - N * dot(N, tangent),
                             abs(N.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f));
    T = SafeNormalize(T - N * dot(N, T), T);
    float3 B = SafeNormalize(cross(N, T), float3(0.0f, 0.0f, 1.0f));

    float3 V      = SafeNormalize(cameraPos - p.worldPos, N);
    float3 L      = SafeNormalize(-lightDir, N);
    float  shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);

    float3 result = iblIntensity > 0.0f
        ? Lighting_PBR_IBL_Advanced(N, V, L, T, B, col, met, rough,
              clearcoat, clearcoatRoughness, 0.0f, anisotropy, float3(1.0f, 1.0f, 1.0f),
              lightColor, lightIntensity, shadow, 1.0f,
              texIBLIrradiance, texIBLPrefilter, texBRDFLut, iblMaxMipLevel,
              iblIntensity, iblDiffuseScale, iblSpecularScale,
              sampDefault, sampLinearClamp)
        : Lighting_PBR_Advanced(N, V, L, T, B, col, met, rough,
              clearcoat, clearcoatRoughness, 0.0f, anisotropy, float3(1.0f, 1.0f, 1.0f),
              lightColor, lightIntensity, shadow);

    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        result += Lighting_PBR_Advanced(N, V, ps.L, T, B, col, met, saturate(rough + ps.roughnessBias),
            clearcoat, clearcoatRoughness, 0.0f, anisotropy, float3(1.0f, 1.0f, 1.0f),
            ps.color, ps.intensity, 1.0f);
    FBZZ_PUNCTUAL_END

    // ── 自発光 ──────────────────────────────────────────────────────────────
    result += emissiveColor * emissiveScale;
    result += hot * heat;

    // 振った瞬間の帯。刃文と違って刀身の全面に乗せる ─ 平地に出ないと、
    // 刃を寝かせた向きのときだけ «振った» が見えなくなる。
    if (sweepPos >= 0.0f)
    {
        float d     = abs(t - sweepPos) / max(sweepWidth, 1.0e-3f);
        float sweep = saturate(1.0f - d);
        sweep *= sweep;
        result += lerp(sweepColor, float3(1.0f, 1.0f, 1.0f), saturate(whiteHeat))
                * sweep * max(sweepIntensity, 0.0f);
    }

    // 縁。刃文が «どこが刃か» を言うのに対し、こちらは «刀そのものが臨戦か» を言う。
    if (rimIntensity > 0.0f)
    {
        float fres = pow(saturate(1.0f - saturate(dot(N, V))), max(rimPower, 0.5f));
        result += lerp(rimColor, float3(1.0f, 1.0f, 1.0f), saturate(whiteHeat))
                * fres * rimIntensity;
    }

    return float4(result, alpha);
}
