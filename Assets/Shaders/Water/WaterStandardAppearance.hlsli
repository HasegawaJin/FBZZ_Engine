/// @file    WaterStandardAppearance.hlsli
/// @brief   Default water pixel appearance, reusable by a project without duplicating shared surface geometry.
/// @author  Hasegawa Jin
/// @date    2026-10-21
#ifndef FBZZ_WATER_STANDARD_APPEARANCE_HLSLI
#define FBZZ_WATER_STANDARD_APPEARANCE_HLSLI
/// @note ユーザー定義エフェクトパラメータ。MaterialComponent.paramData にマップされ、
/// @note Script から mc->SetParam<float>("rimGlowStrength", val) で動的に書き換えられる。
/// @warning HLSL cbuffer のパッキング規則に従い 16B 境界を揃えること (48 bytes = float4 x3)。
cbuffer MaterialConstants : register(b2)
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

struct WaterHorizonRefraction
{
    float2 uv;
    float pathLength;
    float weight;
    float viewDepth;
};

/// @brief Refine the refracted ray against the opaque depth layer with at most two depth reads.
/// @note A foreground hit, screen exit or depth discontinuity falls back to the original pixel.
/// @see https://www.pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission PBRT v4 §9.3.4, water IOR and Snell transmission.
/// @see https://www.jcgt.org/published/0003/04/04/ McGuire and Mara (2014), view-space ray tests against screen depth.
WaterHorizonRefraction TraceWaterHorizonRefraction(float2 screenUV, float3 surfaceVS,
    float3 ray, float sceneDepth, float maxShift)
{
    WaterHorizonRefraction hit = {screenUV, 0.0f, 0.0f, 0.0f};
    const float3 rayVS = mul(float4(ray, 0.0f), view).xyz;
    if (ray.y >= -0.001f || rayVS.z <= 0.05f || sceneDepth <= surfaceVS.z + 0.01f || maxShift <= 0.0f)
        return hit;

    const float waterColumn = sceneDepth - surfaceVS.z;
    float candidateDepth = sceneDepth;
    float shift = 0.0f;
    float residual = 0.0f;
    [unroll]
    for (int step = 0; step < 2; ++step) {
        const float path = (candidateDepth - surfaceVS.z) / rayVS.z;
        const float4 clipPosition = mul(float4(surfaceVS + path * rayVS, 1.0f), projection);
        if (clipPosition.w <= 0.0f) return hit;
        const float2 uv = clipPosition.xy / clipPosition.w * float2(0.5f, -0.5f) + 0.5f;
        const float2 offset = abs(uv - screenUV);
        shift = max(offset.x, offset.y);
        if (any(uv <= 0.0f) || any(uv >= 1.0f) || shift >= maxShift) return hit;

        const float raw = g_sceneDepth.SampleLevel(g_waterPointSampler, uv, 0.0f).r;
        if (raw <= 0.0f) return hit;
        const float sampledDepth = LinearizeDepth(raw);
        residual = abs(sampledDepth - candidateDepth);
        if (sampledDepth <= surfaceVS.z + 0.01f || residual > max(0.06f, 0.45f * waterColumn)) return hit;
        hit.uv = uv;
        candidateDepth = sampledDepth;
    }

    const float2 edge = min(hit.uv, 1.0f - hit.uv);
    const float stableDepth = 1.0f - smoothstep(max(0.025f, 0.08f * waterColumn),
        max(0.08f, 0.24f * waterColumn), residual);
    hit.pathLength = (candidateDepth - surfaceVS.z) / rayVS.z;
    hit.weight = stableDepth * smoothstep(0.0f, 0.025f, min(edge.x, edge.y))
        * (1.0f - smoothstep(0.5f * maxShift, maxShift, shift));
    hit.viewDepth = candidateDepth;
    return hit;
}

