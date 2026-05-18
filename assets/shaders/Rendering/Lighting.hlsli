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

#endif // LIGHTING_HLSLI