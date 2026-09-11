/// @file    VolumeRaymarch.hlsl
/// @brief   Volume Flipbook Baker: ボリュームを平行投影でレイマーチし、色と画面空間速度を横並びに書く。
/// @author  Hasegawa Jin
/// @date    2026-09-11
//
// RT は 2·tile × tile。左半分 = 事前乗算のリニア HDR 色 (a = 1 - 透過率)、
// 右半分 = (画面空間速度 [タイル UV/秒], 重みの合計, 1)。
// WHY 1 枚に並べるか: CPU への読み戻し (CaptureRenderTargetToLinearRGBA) は color 0 しか読めない。
//     MRT にするとバックエンドの改修が要り、RT を 2 枚にすると GPU 待ちが 2 回になる。
#include "Common/Fullscreen.hlsli"

cbuffer VolumeRaymarchConstants : register(b0)
{
    float3 gCamRight;   float gHalfExtent;
    float3 gCamUp;      uint  gTileSize;
    float3 gCamForward; uint  gRaySteps;
    float3 gToLight;    uint  gShadowSteps;
    float3 gLightColor; float gExtinction;
    float3 gAmbient;    float gEmissionIntensity;
    float3 gSmokeAlbedo; float gExposure;
    uint  gDisplayMode;       // 0 = ベイク用の生値 / 1 = プレビュー (色) / 2 = プレビュー (α)
    float gAnisotropy;
    float gPreviewMotionScale;
    uint  gBackground;        // プレビューの背景。0 = 暗 / 1 = 明 / 2 = チェッカー
};

Texture3D<float4> gMedium      : register(t0);
Texture3D<float4> gVelocity    : register(t1);
SamplerState      gLinearClamp : register(s2);

// 背景は «表示の色» (sRGB) で決め、合成のためにリニアへ戻す。
float3 PreviewBackground(float2 local)
{
    if (gBackground == 1u) return pow(float3(0.82f, 0.82f, 0.84f), 2.2f);
    if (gBackground == 2u)
    {
        const uint2 cell = uint2(local / 16.0f);
        return pow(((cell.x + cell.y) & 1u) != 0u ? float3(0.42f, 0.42f, 0.42f) : float3(0.26f, 0.26f, 0.26f), 2.2f);
    }
    return pow(float3(0.18f, 0.18f, 0.2f), 2.2f);
}

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

bool IntersectUnitBox(float3 origin, float3 direction, out float tNear, out float tFar)
{
    // 軸に平行な成分を 0 のまま割ると、原点が面上にある画素で 0·inf = NaN が出る。
    float3 safeDirection = direction;
    safeDirection.x = abs(safeDirection.x) < 1.0e-6f ? 1.0e-6f : safeDirection.x;
    safeDirection.y = abs(safeDirection.y) < 1.0e-6f ? 1.0e-6f : safeDirection.y;
    safeDirection.z = abs(safeDirection.z) < 1.0e-6f ? 1.0e-6f : safeDirection.z;
    const float3 inverse = 1.0f / safeDirection;
    const float3 t0 = (-1.0f - origin) * inverse;
    const float3 t1 = ( 1.0f - origin) * inverse;
    const float3 tMin = min(t0, t1);
    const float3 tMax = max(t0, t1);
    tNear = max(max(tMin.x, tMin.y), tMin.z);
    tFar  = min(min(tMax.x, tMax.y), tMax.z);
    return tFar > max(tNear, 0.0f);
}

float DensityAt(float3 position)
{
    return gMedium.SampleLevel(gLinearClamp, position * 0.5f + 0.5f, 0.0f).r;
}

// 光源方向への透過率。影を落とすのは同じボリュームだけ。
float LightTransmittance(float3 position)
{
    float tNear, tFar;
    if (!IntersectUnitBox(position, gToLight, tNear, tFar))
        return 1.0f;
    const float stepLength = tFar / float(max(gShadowSteps, 1u));
    float opticalDepth = 0.0f;
    [loop] for (uint i = 0; i < gShadowSteps; ++i)
        opticalDepth += DensityAt(position + gToLight * (stepLength * (float(i) + 0.5f)));
    return exp(-gExtinction * opticalDepth * stepLength);
}

// 4π を掛けて等方散乱が 1 になるよう正規化した Henyey-Greenstein。
float PhaseHG(float cosTheta, float g)
{
    const float denominator = 1.0f + g * g - 2.0f * g * cosTheta;
    return (1.0f - g * g) / pow(max(denominator, 1.0e-4f), 1.5f);
}