/// @note The opaque color and depth snapshots share a texel grid. At a depth edge, sample the color of the validated point hit.
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/texture2d-gatherred HLSL GatherRed returns the bilinear footprint.
float3 SampleWaterHorizonRefraction(WaterHorizonRefraction hit, float surfaceDepth, float sceneThickness)
{
    const float4 depth = g_sceneDepth.GatherRed(g_samplerClamp, hit.uv);
    const float nearest = LinearizeDepth(max(max(depth.x, depth.y), max(depth.z, depth.w)));
    const float farthestRaw = min(min(depth.x, depth.y), min(depth.z, depth.w));
    const float farthest = LinearizeDepth(farthestRaw);
    const float tolerance = max(0.06f, 0.24f * sceneThickness);
    if (farthestRaw <= 0.0f || nearest <= surfaceDepth + 0.01f
        || max(hit.viewDepth - nearest, farthest - hit.viewDepth) > tolerance)
        return g_sceneColor.SampleLevel(g_waterPointSampler, hit.uv, 0.0f).rgb;
    return g_sceneColor.SampleLevel(g_samplerClamp, hit.uv, 0.0f).rgb;
}

struct WaterHorizonSun
{
    float3 direction;
    float3 irradiance;
};

/// @param swellNormal Filtered geometric normal without the short-wave normal map.
/// @note Project flux across the same interface on both sides of the dielectric boundary.
/// @see https://www.pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission PBRT v4 §9.3.4–5, Snell transmission and Fresnel reflectance.
WaterHorizonSun WaterTransmitHorizonSun(float3 toSun, float3 irradiance, float3 swellNormal)
{
    WaterHorizonSun sun = {float3(0.0f, -1.0f, 0.0f), float3(0.0f, 0.0f, 0.0f)};
    if (toSun.y <= 0.0f || !any(irradiance > 0.0f)) return sun;

    /// @note Temper the local slope in the mean water column and fade it at sunset where wave occlusion is unresolved.
    const float swellWeight = 0.5f * smoothstep(0.0f, 0.12f, toSun.y);
    const float3 entryNormal = normalize(lerp(float3(0.0f, 1.0f, 0.0f), swellNormal, swellWeight));
    const float incidentCosine = dot(toSun, entryNormal);
    if (incidentCosine <= 0.0f) return sun;

    const WaterDielectricInterface entry = WaterEvaluateDielectricInterface(-toSun, entryNormal,
        1.0f / WATER_REFRACTIVE_INDEX);
    sun.direction = entry.direction;
    sun.irradiance = irradiance * ((1.0f - entry.reflectance) * incidentCosine / -dot(entry.direction, entryNormal));
    return sun;
}

/// @note Depth grows along the viewing ray. Follow the same ray for the scattering phase and the light path.
float3 WaterEvaluateHorizonRadiance(WaterOpticalMedium medium, float pathLength, float3 intoWater,
    float3 bottomRadiance, float3 diffuseSource, WaterHorizonSun sun, float phaseG)
{
    const WaterOpticalTransport viewTransport = WaterEvaluatePreparedTransport(medium, pathLength);
    float3 radiance = bottomRadiance * viewTransport.transmittance + diffuseSource * viewTransport.singleScatterWeight;
    [branch]
    if (any(sun.irradiance > 0.0f)) {
        const float phase = WaterHenyeyGreensteinPhase(dot(sun.direction, -intoWater), phaseG);
        const float lightTravelPerViewMeter = max(-intoWater.y, 0.0f) / -sun.direction.y;
        radiance += sun.irradiance * phase * WaterIntegrateDirectionalScatter(medium, pathLength, lightTravelPerViewMeter);
    }
    return radiance;
}

float WaterHorizonBreakingStrength(float meanDepth, float crestRise)
{
    return smoothstep(0.42f, 0.85f, 2.0f * crestRise / max(meanDepth, 0.05f));
}

/// @brief Move swash foam across the local depth contour with the signed wave height.
/// @note The first-order contour displacement solves depthGradient · offset = -waveHeight.
/// @note Regularize flat terrain and cap the texture motion across unresolved atlas cells.
/// @see https://pubs.usgs.gov/publication/70030520 Stockdon et al. (2006), swash excursion and foreshore slope.
float2 WaterHorizonSwashOffset(float2 worldDX, float2 worldDY, float2 depthDerivative, float waveHeight)
{
    const float determinant = worldDX.x * worldDY.y - worldDX.y * worldDY.x;
    if (abs(determinant) <= max(1.0e-10f, 0.005f * length(worldDX) * length(worldDY))) return 0.0f;

    const float2 gradient = float2(worldDY.y * depthDerivative.x - worldDX.y * depthDerivative.y,
        worldDX.x * depthDerivative.y - worldDY.x * depthDerivative.x) / determinant;
    const float minimumSlopeSquared = 0.0025f;
    const float2 offset = -gradient * (waveHeight / (dot(gradient, gradient) + minimumSlopeSquared));
    return offset * min(1.0f, 2.0f / max(length(offset), 1.0e-5f));
}

