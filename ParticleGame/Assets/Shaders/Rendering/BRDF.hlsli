// FBZZ Engine
// BRDF.hlsli | Rendering
// Cook-Torrance 物理ベースBRDF (GGX / Smith-Schlick / Schlick Fresnel)
#ifndef BRDF_HLSLI
#define BRDF_HLSLI

#include "Common/Math.hlsli"

// =========================================================================
// GGX 法線分布関数 (NDF)
//   roughness が低いほど鋭いハイライトになる。
//   roughness は事前に [0.05, 1] にクランプして NaN を防ぐ。
// =========================================================================
float D_GGX(float NdotH, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = Sq(NdotH) * (a2 - 1.0f) + 1.0f;
    return a2 / (PI * Sq(d) + EPSILON);
}

// =========================================================================
// Smith-Schlick 幾何減衰関数
//   k はダイレクトライティング用のリマッピング: (roughness+1)^2 / 8
//   IBL 用は roughness^2 / 2 を使うが、ここではダイレクト専用。
// =========================================================================
float G_SchlickGGX(float NdotX, float roughness)
{
    float k = Sq(roughness + 1.0f) * 0.125f;
    return NdotX / (NdotX * (1.0f - k) + k + EPSILON);
}

float G_Smith(float NdotV, float NdotL, float roughness)
{
    return G_SchlickGGX(NdotV, roughness) * G_SchlickGGX(NdotL, roughness);
}

// =========================================================================
// Schlick フレネル近似
//   cosTheta : dot(V, H)
//   F0       : 垂直入射時の反射率 (メタル=albedo, 誘電体=0.04)
// =========================================================================
float3 F_Schlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * Pow5(saturate(1.0f - cosTheta));
}

// =========================================================================
// Cook-Torrance 鏡面反射 BRDF
//   N, V, L : ワールド空間の正規化ベクトル
//   L       : 表面 → ライト方向 (=-lightDir)
// =========================================================================
float3 BRDF_Specular(float3 N, float3 V, float3 L, float3 F0, float roughness)
{
    float r    = max(roughness, 0.05f);  // roughness=0 で D_GGX が発散するのを防ぐ
    float3 H   = normalize(V + L);
    float NdotH = saturate(dot(N, H));
    float NdotV = saturate(dot(N, V));
    float NdotL = saturate(dot(N, L));
    float VdotH = saturate(dot(V, H));

    float  D = D_GGX(NdotH, r);
    float  G = G_Smith(NdotV, NdotL, r);
    float3 F = F_Schlick(VdotH, F0);

    return (D * G * F) / max(4.0f * NdotV * NdotL, EPSILON);
}

// =========================================================================
// Lambert 拡散 BRDF (INV_PI で正規化済み)
// =========================================================================
float3 BRDF_Diffuse(float3 albedo) { return albedo * INV_PI; }

// =========================================================================
// EvaluateBRDF — PBR マテリアルの拡散 + 鏡面を一括評価する
//
// 呼び出し側は結果に NdotL * lightColor * lightIntensity * shadow を掛けて
// ライト寄与を計算する。
// =========================================================================
struct BRDFResult
{
    float3 diffuse;
    float3 specular;
    float  NdotL;
};

BRDFResult EvaluateBRDF(float3 N, float3 V, float3 L,
                         float3 albedo, float metallic, float roughness)
{
    // 誘電体の F0 = 0.04, メタルの F0 = albedo (線形補間)
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    float3 H    = normalize(V + L);
    float VdotH = saturate(dot(V, H));
    float NdotL = saturate(dot(N, L));

    float3 F  = F_Schlick(VdotH, F0);
    // メタルは拡散なし (エネルギー保存)
    float3 kD = (1.0f - F) * (1.0f - metallic);

    BRDFResult result;
    result.NdotL   = NdotL;
    result.diffuse = kD * BRDF_Diffuse(albedo);
    result.specular = BRDF_Specular(N, V, L, F0, roughness);
    return result;
}

#endif // BRDF_HLSLI