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

// -------------------------------------------------------------------------
// LIGHT_UNIT_SCALE — intensity のアーティスト単位換算
//
// 本ファイルの拡散項は Lambert BRDF を INV_PI で正規化している (BRDF.hlsli)。
// そのため intensity を厳密な放射照度として扱うと、intensity = 1 の白ライトを
// 白い拡散面へ正面から当てても出力は albedo × 0.318 にしかならない。一方 IBL の
// 環境光は π 正規化済みの irradiance キューブマップから albedo × radiance を返す
// ため、直接光だけが常に π 倍暗く見えていた。これが「明るい空間ではライトが
// 効かない」と感じる主因。
//
// ここで直接光の寄与全体に π を掛け、intensity = 1 が「完全拡散の白面が albedo
// そのままの明るさになる」単位を意味するよう揃える。拡散と鏡面へ等しく掛かるため
// Cook-Torrance のエネルギー配分 (diffuse/specular 比) は変わらない。
//
// この単位は Anisotropic / Subsurface / Foliage / Custom など INV_PI を使わない
// 既存シェーダーの慣習と一致する。つまり本補正はエンジン全体のスケール統一でもある。
// (Toon は非物理モデルで元から同じ単位のため、あえて補正しない)
// -------------------------------------------------------------------------
#define LIGHT_UNIT_SCALE PI

// =========================================================================
// Lambert 拡散のみ
// =========================================================================
float3 Lighting_Lambert(float3 N, float3 L,
                         float3 albedo,
                         float3 lightColor, float lightIntensity,
                         float shadow)
{
    lightIntensity *= LIGHT_UNIT_SCALE;   // アーティスト単位 → 放射照度
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
    lightIntensity *= LIGHT_UNIT_SCALE;   // アーティスト単位 → 放射照度
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
    lightIntensity *= LIGHT_UNIT_SCALE;   // アーティスト単位 → 放射照度
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
    lightIntensity *= LIGHT_UNIT_SCALE;   // アーティスト単位 → 放射照度
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
    // 単位換算はダイレクト光のみ。IBL アンビエントは irradiance 側で π 正規化済みのため掛けない。
    lightIntensity *= LIGHT_UNIT_SCALE;
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
float3 Lighting_PBR_Advanced(
    float3 N, float3 V, float3 L, float3 T, float3 B,
    float3 albedo, float metallic, float roughness,
    float clearcoat, float clearcoatRoughness,
    float sheen, float anisotropy, float3 sheenColor,
    float3 lightColor, float lightIntensity, float shadow)
{
    lightIntensity *= LIGHT_UNIT_SCALE;
    const BRDFResult brdf = EvaluateBRDFAdvanced(
        N, V, L, T, B, albedo, metallic, roughness,
        clearcoat, clearcoatRoughness, sheen, anisotropy, sheenColor);
    const float3 light = lightColor * lightIntensity * shadow * brdf.NdotL;
    return (brdf.diffuse + brdf.specular) * light;
}

float3 Lighting_PBR_IBL_Advanced(
    float3 N, float3 V, float3 L, float3 T, float3 B,
    float3 albedo, float metallic, float roughness,
    float clearcoat, float clearcoatRoughness,
    float sheen, float anisotropy, float3 sheenColor,
    float3 lightColor, float lightIntensity, float shadow, float ao,
    TextureCube irradianceMap, TextureCube prefilterMap,
    Texture2D<float4> brdfLUT, int maxMipLevel, float iblIntensity,
    float diffuseScale, float specularScale,
    SamplerState samp, SamplerState sampClamp)
{
    const float3 direct = Lighting_PBR_Advanced(
        N, V, L, T, B, albedo, metallic, roughness,
        clearcoat, clearcoatRoughness, sheen, anisotropy, sheenColor,
        lightColor, lightIntensity, shadow);
    const float3 ambient = EvaluateIBLAdvanced(
        N, V, T, B, anisotropy, albedo, metallic, roughness, ao,
        clearcoat, clearcoatRoughness, sheen, sheenColor,
        irradianceMap, prefilterMap, brdfLUT, maxMipLevel,
        diffuseScale, specularScale, samp, sampClamp) * max(iblIntensity, 0.0f);
    return ambient + direct;
}

// Toon (Cel) シェーディング
//   NdotL を 3 段階のバンドに量子化して漫画風の陰影にする。
//
// NOTE: Toon は非物理モデルなので INV_PI 正規化も LIGHT_UNIT_SCALE 補正も掛けない。
//       素の `albedo * intensity * band` は結果的に他モデルの補正後と同じ
//       「intensity=1 → albedo そのまま」スケールになっており、既に整合している。
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

// 距離減衰 — 物理的な逆二乗 × range で打ち切るスムーズ窓 (UE4 方式)
//
// 旧実装は `saturate(1 - r*r) / (dist*dist + 1)` で、range 窓と逆二乗を掛けた上に
// 分母の +1 が近距離を支配していた。結果 range=10 のライトでも 3m 地点で減衰 0.091、
// 5m で 0.029 まで落ち、intensity=1 では実質見えなかった。
//
// 新実装の内訳:
//   window = (1 - (d/r)^4)^2 … range 端で値と傾きの両方が 0 になるため打ち切りが目立たない。
//                              二乗する前の (1 - t^4) だけだと端で傾きが残りリングが見える。
//   1 / d^2                 … 物理的な逆二乗。単位は「1m 地点での放射照度 = intensity」。
//   max(d*d, 0.01)          … d→0 の特異点ガード。0.1m 未満は 0.1m 扱いにする。
//                              旧実装の +1 と違い、実用距離 (1m 以上) の明るさを歪めない。
//
// intensity の単位換算 (π 補正 / 点光源スケール) は C++ 側の ApplyLightUnitScale が担う。
float LightAttenuation(float dist, float range)
{
    float t   = saturate(dist / max(range, 1e-4f));
    float t2  = t * t;
    float win = saturate(1.0f - t2 * t2);
    win *= win;
    return win / max(dist * dist, 0.01f);
}

// スポットコーン: L はサーフェス→ライト方向、spotDir はライトの照射方向
float SpotConeWeight(float3 L, float3 spotDir, float innerCos, float outerCos)
{
    float cosA = dot(L, -spotDir);
    return smoothstep(outerCos, innerCos, cosA);
}

// =========================================================================
// Direct-only (アンビエントなし) — ポイント/スポットループで使用
//
// 呼び出し側は lightIntensity に LightAttenuation の結果 (と Spot ならコーン係数) を
// 掛けた値を渡す。LIGHT_UNIT_SCALE は Directional 版と同じくここで掛けるため、
// ポイント/スポットも Directional と同一のアーティスト単位で扱える。
// =========================================================================

float3 Lighting_Lambert_Direct(float3 N, float3 L,
                                float3 albedo,
                                float3 lightColor, float lightIntensity)
{
    lightIntensity *= LIGHT_UNIT_SCALE;
    float NdotL = saturate(dot(N, L));
    return albedo * INV_PI * lightColor * lightIntensity * NdotL;
}

float3 Lighting_Phong_Direct(float3 N, float3 V, float3 L,
                              float3 albedo, float roughness,
                              float3 lightColor, float lightIntensity)
{
    lightIntensity *= LIGHT_UNIT_SCALE;
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
    lightIntensity *= LIGHT_UNIT_SCALE;
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
    lightIntensity *= LIGHT_UNIT_SCALE;
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
    // Lighting_Toon と同じ理由で LIGHT_UNIT_SCALE は掛けない。
    return albedo * lightColor * lightIntensity * band;
}

#endif // LIGHTING_HLSLI
