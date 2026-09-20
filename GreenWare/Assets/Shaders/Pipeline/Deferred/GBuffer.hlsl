// FBZZ Engine
// Pipeline/Deferred/GBuffer.hlsl | Pipeline
// ジオメトリパス — 法線マップ・PBR テクスチャを 2 枚の MRT に書き出す
//
// MRT レイアウト:
//   SV_Target0 (RGBA16F): RGB=albedo,      A=roughness
//   SV_Target1 (RGBA16F): RGB=worldNormal, A=metallic

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
SamplerState sampDefault      : register(SAMPLER_DEFAULT);

// @brief 本体。入口だけが変種ごとに違い、変換そのものは 1 か所に置く。
PSInput GBufferVS(VSInput v, float4x4 objectWorld, float4x4 objectWorldInvTranspose)
{
    PSInput o;
    float4 worldPos4 = mul(float4(v.position, 1.0f), objectWorld);
    o.worldPos   = worldPos4.xyz;
    o.svPosition = mul(worldPos4, viewProjection);
    o.normal     = normalize(mul(v.normal,  (float3x3)objectWorldInvTranspose));
    o.tangent    = normalize(mul(v.tangent, (float3x3)objectWorld));
    o.uv         = v.uv;
    return o;
}

#if defined(FBZZ_SKINNED)
// @note b7 のパレットで頂点を変形してから本体へ渡す。式は SkinnedPBR.hlsl と揃えること
//       (ここだけ違うと «Deferred のときだけキャラの法線が違う» という形で出る)。
// @note コンピュートスキニングが効いているときはこの変種を使わない。変形済みの頂点が
//       静的メッシュと同じレイアウトで来るので、素の GBuffer.hlsl でそのまま描ける。
// @see Docs/design/pipeline-boundary.md §3
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
// @note 束ねた描画。world は b1 でなく VS の t0 から引く。@see Docs/design/gpu-instancing.md
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

    // Albedo + tint
    // sRGB テクスチャを線形空間にデコードしてから tint (線形) を乗算する。
    // テクスチャなし時は (1,1,1) として albedo.rgb をそのまま使用 (SRGBToLinear(1)=1)。
    float4 rawAlbedo = (textureMask & (1u << 0))
        ? texAlbedo.Sample(sampDefault, uv)
        : float4(1.0f, 1.0f, 1.0f, 1.0f);
    float3 col  = SRGBToLinear(rawAlbedo.rgb) * albedo.rgb;
    float  alpha = rawAlbedo.a * albedo.a;
    clip(alpha - alphaCutoff);

    // Normal (法線マップがあれば TBN で変換、normalStrength でブレンド)
    float3 N = normalize(p.normal);
    if (textureMask & (1u << 1))
    {
        float3 ns = texNormal.Sample(sampDefault, uv).rgb;
        float3 nm = ApplyNormalMap(ns, N, normalize(p.tangent));
        N = normalize(lerp(N, nm, normalStrength));
    }

    // Metallic / Roughness (glTF 規約: G=roughness, B=metallic)
    float met   = metallic;
    float rough = roughness;
    if (textureMask & (1u << 2))
    {
        float2 mr = texMetallicRough.Sample(sampDefault, uv).gb;
        rough = mr.x;
        met   = mr.y;
    }

    // 濡れは素材の値なので、法線分散のフィルタより先に掛ける。
    // 逆順にすると、AA で持ち上げた粗さを濡れが再び下げてちらつきが戻る。
    const WetSurface wet = ApplyWetness(col, saturate(rough), N);
    col   = wet.albedo;
    rough = wet.roughness;

    // WHY ここで掛けるか: Deferred では DeferredLighting が読む法線は GBuffer 経由の
    //     隣接ピクセル値で、そこには別オブジェクトの法線も混ざる。1 面ぶんの法線分散は
    //     書き出し側でしか測れない。
    rough = FilterSpecularRoughness(N, rough);

    GBufferOut o;
    o.albedoRoughness = float4(col, rough);
    // 法線は [-1,1] → [0,1] にエンコード (復元: n*2-1)
    o.normalMetallic  = float4(N * 0.5f + 0.5f, met);
    return o;
}
