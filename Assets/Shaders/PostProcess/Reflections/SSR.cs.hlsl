/// @file    SSR.cs.hlsl
/// @brief   画面空間の反射交差と受け側の鏡面間接光近似。
/// @author  Hasegawa Jin
/// @date    2026-06-23
/// @note Hybrid RGB は Fresnel 適用済みの線形 HDR。alpha は材質の反射率と独立した交差信頼度。
/// @see https://jcgt.org/published/0003/04/04/ Efficient GPU Screen-Space Ray Tracing

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Rendering/HybridGlassPolicy.hlsli"


/// @note t5 は SSR / RT をまだ合成していない不変の HDR 入力。
FBZZ_TEX2D(texHDR, 5);
FBZZ_TEX2D(texGBuffer0, 0);
FBZZ_TEX2D(texGBuffer1, 6);
/// @note Hybrid-only t22 contains emission RGB and the typed glass marker; legacy Raster never reads it.
FBZZ_TEX2D(texGBufferEmission, 22);
FBZZ_TEX2D_T(float, texDepth, 7);
/// @note Raster では Forward 不透明物も含む深度、Hybrid では反射源 HDR と同じ時点の深度。
FBZZ_TEX2D_T(float, texSceneDepth, TEX_SCENE_DEPTH_SLOT);

/// @note 全画面フェッチなので clamp (s0 は DX12 では WRAP で、画面端が反対側の色を拾う)。
SamplerState       sampDefault : register(SAMPLER_LINEAR_CLAMP);

FBZZ_RWTEX2D_T(float4, OutputSSR, 3);

/// @brief 画面上のレイ補間位置を視空間へ戻す。
/// @see https://jcgt.org/published/0003/04/04/paper.pdf Section 2: Perspective-Correct Interpolation
float3 HybridSsrRayPosition(float3 originVS, float3 endVS, float traceRatio)
{
    if (isOrthographic > 0.5f)
        return lerp(originVS, endVS, traceRatio);
    float rayDepth = rcp(lerp(rcp(originVS.z), rcp(endVS.z), traceRatio));
    return lerp(originVS / originVS.z, endVS / endVS.z, traceRatio) * rayDepth;
}

/// @brief Hybrid の候補を固定の視空間厚みと表面向きで検証する。
/// @note 二分探索は 6 回まで。2 pixel 未満の L-infinity 移動、背景、裏面、厚み外は別 provider へ委ねる。
/// @note 厚みは視空間の長さであり、探索ステップ幅では拡大しない。失敗時の confidence は 0。
/// @see https://github.com/GPUOpen-Effects/FidelityFX-SSSR/blob/master/ffx-sssr/ffx_sssr.h FFX_SSSR_ValidateHit
bool ValidateHybridSsrHit(float2 originUV, float2 uvDelta, float lowerRatio, float upperRatio,
    float3 originVS, float3 endVS, float3 worldRayDirection, float2 outputSize,
    out float2 hitUV, out float confidence)
{
    hitUV = 0.0f;
    confidence = 0.0f;
    [unroll]
    for (int refinement = 0; refinement < 6; ++refinement)
    {
        float midpoint = (lowerRatio + upperRatio) * 0.5f;
        float2 midpointUV = originUV + uvDelta * midpoint;
        if (!all(isfinite(midpointUV)) || any(midpointUV < 0.0f) || any(midpointUV >= 1.0f))
            return false;
        int2 midpointPixel = int2(midpointUV * outputSize);
        float midpointDepth = texSceneDepth.Load(int3(midpointPixel, 0)).r;
        float3 midpointRayVS = HybridSsrRayPosition(originVS, endVS, midpoint);
        if (!isfinite(midpointDepth) || midpointDepth < 0.0f || midpointDepth > 1.0f
            || !all(isfinite(midpointRayVS)))
            return false;
        float midpointSurfaceZ = LinearizeDepth(midpointDepth, nearZ, farZ, isOrthographic);
        if (!isfinite(midpointSurfaceZ))
            return false;
        if (midpointRayVS.z >= midpointSurfaceZ)
            upperRatio = midpoint;
        else
            lowerRatio = midpoint;
    }

    hitUV = originUV + uvDelta * upperRatio;
    if (!all(isfinite(hitUV)) || any(hitUV < 0.0f) || any(hitUV >= 1.0f))
        return false;
    float2 travelPixels = abs(hitUV - originUV) * outputSize;
    if (max(travelPixels.x, travelPixels.y) < 2.0f)
        return false;
    int2 hitPixel = int2(hitUV * outputSize);
    if (HybridGlassReceiver(texGBufferEmission.Load(int3(hitPixel, 0)).a)) return false;
    float hitDepth = texSceneDepth.Load(int3(hitPixel, 0)).r;
    if (!isfinite(hitDepth) || IsFarDepth(hitDepth) || hitDepth > 1.0f)
        return false;
    float3 hitNormal = texGBuffer1.Load(int3(hitPixel, 0)).xyz * 2.0f - 1.0f;
    float normalLengthSquared = dot(hitNormal, hitNormal);
    if (!all(isfinite(hitNormal)) || !isfinite(normalLengthSquared) || normalLengthSquared <= 1e-8f)
        return false;
    hitNormal *= rsqrt(normalLengthSquared);
    if (!(dot(hitNormal, worldRayDirection) < 0.0f))
        return false;

    float3 hitRayVS = HybridSsrRayPosition(originVS, endVS, upperRatio);
    float3 surfaceWorld = ReconstructWorldPos(hitUV, hitDepth, invViewProjection);
    float3 surfaceVS = mul(float4(surfaceWorld, 1.0f), view).xyz;
    float separation = length(hitRayVS - surfaceVS);
    if (!all(isfinite(hitRayVS)) || !all(isfinite(surfaceVS)) || !isfinite(separation)
        || separation >= ssrThickness)
        return false;
    float thicknessConfidence = 1.0f - smoothstep(0.0f, ssrThickness, separation);
    float candidateConfidence = thicknessConfidence * thicknessConfidence;
    if (!isfinite(candidateConfidence) || candidateConfidence <= 0.0f)
        return false;
    confidence = candidateConfidence;
    return true;
}