/// @brief Appearance coverage for the wet edge, breaking crests and foam on retreating water.
/// @note Depth is world-vertical. The lower ramp and fwidth soften the moving wet edge at pixel scale.
/// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models GPU Gems §1.3.1, shoreline depth and wave behavior.
/// @see https://liris.cnrs.fr/Documents/Liris-5812.pdf Dupuy and Bruneton (2012), wave compression as a foam-generation cue.
/// @see https://doi.org/10.1029/2012JC008147 Callaghan et al. (2012), decay of residual foam after breaking.
float WaterHorizonShoreFoam(float meanDepth, float verticalDepth, float depthFootprint,
    float crestRise, float jacobian, float threshold, float fade, float washPattern, float4 waveHeights)
{
    const float wetRamp = max(depthFootprint, 0.025f);
    const float wet = smoothstep(0.0f, wetRamp, verticalDepth);
    const float breaking = WaterHorizonBreakingStrength(meanDepth, crestRise);
    /// @note The bright rim widens with the incoming crest and thins to a broken line as the water retreats.
    const float washWidth = max(0.35f * threshold, 0.05f) * lerp(0.45f, 1.55f, washPattern)
        * lerp(0.8f, 1.2f, breaking);
    const float washCoverage = lerp(0.16f, 0.68f, washPattern) * lerp(0.92f, 1.0f, breaking);
    const float wash = washCoverage * (1.0f - smoothstep(washWidth, washWidth + max(fade, depthFootprint), verticalDepth));
    const float bandEnd = threshold + max(fade, depthFootprint);
    const float shallowBand = 1.0f - smoothstep(threshold, bandEnd, verticalDepth);
    const float compression = smoothstep(0.02f, 0.25f, 1.0f - jacobian);
    const float currentFoam = max(wash, breaking * shallowBand * lerp(0.65f, 1.0f, compression));
    float trail = 0.0f;
    [branch]
    if (verticalDepth > 0.0f && verticalDepth < bandEnd && g_crestFoamParams.w > 0.001f) {
        [unroll]
        for (int tap = 0; tap < WATER_FOAM_TRAIL_TAPS; ++tap) {
            const float fall = waveHeights[tap] - waveHeights.w;
            /// @note Keep the current bottom estimate fixed while reconstructing the earlier water column.
            const float pastDepth = verticalDepth + fall;
            const float pastBand = 1.0f - smoothstep(threshold, bandEnd, pastDepth);
            const float retreat = smoothstep(wetRamp, wetRamp + 0.12f, fall);
            const float pastBreaker = WaterHorizonBreakingStrength(meanDepth, waveHeights[tap]) * pastBand * retreat;
            trail = max(trail, pastBreaker * exp(-0.5f * (float)(tap + 1)));
        }
        trail *= lerp(0.25f, 0.95f, smoothstep(0.28f, 0.72f, washPattern));
    }
    return wet * (currentFoam + (1.0f - currentFoam) * trail);
}

/// @brief Resolve small terrain contacts from opaque depth while keeping distant intersections tied to the terrain atlas.
/// @note View depth is converted to a world-space displacement along the original camera ray. Far lateral hits can belong to a different bank.
/// @see https://media.gdcvault.com/gdc08/slides/S6172i1.pdf GDC 2008, Shore and Foam, surface-to-world depth as a contact signal.
float WaterHorizonContactDepth(float terrainDepth, float3 toCamera, float viewRayZ, float sceneThickness, float hasBottom)
{
    const float distance = sceneThickness / max(viewRayZ, 0.05f);
    const float lateralDistance = distance * length(toCamera.xz);
    const float confidence = hasBottom * smoothstep(0.10f, 0.35f, toCamera.y)
        * (1.0f - smoothstep(1.0f, 5.0f, lateralDistance));
    return lerp(terrainDepth, distance * max(toCamera.y, 0.0f), confidence);
}

