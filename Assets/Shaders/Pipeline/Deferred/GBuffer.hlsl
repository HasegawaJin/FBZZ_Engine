/// @file    GBuffer.hlsl
/// @brief   標準 PBR の表面と線形 HDR 発光を 3 枚の MRT に保持する。
/// @author  Hasegawa Jin
/// @date    2026-06-23
/// @note SV_Target0: albedo / roughness、SV_Target1: normal / metallic、SV_Target2: emission / validated Hybrid surface marker。

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/Math.hlsli"
#include "Common/Space.hlsli"
#include "Common/Color.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/SpecularAA.hlsli"
#include "Rendering/Wetness.hlsli"
#include "Rendering/LodDither.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/ObjectInstance.hlsli"

FBZZ_TEX2D(texAlbedo, TEX_ALBEDO_SLOT);
FBZZ_TEX2D(texNormal, TEX_NORMAL_SLOT);
FBZZ_TEX2D(texMetallicRough, TEX_METALLIC_ROUGH_SLOT);
FBZZ_TEX2D(texEmissive, TEX_EMISSIVE_SLOT);
SamplerState sampDefault      : register(SAMPLER_DEFAULT);

/// @brief 本体。入口だけが変種ごとに違い、変換そのものは 1 か所に置く。
PSInput GBufferVS(VSInput v, float4x4 objectWorld, float4x4 objectWorldInvTranspose)
{
    PSInput o;
    float4 worldPos4 = mul(float4(v.position, 1.0f), objectWorld);
    o.worldPos   = worldPos4.xyz;
    o.svPosition = mul(worldPos4, viewProjection);
    o.normal     = SafeNormalize(mul(v.normal, (float3x3)objectWorldInvTranspose), float3(0.0f, 1.0f, 0.0f));
    o.tangent    = SafeNormalize(mul(v.tangent, (float3x3)objectWorld), float3(1.0f, 0.0f, 0.0f));
    o.uv         = v.uv;
    return o;
}

#if defined(FBZZ_SKINNED)
/// @note b7 のパレットで頂点を変形してから本体へ渡す。式は SkinnedPBR.hlsl と揃えること
/// @note (ここだけ違うと «Deferred のときだけキャラの法線が違う» という形で出る)。
/// @note コンピュートスキニングが効いているときはこの変種を使わない。変形済みの頂点が
/// @note 静的メッシュと同じレイアウトで来るので、素の GBuffer.hlsl でそのまま描ける。
/// @see Docs/design/pipeline-boundary.md §3
float4x4 GBufferBlendSkinMatrix(SkinnedVSInput v)
{
    return boneMatrices[v.boneIndices.x] * v.boneWeights.x
         + boneMatrices[v.boneIndices.y] * v.boneWeights.y
         + boneMatrices[v.boneIndices.z] * v.boneWeights.z
         + boneMatrices[v.boneIndices.w] * v.boneWeights.w;
}

PSInput VSMain(SkinnedVSInput v)
{
    const float4x4 skin = GBufferBlendSkinMatrix(v);

    VSInput deformed;
    deformed.position = mul(float4(v.position, 1.0f), skin).xyz;
    deformed.normal   = SafeNormalize(mul(v.normal,  (float3x3)skin), float3(0.0f, 1.0f, 0.0f));
    deformed.tangent  = SafeNormalize(mul(v.tangent, (float3x3)skin), float3(1.0f, 0.0f, 0.0f));
    deformed.uv       = v.uv;
    return GBufferVS(deformed, world, worldInvTranspose);
}
#elif defined(FBZZ_INSTANCED)
/// @note 束ねた描画。world は b1 でなく VS の t0 から引く。@see Docs/design/gpu-instancing.md
PSInput VSMain(VSInput v, uint instanceId : SV_InstanceID)
{
    return GBufferVS(v, gObjectInstances[instanceId].world,
                        gObjectInstances[instanceId].worldInvTranspose);
}
#else
PSInput VSMain(VSInput v)
{
    return GBufferVS(v, world, worldInvTranspose);
}
#endif

GBufferOut PSMain(PSInput p)
{
    ApplyLodDither(p.svPosition.xy, objectParams.x);

    float2 uv = p.uv * uvTiling + uvOffset;

    /// @note sRGB テクスチャを線形空間にデコードしてから tint (線形) を乗算する。
    /// @note テクスチャなし時は (1,1,1) として albedo.rgb をそのまま使用 (SRGBToLinear(1)=1)。
    float4 rawAlbedo = (textureMask & (1u << 0))
        ? texAlbedo.Sample(sampDefault, uv)
        : float4(1.0f, 1.0f, 1.0f, 1.0f);
    float3 col  = SRGBToLinear(rawAlbedo.rgb) * albedo.rgb;
    float  alpha = rawAlbedo.a * albedo.a;
    clip(alpha - alphaCutoff);

    float3 N = SafeNormalize(p.normal, float3(0.0f, 1.0f, 0.0f));
    if (textureMask & (1u << 1))
    {
        float3 ns = texNormal.Sample(sampDefault, uv).rgb;
        float3 nm = ApplyNormalMap(ns, N, SafeNormalize(p.tangent, float3(1.0f, 0.0f, 0.0f)));
        N = SafeNormalize(lerp(N, nm, saturate(normalStrength)), N);
    }

    /// @note Metallic / Roughness (glTF 規約: G=roughness, B=metallic)
    float met   = metallic;
    float rough = roughness;
    if (textureMask & (1u << 2))
    {
        float2 mr = texMetallicRough.Sample(sampDefault, uv).gb;
        rough = mr.x;
        met   = mr.y;
    }

    /// @note 濡れは素材の値なので、法線分散のフィルタより先に掛ける。
    /// @note 逆順にすると、AA で持ち上げた粗さを濡れが再び下げてちらつきが戻る。
    const WetSurface wet = ApplyWetness(col, saturate(rough), N);
    col   = wet.albedo;
    rough = wet.roughness;

    /// @note 1 面の法線分散は、別オブジェクトの法線が混ざる GBuffer 読み取りより前に測る。
    rough = FilterSpecularRoughness(N, rough);

    /// @note Forward PBR と同じ UV と sRGB デコードを使い、出力は線形 HDR のまま保持する。
    /// @see https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#additional-textures glTF 2.0 §3.9.3 emissive texture
    const float3 emissionTexture = (textureMask & (1u << 3))
        ? SRGBToLinear(texEmissive.Sample(sampDefault, uv).rgb)
        : float3(1.0f, 1.0f, 1.0f);

    GBufferOut o;
    o.albedoRoughness = float4(col, rough);
    /// @note 法線は [-1,1] → [0,1] にエンコード (復元: n*2-1)
    o.normalMetallic  = float4(N * 0.5f + 0.5f, met);
    /// @note The normalized draw material supplies opaque0 / supportedGlass1 / unsupportedTypedGlass2 at float@84; alpha is not emission energy.
    o.emission = float4(emissionTexture * emissiveColor * emissiveScale, _matPad1.x);
    return o;
}
