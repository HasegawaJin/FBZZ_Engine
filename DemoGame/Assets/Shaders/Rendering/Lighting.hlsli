// FBZZ Engine
// Lighting.hlsli | Rendering
// Lambert / Phong / Blinn-Phong / PBR ライティング関数
//
// 引数の L は常に「表面 → ライト」方向 (=-lightDir) で統一する。
// 呼び出し側: float3 L = normalize(-lightDir);
#ifndef LIGHTING_HLSLI
#define LIGHTING_HLSLI

#include "Rendering/BRDF.hlsli"
#include "Rendering/IBL.hlsli"

// 環境光スケール: ambientColor は LightConstants cbuffer から来るグローバル変数。
// Lit モード: ambientColor = (0.08, 0.08, 0.08)  Unlit モード: ambientColor = (1, 1, 1)

// =========================================================================
// Lambert 拡散のみ
// =========================================================================
float3 Lighting_Lambert(float3 N, float3 L,
                         float3 albedo,
                         float3 lightColor, float lightIntensity,
                         float shadow)
{
    float NdotL  = saturate(dot(N, L));
    float3 ambient = albedo * ambientColor;
    // INV_PI: Lambert 正規化。PBR の BRDF_Diffuse と同じエネルギースケールにする。
    float3 diffuse = albedo * INV_PI * lightColor * lightIntensity * NdotL * shadow;
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

    float3 ambient  = albedo * ambientColor;
    // INV_PI: Lambert 正規化。PBR と同じエネルギースケール。
    float3 diffuse  = albedo * INV_PI * lightColor * lightIntensity * NdotL * shadow;
    float3 specular = lightColor * lightIntensity
                    * pow(RdotV, shininess)
                    * (1.0f - roughness) * 0.5f * INV_PI * shadow;
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

    float3 ambient  = albedo * ambientColor;
    // INV_PI: Lambert 正規化。PBR と同じエネルギースケール。
    float3 diffuse  = albedo * INV_PI * lightColor * lightIntensity * NdotL * shadow;
    float3 specular = lightColor * lightIntensity
                    * pow(NdotH, shininess)
                    * (1.0f - roughness) * 0.5f * INV_PI * shadow;
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
    float3 ambient   = albedo * ambientColor * ao;
    return ambient + direct;
}

// =========================================================================
// PBR + IBL (Image-Based Lighting) ライティング
//
// Lighting_PBR の ambient (定数 albedo * ambientColor * ao) を、
// EvaluateIBL による物理ベースの環境光に差し替えたバリアント。
// ダイレクトライティング部分は Lighting_PBR と同じ。
//
// 設計:
//   ambient = EvaluateIBL(...) * iblIntensity
//   direct  = (diffuse + specular) * lightColor * lightIntensity * shadow * NdotL
//   合計    = ambient + direct
//
//   EvaluateIBL 内で拡散・鏡面を個別にスケールしてから合算する。
//   WHY: 全体強度だけでは diffuse と specular を切り分けられず、鏡面エイリアシングの
//        診断やアート調整に iblDiffuseScale / iblSpecularScale を利用できないため。
//
// 引数:
//   N, V, L, albedo, metallic, roughness, lightColor, lightIntensity,
//   shadow, ao       : Lighting_PBR と同様
//   irradianceMap    : 拡散 IBL cubemap
//   prefilterMap     : 鏡面 IBL cubemap (roughness → mip でフィルタ済み)
//   brdfLUT          : BRDF 積分テーブル (BRDFIntegration.cs.hlsl でベイク)
//   maxMipLevel      : prefilterMap の最大 mip レベル
//   iblIntensity     : 環境光全体スケール (AdvancedGraphicsConstants より)
//   diffuseScale / specularScale : 拡散・鏡面 IBL の独立スケール
//   samp             : 通常サンプラー (irradiance / prefilter 用)
//   sampClamp        : Linear Clamp サンプラー (BRDF LUT 用)
// =========================================================================
float3 Lighting_PBR_IBL(
    float3      N, float3 V, float3 L,
    float3      albedo, float metallic, float roughness,
    float3      lightColor, float lightIntensity,
    float       shadow, float ao,
    TextureCube       irradianceMap,
    TextureCube       prefilterMap,
    Texture2D<float4> brdfLUT,
    int         maxMipLevel,
    float       iblIntensity,
    float       diffuseScale,
    float       specularScale,
    SamplerState      samp,
    SamplerState      sampClamp)
{
    // ---- ダイレクトライティング (ディレクショナルライト) -----------------
    BRDFResult brdf = EvaluateBRDF(N, V, L, albedo, metallic, roughness);
    float3 light    = lightColor * lightIntensity * shadow * brdf.NdotL;
    float3 direct   = (brdf.diffuse + brdf.specular) * light;

    // ---- IBL アンビエント (物理ベース環境光) ----------------------------
    // EvaluateIBL は AO 乗算済みの値を返すため、ここで ao を再乗算しない。
    // iblIntensity で環境光量を制御する (0=IBL なし, 1=フル, >1=過露出演出)
    float3 ambient = EvaluateIBL(N, V, albedo, metallic, roughness, ao,
                                  irradianceMap, prefilterMap, brdfLUT,
                                  maxMipLevel, diffuseScale, specularScale,
                                  samp, sampClamp)
                     * iblIntensity;

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
    float3 ambient = albedo * ambientColor;
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
    return albedo * INV_PI * lightColor * lightIntensity * NdotL;
}

float3 Lighting_Phong_Direct(float3 N, float3 V, float3 L,
                              float3 albedo, float roughness,
                              float3 lightColor, float lightIntensity)
{
    float NdotL    = saturate(dot(N, L));
    float shininess = max(lerp(128.0f, 2.0f, roughness), 2.0f);
    float3 R        = reflect(-L, N);
    float  RdotV    = saturate(dot(R, V));
    float3 diffuse  = albedo * INV_PI * lightColor * lightIntensity * NdotL;
    float3 specular = lightColor * lightIntensity * pow(RdotV, shininess) * (1.0f - roughness) * 0.5f * INV_PI;
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
    float3 diffuse  = albedo * INV_PI * lightColor * lightIntensity * NdotL;
    float3 specular = lightColor * lightIntensity * pow(NdotH, shininess) * (1.0f - roughness) * 0.5f * INV_PI;
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
