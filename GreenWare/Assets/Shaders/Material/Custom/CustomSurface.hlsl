// FBZZ Engine
// Material/Custom/CustomSurface.hlsl | Material
// カスタムマテリアルのスターターテンプレート (Blinn-Phong ベース)
//
// 使い方:
//   1. このファイルを別名でコピーする (例: MyMaterial.hlsl)
//   2. MaterialConstants の変数を目的に合わせて追加・変更する
//   3. PSMain を編集して見た目を実装する
//   4. 保存後はEditor/CMakeが自動収集し、compile_shaders.ps1が差分だけをコンパイルする
//   5. .mat ファイルを作成し、shader フィールドにこのパスを指定する
//
// 共通ヘルパー (Rendering/SurfaceCommon.hlsli):
//   SurfaceTransformUv / SurfaceSampleAlbedo / SurfaceSampleNormal
//   SurfaceSampleMetallicRoughness / SurfaceSampleOcclusion / SurfaceSampleEmissive
//   SurfaceSafeNormalize
//   エンジン標準のテクスチャ規約を使いながら、PSMain のライティングと合成は自由に書ける。
//
// バインディング早見表 (Binding.hlsli より):
//   cbuffer スロット: b0=Camera  b1=Object  b2=Material  b3=Light  b4=Shadow
//   テクスチャ:       t0=Albedo  t1=Normal  t2=MetallicRough  t3=Emissive  t4=AO
//                    t8=Shadow
//   サンプラー:       s0=Default (Aniso)  s1=Shadow (Comparison)
//
// PBR にグレードアップする場合は Material/Surface/PBR.hlsl を参照。

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/Color.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Rendering/SurfaceCommon.hlsli"

// ---- カスタムマテリアル定数 ----------------------------------------
// WHY: #define FBZZ_MATERIAL_CONSTANTS により Constants.hlsli の
//      デフォルト cbuffer は展開されない。ここで自分の変数を宣言する。
//      変数名と型がそのまま Inspector の UI として自動生成される。
// NOTE: textureMask は必ず最後の 16-byte チャンクの先頭に置くこと。
//       Upload() が自動計算して書き込む (手動設定不要)。
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;            // ベースカラー (RGBA)            offset  0
    float  roughness;         // 粗さ [0, 1] (鏡面ハイライト幅) offset 16
    float  normalStrength;    // 法線マップ強度 [0, 1]           offset 20
    float  emissiveScale;     // 自発光強度                      offset 24
    float  alphaCutoff;       // アルファカットオフ閾値 [0, 1]   offset 28
    float2 uvTiling;          // UV スケール (X, Y)              offset 32
    float2 uvOffset;          // UV オフセット (X, Y)            offset 40
    uint   textureMask;       // テクスチャ存在フラグ (自動設定) offset 48
    float3 _pad;              //                                 offset 52
};

// ---- テクスチャ -------------------------------------------------------
Texture2D              texAlbedo  : register(TEX_ALBEDO);   // t0: ベースカラー
Texture2D              texNormal  : register(TEX_NORMAL);   // t1: 法線マップ (Tangent Space)
Texture2D<float>       texShadow  : register(TEX_SHADOW);   // t8: シャドウマップ (RenderSystem が自動バインド)
SamplerState           sampDefault : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow  : register(SAMPLER_SHADOW);

// ---- 頂点シェーダー ---------------------------------------------------
PSInput VSMain(VSInput v)
{
    PSInput o;
    float4 worldPos4 = mul(float4(v.position, 1.0f), world);
    o.worldPos   = worldPos4.xyz;
    o.svPosition = mul(worldPos4, viewProjection);
    o.normal     = SurfaceSafeNormalize(mul(v.normal,  (float3x3)worldInvTranspose),
                                        float3(0.0f, 1.0f, 0.0f));
    o.tangent    = SurfaceSafeNormalize(mul(v.tangent, (float3x3)world),
                                        float3(1.0f, 0.0f, 0.0f));
    o.uv         = v.uv;
    return o;
}

// ---- ピクセルシェーダー -----------------------------------------------
// ここから下を自由にカスタマイズしてください。
float4 PSMain(PSInput p) : SV_Target0
{
    float2 uv = SurfaceTransformUv(p.uv, uvTiling, uvOffset);

    // Albedo: 共通 API がテクスチャ有無・sRGB・フォールバックを処理する。
    float4 surfaceAlbedo = SurfaceSampleAlbedo(texAlbedo, sampDefault, uv,
                                               albedo, textureMask);
    float3 col   = surfaceAlbedo.rgb;
    float  alpha = surfaceAlbedo.a;
    clip(alpha - alphaCutoff);

    // 法線 (法線マップがあれば適用)
    float3 N = SurfaceSampleNormal(texNormal, sampDefault, uv, p.normal,
                                   p.tangent, normalStrength, textureMask);

    // シャドウ
    float3 L      = SurfaceSafeNormalize(-lightDir, float3(0.0f, 1.0f, 0.0f));
    float  shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);

    // Blinn-Phong ライティング
    float3 V        = SurfaceSafeNormalize(cameraPos - p.worldPos, N);
    float3 H        = SurfaceSafeNormalize(L + V, N);
    float  NdotL    = max(0.0f, dot(N, L));
    float  NdotH    = max(0.0f, dot(N, H));
    float  shininess = max(1.0f, (1.0f - roughness) * 128.0f);

    float3 ambient  = ambientColor * col;
    float3 diffuse  = lightColor * lightIntensity * NdotL * shadow * col;
    float3 specular = lightColor * lightIntensity * pow(NdotH, shininess) * shadow * 0.5f;
    float3 result   = ambient + diffuse + specular;

    // ポイントライト (点光源)
    // 点光源 / スポットライト — 走査元は clusterLightMode が決める
    // (b3 の固定長配列 / StructuredBuffer / クラスタリスト)。
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        float3 Hp  = SurfaceSafeNormalize(ps.L + V, N);
        float  dif = max(0.0f, dot(N, ps.L));
        float  spe = pow(max(0.0f, dot(N, Hp)), shininess) * 0.5f;
        result += ps.color * ps.intensity * (dif * col + spe);
    FBZZ_PUNCTUAL_END

    // 自発光
    result += col * emissiveScale;

    return float4(result, alpha);
}
