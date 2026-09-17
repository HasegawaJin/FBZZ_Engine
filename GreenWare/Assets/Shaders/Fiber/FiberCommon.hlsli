/// @file    FiberCommon.hlsli
/// @brief   毛皮と芝の根元固定変形、手続き密度、照明。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#ifndef FBZZ_FIBER_COMMON_HLSLI
#define FBZZ_FIBER_COMMON_HLSLI
#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Rendering/LodDither.hlsli"
#include "Rendering/FlowField.hlsli"

/// @note Asset/FiberMaterialSettings.hpp と一致。風の応答は [s]、長さと曲げ量はワールド [m]。
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 rootColor;
    float4 tipColor;
    float fiberLength;
    float fiberDensity;
    float fiberThickness;
    float fiberTaper;
    float fiberFrequency;
    float windResponse;
    float maxBend;
    float gravityBend;
    float roughness;
    float specularStrength;
    float transmission;
    float rootOcclusion;
    float grassShading;
    float worldMapping;
    float finSilhouetteWidth;
    float _fiberPadding;
};

cbuffer FiberFrameConstants : register(b5)
{
    float3 fiberWind;
    float fiberTime;
    float fiberShellCount;
    float fiberHybrid;
    float fiberTurbulence;
    float fiberPulseFrequency;
    float4 fiberLod;
    /// @note 局所 FlowField の範囲。今フレームは [0, fiberFlowCount)、前フレームは [fiberFlowPreviousFirst, +fiberFlowPreviousCount)。
    uint fiberFlowCount;
    uint fiberFlowPreviousFirst;
    uint fiberFlowPreviousCount;
    /// @note FiberComponent::flowChannels。場の channels と 1 ビットでも重なる場だけを受ける。
    uint fiberFlowChannels;
};

/// @note 環境流は fiberWind が持つ。ここにはシーンに置いた FlowField だけが入り、二重に足さない。
FBZZ_VS_SBUFFER(GpuFlowField, fiberFlowFields, VS_SB_BUFFER1_SLOT);
FBZZ_TEX3D_T(float4, fiberVelocityAtlas, TEX_VELOCITY_FIELD_SLOT);
SamplerState fiberFlowSampler : register(SAMPLER_LINEAR_CLAMP);

/// @brief 根元での局所 FlowField の媒質速度 [m/s]。頂点シェーダー専用。
/// @note 場に覆われない点は 0。GPU 粒子と同じ SampleFlowFields で評価する。
float3 FiberLocalFlow(float3 root, float time, bool previous)
{
    uint count = previous ? fiberFlowPreviousCount : fiberFlowCount;
    if (count == 0u) return float3(0.0f, 0.0f, 0.0f);
    bool covered = false;
    return SampleFlowFields(fiberFlowFields, previous ? fiberFlowPreviousFirst : 0u, count, fiberFlowChannels,
        fiberVelocityAtlas, fiberFlowSampler, root, time, covered);
}

/// @note 最後の 32 接触。発生時刻より前の速度評価には接触を適用しない。
/// @note LAYOUT: FiberRenderPass.hpp の FiberContactCB と一致させる。
cbuffer FiberContactConstants : register(b10)
{
    float4 fiberContactCenters[64];
    float4 fiberContactTimes[64];
    /// @note 走査する枠数 [0,32]。これより後ろの枠は寄与しない (CPU が保証する)。
    uint fiberContactCount;
    uint fiberContactPreviousCount;
    uint _fiberContactPadding0;
    uint _fiberContactPadding1;
};

float3 FiberContactOffset(float3 root, float3 normal, float height, float time, float lengthValue, bool previous = false)
{
    /// @note 戻り値は grassShading を掛けるので、毛皮 (0) と接触 0 件では頂点ごとの 32 回の走査を丸ごと省く。
    uint count = min(previous ? fiberContactPreviousCount : fiberContactCount, 32u);
    if (count == 0u || grassShading <= 0.0f) return float3(0.0f, 0.0f, 0.0f);
    float strongest = 0.0f;
    float3 direction = float3(1, 0, 0);
    [loop] for (uint j=0; j<count; ++j) {
        uint i=j+(previous ? 32 : 0);
        float age = time - fiberContactTimes[i].x;
        float radius = fiberContactCenters[i].w;
        if (radius <= 0 || age < 0) continue;
        float3 delta = root-fiberContactCenters[i].xyz;
        float weight = saturate(1.0f-length(delta)/radius)
            * saturate(1.0f-age/max(fiberContactTimes[i].z, 0.05f))*fiberContactTimes[i].y;
        if (weight > strongest) {
            strongest = weight;
            direction = delta-normal*dot(delta,normal);
            direction = dot(direction,direction)>1.0e-8f ? normalize(direction) : float3(1,0,0);
        }
    }
    return (direction-normal)*lengthValue*0.85f*strongest*height*height*grassShading;
}

FBZZ_TEX2D_T(float, texShadow, TEX_SHADOW_SLOT);
SamplerComparisonState sampShadow : register(SAMPLER_SHADOW);

float3 FiberNormalize(float3 value, float3 fallback)
{
    float lenSq = dot(value, value);
    return lenSq > 1.0e-12f ? value * rsqrt(lenSq) : fallback;
}