/// @note The first interface on a perspective ray uses the camera's medium. Mesh facing can change within a single crest.
bool WaterHorizonCameraUnderwater(float baseWorldY, bool faceUnderwater)
{
    if (isOrthographic > 0.5f) return faceUnderwater;
    const float relativeY = cameraPos.y - baseWorldY;
    if (g_waveShapeParams.y < 0.5f && abs(relativeY) > WaterMaximumWaveRise() + kRippleHeightScale)
        return relativeY < 0.0f;

    const uint owner = (uint)round(abs(g_farOceanParams.w));
    FBZZWaterMediumQueryContext context;
    if (owner == 0u || !FBZZ_WaterMediumHeader(context) || owner > context.bodyCount) return faceUnderwater;
    float4 column;
    if (!FBZZ_WaterAtlasSample(owner - 1u, cameraPos.xz, context, column)) return faceUnderwater;
    const uint row = FBZZ_WATER_MEDIUM_HEADER_ROWS + (owner - 1u) * FBZZ_WATER_MEDIUM_BODY_ROWS;
    const float cameraDepth = FBZZ_WaterMediumRow(row + 6u).x + column.x - cameraPos.y;
    return cameraDepth > 0.0f && cameraDepth <= column.y
        && (g_farOceanParams.w < 0.0f || !FBZZ_WaterPointExcluded(owner - 1u, cameraPos, context));
}

cbuffer WaterZoneAppearanceConstants : register(b11)
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

