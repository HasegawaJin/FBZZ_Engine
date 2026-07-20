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

    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));

    BRDFResult result;
    result.NdotL = NdotL;

    // V と L が逆向き、または面の裏側にある場合は V+L がゼロになり得る。
    // WHY: normalize(0) の NaN は影係数を乗算しても消えず、白い点として露出するため先に除外する。
    if (NdotL <= EPSILON || NdotV <= EPSILON)
    {
        result.diffuse  = (1.0f - metallic) * BRDF_Diffuse(albedo);
        result.specular = float3(0.0f, 0.0f, 0.0f);
        return result;
    }

    float3 H    = normalize(V + L);
    float VdotH = saturate(dot(V, H));

    float3 F  = F_Schlick(VdotH, F0);
    // メタルは拡散なし (エネルギー保存)
    float3 kD = (1.0f - F) * (1.0f - metallic);

    result.diffuse = kD * BRDF_Diffuse(albedo);
    result.specular = BRDF_Specular(N, V, L, F0, roughness);
    return result;
}

// =========================================================================
// F_SchlickRoughness — IBL specular 用ラフネス補正フレネル
//
// 通常の F_Schlick は roughness=1 でも純粋な F0 に収束しないため、
// IBL 事前フィルタリングとの整合性を取るために roughness をフレネル上限に織り込む。
// roughness が高いほど F0 への収束が早まり、鏡面ハイライトが抑制される。
//   cosTheta : dot(N, V)
//   F0       : 垂直入射時の反射率 (メタル=albedo, 誘電体=0.04)
//   roughness: 粗さ [0, 1]
//
// 参考: Sebastien Lagarde, "Moving Frostbite to Physically Based Rendering 3.0"
// =========================================================================
float3 F_SchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    // roughness が高いほど上限が F0 に近づき、フレネル遷移幅が縮まる
    float3 limit = max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), F0);
    return F0 + (limit - F0) * Pow5(saturate(1.0f - cosTheta));
}

#endif // BRDF_HLSLI
