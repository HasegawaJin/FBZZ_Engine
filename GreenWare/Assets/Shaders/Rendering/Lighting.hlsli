/// @file    Lighting.hlsli
/// @brief   Lambert / Phong / Blinn-Phong / PBR / Toon のライティング関数と、点光源の走査層の入口。
/// @author  Hasegawa Jin
/// @date    2026-06-23
/// @note 引数の L は常に «表面 → ライト» 方向 (= -lightDir) で統一する。
/// @note ambientColor は LightConstants (b3) の値。Lit は (0.08, 0.08, 0.08)、Unlit は (1, 1, 1)。
#ifndef LIGHTING_HLSLI
#define LIGHTING_HLSLI

#include "Rendering/BRDF.hlsli"
#include "Rendering/IBL.hlsli"
/// @note 画面空間 AO / 接触影の受け口。対象は «ライティングを持つ全マテリアル» で、このファイルを include するシェーダーとちょうど一致する。
#include "Rendering/ScreenSpaceShading.hlsli"

/// @brief intensity のアーティスト単位換算。
/// @note 拡散は Lambert を INV_PI で正規化している (BRDF.hlsli) ため、intensity を放射照度として扱うと白ライトを白い面へ当てても albedo × 0.318 にしかならず、π 正規化済みの IBL 環境光に対して直接光だけが π 倍暗く見えていた。
/// @note 直接光全体に π を掛けて «intensity = 1 で完全拡散の白面が albedo そのまま» の単位へ揃える。拡散と鏡面へ等しく掛かるので Cook-Torrance の配分は変わらない。
/// @note INV_PI を使わない Anisotropic / Subsurface / Custom の慣習とも一致する。Toon は元から同じ単位なので補正しない。
#define LIGHT_UNIT_SCALE PI

/// @brief Lambert 拡散のみ。
float3 Lighting_Lambert(float3 N, float3 L,
                         float3 albedo,
                         float3 lightColor, float lightIntensity,
                         float shadow)
{
    lightIntensity *= LIGHT_UNIT_SCALE;
    float NdotL  = saturate(dot(N, L));
    float3 ambient = albedo * ambientColor;
    float3 diffuse = albedo * INV_PI * lightColor * lightIntensity * NdotL * shadow;
    return ambient + diffuse;
}

/// @brief Phong 鏡面反射。R = reflect(-L, N) を V と比べる。
float3 Lighting_Phong(float3 N, float3 V, float3 L,
                       float3 albedo, float roughness,
                       float3 lightColor, float lightIntensity,
                       float shadow)
{
    lightIntensity *= LIGHT_UNIT_SCALE;
    float NdotL    = saturate(dot(N, L));
    float shininess = max(lerp(128.0f, 2.0f, roughness), 2.0f);
    float3 R        = reflect(-L, N);
    float  RdotV    = saturate(dot(R, V));

    float3 ambient  = albedo * ambientColor;
    float3 diffuse  = albedo * INV_PI * lightColor * lightIntensity * NdotL * shadow;
    float3 specular = lightColor * lightIntensity
                    * pow(RdotV, shininess)
                    * (1.0f - roughness) * 0.5f * INV_PI * shadow;
    return ambient + diffuse + specular;
}

/// @brief Blinn-Phong 鏡面反射 (旧 Mesh.hlsl の後継)。H = normalize(V + L) を N と比べる。
float3 Lighting_BlinnPhong(float3 N, float3 V, float3 L,
                            float3 albedo, float roughness,
                            float3 lightColor, float lightIntensity,
                            float shadow)
{
    lightIntensity *= LIGHT_UNIT_SCALE;
    float3 H       = normalize(V + L);
    float NdotL    = saturate(dot(N, L));
    float NdotH    = saturate(dot(N, H));
    float shininess = max(lerp(128.0f, 2.0f, roughness), 2.0f);

    float3 ambient  = albedo * ambientColor;
    float3 diffuse  = albedo * INV_PI * lightColor * lightIntensity * NdotL * shadow;
    float3 specular = lightColor * lightIntensity
                    * pow(NdotH, shininess)
                    * (1.0f - roughness) * 0.5f * INV_PI * shadow;
    return ambient + diffuse + specular;
}

/// @brief PBR (Cook-Torrance)。環境光は定数 ambientColor。
float3 Lighting_PBR(float3 N, float3 V, float3 L,
                     float3 albedo, float metallic, float roughness,
                     float3 lightColor, float lightIntensity,
                     float shadow, float ao)
{
    lightIntensity *= LIGHT_UNIT_SCALE;
    BRDFResult brdf  = EvaluateBRDF(N, V, L, albedo, metallic, roughness);
    float3 light     = lightColor * lightIntensity * shadow * brdf.NdotL;
    float3 direct    = (brdf.diffuse + brdf.specular) * light;
    float3 ambient   = albedo * ambientColor * ao;
    return ambient + direct;
}

