/// @file    WaterStandardAppearance.hlsli
/// @brief   Default water pixel appearance, reusable by a project without duplicating shared surface geometry.
/// @author  Hasegawa Jin
/// @date    2026-10-21
#include "Common/Binding.hlsli"
#ifndef FBZZ_WATER_STANDARD_APPEARANCE_HLSLI
#define FBZZ_WATER_STANDARD_APPEARANCE_HLSLI
#include "Common/Space.hlsli"
/// @note ユーザー定義エフェクトパラメータ。MaterialComponent.paramData にマップされ、
/// @note Script から mc->SetParam<float>("rimGlowStrength", val) で動的に書き換えられる。
/// @warning HLSL cbuffer のパッキング規則に従い 16B 境界を揃えること (48 bytes = float4 x3)。
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float  rimGlowStrength;    ///< @brief Row0: 浅い角度で水の層が明るむ量 (光を受ける) default 0.40
    float  minShallowAlpha;    ///< @brief 水深 0 でも残る濁り [0,1] (opacity と積) default 0.65
    float  specularStrength;   ///< @brief スペキュラー強度   default 0.75
    /// @note 旧 specularExponent。ハイライトの鋭さは smoothness と specular AA から導出するため
    /// @note 廃止した。両方から指数を決めると、遠景でハイライトが 1 ピクセルに縮んで這う。
    float  _pad0;
    float3 skyReflectTint;     ///< @brief Row1: 空反射ベース色 RGB default (0.45, 0.82, 1.0)
    /// @note 旧 envMapBlend。空反射の混合率は skyReflection (g_reflectParams.x) が持つ。
    /// @note 同じ役目のつまみが 2 つあり、片方だけ動かしても効かない状態だった。
    float  _pad1;
    float3 rippleRingColor;    ///< @brief Row2: 波紋リング色 RGB   default (0.88, 0.97, 1.0)
    float  rippleRingStrength; ///< @brief 波紋リング強度     default 0.72
}

SamplerState g_waterPointSampler : register(SAMPLER_POINT_CLAMP);

#include "Water/WaterAppearanceOptics.hlsli"

cbuffer WaterZoneAppearanceConstants : register(FBZZ_REGISTER(b, CB_WATER_ZONE_APPEARANCE_SLOT))
{
    WaterMaterialBlendTarget g_zoneMaterials[32];
}

void BlendWaterMaterialProfiles(inout WaterMaterialBlendTarget material,
    WaterMaterialBlendTarget other, float weight)
{
    material.shallowColorDepth = lerp(material.shallowColorDepth, other.shallowColorDepth, weight);
    material.deepColorDepth = lerp(material.deepColorDepth, other.deepColorDepth, weight);
    material.surface = lerp(material.surface, other.surface, weight);
    material.normalReflectionRefraction = lerp(material.normalReflectionRefraction, other.normalReflectionRefraction, weight);
    material.detail = lerp(material.detail, other.detail, weight);
    material.foam = lerp(material.foam, other.foam, weight);
    material.crestFoam = lerp(material.crestFoam, other.crestFoam, weight);
    material.foamColorRoughness = lerp(material.foamColorRoughness, other.foamColorRoughness, weight);
    material.absorptionPhase = lerp(material.absorptionPhase, other.absorptionPhase, weight);
    material.scatteringMode = lerp(material.scatteringMode, other.scatteringMode, weight);
    material.sss = lerp(material.sss, other.sss, weight);
    material.effects = lerp(material.effects, other.effects, weight);
    material.skyTint = lerp(material.skyTint, other.skyTint, weight);
    material.rippleColor = lerp(material.rippleColor, other.rippleColor, weight);
}

WaterMaterialBlendTarget ResolveWaterSurfaceAppearance(WaterPSInput p)
{
    WaterMaterialBlendTarget material;
    material.shallowColorDepth = g_shallowColorDepth;
    material.deepColorDepth = g_deepColorDepth;
    material.surface = g_surfaceParams;
    material.normalReflectionRefraction = float4(g_normalParams.w, g_reflectParams.x, g_refractionFlowParams.x, 0.0f);
    material.detail = g_detailParams;
    material.foam = g_foamParams;
    material.crestFoam = g_crestFoamParams;
    material.foamColorRoughness = g_foamColorRoughness;
    material.absorptionPhase = float4(g_absorptionCoefficients.xyz, g_opticalParams.y);
    material.scatteringMode = float4(g_scatteringCoefficients.xyz, g_opticalParams.x);
    material.sss = g_sssParams;
    material.effects = float4(rimGlowStrength, minShallowAlpha, specularStrength, rippleRingStrength);
    material.skyTint = float4(skyReflectTint, 0.0f);
    material.rippleColor = float4(rippleRingColor, 0.0f);

    const float zoneWeight = dot(p.zoneReceiverWeights + p.zoneSourceWeights, float3(1.0f, 1.0f, 1.0f));
    [branch] if (zoneWeight > 0.5f) {
        const uint uniformIndex = p.zoneMaterialIndices.x & 31u;
        const uint uniformTriangle = uniformIndex * (1u + (1u << 5u) + (1u << 10u));
        if (all(p.zoneMaterialIndices == uniformTriangle)) {
            material = g_zoneMaterials[uniformIndex];
        } else {
        float accumulated = 0.0f;
        [unroll] for (uint corner = 0u; corner < 3u; ++corner) {
            const uint2 indices = (p.zoneMaterialIndices >> (corner * 5u)) & 31u;
            const float receiver = max(0.0f, p.zoneReceiverWeights[corner]);
            const float source = max(0.0f, p.zoneSourceWeights[corner]);
            if (receiver > 0.0f) {
                accumulated += receiver;
                BlendWaterMaterialProfiles(material, g_zoneMaterials[indices.x], receiver / accumulated);
            }
            if (source > 0.0f) {
                accumulated += source;
                BlendWaterMaterialProfiles(material, g_zoneMaterials[indices.y], source / accumulated);
            }
        }
        }
    }
    const float receiverWeight = g_materialBlendMouth.w > 0.0f ? saturate(p.splineSurface.w - 1.0f) : 0.0f;
    [branch] if (receiverWeight > 0.0f) {
        BlendWaterMaterialProfiles(material, g_materialBlendTarget, receiverWeight);
    }
    return material;
}

#include "Water/WaterSurfaceFoam.hlsli"
#include "Water/WaterRiverFlow.hlsli"
#include "Water/WaterRiverObstacles.hlsli"
#include "Water/WaterRiverAppearance.hlsli"

