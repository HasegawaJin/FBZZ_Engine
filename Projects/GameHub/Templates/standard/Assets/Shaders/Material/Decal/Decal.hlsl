/// @file Decal.hlsl
/// @brief 組み込みデカールシェーダー。.mat 未割り当ての DecalComponent が使う
/// @author Hasegawa Jin
/// @date 2026-08-23
///
/// WHY 組み込みも MaterialConstants を宣言するか:
///   宣言しておけば、このシェーダーをそのまま指した .mat が作れる。デカールに
///   マテリアルを持たせたいだけで新しい HLSL を書く必要がなくなり、色や
///   タイリングを変えたいだけの用途が .mat の params だけで済む。
///   .mat を割り当てない DecalComponent へは DecalPass が同じレイアウトの
///   cbuffer をコンポーネントの値から組んで流す。
///
/// Texture (スロット名は .mat の [textures] と共通):
///   t0 albedo / t1 normal / t3 emissive — いずれも任意
#include "Material/Decal/DecalCommon.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/MaterialTextures.hlsli"


// LAYOUT: RenderPassContext.hpp の DecalMaterialCB と一致させること。
// textureMask は renderer::Material::Upload がテクスチャスロットの有効性から埋める。
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedoTint;      // offset  0
    float3 emissiveColor;   // offset 16
    float  emissiveScale;   // offset 28
    float2 uvTiling;        // offset 32
    float2 uvOffset;        // offset 40
    float  normalStrength;  // offset 48
    uint   textureMask;     // offset 52  bit0=albedo bit1=normal bit3=emissive
    float2 _matPad;         // offset 56

    // bindless のテクスチャ添字。Material::Upload が毎フレーム書き込む。
    uint texAlbedoIndex;
    uint texNormalIndex;
    uint texEmissiveIndex;
};

// WHY cbuffer の後ろへ置くか: 添字フィールドを参照して初期化するため、
//     宣言はフィールドより後ろでなければならない。
FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);
FBZZ_MATERIAL_TEX(texNormal, texNormalIndex);
FBZZ_MATERIAL_TEX(texEmissive, texEmissiveIndex);

DecalPixelInput VSMain(uint id : SV_VertexID)
{
    return DecalVertexMain(id);
}

float4 PSMain(DecalPixelInput input) : SV_Target
{
    DecalSurface surface;
    if (!DecalResolve(input.screenUv, surface))
        discard;

    // コマ送りは最後に掛ける。タイリングでコマの外へ出ないよう、変換の中で frac を通す。
    float2 uv = DecalApplyFlipbook(surface.uv * uvTiling + uvOffset);

    float4 albedoSample = (textureMask & 1u)
        ? texAlbedo.Sample(sampDecal, uv) * albedoTint
        : albedoTint;

    float finalAlpha = albedoSample.a * surface.alpha;
    if (finalAlpha < 0.001f)
        discard;

    float3 color = albedoSample.rgb;
    if ((textureMask & 2u) && normalStrength > 0.0f)
    {
        // 陰影の基準は投影面ではなく受け面の法線。投影面を基準にすると
        // 斜めな面で法線マップの凹凸が逆に見える。
        float3 mapped = DecalDecodeNormal(texNormal.Sample(sampDecal, uv).rgb);
        color *= lerp(1.0f, DecalNormalLightRatio(surface.receiverNormal, mapped),
                      saturate(normalStrength));
    }

    float3 emissive = (textureMask & 8u)
        ? texEmissive.Sample(sampDecal, uv).rgb * emissiveColor * emissiveScale
        : emissiveColor * emissiveScale;

    return float4(color + emissive, finalAlpha);
}