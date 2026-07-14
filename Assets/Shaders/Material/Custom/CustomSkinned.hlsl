// FBZZ Engine
// Material/Custom/CustomSkinned.hlsl | Material
// カスタムスキンドマテリアルのスターターテンプレート (Blinn-Phong ベース)
//
// 使い方:
//   1. このファイルを別名でコピーする (例: MySkinnedMaterial.hlsl)
//   2. MaterialConstants の変数を目的に合わせて追加・変更する
//   3. PSMain を編集して見た目を実装する
//   4. Assets/Shaders/compile_shaders.bat を実行してコンパイルする
//   5. .mat ファイルを作成し、shader フィールドにこのパスを指定する
//      (mesh_type = 'skinned' を必ず設定すること)
//
// バインディング早見表 (Binding.hlsli より):
//   cbuffer スロット: b0=Camera  b1=Object  b2=Material  b3=Light  b4=Shadow
//   テクスチャ:       t0=Albedo  t1=Normal  t2=MetallicRough  t3=Emissive  t4=AO
//                    t8=Shadow
//   サンプラー:       s0=Default (Aniso)  s1=Shadow (Comparison)
//
// Skinned 専用:
//   Constants.hlsli の boneMatrices[] を VSMain で参照してボーンブレンドを行う。
//   SkinnedVSInput.boneIndices / boneWeights を必ず使うこと。
//   Static Mesh には割り当て不可 — .mat の mesh_type = 'skinned' が強制する。
//
// PBR にグレードアップする場合は Material/Skinned/SkinnedPBR.hlsl を参照。

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/Color.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"

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

Texture2D              texAlbedo   : register(TEX_ALBEDO);   // t0: ベースカラー
Texture2D              texNormal   : register(TEX_NORMAL);   // t1: 法線マップ (Tangent Space)
Texture2D<float>       texShadow   : register(TEX_SHADOW);   // t8: シャドウマップ (RenderSystem が自動バインド)
SamplerState           sampDefault : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow  : register(SAMPLER_SHADOW);

// 4 本のボーンを線形ブレンドしてスキン行列を計算する。
// WHY: SkinnedBlinnPhong.hlsl と同一ロジックを使い、エンジンのリグ規約に準拠する。
float4x4 BlendSkinMatrix(SkinnedVSInput v)
{
    return boneMatrices[v.boneIndices.x] * v.boneWeights.x
         + boneMatrices[v.boneIndices.y] * v.boneWeights.y
         + boneMatrices[v.boneIndices.z] * v.boneWeights.z
         + boneMatrices[v.boneIndices.w] * v.boneWeights.w;
}

PSInput VSMain(SkinnedVSInput v)
{
    PSInput o;
    float4x4 skin    = BlendSkinMatrix(v);
    float4 localPos  = mul(float4(v.position, 1.0f), skin);
    float3 localN    = normalize(mul(v.normal,  (float3x3)skin));
    float3 localT    = normalize(mul(v.tangent, (float3x3)skin));
    float4 worldPos4 = mul(localPos, world);
    o.worldPos   = worldPos4.xyz;
    o.svPosition = mul(worldPos4, viewProjection);
    o.normal     = normalize(mul(localN, (float3x3)worldInvTranspose));
    o.tangent    = normalize(mul(localT, (float3x3)world));
    o.uv         = v.uv;
    return o;
}

// ここから下を自由にカスタマイズしてください。
float4 PSMain(PSInput p) : SV_Target0
{
    float2 uv = p.uv * uvTiling + uvOffset;

    // Albedo
    float4 rawAlbedo = (textureMask & (1u << 0))
        ? texAlbedo.Sample(sampDefault, uv)
        : float4(1.0f, 1.0f, 1.0f, 1.0f);
    float3 col   = SRGBToLinear(rawAlbedo.rgb) * albedo.rgb;
    float  alpha = rawAlbedo.a * albedo.a;
    clip(alpha - alphaCutoff);

    // 法線 (法線マップがあれば適用)
    float3 N = normalize(p.normal);
    if (textureMask & (1u << 1))
    {
        float3 ns = texNormal.Sample(sampDefault, uv).rgb;
        float3 nm = ApplyNormalMap(ns, N, normalize(p.tangent));
        N = normalize(lerp(N, nm, normalStrength));
    }

    // シャドウ
    float3 L      = normalize(-lightDir);
    float  shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);

    // Blinn-Phong ライティング
    float3 V        = normalize(cameraPos - p.worldPos);
    float3 H        = normalize(L + V);
    float  NdotL    = max(0.0f, dot(N, L));
    float  NdotH    = max(0.0f, dot(N, H));
    float  shininess = max(1.0f, (1.0f - roughness) * 128.0f);

    float3 ambient  = ambientColor * col;
    float3 diffuse  = lightColor * lightIntensity * NdotL * shadow * col;
    float3 specular = lightColor * lightIntensity * pow(NdotH, shininess) * shadow * 0.5f;
    float3 result   = ambient + diffuse + specular;

    // ポイントライト (点光源)
    [loop] for (int pi = 0; pi < pointLightCount; ++pi)
    {
        float3 toLight = pointLights[pi].position - p.worldPos;
        float  dist    = length(toLight);
        float3 Lp      = toLight / dist;
        float  atten   = LightAttenuation(dist, pointLights[pi].range);
        float3 Hp      = normalize(Lp + V);
        float  dif     = max(0.0f, dot(N, Lp)) * atten;
        float  spe     = pow(max(0.0f, dot(N, Hp)), shininess) * atten * 0.5f;
        result += pointLights[pi].color * pointLights[pi].intensity * (dif * col + spe);
    }

    // 自発光
    result += col * emissiveScale;

    return float4(result, alpha);
}
