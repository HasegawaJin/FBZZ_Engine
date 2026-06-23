// FBZZ Engine
// IBL.hlsli | Rendering
// Image-Based Lighting (IBL) 評価関数
//
// EvaluateIBL() を呼び出すことで、拡散 IBL (irradiance cubemap) と
// 鏡面 IBL (split-sum approximation) を合算した環境光寄与を取得できる。
//
// 設計方針:
//   - 拡散: Lambertian 仮定のもと事前畳み込み済み irradiance cubemap をサンプリング
//   - 鏡面: UE4 方式の split-sum近似
//           prefilter cubemap (roughness → mip) × BRDF LUT (NdotV, roughness)
//   - IBL 用 G_Smith は k = roughness^2/2 でリマップ (ダイレクト用の (r+1)^2/8 と異なる)
//
// 参考: Brian Karis, "Real Shading in Unreal Engine 4" (SIGGRAPH 2013)
//       Sebastien Lagarde, "Moving Frostbite to Physically Based Rendering 3.0"
#ifndef IBL_HLSLI
#define IBL_HLSLI

#include "Common/Math.hlsli"
#include "Rendering/BRDF.hlsli"

// =========================================================================
// IBL 専用 Smith-Schlick 幾何減衰関数
//
// ダイレクトライティング用の G_SchlickGGX は k=(roughness+1)^2/8 でリマップするが、
// IBL では重要度サンプリングによる積分と合わせるため k=roughness^2/2 を使う。
// (ダイレクト用の k は光源の固体角を考慮した補正値)
//   NdotX    : dot(N, V) または dot(N, L)、0 クランプ済み
//   roughness: 粗さ [0, 1] (α = roughness^2 の前に渡す)
// =========================================================================
float G_SchlickGGX_IBL(float NdotX, float roughness)
{
    // IBL 用リマッピング: k = roughness^2 / 2
    float k = roughness * roughness * 0.5f;
    return NdotX / (NdotX * (1.0f - k) + k + EPSILON);
}

// Smith の双方向幾何減衰 (IBL 版)
float G_Smith_IBL(float NdotV, float NdotL, float roughness)
{
    return G_SchlickGGX_IBL(NdotV, roughness) * G_SchlickGGX_IBL(NdotL, roughness);
}

// =========================================================================
// EvaluateIBL — PBR マテリアルへの環境光寄与を計算する
//
// split-sum 近似 (UE4 方式):
//   ∫ f(l,v) L(l) cos(θ_l) dl ≈ L_prefilter(r, mip) × (F0 × brdf.x + brdf.y)
//
// 引数:
//   N              : ワールド空間 法線 (正規化済み)
//   V              : ワールド空間 視線ベクトル (正規化済み、サーフェス→カメラ)
//   albedo         : ベースカラー
//   metallic       : 金属度 [0, 1]
//   roughness      : 粗さ [0, 1]
//   ao             : アンビエントオクルージョン [0, 1]
//   irradianceMap  : 拡散 IBL cubemap (事前畳み込み済み)
//   prefilterMap   : 鏡面 IBL cubemap (roughness → mip でフィルタ済み)
//   brdfLUT        : BRDF 積分テーブル (x=scale, y=bias)
//   maxMipLevel    : prefilterMap の最大 mip レベル
//   diffuseScale   : 拡散 IBL の独立スケール
//   specularScale  : 鏡面 IBL の独立スケール
//   samp           : 通常サンプラー (irradiance 用)
//   sampClamp      : Linear Clamp サンプラー (BRDF LUT 用)
//
// 戻り値: 環境光寄与 (ダイレクトライティングに加算する)
// =========================================================================
float3 EvaluateIBL(
    float3      N,
    float3      V,
    float3      albedo,
    float       metallic,
    float       roughness,
    float       ao,
    TextureCube irradianceMap,
    TextureCube prefilterMap,
    Texture2D<float4> brdfLUT,
    int         maxMipLevel,
    float       diffuseScale,
    float       specularScale,
    SamplerState      samp,
    SamplerState      sampClamp)
{
    // 反射ベクトルと法線-視線角
    float3 R    = reflect(-V, N);
    float  NdotV = saturate(dot(N, V));

    // F0: 誘電体=0.04, メタル=albedo
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    // ---- 拡散 IBL --------------------------------------------------------
    // ラフネス補正フレネル: IBL の diffuse kD に使う
    float3 F        = F_SchlickRoughness(NdotV, F0, roughness);
    float3 kD       = (1.0f - F) * (1.0f - metallic);  // エネルギー保存 (メタルは拡散なし)
    float3 irradiance = irradianceMap.Sample(samp, N).rgb;
    float3 diffuse  = kD * irradiance * albedo * max(diffuseScale, 0.0f);

    // ---- 鏡面 IBL (split-sum) -------------------------------------------
    // roughness から prefilter mip を決定 (高 roughness ほど低解像度 mip をサンプル)
    float  mip           = roughness * (float)maxMipLevel;
    float3 prefilteredColor = prefilterMap.SampleLevel(samp, R, mip).rgb;

    // BRDF LUT: x=scale(F0 倍率), y=bias (定数加算)
    // UV: (NdotV, roughness) — Linear Clamp でサンプリング
    float2 brdf = saturate(brdfLUT.Sample(sampClamp, float2(NdotV, roughness)).rg);
    float3 specular = prefilteredColor * (F0 * brdf.x + brdf.y)
                    * max(specularScale, 0.0f);

    // ---- 合算 + AO -------------------------------------------------------
    // SSAO は近傍ジオメトリによる拡散遮蔽の近似なので diffuse のみに適用する。
    // WHY: 低サンプルSSAOを鏡面へ直接掛けると、明るいIBL反射との白黒差が点状に強調される。
    //      鏡面遮蔽には bent normal 等が必要で、SSAOの流用はしない。
    return diffuse * saturate(ao) + specular;
}

#endif // IBL_HLSLI
