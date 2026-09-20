// FBZZ Engine
// BRDF.hlsli | Rendering
// Cook-Torrance 物理ベースBRDF (GGX / Smith-Schlick / Schlick Fresnel)
#ifndef BRDF_HLSLI
#define BRDF_HLSLI

#include "Common/Math.hlsli"

// @note SafeNormalize は Common/Math.hlsli へ移した (このファイルが include している)。
//       ライティング以外 (GBuffer のスキンド変種) からも要るようになったため。

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
    float3 H   = SafeNormalize(V + L, N);
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
    const float m = saturate(metallic);
    const float r = max(saturate(roughness), 0.045f);
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), saturate(albedo), m);

    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));

    BRDFResult result;
    result.NdotL = NdotL;

    // V と L が逆向き、または面の裏側にある場合は V+L がゼロになり得る。
    // WHY: normalize(0) の NaN は影係数を乗算しても消えず、白い点として露出するため先に除外する。
    if (NdotL <= EPSILON || NdotV <= EPSILON)
    {
        result.diffuse  = (1.0f - m) * BRDF_Diffuse(saturate(albedo));
        result.specular = float3(0.0f, 0.0f, 0.0f);
        return result;
    }

    float3 H    = SafeNormalize(V + L, N);
    float VdotH = saturate(dot(V, H));

    float3 F  = F_Schlick(VdotH, F0);
    // メタルは拡散なし (エネルギー保存)
    float3 kD = (1.0f - F) * (1.0f - m);

    result.diffuse = kD * BRDF_Diffuse(saturate(albedo));
    result.specular = BRDF_Specular(N, V, L, F0, r);
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

// 異方性 GGX。tangent / bitangent 方向の粗さを変えてブラシ状の反射を作る。
float D_GGX_Anisotropic(float3 N, float3 H, float3 T, float3 B,
                        float roughness, float anisotropy)
{
    const float r = max(roughness, 0.045f);
    const float aspect = sqrt(max(1.0f - 0.9f * abs(anisotropy), 0.1f));
    const float ax = max(r * r / aspect, 0.001f);
    const float ay = max(r * r * aspect, 0.001f);
    const float3 h = float3(dot(H, T), dot(H, B), dot(H, N));
    const float d = Sq(h.x / ax) + Sq(h.y / ay) + Sq(h.z);
    return 1.0f / max(PI * ax * ay * Sq(d), EPSILON);
}

float3 BRDF_SpecularAdvanced(float3 N, float3 V, float3 L, float3 F0,
                             float roughness, float3 T, float3 B, float anisotropy)
{
    const float r = max(roughness, 0.045f);
    const float3 H = SafeNormalize(V + L, N);
    const float NdotH = saturate(dot(N, H));
    const float NdotV = saturate(dot(N, V));
    const float NdotL = saturate(dot(N, L));
    const float VdotH = saturate(dot(V, H));
    const float D = abs(anisotropy) > 0.001f
        ? D_GGX_Anisotropic(N, H, T, B, r, clamp(anisotropy, -0.95f, 0.95f))
        : D_GGX(NdotH, r);
    const float G = G_Smith(NdotV, NdotL, r);
    const float3 F = F_Schlick(VdotH, F0);
    return (D * G * F) / max(4.0f * NdotV * NdotL, EPSILON);
}

// Disney 系の sheen 近似。布の grazing 反射を diffuse と独立した lobe として扱う。
float3 BRDF_Sheen(float3 N, float3 V, float3 L, float3 sheenColor, float sheen)
{
    const float NdotL = saturate(dot(N, L));
    const float NdotV = saturate(dot(N, V));
    const float3 H = SafeNormalize(V + L, N);
    const float NdotH = saturate(dot(N, H));
    const float VdotH = saturate(dot(V, H));
    const float rough = 0.5f;
    const float charlie = (2.0f + 1.0f / rough) *
        pow(max(1.0f - Sq(NdotH), 0.0f), 0.5f / rough) / (2.0f * PI);
    const float3 fresnel = F_Schlick(VdotH, saturate(sheenColor));
    const float visibility = 1.0f / max(4.0f * (NdotL + NdotV - NdotL * NdotV), EPSILON);
    return charlie * fresnel * saturate(sheen) * visibility;
}

float3 BRDF_Clearcoat(float3 N, float3 V, float3 L,
                      float clearcoatRoughness, float clearcoat)
{
    const float NdotL = saturate(dot(N, L));
    const float NdotV = saturate(dot(N, V));
    const float3 H = SafeNormalize(V + L, N);
    const float NdotH = saturate(dot(N, H));
    const float VdotH = saturate(dot(V, H));
    const float r = max(saturate(clearcoatRoughness), 0.03f);
    const float D = D_GGX(NdotH, r);
    const float G = G_Smith(NdotV, NdotL, r);
    const float3 F = F_Schlick(VdotH, float3(0.04f, 0.04f, 0.04f));
    return saturate(clearcoat) * (D * G * F) /
           max(4.0f * NdotV * NdotL, EPSILON);
}

// 追加ローブを base diffuse/base specular のエネルギーから差し引いて評価する。
BRDFResult EvaluateBRDFAdvanced(
    float3 N, float3 V, float3 L, float3 T, float3 B,
    float3 albedo, float metallic, float roughness,
    float clearcoat, float clearcoatRoughness,
    float sheen, float anisotropy, float3 sheenColor)
{
    const float m = saturate(metallic);
    const float r = max(saturate(roughness), 0.045f);
    const float3 H = SafeNormalize(V + L, N);
    const float VdotH = saturate(dot(V, H));
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), saturate(albedo), m);
    const float3 F = F_Schlick(VdotH, F0);
    const float coat = saturate(clearcoat);
    const float coatF = coat * F_Schlick(VdotH, float3(0.04f, 0.04f, 0.04f)).r;
    const float baseEnergy = saturate(1.0f - coatF);
    const float sheenEnergy = saturate(sheen) * 0.5f;

    BRDFResult result;
    result.NdotL = saturate(dot(N, L));
    if (result.NdotL <= EPSILON || saturate(dot(N, V)) <= EPSILON) {
        result.diffuse = (1.0f - m) * BRDF_Diffuse(albedo);
        result.specular = float3(0.0f, 0.0f, 0.0f);
        return result;
    }

    const float3 kD = (1.0f - F) * (1.0f - m);
    result.diffuse = kD * (1.0f - sheenEnergy) * baseEnergy * BRDF_Diffuse(albedo);
    result.specular = baseEnergy * BRDF_SpecularAdvanced(
        N, V, L, F0, r, T, B, clamp(anisotropy, -0.95f, 0.95f));
    result.specular += BRDF_Clearcoat(N, V, L, clearcoatRoughness, coat);
    result.specular += BRDF_Sheen(N, V, L, sheenColor, sheen);
    return result;
}

#endif // BRDF_HLSLI
