// FBZZ Engine
// Lighting.hlsli | Rendering
// Lambert / Phong / Blinn-Phong / PBR ライティング関数
//
// 引数の L は常に「表面 → ライト」方向 (=-lightDir) で統一する。
// 呼び出し側: float3 L = normalize(-lightDir);
#ifndef LIGHTING_HLSLI
#define LIGHTING_HLSLI

#include "Rendering/BRDF.hlsli"

// 環境光の定数 (IBL が未実装の間の仮固定値)
static const float AMBIENT_SCALE = 0.08f;

// =========================================================================
// Lambert 拡散のみ
// =========================================================================
float3 Lighting_Lambert(float3 N, float3 L,
                         float3 albedo,
                         float3 lightColor, float lightIntensity,
                         float shadow)
{
    float NdotL  = saturate(dot(N, L));
    float3 ambient = albedo * AMBIENT_SCALE;
    float3 diffuse = albedo * lightColor * lightIntensity * NdotL * shadow;
    return ambient + diffuse;
}

// =========================================================================
// Phong 鏡面反射
//   R = reflect(-L, N) を V と比較してスペキュラを計算する。
// =========================================================================
float3 Lighting_Phong(float3 N, float3 V, float3 L,
                       float3 albedo, float roughness,
                       float3 lightColor, float lightIntensity,
                       float shadow)
{
    float NdotL    = saturate(dot(N, L));
    float shininess = max(lerp(128.0f, 2.0f, roughness), 2.0f);
    float3 R        = reflect(-L, N);
    float  RdotV    = saturate(dot(R, V));

    float3 ambient  = albedo * AMBIENT_SCALE;
    float3 diffuse  = albedo * lightColor * lightIntensity * NdotL * shadow;
    float3 specular = lightColor * lightIntensity
                    * pow(RdotV, shininess)
                    * (1.0f - roughness) * 0.5f * shadow;
    return ambient + diffuse + specular;
}

// =========================================================================
// Blinn-Phong 鏡面反射 (旧 Mesh.hlsl の後継)
//   H = normalize(V + L) を N と比較してスペキュラを計算する。
//   Phong より物理的に正確でハイライトが自然。
// =========================================================================
float3 Lighting_BlinnPhong(float3 N, float3 V, float3 L,
                            float3 albedo, float roughness,
                            float3 lightColor, float lightIntensity,
                            float shadow)
{
    float3 H       = normalize(V + L);
    float NdotL    = saturate(dot(N, L));
    float NdotH    = saturate(dot(N, H));
    float shininess = max(lerp(128.0f, 2.0f, roughness), 2.0f);

    float3 ambient  = albedo * AMBIENT_SCALE;
    float3 diffuse  = albedo * lightColor * lightIntensity * NdotL * shadow;
    float3 specular = lightColor * lightIntensity
                    * pow(NdotH, shininess)
                    * (1.0f - roughness) * 0.5f * shadow;
    return ambient + diffuse + specular;
}

// =========================================================================
// PBR (Cook-Torrance) ライティング
//   EvaluateBRDF で拡散 + 鏡面を評価し、ライト寄与を掛け合わせる。
//   IBL 実装後は ambient を差し替える (AMBIENT_SCALE は仮値)。
// =========================================================================
float3 Lighting_PBR(float3 N, float3 V, float3 L,
                     float3 albedo, float metallic, float roughness,
                     float3 lightColor, float lightIntensity,
                     float shadow, float ao)
{
    BRDFResult brdf  = EvaluateBRDF(N, V, L, albedo, metallic, roughness);
    float3 light     = lightColor * lightIntensity * shadow * brdf.NdotL;
    float3 direct    = (brdf.diffuse + brdf.specular) * light;
    float3 ambient   = albedo * AMBIENT_SCALE * ao;
    return ambient + direct;
}

// =========================================================================
// Toon (Cel) シェーディング
//   NdotL を 3 段階のバンドに量子化して漫画風の陰影にする。
// =========================================================================
float3 Lighting_Toon(float3 N, float3 L,
                     float3 albedo,
                     float3 lightColor, float lightIntensity,
                     float shadow)
{
    float NdotL = dot(N, L);
    float band  = NdotL > 0.5f ? 1.0f : (NdotL > 0.0f ? 0.5f : 0.12f);
    float3 ambient = albedo * AMBIENT_SCALE;
    float3 diffuse = albedo * lightColor * lightIntensity * band * shadow;
    return ambient + diffuse;
}

// =========================================================================
// ポイント/スポットライト用ヘルパー
// =========================================================================

// 距離ベースの二乗減衰 (range で正規化)
float LightAttenuation(float dist, float range)
{
    float r = saturate(dist / range);
    return saturate(1.0f - r * r) / (dist * dist + 1.0f);
}

// スポットコーン: L はサーフェス→ライト方向、spotDir はライトの照射方向
float SpotConeWeight(float3 L, float3 spotDir, float innerCos, float outerCos)
{
    float cosA = dot(L, -spotDir);
    return smoothstep(outerCos, innerCos, cosA);
}

// =========================================================================
// Direct-only (アンビエントなし) — ポイント/スポットループで使用
// =========================================================================

float3 Lighting_Lambert_Direct(float3 N, float3 L,
                                float3 albedo,
                                float3 lightColor, float lightIntensity)
{
    float NdotL = saturate(dot(N, L));
    return albedo * lightColor * lightIntensity * NdotL;
}

float3 Lighting_Phong_Direct(float3 N, float3 V, float3 L,
                              float3 albedo, float roughness,
                              float3 lightColor, float lightIntensity)
{
    float NdotL    = saturate(dot(N, L));
    float shininess = max(lerp(128.0f, 2.0f, roughness), 2.0f);
    float3 R        = reflect(-L, N);
    float  RdotV    = saturate(dot(R, V));
    float3 diffuse  = albedo * lightColor * lightIntensity * NdotL;
    float3 specular = lightColor * lightIntensity * pow(RdotV, shininess) * (1.0f - roughness) * 0.5f;
    return diffuse + specular;
}

float3 Lighting_BlinnPhong_Direct(float3 N, float3 V, float3 L,
                                   float3 albedo, float roughness,
                                   float3 lightColor, float lightIntensity)
{
    float3 H        = normalize(V + L);
    float  NdotL    = saturate(dot(N, L));
    float  NdotH    = saturate(dot(N, H));
    float  shininess = max(lerp(128.0f, 2.0f, roughness), 2.0f);
    float3 diffuse  = albedo * lightColor * lightIntensity * NdotL;
    float3 specular = lightColor * lightIntensity * pow(NdotH, shininess) * (1.0f - roughness) * 0.5f;
    return diffuse + specular;
}

float3 Lighting_PBR_Direct(float3 N, float3 V, float3 L,
                            float3 albedo, float metallic, float roughness,
                            float3 lightColor, float lightIntensity)
{
    BRDFResult brdf  = EvaluateBRDF(N, V, L, albedo, metallic, roughness);
    float3 light     = lightColor * lightIntensity * brdf.NdotL;
    return (brdf.diffuse + brdf.specular) * light;
}

float3 Lighting_Toon_Direct(float3 N, float3 L,
                             float3 albedo,
                             float3 lightColor, float lightIntensity)
{
    float NdotL = dot(N, L);
    float band  = NdotL > 0.5f ? 1.0f : (NdotL > 0.0f ? 0.5f : 0.0f);
    return albedo * lightColor * lightIntensity * band;
}

#endif // LIGHTING_HLSLI