// 温度 [0,1] → 暗い赤 → 橙 → 黄 → ほぼ白。
float3 FireRamp(float t)
{
    const float3 c0 = float3(0.0f, 0.0f, 0.0f);
    const float3 c1 = float3(0.9f, 0.08f, 0.01f);
    const float3 c2 = float3(1.0f, 0.45f, 0.06f);
    const float3 c3 = float3(1.0f, 0.9f, 0.6f);
    const float s = saturate(t) * 3.0f;
    if (s < 1.0f) return lerp(c0, c1, s);
    if (s < 2.0f) return lerp(c1, c2, s - 1.0f);
    return lerp(c2, c3, s - 2.0f);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    const uint2 pixel = uint2(p.svPosition.xy);
    const bool motionHalf = pixel.x >= gTileSize;
    const float2 local = float2(pixel.x - (motionHalf ? gTileSize : 0u), pixel.y) + 0.5f;
    const float2 screen = float2(local.x / float(gTileSize) * 2.0f - 1.0f,
                                 1.0f - local.y / float(gTileSize) * 2.0f);

    const float3 origin = (gCamRight * screen.x + gCamUp * screen.y) * gHalfExtent - gCamForward * 4.0f;
    float tNear, tFar;
    float3 color = 0.0f;
    float transmittance = 1.0f;
    float weightSum = 0.0f;
    float2 motionSum = 0.0f;

    if (IntersectUnitBox(origin, gCamForward, tNear, tFar))
    {
        tNear = max(tNear, 0.0f);
        const float stepLength = (tFar - tNear) / float(max(gRaySteps, 1u));
        const float phase = PhaseHG(dot(gToLight, gCamForward), gAnisotropy);
        [loop] for (uint i = 0; i < gRaySteps; ++i)
        {
            const float3 position = origin + gCamForward * (tNear + stepLength * (float(i) + 0.5f));
            const float4 medium = gMedium.SampleLevel(gLinearClamp, position * 0.5f + 0.5f, 0.0f);
            if (medium.r <= 1.0e-5f)
                continue;
            const float sampleTransmittance = exp(-medium.r * gExtinction * stepLength);
            // この標本が画素へ寄与する割合。速度の重みも同じ量を使う (見えている煙の動きを取る)。
            const float weight = transmittance * (1.0f - sampleTransmittance);

            if (motionHalf)
            {
                const float3 v = gVelocity.SampleLevel(gLinearClamp, position * 0.5f + 0.5f, 0.0f).xyz;
                // 画像は +V が下なので up 成分の符号を反転する。タイルは 2·halfExtent を覆う。
                motionSum += weight * float2(dot(v, gCamRight), -dot(v, gCamUp)) / (2.0f * gHalfExtent);
            }
            else
            {
                const float3 scatter = gSmokeAlbedo
                    * (gLightColor * phase * LightTransmittance(position) + gAmbient);
                const float temperature = saturate(medium.g);
                // 発光は «不透明な炎の輝度» として散乱と同じ不透明度で重み付けする。
                // WHY: 密度 × 距離で積むと、消散係数 k の煙からは実質 emission / k しか出てこない
                //      (既定の k = 10 で炎がほぼ見えなかった)。この形なら Emission の値がそのまま
                //      炎の芯の明るさになり、消散係数を変えても明るさがずれない。
                //      輝度が 1 を超えれば RGB > α になるので、Atlas は事前乗算で持つ。
                const float3 emission = gEmissionIntensity * FireRamp(temperature) * temperature * temperature;
                color += transmittance * (1.0f - sampleTransmittance) * (scatter + emission);
            }
            weightSum += weight;
            transmittance *= sampleTransmittance;
            if (transmittance < 1.0e-3f)
                break;
        }
    }

    const float3 background = PreviewBackground(local);
    if (motionHalf)
    {
        const float2 motion = motionSum / max(weightSum, 1.0e-5f);
        if (gDisplayMode != 0u)
        {
            // 赤 = 右へ / 緑 = 下へ。静止は (0.5, 0.5) の灰色。
            const float3 visual = pow(float3(saturate(0.5f + motion * gPreviewMotionScale), 0.5f), 2.2f);
            return float4(pow(lerp(background, visual, saturate(weightSum)), 1.0f / 2.2f), 1.0f);
        }
        return float4(motion, weightSum, 1.0f);
    }

    const float alpha = 1.0f - transmittance;
    if (gDisplayMode == 2u)
        return float4(alpha.xxx, 1.0f);
    if (gDisplayMode == 1u)
    {
        const float3 composite = saturate(color * gExposure + background * transmittance);
        return float4(pow(composite, 1.0f / 2.2f), 1.0f);
    }
    return float4(color, alpha);
}
