/// @file DecalScorch.hlsl
/// @brief 焼け跡デカール。テクスチャ無しで円形の焦げと縁の熱を手続き的に描く
/// @author Hasegawa Jin
/// @date 2026-08-23
///
/// WHY 手続きで描くか:
///     着弾の焼け跡は「輪郭がぼやけた円に、縁だけ熱が残る」という形が決まっている。
///     テクスチャで持つと解像度ぶんのメモリと 1 発ごとの見た目の固定を抱えるのに対し、
///     半径と柔らかさを公開しておけば .mat 1 枚で大小・新旧を作り分けられる。
///
/// 既定値の取り方: InitDefaultMaterialParams が全 float を 1.0 で敷くため、
///     「1.0 が自然な状態」になるようパラメータを選んである (coverage は 1 で満開)。
#include "Material/Decal/DecalCommon.hlsli"

Texture2D texAlbedo : register(TEX_ALBEDO);

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 scorchColor;   // offset  0  焦げの色 (a = 全体の不透明度)
    float4 rimColor;      // offset 16  縁に残る熱の色
    float  edgeSoftness;  // offset 32  輪郭のぼけ幅 [0, 1]
    float  rimIntensity;  // offset 36  縁の加算量 (0 で縁なし)
    float  coverage;      // offset 40  1 で満開、0 で消滅。時間で縮めると鎮火に見える
    uint   textureMask;   // offset 44  bit0 = albedo で焦げを変調する
};

DecalPixelInput VSMain(uint id : SV_VertexID)
{
    return DecalVertexMain(id);
}

float4 PSMain(DecalPixelInput input) : SV_Target
{
    DecalSurface surface;
    if (!DecalResolve(input.screenUv, surface))
        discard;

    // 投影 UV の中心からの距離。1.0 が OBB の外接円。
    float dist   = saturate(length(surface.uv - 0.5f) * 2.0f);
    float radius = saturate(coverage);
    float soft   = max(saturate(edgeSoftness), 1.0e-3f);
    float body   = 1.0f - smoothstep(radius - soft, radius, dist);

    // 落ち際の帯だけを取り出して縁の熱にする。body の山ではなく傾きが欲しいので、
    // body(1-body) を使う (中心と外側で 0、遷移帯の中央で最大)。
    float rim = saturate(body * (1.0f - body) * 4.0f);

    float3 color = scorchColor.rgb + rimColor.rgb * rim * rimIntensity;
    float  alpha = scorchColor.a * body * surface.alpha;

    if (textureMask & 1u)
    {
        float4 texel = texAlbedo.Sample(sampDecal, surface.uv);
        color *= texel.rgb;
        alpha *= texel.a;
    }

    if (alpha < 0.001f)
        discard;

    return float4(color, alpha);
}
