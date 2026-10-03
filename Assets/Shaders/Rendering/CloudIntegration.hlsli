/// @file    CloudIntegration.hlsli
/// @brief   主ビューと空 IBL 捕捉で共有する雲の視線積分と照明。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#ifndef FBZZ_CLOUD_INTEGRATION_HLSLI
#define FBZZ_CLOUD_INTEGRATION_HLSLI
#include "Rendering/CloudVolume.hlsli"

/// @note 空白は粗く進め、雲内だけ密度・太陽の遮蔽を求める。
static const float kCloudEmptyStep = 2.0f;
static const int kCloudMsOctaves = 3;
static const float kCloudMsAttenuation = 0.5f;
static const float kCloudMsEccentricity = 0.5f;

/// @note 放射輝度の既存規約に合わせ、PDF の 1/(4π) はここでは含めない。
/// @see https://www.pbr-book.org/4ed/Volume_Scattering/Phase_Functions Henyey-Greenstein phase function
float FBZZCloudHenyeyGreenstein(float cosT, float g)
{
    float g2 = g * g;
    return (1.0f - g2) / pow(max(1.0f + g2 - 2.0f * g * cosT, 1e-4f), 1.5f);
}

float FBZZCloudPhase(float cosT, float eccentricity)
{
    float g = clamp(cloudProfile.z, 0.0f, 0.95f) * eccentricity;
    return lerp(FBZZCloudHenyeyGreenstein(cosT, g), FBZZCloudHenyeyGreenstein(cosT, -g * 0.55f), 0.35f);
}

/// @note 散乱の次数を上げるほど消散と位相を弱め、雲内へ回り込む光を近似する。
/// @see https://www.advances.realtimerendering.com/s2015/ The Real-time Volumetric Cloudscapes of Horizon: Zero Dawn, multiple scattering approximation
float FBZZCloudSunEnergy(float opticalToSun, float cosT)
{
    float msContribution = saturate(cloudShading.w);
    float energy = 0.0f;
    float attenuation = 1.0f, contribution = 1.0f, eccentricity = 1.0f;
    [unroll]
    for (int n = 0; n < kCloudMsOctaves; ++n)
    {
        energy += contribution * FBZZCloudPhase(cosT, eccentricity) * exp(-opticalToSun * attenuation);
        attenuation *= kCloudMsAttenuation;
        contribution *= msContribution;
        eccentricity *= kCloudMsEccentricity;
    }
    return energy;
}

/// @param ro ワールド空間の捕捉位置 [m]。
/// @param rd ワールド空間の正規化済み視線。
/// @param sceneDepth 視線上の遮蔽物までの距離 [m]。空では雲の maxDistance。
/// @param dither 区間内の標本位置 [0,1]。捕捉では固定 0.5 で時間的なノイズを作らない。
/// @param stepLimit 主ビューは 96、128² の IBL 捕捉は 32 を上限とする。
/// @return rgb は premultiplied の HDR 散乱光、a は雲の覆い率。
float4 FBZZIntegrateCloud(float3 ro, float3 rd, float sceneDepth, float dither, int stepLimit)
{
    float t0, t1;
    if (!FBZZCloudSlabIntersect(ro, rd, t0, t1)) return 0.0f;
    t1 = min(t1, min(sceneDepth, cloudNoise.w));
    if (t1 <= t0) return 0.0f;

    int steps = clamp((int)cloudWind.w, 8, max(stepLimit, 8));
    float density = max(cloudLayer.z, 0.0f);
    float extinction = FBZZCloudExtinction();
    /// @note 水平に近い視線でも雲層の後端へ届くよう、厚みではなく交差区間を分割する。
    float pathLength = max(t1 - t0, 0.0f);
    float stepLen = max(pathLength / (float)steps, 0.5f);
    int maxIterations = clamp((int)ceil(pathLength / stepLen), 1, 256);

    float2 windWorld = FBZZCloudWindOffset();
    float lightLen = length(lightDir);
    float3 sunDir = lightLen > 1.0e-4f ? -lightDir / lightLen : float3(0.0f, 1.0f, 0.0f);
    /// @note 地表用の lightIntensity と独立し、主ビューの空と同じ skyDimmer で照らす。
    float3 lightCol = lightColor * max(skyDimmer, 0.0f) * max(cloudShading.y, 0.0f) * cloudSunTint.rgb;
    float3 ambientTop = (ambientColor * 2.0f + lightCol * 0.2f)
                      * max(cloudLighting.y, 0.0f) * cloudAmbTint.rgb;
    float ambientFloor = saturate(cloudAlbedo.w);
    float cosT = dot(rd, sunDir);
    float silver = pow(saturate(cosT), 4.0f) * max(cloudLighting.z, 0.0f);
    float powderStrength = saturate(cloudShading.z);
    float nearStart = max(cloudRange.x, 0.0f);
    float nearFade = max(cloudRange.y, 1.0e-3f);
    float t = t0 + saturate(dither) * stepLen;
    float transmittance = 1.0f;
    float3 scatter = 0.0f;

    [loop]
    for (int i = 0; i < maxIterations && t < t1 && transmittance > 0.01f; ++i)
    {
        float3 wp = ro + rd * t;
        float weather = FBZZCloudWeather(wp, windWorld);
        float base = FBZZCloudShape01(wp, windWorld, weather);
        if (base <= 0.001f) { t += stepLen * kCloudEmptyStep; continue; }
        /// @note 雲内からの視線が最初の標本で飽和しないよう、近距離の濃度を落とす。
        float d = FBZZCloudErodeDetail(base, wp, windWorld) * density * saturate((t - nearStart) / nearFade);
        if (d > 0.001f)
        {
            float sigmaE = d * extinction;
            float stepTrans = exp(-sigmaE * stepLen);
            float opticalToSun = FBZZCloudOpticalDepthToSun(wp, sunDir, windWorld, weather);
            float powder = lerp(1.0f, 1.0f - exp(-sigmaE * stepLen * 2.0f), powderStrength);
            float3 sun = lightCol * (FBZZCloudSunEnergy(opticalToSun, cosT)
                                   + silver * exp(-opticalToSun * 0.25f)) * powder;
            float hFrac = saturate((wp.y - cloudLayer.x) / max(cloudLayer.y - cloudLayer.x, 1.0f));
            float3 ambient = ambientTop * lerp(ambientFloor, 1.0f, hFrac);
            /// @note σs=σe として区間を解析積分し、散乱と透過のエネルギーを分ける。
            /// @see https://www.pbr-book.org/4ed/Volume_Scattering/The_Equation_of_Transfer equation of transfer
            scatter += transmittance * (sun + ambient) * cloudAlbedo.rgb * (1.0f - stepTrans);
            transmittance *= stepTrans;
        }
        t += stepLen;
    }

    /// @note maxDistance 直前で雲を薄め、水平線に硬い切れ目を作らない。
    float horizonFade = saturate(cloudRange.z);
    float horizonMask = horizonFade > 1.0e-4f
        ? 1.0f - smoothstep(cloudNoise.w * (1.0f - horizonFade), cloudNoise.w, t0)
        : 1.0f;
    return float4(scatter * horizonMask, saturate(1.0f - transmittance) * horizonMask);
}
#endif