float4 PSMain(WaterPSInput p) : SV_Target0
{
    bool splineSurface = p.splineSurface.w > 0.5f;
#ifndef FBZZ_WATER_SMOOTH_SPLINE_OPTICS
    if (splineSurface) {
        float3 triangleNormal = cross(ddy(p.worldPos), ddx(p.worldPos));
        float normalLengthSq = dot(triangleNormal, triangleNormal);
        if (normalLengthSq > 1.0e-8f) {
            triangleNormal *= rsqrt(normalLengthSq);
            if (dot(triangleNormal, p.normal) < 0.0f) triangleNormal = -triangleNormal;
            p.normal = triangleNormal;
        }
    }
#endif
    if (WaterSurfaceExcluded(p)) discard;
    /// @note ワイヤーは変形後のメッシュを診断する。屈折・SSR・透明度・外周フェードを通すと線が背景へ溶け込むため、不透明な診断色で描く。
    if (waterWireframeMode > 0.5f) {
        const float3 wireNormal = normalize(p.normal);
        const float diffuse = abs(dot(wireNormal, normalize(-lightDir)));
        const float lighting = waterWireframeMode > 1.5f ? 1.0f
            : max(0.25f, saturate(diffuse * lightIntensity + dot(ambientColor, float3(0.2126f, 0.7152f, 0.0722f))));
        return float4(float3(1.0f, 0.65f, 0.1f) * lighting, 1.0f);
    }
    const WaterMaterialBlendTarget material = ResolveWaterSurfaceAppearance(p);
    float time = g_timeParams.w;
    float2 screenUV = p.screenPos.xy / p.screenPos.w * float2(0.5f, -0.5f) + 0.5f;

    /// @note 1 ピクセルが覆うワールド距離。さざ波の LOD・タイルの異方フィルタ・specular AA が
    /// @note すべてこれを基準にする。
    float2 footprintDX = ddx(p.worldPos.xz);
    float2 footprintDY = ddy(p.worldPos.xz);
    float  footprint   = max(length(footprintDX), length(footprintDY));
    float2 baseDX = ddx(p.baseWorldXZ);
    float2 baseDY = ddy(p.baseWorldXZ);

    /// @note 場の評価は頂点で行う。補間した向きは渦の中心でゼロになり得るため安全に正規化する。
    WaterFlowSample flowSample;
    float flowLength = length(p.flowDirectionDetail.xy);
    flowSample.direction = flowLength > 1.0e-4f ? p.flowDirectionDetail.xy / flowLength : g_flowParams.xy;
    flowSample.detailGain = p.flowDirectionDetail.z;
    flowSample.chop = p.flowDirectionDetail.w;
    flowSample.eye = p.flowFoam.x;
    flowSample.ring = p.flowFoam.y;
    float2 authoredFlow = p.splineSurface.yz;
    float authoredFlowSpeed = length(authoredFlow);
    if (splineSurface && authoredFlowSpeed > 1.0e-4f)
        flowSample.direction = authoredFlow / authoredFlowSpeed;
    float2 flowDir = flowSample.direction;
    const WaterCurrentAdvection current = WaterCurrentAt(authoredFlow, time);

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
                                              flowSample.detailGain, p.waveMeshFade, p.baseWorldXZ, baseDX, baseDY, splineSurface,
                                              material.normalReflectionRefraction.x, material.detail.z, followCurrent, current);
    float3 tangentNormal = normalSample.xyz;
    float  lostSlopeVariance = normalSample.w;

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
    /// @note 平行投影の視線は画素位置によらない。係数光学の光路と位相で透視のeye-rayを使わない。
    if (isOrthographic > 0.5f && material.scatteringMode.w > 0.5f)
        V = -normalize(float3(view[0][2], view[1][2], view[2][2]));
    /// @note 水面下から見上げているときは面の裏側を見ている (PSO は両面描画)。
    /// @warning 向きの判定に N を使うと、浅い角度のピクセルだけがばらばらに反転して斑になる。
    /// @note 決定は滑らかな幾何法線に任せる。
    bool cameraUnderwater = dot(p.normal, V) < 0.0f;
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
    /// @note Keep the optical normal on the incident hemisphere at grazing silhouettes without flipping an entire wave face.
    if (horizonSurface) N = normalize(N + V * max(0.0f, 0.001f - incidentCosine));
    float NdotV = saturate(dot(N, V));

    /// @note F0 は垂直入射の反射率。反射率を倍増すると浅い角度で飽和し、水中の散乱光が消える。
    /// @see https://doi.org/10.1111/1467-8659.1330233 Schlick, "An Inexpensive BRDF Model for Physically-based Rendering" (1994)
    float f0 = saturate(material.surface.z);
    const float localFresnel = saturate((f0 + (1.0f - f0)
        * pow(1.0f - NdotV, max(material.surface.w, 1.0f))) * material.surface.y);
    float fresnel = localFresnel;
    float3 refractedViewRay = float3(0.0f, 0.0f, 0.0f);
    if (horizonOptics || (useCoefficientOptics && cameraUnderwater)) {
        const float eta = cameraUnderwater ? WATER_REFRACTIVE_INDEX : 1.0f / WATER_REFRACTIVE_INDEX;
        const WaterDielectricInterface interfaceSample = WaterEvaluateDielectricInterface(-V, N, eta);
        refractedViewRay = interfaceSample.direction;
        const float dielectricFresnel = dot(refractedViewRay, refractedViewRay) <= 1.0e-8f
            ? 1.0f : saturate(interfaceSample.reflectance * material.surface.y);
        fresnel = lerp(localFresnel, dielectricFresnel, cameraUnderwater ? 1.0f : horizonWeight);
        if (horizonSurface) {
            const float normalReflectance = (1.0f - eta) / (1.0f + eta);
            f0 = lerp(f0, normalReflectance * normalReflectance, horizonWeight);
        }
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
#ifdef FBZZ_WATER_SMOOTH_SPLINE_OPTICS
    float2 splineTerrainDepth = float2(0.0f, 0.0f);
#endif
    if (splineSurface && horizonWeight < 1.0f) {
        const float sceneWaterDepth = waterDepth;
#ifdef FBZZ_WATER_SMOOTH_SPLINE_OPTICS
        /// @note Bilinear world-space bottom depth keeps optical bands independent of spline tessellation and viewing distance.
        if (g_terrainInfoParams.x > 0.5f)
            splineTerrainDepth = SampleWaterTerrainDepth(p.uv);
        const float verticalDepth = max(0.0f, splineTerrainDepth.y + p.worldPos.y - p.baseWorldY);
        const float terrainPath = verticalDepth * lerp(1.0f, 1.65f, 1.0f - abs(V.y));
        const float fallbackDepth = min(waterDepth, max(p.splineSurface.x, 0.0f));
        waterDepth = lerp(fallbackDepth, terrainPath, saturate(splineTerrainDepth.x));
#else
        float authoredDepthView = max(p.splineSurface.x, 0.0f)
            * viewRayZ / max(abs(V.y), 0.05f);
        waterDepth = min(waterDepth, authoredDepthView);
#endif
        waterDepth = lerp(waterDepth, sceneWaterDepth, horizonWeight);
    }
    float depthFactor = smoothstep(0.0f, 1.0f, saturate(waterDepth / max(material.deepColorDepth.w, 0.0001f)));

    float shallowFactor = smoothstep(0.0f, 1.0f, saturate(waterDepth / max(material.shallowColorDepth.w, 0.0001f)));
    /// @note 色だけは «うねりの山ほど水の層が薄い» ことを織り込む。水底までの距離だけで決めると
    /// @note 波の山も谷も同じ色になり、うねりが «色の付いた板» にしか見えない。
    /// @note アルファと屈折の判定には素の depthFactor を使う (そちらは «底までの距離» の話)。
    float colorDepthFactor = saturate(depthFactor * (1.0f - p.waveState.x * 0.40f));
    /// @note 浅瀬色 / 深部色は «水の層が散乱して返す色» (反射率)。光はこの後で掛ける。
    float3 scatterAlbedo = lerp(material.shallowColorDepth.xyz, material.deepColorDepth.xyz, colorDepthFactor);

    /// @note 水底からの光は Beer–Lambert で減る。赤から先に吸われるのが «水の色» の正体なので、
    /// @note 深部色の色相を 1 深部深度あたりの透過率として使い、明るさは別に e^-1 ずつ落とす。
    /// @see https://doi.org/10.1364/AO.36.008710 Pope & Fry, "Absorption spectrum (380–700 nm) of pure water" (1997)
    float  deepDepth = max(material.deepColorDepth.w, 1.0e-4f);
    float  deepPeak  = max(max(material.deepColorDepth.x, material.deepColorDepth.y), max(material.deepColorDepth.z, 1.0e-4f));
    float3 deepHue   = max(material.deepColorDepth.xyz / deepPeak, 0.02f);
    float  opticalDepth = waterDepth / deepDepth;
    float3 transmittance = pow(deepHue, opticalDepth) * exp(-opticalDepth) * (1.0f - backgroundMask);
    /// @note 散乱で水の層そのものの色へ置き換わる割合。深部深度で 95%。
    /// @note opacity と material.effects.y は «濁り» の上限と下限として残す。
    float  scatterOpacity = 1.0f - exp(-3.0f * opticalDepth);
    float  murk = lerp(saturate(material.surface.x) * lerp(saturate(material.effects.y), 1.0f, scatterOpacity),
                       1.0f, backgroundMask);

    /// @note スクリーンスペース屈折は水面法線とスネル方向で HDR カラー参照 UV をずらす。水底ジオメトリを
    /// @note 再描画せず透明水面らしい歪みを得る。現在描画中の HDR RT を直接読むと read/write
    /// @note 競合になるため、Water 直前にコピーした sceneColor を参照する。
    float refractionMask = shallowFactor * (1.0f - backgroundMask);
    /// @note 水面を通る光は面の傾きと反対側へ曲がる。画面 UV の上下反転もカメラ空間で合わせる。
    /// @see https://www.graphics.cornell.edu/~bjw/microfacetbsdf.pdf Walter et al. (2007) §4.2 Ideal Refraction
    float3 viewNormal = mul(float4(N, 0.0f), view).xyz;
    float2 refrOffset = float2(-viewNormal.x, viewNormal.y) * material.normalReflectionRefraction.z
                      * (1.0f - fresnel) * refractionMask;
    if (useCoefficientOptics && cameraUnderwater && fresnel < 1.0f)
    {
        float3 incidentViewRay = mul(float4(-V, 0.0f), view).xyz;
        float3 transmittedViewRay = mul(float4(refractedViewRay, 0.0f), view).xyz;
        float2 incidentSlope = incidentViewRay.xy / max(abs(incidentViewRay.z), 0.05f);
        float2 transmittedSlope = transmittedViewRay.xy / max(abs(transmittedViewRay.z), 0.05f);
        float2 slopeDelta = transmittedSlope - incidentSlope;
        refrOffset = 0.5f * float2(slopeDelta.x * projection[0][0], -slopeDelta.y * projection[1][1])
                   * material.normalReflectionRefraction.z * (1.0f - fresnel);
    }
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
        float2 refrUV = saturate(screenUV + refrOffset);

        /// @note 屈折先が水面より手前なら、前景のシルエットを水中に混ぜない。
        float refrRawDepth = g_sceneDepth.Sample(g_samplerClamp, refrUV).r;
        if (LinearizeDepth(refrRawDepth) < linearSurfDepth) refrUV = screenUV;
        refractColor = g_sceneColor.Sample(g_samplerClamp, refrUV).rgb;
    }
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
    float microfacetAlpha = roughness * roughness;
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
    if (horizonOptics) {
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
            /// @note For the original pixel, convert view-Z thickness into world meters along the original ray.
            float opticalPathLength = isOrthographic > 0.5f ? waterDepth : waterDepth / max(viewRayZ, 0.05f);
            const float phaseG = material.absorptionPhase.w;
            /// @note Approximate the environment as uniform radiance over the visible hemisphere; isotropic integration gives one half.
            const float3 diffuseSource = skyIrradianceOverPi * 0.5f + punctualSingleScatterSource;
            /// @note Rectangle water over cleared depth has no transmitted bottom; authored spline depth remains finite without terrain.
            /// @see https://dev.epicgames.com/documentation/unreal-engine/single-layer-water-shading-model-in-unreal-engine Epic Single Layer Water, homogeneous volume and scene-depth refraction inputs.
            const float bottomTransmission = splineSurface ? 1.0f : (1.0f - backgroundMask);
            body = 0.0f;
            [branch]
            if (!horizonOptics || horizonWeight < 1.0f) {
                const WaterOpticalTransport opticalTransport = WaterEvaluateOpticalTransport(
                    material.absorptionPhase.xyz, material.scatteringMode.xyz, opticalPathLength);
                const float directPhase = WaterHenyeyGreensteinPhase(dot(-L, V), phaseG);
                const float3 directSource = sunOverPi * ((L.y > 0.0f ? 1.0f : 0.0f) * shadow * (PI * directPhase));
                body = refractColor * opticalTransport.transmittance * bottomTransmission
                    + opticalTransport.singleScatterWeight * (directSource + diffuseSource);
            }
            [branch]
            if (horizonOptics) {
                const bool bentSample = horizonHit.weight >= 0.999f;
                const bool openWater = backgroundMask > 0.5f;
                if (bentSample) opticalPathLength = horizonHit.pathLength;
                if (openWater) opticalPathLength = WATER_OPTICAL_MAX_DISTANCE;
                const WaterOpticalMedium medium = WaterPrepareOpticalMedium(
                    material.absorptionPhase.xyz, material.scatteringMode.xyz);
                const WaterHorizonSun sun = WaterTransmitHorizonSun(L, sunOverPi * (shadow * PI), swellNormal);
                /// @note The fallback depth estimates path length. The interface sets the scattering direction in every case.
                /// @see https://www.pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission PBRT v4 §9.3.4, the transmitted direction is set at the interface.
                float3 horizonBody = WaterEvaluateHorizonRadiance(medium, opticalPathLength, refractedViewRay,
                    horizonRefractColor * (1.0f - backgroundMask), diffuseSource, sun, phaseG);
                /// @note Blend complete radiance samples at uncertain hits so the scene colors keep their own path lengths.
                [branch]
                if (horizonHit.weight > 0.0f && !bentSample) {
                    const float3 bentColor = SampleWaterHorizonRefraction(horizonHit, linearSurfDepth, sceneThickness);
                    const float3 bentRadiance = WaterEvaluateHorizonRadiance(medium, horizonHit.pathLength,
                        refractedViewRay, bentColor, diffuseSource, sun, phaseG);
                    horizonBody = lerp(horizonBody, bentRadiance, horizonHit.weight);
                }
                body = lerp(body, horizonBody, horizonWeight);
            }
        }
    } else {
        /// @note 水の層が返す光 = 散乱色 × (太陽 + 空 + 局所光)。定数色のまま出すと昼夜・影に追随しない。
        bodyIrradiance = sunOverPi * (sunCosFlat * shadow) + skyIrradianceOverPi + punctualOverPi;
        float3 inscatter = scatterAlbedo * bodyIrradiance;
        body = lerp(refractColor * transmittance, inscatter, murk);
    }

    /// @note 太陽のきらめきは GGX で引く。Blinn-Phong は裾が急に落ちるため、海のきらめきの
    /// @note «広がった尾» が出ず、ハイライトが «板に当たった光» に見える。
    /// @note 異方にするのは、さざ波が detailAnisotropy で風向と直交して伸びているから。同じ
    /// @note 異方を反射ローブへ渡すと、きらめきが風向に沿った帯になる ── 海面の見え方そのもの。
    /// @note 接空間は «風向» で張り直す。p.tangent はワールド +X 基準なので、そのまま渡すと
    /// @note 異方の向きが風と無関係になる。うねの筋を横切る «風向» 側が粗い。
    float3 flowWorld = float3(rippleDirection.x, 0.0f, rippleDirection.y);
    float3 specT = flowWorld - N * dot(N, flowWorld);
    float  specTLen = length(specT);
    specT = specTLen > 1.0e-4f ? specT / specTLen : normalize(p.tangent);
    float3 specB = normalize(cross(N, specT));
    float  specAniso = saturate((max(g_flowParams.z, 1.0f) - 1.0f) * 0.35f);

    /// @note 太陽は点ではなく角半径 0.0047 rad の円盤。α に円盤ぶんを足して峰を広げないと、
    /// @note GGX の頂点が 1 ピクセルへ落ちて Bloom がちらつく (球光源の正規化と同じ扱い)。
    /// @note GGX 分布は投影面積で正規化済み。粗さを広げた後に峰の比を全体へ掛けると裾の光まで失う。
    float sunAlpha = max(roughnessAA * roughnessAA, 1.0e-5f);
    float sunAlphaWide = saturate(sunAlpha + 0.0023f);
    float NdotL = saturate(dot(N, L));
    /// @see https://www.graphics.cornell.edu/~bjw/microfacetbsdf.pdf Walter et al., "Microfacet Models for Refraction through Rough Surfaces" (2007) §5.2 GGX
    float3 sunSpecular = BRDF_SpecularAdvanced(N, V, L, f0.xxx, sqrt(sunAlphaWide),
                                               specT, specB, specAniso)
                        * (NdotL * shadow * PI) * sunOverPi;

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
    float foamThreshold = material.foam.x;
    float foamFade      = max(material.foam.y, 0.0001f);
    float foamScale = max(material.foam.w, 1.0e-4f);
    float shoreFoam     = (1.0f - smoothstep(foamThreshold, foamThreshold + foamFade, waterDepth)) * (1.0f - backgroundMask);

    float foamMaskVal = 0.0f;
    float2 swashOffset = 0.0f;
    float swashWeight = 0.0f;
    if (g_terrainInfoParams.x > 0.5f) {
        float2 terrainUv = splineSurface ? p.uv : WaterTerrainUvAtWorldXZ(p.worldPos.xz);
#ifdef FBZZ_WATER_SMOOTH_SPLINE_OPTICS
        float2 terrainDepth = splineSurface ? splineTerrainDepth : SampleWaterTerrainDepth(terrainUv);
#else
        float2 terrainDepth = SampleWaterTerrainDepth(terrainUv);
#endif
        float verticalDepth = terrainDepth.y + p.worldPos.y - p.baseWorldY;
        float wet = verticalDepth > 0.0f ? 1.0f : 0.0f;
        float terrainFoam = (1.0f - smoothstep(foamThreshold, foamThreshold + foamFade,
            max(verticalDepth, 0.0f))) * wet;
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
    const float crestStrength = max(material.crestFoam.z, 0.0f);
    const float crestSignal = saturate(horizonOptics ? p.waveState.w : p.waveState.y);
    const float crestFoam = saturate(crestSignal * crestStrength);
    /// @note 渦と排水口の目は実際に白く泡立つ。足さないと «ただ凹んだ穴» にしか見えない。
    /// @note 湧き出しは縁で水がぶつかるので、輪として出す。
    float eyeFoam = max(flowSample.eye * 0.9f, flowSample.ring * 0.7f);
    float localFoamAmount = max(max(foamMaskVal, shoreFoam), eyeFoam) * foamTexVal;
    float localFoam = saturate(smoothstep(0.05f, 1.0f, localFoamAmount) * max(material.foam.z, 0.0f));
    float crestCoverage = smoothstep(0.05f, 1.0f, crestFoam * foamTexVal);
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
    float foam = saturate(max(localFoam, crestCoverage));
    /// @note 泡と波紋の輪は «水面に浮いた白い粗面» なので、定数色ではなく Lambert で照らす。
    /// @note 太陽は泡の面の向きで受け、影の中では空と局所光だけが残る。
    float3 surfaceIrradiance = sunOverPi * (NdotL * shadow) + skyIrradianceOverPi + punctualOverPi;
    [branch]
    if (foam > 0.001f) {
        float3 foamAlbedo = saturate(material.foamColorRoughness.rgb) * lerp(0.85f, 1.0f, saturate(foamTexVal));
        float foamRoughness = clamp(material.foamColorRoughness.w, 0.05f, 1.0f);
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