[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    float2 outputSize;
    OutputSSR.GetDimensions(outputSize.x, outputSize.y);

    if ((float)id.x >= outputSize.x || (float)id.y >= outputSize.y)
        return;

    if (HybridReflectionResolveActive(reflectionResolveEnabled) && (!isfinite(ssrMaxDistance) || !isfinite(ssrThickness)
        || !isfinite(ssrIntensity) || ssrThickness <= 0.0f))
    {
        OutputSSR[id.xy] = 0.0f;
        return;
    }

    if (ssrIntensity <= 0.0f || ssrMaxDistance <= 0.0f || ssrSteps <= 0)
    {
        OutputSSR[id.xy] = 0.0f;
        return;
    }

    float2 uv = (float2(id.xy) + 0.5f) / outputSize;
    if (HybridReflectionResolveActive(reflectionResolveEnabled)
        && HybridGlassReceiver(texGBufferEmission.Load(int3(id.xy, 0)).a)) {
        OutputSSR[id.xy] = 0.0f;
        return;
    }


    /// @note GBuffer と深度を点取得し、輪郭で隣接する面の法線・深度を混ぜない。
    float4 gb0  = texGBuffer0.Load(int3(id.xy, 0));
    float4 gb1  = texGBuffer1.Load(int3(id.xy, 0));
    float3 N    = normalize(gb1.xyz * 2.0f - 1.0f);
    float  met  = gb1.w;
    float3 baseColor = max(gb0.rgb, 0.0f);
    float  rough = saturate(gb0.a);

    /// @note 単一鏡面方向の近似では極端に粗い面を表せないので、別 provider へ委ねる。
    if (rough >= 0.95f || (HybridReflectionResolveActive(reflectionResolveEnabled) && rough >= 0.25f))
    {
        OutputSSR[id.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float ndcDepth = texDepth.Load(int3(id.xy, 0)).r;

    /// @note GBuffer 後に描いた Forward 不透明物の遮蔽は最終シーン深度で判定する。
    /// @note カメラ深度は Reversed-Z なので «手前» は値が大きい側。
    float sceneNdcDepth = texSceneDepth.Load(int3(id.xy, 0)).r;
    if (sceneNdcDepth - 1e-5f > ndcDepth)
    {
        OutputSSR[id.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    /// @note 最遠 (Reversed-Z で 0) はスカイボックス。反射不要。
    if (IsFarDepth(ndcDepth))
    {
        OutputSSR[id.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }


    float3 worldPos = ReconstructWorldPos(uv, ndcDepth, invViewProjection);

    /// @note 平行投影では視線が全画素で同じ (カメラ前方)。視空間 +Z をワールドへ戻す。
    float3 V = isOrthographic > 0.5f
        ? normalize(mul((float3x3)view, float3(0.0f, 0.0f, 1.0f)))
        : normalize(worldPos - cameraPos);


    float3 R = reflect(V, N);

    float3 rayOriginVS = mul(float4(worldPos, 1.0f), view).xyz;
    float3 rayDirVS    = normalize(mul(float4(R, 0.0f), view).xyz);

    if (HybridReflectionResolveActive(reflectionResolveEnabled) && (!all(isfinite(gb0)) || !all(isfinite(gb1))
        || !all(isfinite(rayOriginVS)) || !all(isfinite(rayDirVS)) || !all(isfinite(R))))
    {
        OutputSSR[id.xy] = 0.0f;
        return;
    }

    int stepCount = max(ssrSteps, 1);

    /// @note 投影不能な near / far 外にステップを消費せず、画面内の探索密度を維持する。
    float traceDistance = ssrMaxDistance;
    if (rayDirVS.z < -1e-5f && rayOriginVS.z + rayDirVS.z * traceDistance < nearZ)
        traceDistance = max((nearZ * 1.001f - rayOriginVS.z) / rayDirVS.z, 0.0f);
    else if (rayDirVS.z > 1e-5f && rayOriginVS.z + rayDirVS.z * traceDistance > farZ)
        traceDistance = max((farZ - rayOriginVS.z) / rayDirVS.z, 0.0f);

    float3 rayEndVS = rayOriginVS + rayDirVS * traceDistance;
    float4 originClip = mul(float4(rayOriginVS, 1.0f), projection);
    float4 endClip    = mul(float4(rayEndVS, 1.0f), projection);
    if (traceDistance <= 0.0f || abs(originClip.w) < 1e-5f || abs(endClip.w) < 1e-5f)
    {
        OutputSSR[id.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float2 originUV = NdcToUv(originClip.xy / originClip.w);
    float2 endUV    = NdcToUv(endClip.xy / endClip.w);
    float2 uvDelta  = endUV - originUV;

    float maxTraceRatio = 1.0f;
    if (uvDelta.x > 1e-6f)
        maxTraceRatio = min(maxTraceRatio, (1.0f - originUV.x) / uvDelta.x);
    else if (uvDelta.x < -1e-6f)
        maxTraceRatio = min(maxTraceRatio, -originUV.x / uvDelta.x);
    if (uvDelta.y > 1e-6f)
        maxTraceRatio = min(maxTraceRatio, (1.0f - originUV.y) / uvDelta.y);
    else if (uvDelta.y < -1e-6f)
        maxTraceRatio = min(maxTraceRatio, -originUV.y / uvDelta.y);
    maxTraceRatio = saturate(maxTraceRatio);

    /// @note 画面上で均等に進め、斜角の固定ワールドステップによる縞状の欠落を防ぐ。
    float pixelLength = length(uvDelta * maxTraceRatio * outputSize);
    int marchStepCount = min(stepCount, max((int)ceil(pixelLength), 1));
    float previousRayDepth = rayOriginVS.z;
    float previousDepthDiff = 0.0f;
    float previousTraceRatio = 0.0f;
    float4 hitColor  = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float2 hitUV = 0.0f;
    float hitDepthConfidence = 1.0f;
    bool   bHit      = false;

    [loop]
    for (int step = 1; step <= marchStepCount; ++step)
    {
        float traceRatio = maxTraceRatio * (float(step) / float(marchStepCount));
        float2 rayUV = originUV + uvDelta * traceRatio;

        /// @note 非線形深度を点取得し、輪郭で存在しないビュー深度を生成しない。
        int2 depthPixel = clamp(int2(rayUV * outputSize), int2(0, 0), int2(outputSize) - 1);
        float sampleNdcDepth = texSceneDepth.Load(int3(depthPixel, 0)).r;

        float sampleDepthLinear = LinearizeDepth(sampleNdcDepth, nearZ, farZ, isOrthographic);
        /// @note UV の線形補間と異なり、透視投影のビュー深度は逆数補間する。
        /// @see https://jcgt.org/published/0003/04/04/paper.pdf Section 2: Perspective-Correct Interpolation
        /// @note 平行投影では画面上の補間がそのまま視空間 Z の補間になる。
        float rayDepthLinear = isOrthographic > 0.5f
            ? lerp(rayOriginVS.z, rayEndVS.z, traceRatio)
            : rcp(lerp(rcp(rayOriginVS.z), rcp(rayEndVS.z), traceRatio));

        /// @note Raster の従来判定だけはステップ分の深度移動を交差幅に含める。
        float hitThickness = max(ssrThickness, abs(rayDepthLinear - previousRayDepth));

        /// @note 手前から奥への横切りだけを拾い、既に遮蔽物の背後にあるレイを採用しない。
        float depthDiff = rayDepthLinear - sampleDepthLinear;
        bool crossedDepthSurface = previousDepthDiff <= 0.0f && depthDiff > 0.0f;
        if (HybridReflectionResolveActive(reflectionResolveEnabled) && crossedDepthSurface)
        {
            bHit = ValidateHybridSsrHit(originUV, uvDelta, previousTraceRatio, traceRatio,
                rayOriginVS, rayEndVS, R, outputSize, hitUV, hitDepthConfidence);
            if (bHit)
            {
                /// @note 色も検証済み深度・法線と同じ画素から読み、輪郭外の HDR を混ぜない。
                hitColor = texHDR.Load(int3(int2(hitUV * outputSize), 0));
            }
            break;
        }
        if (!HybridReflectionResolveActive(reflectionResolveEnabled) && crossedDepthSurface && depthDiff < hitThickness)
        {
            hitColor = texHDR.SampleLevel(sampDefault, rayUV, 0);
            hitUV = rayUV;
            bHit     = true;
            break;
        }

        previousRayDepth = rayDepthLinear;
        previousDepthDiff = depthDiff;
        previousTraceRatio = traceRatio;
    }


    if (bHit)
    {
        /// @note Hybrid は単一鏡面方向の Schlick 応答であり、粗い GGX ローブの畳み込みではない。
        float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, saturate(met));
        float  NdotV = saturate(dot(N, -V));
        float3 fresnel = F0 + (1.0f - F0) * pow(1.0f - NdotV, 5.0f);
        float  reflectance = max(fresnel.r, max(fresnel.g, fresnel.b));
        float  roughnessConfidence = 1.0f - smoothstep(0.20f, 0.95f, rough);
        /// @note Hybrid の ssrIntensity は resolver の採用 weight だけに一度適用する。
    if (HybridReflectionResolveActive(reflectionResolveEnabled))
        {
            float3 hybridF0 = lerp(float3(0.04f, 0.04f, 0.04f), saturate(baseColor), saturate(met));
            float3 hybridFresnel = hybridF0 + (1.0f - hybridF0) * pow(1.0f - NdotV, 5.0f);
            /// @note 交差先の画面端 8 pixel で信頼度を下げ、F0 は採用率へ掛けない。
            float2 edgePixels = min(hitUV, 1.0f - hitUV) * outputSize;
            float edgeConfidence = smoothstep(0.0f, 8.0f, min(edgePixels.x, edgePixels.y));
            float3 indirectSpecular = max(hitColor.rgb, 0.0f) * hybridFresnel;
            /// @note 単一鏡面レイは粗い GGX ローブを表せないので、Hybrid だけ RT へ早めに委ねる。
            float hybridRoughnessConfidence = 1.0f - smoothstep(0.05f, 0.25f, rough);
            float geometricConfidence = saturate(hybridRoughnessConfidence * edgeConfidence * hitDepthConfidence);
            if (!all(isfinite(hitColor.rgb)) || !all(isfinite(indirectSpecular))
                || any(indirectSpecular > 65504.0f) || !isfinite(geometricConfidence))
                OutputSSR[id.xy] = 0.0f;
            else
                OutputSSR[id.xy] = float4(indirectSpecular, geometricConfidence);
            return;
        }

        /// @note Raster の従来出力は RGB tint と Fresnel 込み alpha をそのまま維持する。
        float  confidence = saturate(reflectance * roughnessConfidence);
        float3 reflectionTint = fresnel / max(reflectance, 1e-4f);

        /// @note Raster の ssrIntensity は Composite で一度だけ適用する。
        OutputSSR[id.xy] = float4(hitColor.rgb * reflectionTint, confidence);
    }
    else
    {
        OutputSSR[id.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
}