/// @note 位相は根元のワールド位置で固定し、Shell の層やビューごとに変更しない。
/// @note h^2 は根元固定の見た目用近似。毛の物理シミュレーションではない。
/// @param localFlow 局所 FlowField の媒質速度 [m/s]。環境風と同じ応答係数で曲げへ足す。
/// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-7-rendering-countless-blades-waving-grass
float3 FiberBendAt(float3 root, float3 normal, float3 wind, float time,
    float turbulence, float pulseFrequency, float response, float bendLimit, float gravity, float3 localFlow)
{
    float phase = dot(root, float3(0.73f, 0.31f, 0.57f)) + time * pulseFrequency * 6.2831853f;
    float3 gust = float3(sin(phase), 0.0f, cos(phase * 0.79f)) * turbulence;
    float3 force = (wind + gust + localFlow) * response + float3(0.0f, -gravity, 0.0f);
    float3 bend = force - normal * dot(force, normal);
    float len = length(bend);
    return bend * min(1.0f, bendLimit / max(len, 1.0e-6f));
}

float3 FiberBend(float3 root, float3 normal, float3 localFlow)
{
    return FiberBendAt(root, normal, fiberWind, fiberTime, fiberTurbulence,
        fiberPulseFrequency, windResponse, maxBend, gravityBend, localFlow);
}

float3 FiberPosition(float3 root, float3 normal, float height, float3 localFlow)
{
    return root + normal * (fiberLength * height) + FiberBend(root, normal, localFlow) * (height * height)
        + FiberContactOffset(root, normal, height, fiberTime, fiberLength);
}

float2 FiberRootCoordinates(float2 uv, float3 root)
{
    return (worldMapping >= 0.5f ? root.xz : uv) * fiberFrequency;
}

float FiberHash(float2 cell)
{
    float3 p = frac(float3(cell.xyx) * 0.1031f);
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}

/// @note 全層が同じセル中心と高さを参照することで、点の集合を連続した繊維にする。
/// @note 論文のボリューム断面を、決定的なセル内分布で近似する。層ごとの独立ノイズは使わない。
/// @see https://hhoppe.com/fur.pdf
void FiberClipShell(float2 coordinate, float height)
{
    float2 cell = floor(coordinate);
    clip(fiberDensity - FiberHash(cell + 19.7f) - 1.0e-6f);
    float strandHeight = lerp(0.55f, 1.0f, FiberHash(cell + 73.1f));
    clip(strandHeight - height);
    float2 center = 0.25f + 0.5f * float2(FiberHash(cell), FiberHash(cell + 11.3f));
    float radius = fiberThickness * (1.0f - fiberTaper * height / strandHeight);
    clip(radius - length(frac(coordinate) - center));
}

/// @note Fin は毛の側面投影を一次元の分布で近似する。Shell と同じ高さ・先細り分布を使う。
/// @see https://hhoppe.com/fur.pdf
void FiberClipFin(float coordinate, float height)
{
    float cell = floor(coordinate);
    float2 key = float2(cell, 7.0f);
    clip(fiberDensity - FiberHash(key + 19.7f) - 1.0e-6f);
    float strandHeight = lerp(0.55f, 1.0f, FiberHash(key + 73.1f));
    clip(strandHeight - height);
    float center = 0.25f + 0.5f * FiberHash(key);
    float radius = fiberThickness * (1.0f - fiberTaper * height / strandHeight);
    clip(radius - abs(frac(coordinate) - center));
}

float3 FiberDirect(float3 N, float3 T, float3 V, float3 L, float3 color, float3 radiance)
{
    float diffuse = saturate(dot(N, L));
    float backlight = saturate(dot(-L, V)) * transmission * (1.0f - diffuse);
    float3 H = FiberNormalize(L + V, N);
    /// @note 繊維接線に直交する半角ベクトルのローブ。完全な毛髪散乱モデルではない。
    /// @see https://hhoppe.com/fur.pdf
    float tangentDot = dot(T, H);
    float specular = pow(saturate(1.0f - tangentDot * tangentDot), lerp(128.0f, 2.0f, roughness));
    return radiance * (color * (diffuse + grassShading * backlight)
        + specularStrength * specular * (1.0f - grassShading) * diffuse);
}

/// @param localFlow 頂点で評価した局所 FlowField。ピクセルごとに場を引き直さない。
float4 FiberLighting(float3 position, float3 normal, float3 root, float height, float2 pixel, float3 localFlow)
{
    float3 N = FiberNormalize(normal, float3(0.0f, 1.0f, 0.0f));
    float3 T = FiberNormalize(N * fiberLength + 2.0f * height * FiberBend(root, N, localFlow), N);
    float3 V = FiberNormalize(cameraPos - position, N);
    float3 L = FiberNormalize(-lightDir, N);
    float3 color = lerp(rootColor.rgb, tipColor.rgb, height);
    float ao = (1.0f - rootOcclusion * (1.0f - height)) * FBZZ_ScreenAO(pixel);
    float shadow = ComputeShadow(texShadow, sampShadow, position,
        lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    shadow *= FBZZ_ScreenContactShadow(pixel);
    float3 result = color * ambientColor * ao;
    result += FiberDirect(N, T, V, L, color, lightColor * lightIntensity * shadow);
    FBZZ_PUNCTUAL_BEGIN(position, pixel, N)
        result += FiberDirect(N, T, V, ps.L, color, ps.color * ps.intensity);
    FBZZ_PUNCTUAL_END
    return float4(result, 1.0f);
}
#endif