/// @brief PBR + IBL。Lighting_PBR の定数環境光を EvaluateIBL に差し替えたもの。拡散はキューブだけ。
/// @param ao 拡散環境光だけに掛かる遮蔽。EvaluateIBL が乗算済みなので戻り値へ再乗算しない。
/// @param iblIntensity 環境光全体の倍率 (0 = IBL なし)。
/// @param samp irradiance / prefilter 用。
/// @param sampClamp BRDF LUT 用の Linear clamp。
/// @note 拡散と鏡面を EvaluateIBL 内で別々に倍率を掛けるのは、鏡面エイリアシングの診断やアート調整で iblDiffuseScale / iblSpecularScale を切り分けるため。
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
    /// @note 単位換算は直接光だけ。IBL は irradiance 側で π 正規化済み。
    lightIntensity *= LIGHT_UNIT_SCALE;
    BRDFResult brdf = EvaluateBRDF(N, V, L, albedo, metallic, roughness);
    float3 light    = lightColor * lightIntensity * shadow * brdf.NdotL;
    float3 direct   = (brdf.diffuse + brdf.specular) * light;

    float3 ambient = EvaluateIBL(N, V, albedo, metallic, roughness, ao,
                                  irradianceMap, prefilterMap, brdfLUT,
                                  maxMipLevel, diffuseScale, specularScale,
                                  samp, sampClamp)
                     * iblIntensity;

    return ambient + direct;
}

/// @brief PBR + IBL の Light Probe Volume を受ける版。
/// @param worldPos シェーディング点 [world]。ボリュームの中では拡散環境光がプローブから来る。
float3 Lighting_PBR_IBL(
    float3      worldPos,
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
    lightIntensity *= LIGHT_UNIT_SCALE;
    BRDFResult brdf = EvaluateBRDF(N, V, L, albedo, metallic, roughness);
    float3 light    = lightColor * lightIntensity * shadow * brdf.NdotL;
    float3 direct   = (brdf.diffuse + brdf.specular) * light;

    float3 ambient = EvaluateIBL(worldPos, N, V, albedo, metallic, roughness, ao,
                                  irradianceMap, prefilterMap, brdfLUT,
                                  maxMipLevel, diffuseScale, specularScale,
                                  samp, sampClamp)
                     * iblIntensity;

    return ambient + direct;
}

/// @brief 拡張 PBR の直接光。
/// @param T 法線マップから作った正規直交基底の接線。
/// @param B 同じく従法線。
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

/// @brief 拡張 PBR + IBL。拡散はキューブだけ。
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

/// @brief 拡張 PBR + IBL の Light Probe Volume を受ける版。
/// @param worldPos シェーディング点 [world]。
float3 Lighting_PBR_IBL_Advanced(
    float3 worldPos,
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
        worldPos, N, V, T, B, anisotropy, albedo, metallic, roughness, ao,
        clearcoat, clearcoatRoughness, sheen, sheenColor,
        irradianceMap, prefilterMap, brdfLUT, maxMipLevel,
        diffuseScale, specularScale, samp, sampClamp) * max(iblIntensity, 0.0f);
    return ambient + direct;
}

/// @brief Toon (Cel)。NdotL を 3 段へ量子化する。
/// @note 非物理モデルなので INV_PI 正規化も LIGHT_UNIT_SCALE も掛けない。素の albedo * intensity * band が他モデルの補正後と同じ «intensity = 1 → albedo そのまま» の単位になっている。
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

/// @brief 点光源の距離減衰。物理的な逆二乗を range で滑らかに打ち切る。
/// @note 窓 (1 - (d/r)^4)^2 は range 端で値と傾きの両方が 0 になり、二乗前の (1 - t^4) だと端で傾きが残ってリングが見える。
/// @note max(d*d, 0.01) は d→0 の特異点ガード (0.1 m 未満は 0.1 m 扱い)。旧実装の分母 +1 と違い 1 m 以上の明るさを歪めない。
/// @note intensity の単位換算 (π 補正 / 点光源スケール) は C++ 側の ApplyLightUnitScale が担う。
/// @see https://cdn2.unrealengine.com/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf Karis, "Real Shading in Unreal Engine 4", Light Falloff
float LightAttenuation(float dist, float range)
{
    float t   = saturate(dist / max(range, 1e-4f));
    float t2  = t * t;
    float win = saturate(1.0f - t2 * t2);
    win *= win;
    return win / max(dist * dist, 0.01f);
}

/// @brief スポットのコーン係数。
/// @param L サーフェス → ライト方向。
/// @param spotDir ライトの照射方向。
float SpotConeWeight(float3 L, float3 spotDir, float innerCos, float outerCos)
{
    float cosA = dot(L, -spotDir);
    return smoothstep(outerCos, innerCos, cosA);
}

/// @name 直接光だけ (環境光なし)。点光源 / スポットのループで使う。
/// @note 呼び出し側は lightIntensity に LightAttenuation (とスポットならコーン係数) を掛けて渡す。LIGHT_UNIT_SCALE はここで掛けるので Directional と同じ単位で扱える。
/// @{

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

/// @note Lighting_Toon と同じ理由で LIGHT_UNIT_SCALE は掛けない。
float3 Lighting_Toon_Direct(float3 N, float3 L,
                             float3 albedo,
                             float3 lightColor, float lightIntensity)
{
    float NdotL = dot(N, L);
    float band  = NdotL > 0.5f ? 1.0f : (NdotL > 0.0f ? 0.5f : 0.0f);
    return albedo * lightColor * lightIntensity * band;
}

/// @}

/// @note 点光源 / スポットの走査層。LightAttenuation / SpotConeWeight / SafeNormalize を呼ぶので定義より後に置く (HLSL に前方宣言は無い)。光源ループを持つシェーダーは例外なくこのファイルを include しているので、各ファイルへ足さずに行き渡る。
#include "Rendering/ClusteredLights.hlsli"

#endif // LIGHTING_HLSLI