float4 PSMain(WaterPSInput p) : SV_Target0
{
    bool splineSurface = p.splineSurface.w > 0.5f;
    const float time = g_timeParams.w;
    const float2 baseVelocity = p.splineSurface.yz + p.splineLocalVelocity;
    WaterRiverObstacleFlow obstacleFlow = {baseVelocity, 0.0f, false, 0.0f, float2(0.0f, 0.0f)};
    [branch] if (splineSurface && g_farOceanParams.w > 0.5f && waterWireframeMode <= 0.5f) {
        obstacleFlow = WaterRiverFlowAroundExclusions(p, baseVelocity, time);
        if (obstacleFlow.excluded) discard;
    } else if (WaterSurfaceExcluded(p)) {
        discard;
    }

    const float3 worldDX = ddx(p.worldPos);
    const float3 worldDY = ddy(p.worldPos);
    const float2 baseDX = ddx(p.baseWorldXZ);
    const float2 baseDY = ddy(p.baseWorldXZ);
    const float2 baseHeightGradient = float2(ddx(p.baseWorldY), ddy(p.baseWorldY));
    float2x2 worldToPixel = 0.0f;
    float3 facingNormal = p.normal;
    [branch] if (splineSurface) {
        float determinant;
        worldToPixel = WaterRiverWorldToPixel(baseDX, baseDY, determinant);
        const float3 triangleNormal = cross(worldDY, worldDX) * (determinant < 0.0f ? -1.0f : 1.0f);
        facingNormal = dot(triangleNormal, triangleNormal) > 1.0e-12f ? triangleNormal : p.normal;
        const float2 smallWaveGradient = float2(ddx(p.splineSmallWaveHeight), ddy(p.splineSmallWaveHeight));
        /// @note The rasterized face determines which side the camera sees. Interpolated surface derivatives shade that face.
        if (abs(determinant) > 1.0e-10f) {
            const float2 slope = mul(baseHeightGradient + smallWaveGradient, worldToPixel);
            const float3 tangent = p.tangent + float3(0.0f, slope.x, 0.0f);
            const float3 binormal = p.binormal + float3(0.0f, slope.y, 0.0f);
            p.normal = SafeNormalize(cross(binormal, tangent), p.normal);
        } else {
            p.normal = normalize(facingNormal);
        }
    }
    /// @note ワイヤーは変形後のメッシュを診断する。屈折・SSR・透明度・外周フェードを通すと線が背景へ溶け込むため、不透明な診断色で描く。
    if (waterWireframeMode > 0.5f) {
        const float3 wireNormal = normalize(facingNormal);
        const float diffuse = abs(dot(wireNormal, normalize(-lightDir)));
        const float lighting = waterWireframeMode > 1.5f ? 1.0f
            : max(0.25f, saturate(diffuse * lightIntensity + dot(ambientColor, float3(0.2126f, 0.7152f, 0.0722f))));
        return float4(float3(1.0f, 0.65f, 0.1f) * lighting, 1.0f);
    }
    const WaterMaterialBlendTarget material = ResolveWaterSurfaceAppearance(p);
    float2 screenUV = p.screenPos.xy / p.screenPos.w * float2(0.5f, -0.5f) + 0.5f;

    /// @note 1 ピクセルが覆うワールド距離。さざ波の LOD・タイルの異方フィルタ・specular AA が
    /// @note すべてこれを基準にする。
    float2 footprintDX = worldDX.xz;
    float2 footprintDY = worldDY.xz;
    float  footprint   = max(length(footprintDX), length(footprintDY));

    /// @note 場の評価は頂点で行う。補間した向きは渦の中心でゼロになり得るため安全に正規化する。
    WaterFlowSample flowSample;
    float flowLength = length(p.flowDirectionDetail.xy);
    flowSample.direction = flowLength > 1.0e-4f ? p.flowDirectionDetail.xy / flowLength : g_flowParams.xy;
    flowSample.detailGain = p.flowDirectionDetail.z;
    flowSample.chop = p.flowDirectionDetail.w;
    flowSample.eye = p.flowFoam.x;
    flowSample.ring = p.flowFoam.y;
    const float2 currentVelocity = obstacleFlow.velocity;
    const float currentSpeed = length(currentVelocity);
    if (splineSurface)
        flowSample.direction = currentSpeed > 1.0e-4f ? currentVelocity / currentSpeed : g_flowParams.xy;
    float2 flowDir = flowSample.direction;

    float2 splineTerrainDepth = 0.0f;
    if (splineSurface && g_terrainInfoParams.x > 0.5f)
        splineTerrainDepth = SampleWaterTerrainDepth(p.uv);
    /// @note 基準面の水深を使い、視点や波の上下動で急瀬の発生位置が変わらないようにする。
    const float authoredDepth = max(p.splineSurface.x, 0.0f);
    const float splineMeanDepth = lerp(authoredDepth, min(authoredDepth, splineTerrainDepth.y),
        saturate(splineTerrainDepth.x));
    /// @note Use the observed shallows for visual advection only. Authored speed still controls breaking.
    const float bankDrag = saturate(splineTerrainDepth.x) * (1.0f - smoothstep(0.10f, 0.90f, splineMeanDepth));
    const float driftScale = 1.0f - 0.42f * bankDrag;
    const WaterCurrentAdvection current = WaterCurrentAt(currentVelocity * driftScale, time);
    const float foamThreshold = material.foam.x;
    const float foamFade = max(material.foam.y, 0.0001f);
    const WaterRiverAppearanceState river = EvaluateWaterRiverAppearance(p, material, obstacleFlow, flowDir,
        currentSpeed, driftScale, baseDX, baseDY, footprint, worldToPixel, splineTerrainDepth, splineMeanDepth);

    /// @note Wind-driven surface ripples are independent of swell and the advecting current.
    /// @see https://github.com/Unity-Technologies/Graphics/blob/master/Packages/com.unity.render-pipelines.high-definition/Documentation~/water-water-system-simulation.md Unity HDRP, Wind and wave types.
    const bool overrideRipples = dot(g_waveControls.yz, g_waveControls.yz) > 0.5f;
    const bool followCurrent = splineSurface && !overrideRipples;
    const float2 rippleDirection = overrideRipples ? g_waveControls.yz : flowDir;
    const float2 rippleTravel = followCurrent ? 0.0f : rippleDirection * (material.detail.y * time);
    const float2 rippleXZ = followCurrent ? p.baseWorldXZ : p.worldPos.xz;
    const float2 rippleDx = followCurrent ? baseDX : footprintDX;
    const float2 rippleDy = followCurrent ? baseDY : footprintDY;
    float4 normalSample  = SampleWaterNormal(rippleXZ, p.uv, rippleDirection, rippleTravel, time, footprint,
                                             rippleDx, rippleDy, p.normal,
                                              flowSample.detailGain * (1.0f + 0.75f * river.rapids),
                                              p.waveMeshFade, p.baseWorldXZ, baseDX, baseDY, splineSurface,
                                              material.normalReflectionRefraction.x, material.detail.z, followCurrent,
                                              current, river.crestDetail);
    float3 tangentNormal = normalSample.xyz;
    const float normalStrength = material.normalReflectionRefraction.x;
    if (followCurrent || g_riverWaveParams.x > 0.0f) {
        const float2 slope = river.waveOptics.xy + (followCurrent ? river.flowSlope : 0.0f);
        tangentNormal = normalize(tangentNormal + float3(-slope * normalStrength, 0.0f));
    }
    float lostSlopeVariance = normalSample.w + river.waveOptics.z * normalStrength * normalStrength;

    float3x3 tbn;
    if (splineSurface) {
        float3 tangent = float3(1.0f, 0.0f, 0.0f) - p.normal * p.normal.x;
        if (dot(tangent, tangent) <= 1.0e-8f)
            tangent = float3(0.0f, 0.0f, 1.0f) - p.normal * p.normal.z;
        tangent = normalize(tangent);
        tbn = float3x3(tangent, normalize(cross(tangent, p.normal)), normalize(p.normal));
    } else {
        tbn = float3x3(normalize(p.tangent), normalize(p.binormal), normalize(p.normal));
    }
    float3 N = normalize(mul(tangentNormal, tbn));
    float3 V = normalize(cameraPos - p.worldPos);
    bool useCoefficientOptics = material.scatteringMode.w > 0.5f;
    /// @note 平行投影の視線は画素位置によらない。物理光路に透視の eye-ray を使わない。
    if (isOrthographic > 0.5f && (useCoefficientOptics || splineSurface))
        V = -normalize(float3(view[0][2], view[1][2], view[2][2]));
    /// @note 水面下から見上げているときは面の裏側を見ている (PSO は両面描画)。
    /// @warning 向きの判定に N を使うと、浅い角度のピクセルだけがばらばらに反転して斑になる。
    /// @note 決定は滑らかな幾何法線に任せる。
    bool cameraUnderwater = dot(facingNormal, V) < 0.0f;
    const bool oceanSurface = !splineSurface && g_farOceanParams.x > 0.5f;
    const float horizonWeight = oceanSurface ? 1.0f
        : (g_materialBlendMouth.w > 0.0f ? saturate(p.splineSurface.w - 1.0f) : 0.0f);
    const bool horizonSurface = useCoefficientOptics && horizonWeight > 0.0f;
    if (horizonSurface && (oceanSurface || horizonWeight > 0.5f)) {
        /// @note The signed XZ area of the rasterized triangle distinguishes the top from the underside even when interpolated swell normals face away.
        const float signedArea = footprintDX.y * footprintDY.x - footprintDX.x * footprintDY.y;
        if (abs(signedArea) > 1.0e-10f) cameraUnderwater = signedArea < 0.0f;
        cameraUnderwater = WaterHorizonCameraUnderwater(p.baseWorldY, cameraUnderwater);
    }
    bool horizonOptics = horizonSurface && !cameraUnderwater;
    if (cameraUnderwater) N = -N;
    float3 swellNormal = tbn[2];
    const float3 viewDirectionDX = ddx(V);
    const float3 viewDirectionDY = ddy(V);
    [branch]
    if (horizonOptics) {
        float reflectionFootprint = footprint;
        float2 distantDX = baseDX;
        float2 distantDY = baseDY;
        const float meanSurfaceHeight = cameraPos.y - p.baseWorldY;
        /// @note A grazing ray can hit a nearby crest above the mean plane. Keep its footprint local until the two intersections agree.
        const float surfaceDrop = cameraPos.y - p.worldPos.y;
        const float planeWeight = smoothstep(0.3f, 0.8f, surfaceDrop / max(meanSurfaceHeight, 0.25f));
        if (isOrthographic < 0.5f && meanSurfaceHeight > 0.25f && V.y > 0.001f && planeWeight > 0.0f) {
            /// @note Differentiate the eye ray's intersection with the mean plane so a crest cannot change its own reflection LOD.
            const float2 planeDX = viewDirectionDX.xz * V.y - V.xz * viewDirectionDX.y;
            const float2 planeDY = viewDirectionDY.xz * V.y - V.xz * viewDirectionDY.y;
            const float planeScale = -meanSurfaceHeight / (V.y * V.y);
            distantDX = planeScale * planeDX;
            distantDY = planeScale * planeDY;
            const float planeFootprint = max(length(distantDX), length(distantDY));
            distantDX = lerp(baseDX, distantDX, planeWeight);
            distantDY = lerp(baseDY, distantDY, planeWeight);
            reflectionFootprint = lerp(footprint, max(planeFootprint, 0.5f * footprint), planeWeight);
        }
        const float footprintInDetailCells = reflectionFootprint * max(material.detail.x, 1.0e-4f);
        const float distantWeight = smoothstep(0.22f, 0.95f, footprintInDetailCells);
        if (distantWeight > 0.0f) {
            /// @note Resolve visible long waves analytically before transferring subpixel slopes to BRDF roughness.
            /// @see https://doi.org/10.1111/j.1467-8659.2009.01618.x Bruneton, Neyret and Holzschuch (2010), geometry-to-BRDF slope filtering.
            const float4 noMesh[4] = {(float4)0.0f, (float4)0.0f, (float4)0.0f, (float4)0.0f};
            float distantVariance;
            const float waveWeight = WaterfallWaveWeight(p.baseWorldXZ);
            const float2 slope = WaterResidualWaveSlope(p.baseWorldXZ, time, distantDX, distantDY, noMesh, distantVariance) * waveWeight;
            distantVariance *= waveWeight * waveWeight;
            const float3 distantNormal = normalize(float3(-slope.x, 1.0f, -slope.y));
            const float normalWeight = distantWeight * horizonWeight;
            N = normalize(lerp(N, distantNormal, normalWeight));
            swellNormal = normalize(lerp(swellNormal, distantNormal, normalWeight));
            lostSlopeVariance = lerp(lostSlopeVariance, max(lostSlopeVariance, distantVariance), normalWeight);
        }
    }
    const float incidentCosine = dot(N, V);
    const float2 incidentGradient = float2(ddx(incidentCosine), ddy(incidentCosine));
    const bool splineOptics = splineSurface && !cameraUnderwater;
    /// @note Keep the optical normal on the incident hemisphere at grazing silhouettes without flipping an entire wave face.
    if (horizonSurface || splineOptics) N = normalize(N + V * max(0.0f, 0.001f - incidentCosine));
    float NdotV = saturate(dot(N, V));

    /// @note F0 は垂直入射の反射率。反射率を倍増すると浅い角度で飽和し、水中の散乱光が消える。
    /// @see https://doi.org/10.1111/1467-8659.1330233 Schlick, "An Inexpensive BRDF Model for Physically-based Rendering" (1994)
    float f0 = saturate(material.surface.z);
    const float localFresnel = saturate((f0 + (1.0f - f0)
        * pow(1.0f - NdotV, max(material.surface.w, 1.0f))) * material.surface.y);
    float fresnel = localFresnel;
    float3 refractedViewRay = float3(0.0f, 0.0f, 0.0f);
    const bool dielectricVolume = horizonOptics || (useCoefficientOptics && cameraUnderwater);
    [branch] if (splineOptics || dielectricVolume) {
        const float eta = cameraUnderwater ? WATER_REFRACTIVE_INDEX : 1.0f / WATER_REFRACTIVE_INDEX;
        const WaterDielectricInterface interfaceSample = WaterEvaluateDielectricInterface(-V, N, eta);
        refractedViewRay = interfaceSample.direction;
        const float normalAmplitude = (1.0f - eta) / (1.0f + eta);
        const float waterF0 = normalAmplitude * normalAmplitude;
        if (splineOptics) {
            /// @note Reapply the authored normal reflectance and multiplier to the dielectric angular response.
            const float grazing = saturate((interfaceSample.reflectance - waterF0) / (1.0f - waterF0));
            const float angularShape = pow(grazing, 0.2f * max(material.surface.w, 1.0f));
            fresnel = saturate(lerp(f0, 1.0f, angularShape) * material.surface.y);
        }
        if (dielectricVolume) {
            const float dielectricFresnel = dot(refractedViewRay, refractedViewRay) <= 1.0e-8f
                ? 1.0f : saturate(interfaceSample.reflectance * material.surface.y);
            fresnel = lerp(fresnel, dielectricFresnel, cameraUnderwater ? 1.0f : horizonWeight);
        }
        if (horizonSurface) f0 = lerp(f0, waterF0, horizonWeight);
    }

    float rawSceneDepth = horizonOptics ? g_sceneDepth.SampleLevel(g_waterPointSampler, screenUV, 0.0f).r
        : g_sceneDepth.Sample(g_samplerClamp, screenUV).r;
    /// @note 深度が far plane に張り付く場所は Terrain / Mesh が無い背景ピクセルとして扱う。
    /// @note 背景の skydome 色を屈折色として読むと、水面が空そのものに溶けてしまうため、
    /// @note «底が見えない深い水» として描く。
    /// @note 深度は Reversed-Z なので背景 (クリア値のまま・空) は 0。
    float backgroundMask = rawSceneDepth <= 0.0f ? 1.0f : 0.0f;
    float linearSceneDepth = LinearizeDepth(rawSceneDepth);
    /// @note クリップ w は平行投影では 1。両投影とも同じ view Z で水層の距離を求める。
    float3 surfaceVS = mul(float4(p.worldPos, 1.0f), view).xyz;
    float linearSurfDepth = max(surfaceVS.z, 0.0001f);
    float sceneThickness = max(0.0f, linearSceneDepth - linearSurfDepth);
    float waterDepth = lerp(sceneThickness, material.deepColorDepth.w, backgroundMask);
    float viewRayZ = abs(mul(float4(-V, 0.0f), view).z);
    float opticalPathLength = isOrthographic > 0.5f ? waterDepth : waterDepth / max(viewRayZ, 0.05f);
    const float screenColumn = sceneThickness * abs(V.y) / max(viewRayZ, 0.05f);
    float2 splineColumnDepths = 0.0f;
    float splinePath = 0.0f;
    float3 splineMediumRay = float3(0.0f, -1.0f, 0.0f);
    if (splineSurface && horizonWeight < 1.0f) {
        /// @note 波の上下で基準面から水底までの位置を動かさない。水柱ゼロの明示設定も保つ。
        const float columnDepth = authoredDepth > 0.0f
            ? max(0.0f, splineMeanDepth + p.worldPos.y - p.baseWorldY) : 0.0f;
        splineColumnDepths = float2(backgroundMask > 0.5f ? columnDepth : min(columnDepth, screenColumn), columnDepth);
        splineMediumRay = WaterSplineMeanRefraction(V);
        splinePath = splineColumnDepths.x / -splineMediumRay.y;
        opticalPathLength = lerp(splinePath, opticalPathLength, horizonWeight);
        waterDepth = lerp(splinePath, waterDepth, horizonWeight);
    }

    /// @note スクリーンスペース屈折は水面法線とスネル方向で HDR カラー参照 UV をずらす。水底ジオメトリを
    /// @note 再描画せず透明水面らしい歪みを得る。現在描画中の HDR RT を直接読むと read/write
    /// @note 競合になるため、Water 直前にコピーした sceneColor を参照する。
    /// @note sceneColor はシーン側で照明・影が済んだ色なので、ここでは減衰だけを掛ける。
    float3 refractColor = 0.0f;
    float3 horizonRefractColor = 0.0f;
    WaterHorizonRefraction horizonHit = {screenUV, 0.0f, 0.0f, 0.0f};
    if (horizonOptics) {
        [branch]
        if (backgroundMask < 0.5f && fresnel < 0.995f && material.normalReflectionRefraction.z > 0.0f) {
            /// @note Reuse the material control as a screen-space search budget for the Snell ray.
            horizonHit = TraceWaterHorizonRefraction(screenUV, surfaceVS, refractedViewRay, linearSceneDepth,
                min(4.0f * material.normalReflectionRefraction.z, 0.25f));
        }
        [branch]
        if (horizonHit.weight >= 0.999f)
            horizonRefractColor = SampleWaterHorizonRefraction(horizonHit, linearSurfDepth, sceneThickness);
        else
            horizonRefractColor = g_sceneColor.SampleLevel(g_waterPointSampler, screenUV, 0.0f).rgb;
    }
    if (!horizonOptics || horizonWeight < 1.0f) {
        float2 refrUV;
        [branch] if (splineOptics) {
            const float strength = material.normalReflectionRefraction.z * (1.0f - fresnel) * (1.0f - backgroundMask);
            const WaterSplineRefraction hit = WaterTraceSplineRefraction(screenUV, surfaceVS, V, swellNormal, N,
                splineColumnDepths, strength, sceneThickness);
            refrUV = hit.uv;
            if (hit.weight > 0.0f) {
                const float pathCorrection = (hit.pathLength - splinePath) * hit.weight * (1.0f - horizonWeight);
                opticalPathLength += pathCorrection;
                waterDepth += pathCorrection;
                splineMediumRay = SafeNormalize(lerp(splineMediumRay, hit.direction, hit.weight), splineMediumRay);
            }
        } else {
            /// @note 水面を通る光は面の傾きと反対側へ曲がる。画面 UV の上下反転もカメラ空間で合わせる。
            /// @see https://www.graphics.cornell.edu/~bjw/microfacetbsdf.pdf Walter et al. (2007) §4.2 Ideal Refraction
            const float3 viewNormal = mul(float4(N, 0.0f), view).xyz;
            const float shallowFactor = smoothstep(0.0f, 1.0f,
                saturate(waterDepth / max(material.shallowColorDepth.w, 0.0001f)));
            const float refractionMask = shallowFactor * (1.0f - backgroundMask);
            const float strength = material.normalReflectionRefraction.z * (1.0f - fresnel);
            float2 refrOffset = float2(-viewNormal.x, viewNormal.y) * strength * refractionMask;
            if (useCoefficientOptics && cameraUnderwater && fresnel < 1.0f) {
                const float3 incidentViewRay = mul(float4(-V, 0.0f), view).xyz;
                const float3 transmittedViewRay = mul(float4(refractedViewRay, 0.0f), view).xyz;
                const float2 incidentSlope = incidentViewRay.xy / max(abs(incidentViewRay.z), 0.05f);
                const float2 transmittedSlope = transmittedViewRay.xy / max(abs(transmittedViewRay.z), 0.05f);
                const float2 slopeDelta = transmittedSlope - incidentSlope;
                refrOffset = 0.5f * float2(slopeDelta.x * projection[0][0], -slopeDelta.y * projection[1][1]) * strength;
            }
            refrUV = saturate(screenUV + refrOffset);
            /// @note 屈折先が水面より手前なら、前景のシルエットを水中に混ぜない。
            const float refrRawDepth = g_sceneDepth.SampleLevel(g_samplerClamp, refrUV, 0.0f).r;
            if (LinearizeDepth(refrRawDepth) < linearSurfDepth) refrUV = screenUV;
        }
        refractColor = g_sceneColor.SampleLevel(g_samplerClamp, refrUV, 0.0f).rgb;
    }

    const float deepDepth = max(material.deepColorDepth.w, 1.0e-4f);
    const float opticalDepth = waterDepth / deepDepth;
    const float depthFactor = smoothstep(0.0f, 1.0f, saturate(opticalDepth));
    /// @note Gerstner crests temper the legacy scattering tint while the accepted ray controls extinction.
    const float colorDepthFactor = saturate(depthFactor * (1.0f - p.waveState.x * 0.40f));
    const float3 scatterAlbedo = lerp(material.shallowColorDepth.xyz, material.deepColorDepth.xyz, colorDepthFactor);
    /// @note 空反射は空連動 IBL の事前フィルタ済みキューブから引く (専用の環境テクスチャは不要)。
    /// @note 水面上からの反射だけを上半球へ保つ。水中から見た係数光学では下半球の反射を使う。
    /// @note 水平線際の上面反射はさざ波による下半球サンプルで黒帯にならないよう保守的に制限する。
    float3 R = reflect(-V, N);
    if (!(useCoefficientOptics && cameraUnderwater))
        R = normalize(float3(R.x, max(R.y, 0.02f), R.z));
    /// @note 消失割合でなく強度込みの傾き分散を GGX の alpha^2 へ足す。弱いさざ波まで遠景で一律に粗さ 0.7 へしない。
    /// @note 可視法線の画面内分散も含め、残した長いうねりのハイライトが subpixel へ縮む場合の明滅を抑える。
    /// @see https://www.jp.square-enix.com/tech/library/pdf/ImprovedGeometricSpecularAA.pdf Tokuyoshi & Kaplanyan (2019), filtered NDF roughness
    float  roughness = saturate(1.0f - material.detail.w);
    float3 normalDX = ddx(N);
    float3 normalDY = ddy(N);
    float normalVariance = 0.15f * (dot(normalDX, normalDX) + dot(normalDY, normalDY));
    const float currentAnisoWeight = followCurrent
        ? river.streakVisibility * saturate(material.detail.z * material.normalReflectionRefraction.x) : 0.0f;
    const float2 visibleStandingFoam = cameraUnderwater ? 0.0f : river.standingFoam;
    const float standingCoverage = max(visibleStandingFoam.x, visibleStandingFoam.y);
    /// @note Thick foam softens the sharp lobe. The shoulders of the same streak remain exposed through the thinner film.
    const float exposedWater = 1.0f - 0.72f * smoothstep(0.025f, 0.49f, standingCoverage);
    /// @note Sharper reflection travels with the same visible packet as the wave slope and keeps the existing AA variance.
    const float standingFocus = river.streakVisibility * dot(river.standingReflection.xyz, float3(0.42f, 0.72f, 0.33f));
    const float reflectionPacket = cameraUnderwater ? 0.0f
        : saturate((currentAnisoWeight + standingFocus) * river.standingStreak.x) * exposedWater;
    const float reflectionSpread = cameraUnderwater ? 0.0f
        : currentAnisoWeight * max(river.standingReflection.z, 0.54f * river.standingTrail.x) * (0.30f + 0.70f * river.standingStreak.y);
    const float microfacetAlpha = roughness * roughness * (1.0f - 0.32f * reflectionPacket) * (1.0f + 0.55f * reflectionSpread);
    float roughnessAA = sqrt(sqrt(saturate(microfacetAlpha * microfacetAlpha + min(2.0f * (lostSlopeVariance + normalVariance), 0.25f))));
    /// @note 寝た視線では粗さを引き戻して環境キューブを鮮明な mip から引く。粗い mip は上下 90 度ぶんを
    /// @note 平均した色で、水平線際では空に地面が混ざって沈む。実際の水面は入射が浅いほど反射ローブが
    /// @note 細くなるのでこちらが正しい。ハイライト側は 1 ピクセルのちらつきを避けるため据え置く。
    float  horizonSharpen = lerp(0.35f, 1.0f, NdotV);
    const float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);
    /// @note IBL が無いときの空反射。.mat の空色を空の明るさ (skyDimmer) で落とす。Unlit 表示では
    /// @note skyDimmer が 0 になるので ambientColor (白) を下限にする。
    float3 reflectColor = material.skyTint.xyz * max(skyDimmer, dot(ambientColor, kLuma));
    /// @note 水の層・泡が空から受ける放射照度 / π (Lambert の反射率に掛ければそのまま放射輝度)。
    float3 skyIrradianceOverPi = ambientColor;
    [branch]
    if (material.normalReflectionRefraction.y > 0.001f && iblIntensity > 0.0f)
    {
        /// @note 通常マテリアルと同じく IBL の全体スケールと鏡面 / 拡散スケールを通す。
        float  iblSpecular = iblIntensity * max(iblSpecularScale, 0.0f);
        float3 sky = g_skyReflection.SampleLevel(g_samplerClamp, R,
                                                 roughnessAA * horizonSharpen
                                                     * (float)max(iblMaxMipLevel, 0)).rgb;
        /// @note 一番粗いミップを真上から引くと «上半球を余弦で平均した放射輝度» に近く、π 倍が
        /// @note 水平面の放射照度になる。LightConstants の ambientColor は時刻に追随しない。
        float3 skyAverage = g_skyReflection.SampleLevel(g_samplerClamp, float3(0.0f, 1.0f, 0.0f),
                                                        (float)max(iblMaxMipLevel, 0)).rgb;
        /// @note 空色は «色相» だけを .mat から取り、明るさは空から取る。定数のまま混ぜると夜も光る。
        float3 tintLit = material.skyTint.xyz * (dot(skyAverage, kLuma)
                                           / max(dot(material.skyTint.xyz, kLuma), 1.0e-3f));
        reflectColor = lerp(tintLit, sky, saturate(material.normalReflectionRefraction.y)) * iblSpecular;
        skyIrradianceOverPi = skyAverage * (iblIntensity * max(iblDiffuseScale, 0.0f));
    }

    /// @note 反射ウェイト (フレネル) を先に求め、SSR は寄与が実際に見えるピクセルだけトレースする。
    /// @note TraceWaterSSR は WaterForward の主コストで、水面を見下ろす (低フレネル) ピクセルは
    /// @note レイマーチを丸ごと省いても結果はほぼ不変。背景ピクセルは画面内にヒット候補が無く
    /// @note 長い空走査になるため環境反射へフォールバックする。
    /// @warning 背景ピクセルで反射を減らさないこと。そこはフレネルが 1 に張り付き本来は鏡になる
    /// @note 水平線際で、以前の 0.45 倍で遠い水面が黒く沈んでいた。
    float reflectionWeight = saturate(fresnel);
    if (horizonOptics || splineOptics) {
        /// @note Only footprints spanning a rapidly changing grazing angle need averaged reflection.
        const float angleWidth = abs(incidentGradient.x) + abs(incidentGradient.y);
        const float grazingFilter = smoothstep(0.025f, 0.11f, angleWidth)
            * (1.0f - smoothstep(0.06f, 0.23f, NdotV));
        [branch]
        if (grazingFilter > 0.0f) {
            /// @note Add the screen-pixel box variance to the unresolved slope variance projected onto the viewing direction.
            const float variance = (1.0f - NdotV * NdotV) * lostSlopeVariance
                + dot(incidentGradient, incidentGradient) / 12.0f;
            const float sigma = min(sqrt(max(variance, 0.0f)), 0.5f);
            /// @see https://morpho.inrialpes.fr/Publications/2010/BNH10/article.pdf Bruneton et al. (2010), §5.2 Eq. 26, average sky Fresnel.
            const float excessWeight = exp(-2.69f * sigma) / (1.0f + 22.7f * sigma * sqrt(sigma));
            const float normalReflection = saturate(f0 * material.surface.y);
            const float meanReflection = lerp(normalReflection, reflectionWeight, excessWeight);
            reflectionWeight = lerp(reflectionWeight, meanReflection, grazingFilter);
        }
    }
    [branch]
    if (reflectionWeight > 0.04f && backgroundMask < 0.5f)
    {
        float4 ssrReflection = TraceWaterSSR(p.worldPos, N);
        /// @note Screen-color SSR is unfiltered; let roughness-AA IBL replace its sharp hit as fine slopes leave the pixel footprint.
        float ssrRoughnessWeight = saturate(1.0f - roughnessAA);
#ifdef FBZZ_WATER_SMOOTH_SPLINE_OPTICS
        /// @note Broad river reflections blend with the sky to soften abrupt hits at the banks.
        if (splineSurface) ssrRoughnessWeight *= 0.32f;
#endif
        reflectColor = lerp(reflectColor, ssrReflection.rgb, ssrReflection.a * ssrRoughnessWeight);
    }

    float3 L = normalize(-lightDir);
    /// @note 影は «太陽が届くか» だけを表す。掛けるのは太陽由来の項 (水の層・泡・きらめき・透過光) だけ。
    /// @warning 合成後の色全体へ掛けないこと。空の反射は影で暗くならず、屈折した水底は
    /// @note シーン側で影が済んでいるので二重に沈む。
    float shadow = ComputeShadow(g_shadowMap, g_shadowSampler, p.worldPos,
        lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    /// @note Forward の画面空間 AO / 接触影。Deferred では b8 が 0 なので素通りする。
    shadow *= FBZZ_ScreenContactShadow(p.svPosition.xy);

    /// @note 太陽の放射照度 / π。通常マテリアルと同じ LIGHT_UNIT_SCALE を通し、intensity=1 で
    /// @note 反射率そのままの明るさになる規約に揃える。
    float3 sunOverPi = lightColor * (max(lightIntensity, 0.0f) * LIGHT_UNIT_SCALE * INV_PI);
    /// @note 水の層へ入る太陽光は水平面で受けるので、さざ波の法線ではなく高度の余弦で決める。
    float  sunCosFlat = saturate(L.y);

    /// @note 局所光。水の層と泡を照らす放射照度 / π と、水面のきらめき (GGX) を分けて集める。
    /// @warning 屈折色へ拡散光を足さないこと。水底はシーン側で局所光も受け済みで、二重に照らされる。
    float3 punctualOverPi   = float3(0.0f, 0.0f, 0.0f);
    float3 punctualSpecular = float3(0.0f, 0.0f, 0.0f);
    float3 punctualSingleScatterSource = float3(0.0f, 0.0f, 0.0f);
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        float3 punctualRadiant = ps.color * (ps.intensity * LIGHT_UNIT_SCALE);
        float  punctualNdotL   = saturate(dot(N, ps.L));
        punctualOverPi   += punctualRadiant * (punctualNdotL * INV_PI);
        punctualSpecular += BRDF_Specular(N, V, ps.L, f0.xxx, saturate(roughnessAA + ps.roughnessBias))
                          * punctualRadiant * punctualNdotL;
        if (useCoefficientOptics && !cameraUnderwater) {
            float localPhase = WaterHenyeyGreensteinPhase(dot(-ps.L, V), material.absorptionPhase.w);
            punctualSingleScatterSource += punctualRadiant * (PI * localPhase);
        }
    FBZZ_PUNCTUAL_END

    float3 body;
    float3 bodyIrradiance = float3(0.0f, 0.0f, 0.0f);
    if (useCoefficientOptics) {
        if (cameraUnderwater)
        {
            /// @note Composite owns the camera-to-interface water path; the surface contributes only the refracted air-side radiance.
            body = refractColor;
        }
        else
        {
            const float phaseG = material.absorptionPhase.w;
            /// @note Approximate the environment as uniform radiance over the visible hemisphere; isotropic integration gives one half.
            const float3 diffuseSource = skyIrradianceOverPi * 0.5f + punctualSingleScatterSource;
            /// @note Rectangle water over cleared depth has no transmitted bottom; authored spline depth remains finite without terrain.
            /// @see https://dev.epicgames.com/documentation/unreal-engine/single-layer-water-shading-model-in-unreal-engine Epic Single Layer Water, homogeneous volume and scene-depth refraction inputs.
            const float bottomTransmission = splineSurface ? 1.0f : (1.0f - backgroundMask);
            const WaterOpticalMedium medium = WaterPrepareOpticalMedium(
                material.absorptionPhase.xyz, material.scatteringMode.xyz);
            WaterTransmittedSun sun = {float3(0.0f, -1.0f, 0.0f), float3(0.0f, 0.0f, 0.0f)};
            [branch] if (splineOptics || horizonOptics)
                sun = WaterTransmitSurfaceSun(L, sunOverPi * (shadow * PI), swellNormal);
            body = 0.0f;
            [branch]
            if (!horizonOptics || horizonWeight < 1.0f) {
                const float3 bottomRadiance = refractColor * bottomTransmission;
                [branch] if (splineOptics) {
                    body = WaterEvaluateColumnRadiance(medium, opticalPathLength, splineMediumRay,
                        bottomRadiance, diffuseSource, sun, phaseG);
                } else {
                    const WaterOpticalTransport opticalTransport = WaterEvaluatePreparedTransport(medium, opticalPathLength);
                    const float directPhase = WaterHenyeyGreensteinPhase(dot(-L, V), phaseG);
                    const float3 directSource = sunOverPi * ((L.y > 0.0f ? 1.0f : 0.0f) * shadow * (PI * directPhase));
                    body = bottomRadiance * opticalTransport.transmittance
                        + opticalTransport.singleScatterWeight * (directSource + diffuseSource);
                }
            }
            [branch]
            if (horizonOptics) {
                const bool bentSample = horizonHit.weight >= 0.999f;
                const bool openWater = backgroundMask > 0.5f;
                if (bentSample) opticalPathLength = horizonHit.pathLength;
                if (openWater) opticalPathLength = WATER_OPTICAL_MAX_DISTANCE;
                /// @note The fallback depth estimates path length. The interface sets the scattering direction in every case.
                /// @see https://www.pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission PBRT v4 §9.3.4, the transmitted direction is set at the interface.
                float3 horizonBody = WaterEvaluateColumnRadiance(medium, opticalPathLength, refractedViewRay,
                    horizonRefractColor * (1.0f - backgroundMask), diffuseSource, sun, phaseG);
                /// @note Blend complete radiance samples at uncertain hits so the scene colors keep their own path lengths.
                [branch]
                if (horizonHit.weight > 0.0f && !bentSample) {
                    const float3 bentColor = SampleWaterHorizonRefraction(horizonHit, linearSurfDepth, sceneThickness);
                    const float3 bentRadiance = WaterEvaluateColumnRadiance(medium, horizonHit.pathLength,
                        refractedViewRay, bentColor, diffuseSource, sun, phaseG);
                    horizonBody = lerp(horizonBody, bentRadiance, horizonHit.weight);
                }
                body = lerp(body, horizonBody, horizonWeight);
            }
        }
    } else {
        /// @note 深部色の色相を 1 深部深度あたりの透過率として使う。
        /// @see https://doi.org/10.1364/AO.36.008710 Pope & Fry (1997), visible absorption spectrum of pure water.
        const float deepPeak = max(max(material.deepColorDepth.x, material.deepColorDepth.y),
            max(material.deepColorDepth.z, 1.0e-4f));
        const float3 deepHue = max(material.deepColorDepth.xyz / deepPeak, 0.02f);
        const float3 transmittance = pow(deepHue, opticalDepth) * exp(-opticalDepth) * (1.0f - backgroundMask);
        const float scatterOpacity = 1.0f - exp(-3.0f * opticalDepth);
        const float murk = lerp(saturate(material.surface.x) * lerp(saturate(material.effects.y), 1.0f, scatterOpacity),
            1.0f, backgroundMask);
        /// @note 水の層が返す光 = 散乱色 × (太陽 + 空 + 局所光)。定数色のまま出すと昼夜・影に追随しない。
        bodyIrradiance = sunOverPi * (sunCosFlat * shadow) + skyIrradianceOverPi + punctualOverPi;
        float3 inscatter = scatterAlbedo * bodyIrradiance;
        body = lerp(refractColor * transmittance, inscatter, murk);
    }

    /// @note 太陽のきらめきは GGX で引く。Blinn-Phong は裾が急に落ちるため、海のきらめきの
    /// @note «広がった尾» が出ず、ハイライトが «板に当たった光» に見える。
    /// @note 旗状に伸びる波の筋を横切る軸に粗さを配る。風で進むさざ波では風向がその軸になる。
    const float rippleAniso = saturate((max(g_flowParams.z, 1.0f) - 1.0f) * 0.35f);
    /// @note Long current streaks have more slope variation across the flow. Crossfade through isotropy as the dominant axis changes.
    const float crestAniso = river.standingReflection.y * reflectionPacket;
    const float currentAniso = max(rippleAniso, lerp(0.60f, 0.86f, crestAniso));
    const float signedAniso = lerp(rippleAniso, -currentAniso, currentAnisoWeight * (1.0f - 0.48f * reflectionSpread));
    const float2 specAxis = signedAniso < 0.0f ? float2(-rippleDirection.y, rippleDirection.x) : rippleDirection;
    const float3 specWorld = float3(specAxis.x, 0.0f, specAxis.y);
    float3 specT = specWorld - N * dot(N, specWorld);
    float  specTLen = length(specT);
    const float3 fallbackAxis = abs(N.y) > 0.5f ? float3(1, 0, 0) : float3(0, 1, 0);
    specT = specTLen > 1.0e-4f ? specT / specTLen : normalize(cross(fallbackAxis, N));
    float3 specB = normalize(cross(N, specT));
    const float specAniso = abs(signedAniso);

    /// @note 太陽は点ではなく角半径 0.0047 rad の円盤。α に円盤ぶんを足して峰を広げないと、
    /// @note GGX の頂点が 1 ピクセルへ落ちて Bloom がちらつく (球光源の正規化と同じ扱い)。
    /// @note GGX 分布は投影面積で正規化済み。粗さを広げた後に峰の比を全体へ掛けると裾の光まで失う。
    float sunAlpha = max(roughnessAA * roughnessAA, 1.0e-5f);
    float sunAlphaWide = saturate(sunAlpha + 0.0023f);
    float NdotL = saturate(dot(N, L));
    /// @see https://www.graphics.cornell.edu/~bjw/microfacetbsdf.pdf Walter et al., "Microfacet Models for Refraction through Rough Surfaces" (2007) §5.2 GGX
    const float sunRoughness = sqrt(sunAlphaWide);
    float3 sunSpecular;
    [branch] if (followCurrent)
        sunSpecular = WaterRiverSunSpecular(N, V, L, f0, sunRoughness, specT, specB, specAniso);
    else
        sunSpecular = BRDF_SpecularAdvanced(N, V, L, f0.xxx, sunRoughness, specT, specB, specAniso);
    sunSpecular *= (NdotL * shadow * PI) * sunOverPi;

    if (!useCoefficientOptics) {
        /// @note 波の背面から透ける光 (subsurface)。«光を通す液体» に見えるかはここで決まり、
        /// @note 反射とスペキュラだけだと金属板のような水面になる。
        /// @note 光を法線で «曲げて» から視線と比べる (fast subsurface scattering)。素の dot(V,-L)
        /// @note だと太陽をちょうど背にした一瞬しか光らず、それ以外の角度で波の内側が常に暗い。
        float3 scatterDir = normalize(L + N * 0.30f);
        float forwardScatter = pow(saturate(dot(V, -scatterDir)), 3.0f);
        /// @note うねりの山ほど水の層が薄く、光が通り抜ける。
        float thickness = p.waveState.x * saturate(material.sss.w);
        /// @note 空からの散乱は影を受けない (空の光は影の中にも届く)。これが無いと曇りや夜の海だけが
        /// @note «光を通さない板» に戻る。
        body += material.sss.rgb * thickness
              * (sunOverPi * (forwardScatter * shadow) + skyIrradianceOverPi * 0.5f);

        /// @note 浅い角度では視線が水の層を長く通り、層の色が明るむ。自発光ではなく受けた光で出す。
        float rim = pow(1.0f - NdotV, 3.0f) * material.effects.x;
        body += lerp(scatterAlbedo, material.skyTint.xyz, 0.35f) * bodyIrradiance * rim;
    }

    /// @note 透過した水の層と反射をフレネルで分け合い、表面のきらめきは上に足す。
    float3 color = lerp(body, reflectColor, reflectionWeight)
                 + (sunSpecular + punctualSpecular) * material.effects.z;

    /// @note 波打ち際 (地形と水面が交差する浅瀬) に発生する接岸泡。waterDepth が threshold より
    /// @note 浅いほど強くし、岸辺に沿った白い帯を作る。
    float foamScale = max(material.foam.w, 1.0e-4f);
    float shoreFoam = splineSurface ? WaterSplineShoreCoverage(screenColumn, foamThreshold, foamFade)
        : (1.0f - smoothstep(foamThreshold, foamThreshold + foamFade, waterDepth));
    shoreFoam *= 1.0f - backgroundMask;

    float foamMaskVal = 0.0f;
    float2 swashOffset = 0.0f;
    float swashWeight = 0.0f;
    if (g_terrainInfoParams.x > 0.5f) {
        float2 terrainDepth = splineSurface ? splineTerrainDepth
            : SampleWaterTerrainDepth(WaterTerrainUvAtWorldXZ(p.worldPos.xz));
        float verticalDepth = terrainDepth.y + p.worldPos.y - p.baseWorldY;
        float wet = verticalDepth > 0.0f ? 1.0f : 0.0f;
        float terrainFoam = splineSurface ? WaterSplineShoreCoverage(verticalDepth, foamThreshold, foamFade)
            : (1.0f - smoothstep(foamThreshold, foamThreshold + foamFade, max(verticalDepth, 0.0f))) * wet;
        if (!splineSurface && g_farOceanParams.x > 0.5f) {
            const float contactDepth = horizonOptics
                ? WaterHorizonContactDepth(verticalDepth, V, viewRayZ, sceneThickness, 1.0f - backgroundMask)
                : verticalDepth;
            const float depthFootprint = min(fwidth(contactDepth), foamFade);
            const float2 depthDerivative = float2(ddx(terrainDepth.y), ddy(terrainDepth.y));
            float washPattern = 0.5f;
            [branch]
            if (terrainDepth.x > 0.0f && contactDepth > 0.0f && contactDepth < foamThreshold + foamFade) {
                /// @note The slow layer varies the width of the wash; the existing advected foam layer supplies the fine breakup.
                const float washScale = 0.47f * foamScale;
                const float2 washPosition = p.baseWorldXZ * washScale + float2(11.5f, 23.0f);
                washPattern = WaterNoiseTile(washPosition, baseDX * washScale, baseDY * washScale).z;
                swashWeight = smoothstep(0.5f, 1.0f, terrainDepth.x)
                    * (1.0f - smoothstep(0.55f * foamThreshold, foamThreshold + foamFade, contactDepth));
                swashOffset = WaterHorizonSwashOffset(footprintDX, footprintDY, depthDerivative, p.worldPos.y - p.baseWorldY);
            }
            const float crestRise = max(p.worldPos.y - p.baseWorldY, 0.0f);
            terrainFoam = WaterHorizonShoreFoam(terrainDepth.y, contactDepth, depthFootprint,
                crestRise, p.waveJacobian, max(foamThreshold, 0.0f), foamFade, washPattern, p.shoreWaveHeights);
        }
        shoreFoam = lerp(shoreFoam, terrainFoam, saturate(terrainDepth.x));
    } else {
        foamMaskVal = g_foamMask.Sample(g_samplerClamp, p.uv).r;
    }
    float localDataWeight = WaterLocalDataWeight(p.uv);
    /// @note 岸泡はワールド地形またはシーン深度から得る。旧矩形に固定されるのは局所マスクと波紋だけ。
    foamMaskVal *= localDataWeight;
    /// @note 泡のムラもさざ波タイルから引く。ワールド座標なので水面の大きさに依らず粒が揃う。
    const float2 foamAdvection = splineSurface ? 0.0f : flowDir * (time * 0.35f);
    /// @note The coast follows the changing water level. Open water and incomplete terrain coverage retain the current-driven pattern.
    const float2 swashTextureShift = -(swashOffset * foamScale + 0.8f * foamAdvection) * swashWeight;
    const float2 foamXZ = splineSurface ? p.baseWorldXZ : p.worldPos.xz;
    const float2 foamP = foamXZ * foamScale + foamAdvection + swashTextureShift;
    const float2 foamDx = (splineSurface ? baseDX : footprintDX) * foamScale + ddx(swashTextureShift);
    const float2 foamDy = (splineSurface ? baseDY : footprintDY) * foamScale + ddy(swashTextureShift);
    const float2 firstFoamOffset = current.firstOffset * foamScale;
    const float2 secondFoamOffset = current.secondOffset * foamScale;
    const float2 firstFoamDx = foamDx + ddx(firstFoamOffset);
    const float2 firstFoamDy = foamDy + ddy(firstFoamOffset);
    const float2 secondFoamDx = foamDx + ddx(secondFoamOffset);
    const float2 secondFoamDy = foamDy + ddy(secondFoamOffset);
    float foamNoise;
    if (splineSurface && current.directionWeight > 0.0f) {
        const float first = WaterNoiseTile(foamP + firstFoamOffset, firstFoamDx, firstFoamDy).z;
        const float second = WaterNoiseTile(foamP + secondFoamOffset, secondFoamDx, secondFoamDy).z;
        foamNoise = lerp(second, first, current.firstWeight);
    } else {
        foamNoise = WaterNoiseTile(foamP, foamDx, foamDy).z;
    }
    /// @note 乱流 (Curl) はムラのコントラストを上げる。«ざわついた水» は泡の粒が揃わない。
    const float foamContrast = lerp(1.6f, 3.0f, flowSample.chop);
    float foamTexVal = saturate(foamNoise * foamContrast);
    /// @note Break up the shore band with advected noise independently of vortex and rapids foam.
    if (splineSurface) shoreFoam *= smoothstep(0.38f, 0.68f, foamNoise) * river.shoreFoamBreakup;
    const float crestStrength = max(material.crestFoam.z, 0.0f);
    const float crestSignal = saturate(horizonOptics ? p.waveState.w : p.waveState.y);
    const float crestFoam = saturate(crestSignal * crestStrength);
    /// @note 渦と排水口の目は実際に白く泡立つ。足さないと «ただ凹んだ穴» にしか見えない。
    /// @note 湧き出しは縁で水がぶつかるので、輪として出す。
    float eyeFoam = max(flowSample.eye * 0.9f, flowSample.ring * 0.7f);
    float localFoamAmount = max(max(foamMaskVal, shoreFoam), eyeFoam) * foamTexVal;
    /// @note Advected noise limits the foam to broken patches outside the stationary shore band.
    const float standingInfluence = cameraUnderwater ? 0.0f : saturate(max(river.standingReflection.y, river.standingTrail.x));
    const float rapidFoam = 0.85f * river.rapids * smoothstep(0.52f, 0.82f, foamNoise) * (1.0f - 0.70f * standingInfluence);
    localFoamAmount = max(localFoamAmount, max(rapidFoam, river.carriedFoam));
    [branch] if (splineSurface && !cameraUnderwater && material.foam.z > 0.0f) {
        const float worldWake = 0.86f * obstacleFlow.wake * river.foamWetness * river.foamProximity
            * lerp(0.32f, 1.0f, smoothstep(0.34f, 0.70f, foamNoise));
        localFoamAmount = max(localFoamAmount, worldWake);
        const float wakeVisibility = smoothstep(0.35f, 1.6f, currentSpeed)
            * river.foamProximity * min(river.foamWetness, smoothstep(0.045f, 0.18f, screenColumn)) * (1.0f - backgroundMask);
        [branch] if (wakeVisibility > 0.0f) {
            const float2 surfaceSlope = mul(baseHeightGradient, worldToPixel);
            const float3 surfacePoint = float3(p.baseWorldXZ.x, p.baseWorldY, p.baseWorldXZ.y);
            const float wake = WaterRiverObstacleFoam(surfacePoint, surfaceSlope, flowDir, currentSpeed, foamNoise);
            localFoamAmount = max(localFoamAmount, wake * wakeVisibility);
        }
    }
    float localFoam = saturate(smoothstep(0.05f, 1.0f, localFoamAmount) * max(material.foam.z, 0.0f));
    float crestCoverage = smoothstep(0.05f, 1.0f, crestFoam * foamTexVal);
    float freshRiverFoam = 0.0f;
    float residualRiverFoam = 0.0f;
    [branch] if (splineSurface && !cameraUnderwater && any(river.breakingFoam.xy > 0.0f)) {
        /// @note Reuse the advected streaks to open gaps across old foam and expose the water's reflection.
        /// @see https://agupubs.onlinelibrary.wiley.com/doi/10.1029/2012JC008147 Callaghan et al. (2012), stages A and B and deformation of residual foam.
        const float age = river.breakingFoam.z;
        const float grain = smoothstep(0.30f, 0.72f, foamNoise);
        const float fresh = smoothstep(0.018f, 0.54f, river.breakingFoam.x) * lerp(0.87f, 1.0f, grain);
        const float filaments = smoothstep(0.34f, 0.96f, river.streakPacket) * lerp(0.18f, 1.0f, grain);
        const float breakup = lerp(lerp(0.62f, 1.0f, grain), 0.78f * filaments, age);
        const float residual = smoothstep(0.012f, 0.62f, river.breakingFoam.y) * breakup;
        freshRiverFoam = saturate(crestStrength * fresh);
        residualRiverFoam = saturate(crestStrength * residual);
    }
    const float grainScale = foamScale * 2.37f;
    [branch]
    if (horizonOptics && crestFoam > 0.0f) {
        /// @note Compression lights the advancing lip. The older advected foam breaks into weaker patches.
        /// @see https://liris.cnrs.fr/Documents/Liris-5812.pdf Dupuy and Bruneton (2012), deformation-driven whitecap coverage.
        const float compression = smoothstep(0.08f, 0.37f, 1.0f - p.waveJacobian);
        const float crestHeight = smoothstep(0.16f, 0.54f, p.waveState.x);
        const float frontSlope = smoothstep(-0.04f, 0.25f, dot(p.normal.xz, flowDir));
        /// @note Fade the independent lip with the interpolated crest signal to avoid a jump along triangle contours.
        const float support = smoothstep(0.0f, 0.20f, crestSignal);
        const float breaker = compression * crestHeight * lerp(0.55f, 1.0f, frontSlope)
            * saturate(material.crestFoam.z) * support;
        /// @note Match the particle coordinate used to evaluate the crest history.
        const float2 grainPosition = p.baseWorldXZ * grainScale + float2(17.6f, 8.3f);
        const float grain = WaterNoiseTile(grainPosition, baseDX * grainScale, baseDY * grainScale).z;
        const float lip = breaker * lerp(0.27f, 1.0f, smoothstep(0.33f, 0.70f, grain));
        const float fresh = smoothstep(0.05f, 1.0f, crestFoam * saturate(grain * foamContrast));
        crestCoverage = max(lip, fresh * lerp(0.48f, 0.86f, grain));
    }
    const float trailFoam = saturate(p.waveState.z * crestStrength);
    [branch]
    if (horizonOptics && trailFoam > 0.0f) {
        /// @note Stretch the remaining film in the base current's frame. Its coverage already follows the local flow.
        const float2 driftAxis = g_flowParams.xy;
        const float2 crossAxis = float2(-driftAxis.y, driftAxis.x);
        const float2x2 stretch = float2x2(0.48f * driftAxis, crossAxis);
        const float driftSpeed = clamp(g_waveDepthParams.w + g_refractionFlowParams.y, -4.0f, 4.0f);
        const float2 trailP = mul(stretch, p.baseWorldXZ - driftAxis * (driftSpeed * time)) * grainScale;
        const float grain = WaterNoiseTile(trailP + float2(7.4f, 15.1f),
            mul(stretch, baseDX) * grainScale, mul(stretch, baseDY) * grainScale).z;
        const float trail = 0.62f * trailFoam * smoothstep(0.18f, 0.78f, grain);
        crestCoverage += (1.0f - crestCoverage) * trail;
    }
    const float baseDenseFoam = saturate(max(max(localFoam, crestCoverage), freshRiverFoam));
    const float standingFilm = visibleStandingFoam.y;
    const float denseFoam = max(baseDenseFoam, visibleStandingFoam.x);
    const float residualFoam = max(residualRiverFoam, standingFilm);
    const float foam = max(denseFoam, residualFoam);
    /// @note 泡と波紋の輪は «水面に浮いた白い粗面» なので、定数色ではなく Lambert で照らす。
    /// @note 太陽は泡の面の向きで受け、影の中では空と局所光だけが残る。
    float3 surfaceIrradiance = sunOverPi * (NdotL * shadow) + skyIrradianceOverPi + punctualOverPi;
    [branch]
    if (foam > 0.001f) {
        const float standingWeight = saturate((standingCoverage - max(baseDenseFoam, residualRiverFoam)) / foam);
        const float standingGrain = lerp(river.standingStreak.z, river.standingStreak.w, river.standingTrail.y);
        const float grain = lerp(saturate(foamTexVal), standingGrain, standingWeight);
        float3 foamAlbedo = saturate(material.foamColorRoughness.rgb) * lerp(0.85f, 1.0f, grain);
        float foamRoughness = clamp(material.foamColorRoughness.w, 0.05f, 1.0f);
        [branch] if (residualFoam > denseFoam) {
            const float age = (river.breakingFoam.z * residualRiverFoam + river.standingTrail.y * standingFilm)
                / max(residualRiverFoam + standingFilm, 1.0e-5f);
            const float thinFoam = age * (residualFoam - denseFoam) / foam;
            foamAlbedo *= lerp(1.0f, 0.73f, thinFoam);
            foamRoughness = lerp(foamRoughness, min(foamRoughness, max(roughnessAA, 0.17f)), 0.80f * thinFoam);
        }
        float foamFresnel = 0.02f + 0.98f * pow(1.0f - NdotV, 5.0f);
        /// @note 泡は粗い誘電体として光を受ける。白色を直接加算せず、空と局所光の陰影も保持する。
        /// @see https://www.graphics.cornell.edu/~bjw/microfacetbsdf.pdf Walter et al. (2007), GGX dielectric reflection.
        float3 foamColor = foamAlbedo * surfaceIrradiance * (1.0f - foamFresnel);
        if (lightIntensity > 0.0f && NdotL > 0.0f) {
            foamColor += BRDF_Specular(N, V, L, float3(0.02f, 0.02f, 0.02f), foamRoughness)
                       * (NdotL * shadow * PI) * sunOverPi;
        }
        if (material.normalReflectionRefraction.y > 0.001f && iblIntensity > 0.0f) {
            float3 foamSky = g_skyReflection.SampleLevel(g_samplerClamp, R,
                foamRoughness * (float)max(iblMaxMipLevel, 0)).rgb;
            foamColor += foamSky * (foamFresnel * iblIntensity * max(iblSpecularScale, 0.0f));
        }
        color = lerp(color, foamColor, foam);
    }

    float2 rippleRG = DecodeWaterRipple(g_rippleTex.Sample(g_samplerClamp, p.uv).rgb).rg * localDataWeight;
    float rippleRing = saturate(length(rippleRG) * material.effects.w);
    color = lerp(color, material.rippleColor.xyz * surfaceIrradiance, rippleRing);
    /// @warning ここで色を事前圧縮しないこと。水面は HDR バッファへブレンド描画され、露出・ACES
    /// @note トーンマップは Composite パスが一括で行う。事前圧縮するときらめきが Bloom に乗らない。
    color = max(color, 0.0f);

    /// @note 屈折と濁りはこの色の中で合成済み。ブレンドで背後をもう一度混ぜると、水底が二重に
    /// @note 入って反射ときらめきが薄まるので、アルファは外周フェードだけに使う。
    /// @note 縁ではコピー前のシーンと同じ背後へ溶けるので、屈折を合成済みでも継ぎ目は出ない。
    float alpha = splineSurface ? 1.0f
        : WaterEdgeFade(p.uv, g_refractionFlowParams.zw, g_normalParams.xy, footprint);
    [unroll] for (int capIndex = 0; capIndex < 2; ++capIndex) {
        const float4 cap = g_riverHorizonCaps[capIndex];
        [branch] if (cap.z > 0.0f) {
            const float relativeDistance = length(p.baseWorldXZ - cap.xy) / cap.z;
            const float inland = smoothstep(0.4f, 1.0f, relativeDistance);
            const float aboveSea = smoothstep(-0.08f, 0.65f, p.baseWorldY - cap.w);
            alpha *= lerp(aboveSea, 1.0f, inland);
        }
    }
    /// @note 水面は自分自身の重なりを解決するため深度を書く。フェードで見えなくなった縁がそのまま
    /// @note 深度を書くと、その裏の半透明やデカールを «見えない板» が遮る。絵に出ない画素は残さない。
    clip(alpha - 0.003f);
    return float4(color, alpha);
}

#endif
