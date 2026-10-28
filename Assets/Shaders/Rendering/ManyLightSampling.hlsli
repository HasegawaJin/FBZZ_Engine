/// @file    ManyLightSampling.hlsli
/// @brief   Receiver-guided stratified sampling bounds light evaluations while preserving expected radiance.
/// @author  Hasegawa Jin
/// @date    2026-10-09
#ifndef FBZZ_MANY_LIGHT_SAMPLING_HLSLI
#define FBZZ_MANY_LIGHT_SAMPLING_HLSLI

uint FBZZ_ManyLightHash(uint value)
{
    value ^= value >> 16u;
    value *= 0x7FEB352Du;
    value ^= value >> 15u;
    value *= 0x846CA68Bu;
    return value ^ (value >> 16u);
}

uint FBZZ_PunctualEvaluationCount(uint count)
{
    return clusterLightMode == FBZZ_LIGHT_MODE_SAMPLED
        ? min(count, clamp(manyLightControl & 63u, 1u, 32u)) : count;
}

struct FBZZManyLightReceiver
{
    float3 normal;
    float3 viewDirection;
    float3 albedo;
    float metallic;
    float roughness;
};

/// @note A negative roughness selects geometric guiding for non-PBR surfaces and volumes.
FBZZManyLightReceiver FBZZ_ManyLightReceiver(float3 normal)
{
    FBZZManyLightReceiver receiver = (FBZZManyLightReceiver)0;
    receiver.normal = normal;
    receiver.roughness = -1.0f;
    return receiver;
}

FBZZManyLightReceiver FBZZ_ManyLightPbrReceiver(float3 normal, float3 viewDirection,
                                              float3 albedo, float metallic, float roughness)
{
    FBZZManyLightReceiver receiver;
    receiver.normal = normal;
    receiver.viewDirection = viewDirection;
    receiver.albedo = albedo;
    receiver.metallic = metallic;
    receiver.roughness = roughness;
    return receiver;
}

/// @note This proposal reads no visibility or medium textures; the selected light retains its original shadow, cookie and BRDF evaluation.
/// @see https://www.pbr-book.org/4ed/Light_Sources/Light_Sampling Section 12.6: receiver-dependent importance and inverse probability compensation.
float FBZZ_ManyLightReceiverImportance(PunctualLight light, float3 position, FBZZManyLightReceiver receiver, float estimate)
{
    const float3 delta = light.position - position;
    float distance = length(delta);
    const float3 L = SafeNormalize(delta, float3(0.0f, 1.0f, 0.0f));
    if (light.type == FBZZ_LIGHT_TYPE_SPHERE)
        distance = max(distance - max(light.halfWidth, 0.0f), 0.0f);
    if (light.type == FBZZ_LIGHT_TYPE_TUBE) {
        const float3 axis = SafeNormalize(light.tangent, float3(1.0f, 0.0f, 0.0f));
        const float extent = max(light.halfHeight, 0.0f);
        const float3 closest = light.position + axis * clamp(dot(position - light.position, axis), -extent, extent);
        distance = max(length(closest - position) - max(light.halfWidth, 0.0f), 0.0f);
    }
    float attenuation = LightAttenuation(distance, light.range);
    if (light.type == FBZZ_LIGHT_TYPE_SPOT)
        attenuation *= SpotConeWeight(L, light.direction, light.innerCos, light.outerCos);
    /// @note A zero normal denotes a volume sample; surface orientation guides only surfaces, with uniform support retained for other shading models.
    if (dot(receiver.normal, receiver.normal) > 0.0f)
        attenuation *= saturate(dot(receiver.normal, L));
    float3 reflectedColor = min(max(light.color, 0.0f), 1.0e3f);
    /// @note Diffuse-only guiding misses glossy and metallic highlights; the base GGX lobe guides selection while uniform support retains extra material lobes and light shapes.
    /// @see https://www.pbr-book.org/4ed/Light_Sources/Light_Sampling Section 12.6: effective probabilities vary with the BSDF.
    if (receiver.roughness >= 0.0f) {
        const BRDFResult brdf = EvaluateBRDF(receiver.normal, receiver.viewDirection, L,
            receiver.albedo, receiver.metallic, receiver.roughness);
        reflectedColor *= brdf.diffuse + brdf.specular;
    }
    const float luminance = dot(reflectedColor, float3(0.2126f, 0.7152f, 0.0722f));
    const float score = min(max(light.intensity, 0.0f), 1.0e6f) * luminance * attenuation * estimate;
    return isfinite(score) ? max(score, 0.0f) : 0.0f;
}

/// @note Each disjoint stratum samples receiver importance with a 5% uniform mixture; inverse probabilities preserve unequal lengths and pool estimates.
/// @see https://www.pbr-book.org/4ed/Sampling_and_Reconstruction/Stratified_Sampler Section 8.4: disjoint strata reduce variance.
PunctualLight FBZZ_PunctualEvaluationAtBudget(uint base, uint count, uint sampleIndex,
                                       float2 pixelCenter, float3 lookupPosition, uint evaluations, uint sequence, FBZZManyLightReceiver receiver)
{
    if (clusterLightMode != FBZZ_LIGHT_MODE_SAMPLED) return FBZZ_PunctualAt(base, sampleIndex);
    const uint2 pixel = (uint2)max(floor(pixelCenter), 0.0f);
    const float viewZ = mul(float4(lookupPosition, 1.0f), view).z;
    const uint phase = manyLightControl >> 6u;
    const uint seed = pixel.x * 1597334677u ^ pixel.y * 3812015801u
        ^ base ^ (FBZZ_ClusterSliceFromViewZ(viewZ) * 0x9E3779B9u) ^ phase ^ (sequence * 0x85EBCA6Bu);
    if ((gClusterIndices[base - 1u] >> 16u) > FBZZ_MANY_LIGHT_POOL_SIZE) {
        /// @note One overlapping pool serves all strata of this pixel; choosing a new pool per sample destroys stratification and repeats dominant lights.
        const float2 coordinates = pixelCenter / max(clusterTilePx, 1.0e-4f) - 0.5f;
        const int2 lower = (int2)floor(coordinates);
        const float2 fraction = frac(coordinates);
        /// @note Keep the overlapping pool stable across phases so its low-discrepancy temporal sequence is not broken by switching pools.
        const uint selection = FBZZ_ManyLightHash(seed ^ phase ^ 0xA511E9B3u);
        const float2 randomPool = float2(selection & 65535u, selection >> 16u) * (1.0f / 65536.0f);
        const uint2 neighbor = (uint2)clamp(lower + (int2)(randomPool < fraction), int2(0, 0),
            int2(FBZZ_CLUSTER_GRID_X - 1, FBZZ_CLUSTER_GRID_Y - 1));
        const uint neighborBase = FBZZ_ClusterIndex(uint3(neighbor, FBZZ_ClusterSliceFromViewZ(viewZ)))
            * FBZZ_CLUSTER_STRIDE + 1u;
        const uint neighborCount = min(gClusterIndices[neighborBase - 1u] & 65535u, (uint)FBZZ_MANY_LIGHT_POOL_SIZE);
        if (neighborCount > 0u) {
            base = neighborBase;
            count = neighborCount;
        }
    }
    const uint first = sampleIndex * count / evaluations;
    const uint last = (sampleIndex + 1u) * count / evaluations;
    const uint length = last - first;
    if (length == 0u) {
        PunctualLight empty = (PunctualLight)0;
        return empty;
    }
    const uint stratumSeed = FBZZ_ManyLightHash(seed ^ (sampleIndex * 0xD1B54A35u));
    uint slot = first;
    float selectedScore = 0.0f;
    float scoreSum = 0.0f;
    if (length > 1u) {
        /// @note Weighted reservoir selection scans only this stratum, so all samples together inspect at most 32 pool records.
        /// @see https://www.pbr-book.org/4ed/Sampling_Algorithms/Reservoir_Sampling Section A.2: weighted reservoir probabilities.
        [loop] for (uint candidate = first; candidate < last; ++candidate) {
            const PunctualLight proposed = FBZZ_PunctualAt(base, candidate);
            const float estimate = asfloat(gClusterIndices[base + FBZZ_MANY_LIGHT_POOL_SIZE + candidate]);
            const float score = FBZZ_ManyLightReceiverImportance(proposed, lookupPosition, receiver, estimate);
            scoreSum += score;
            const float random = (float)(FBZZ_ManyLightHash(stratumSeed ^ (candidate * 0x9E3779B9u)) >> 8u)
                * (1.0f / 16777216.0f);
            if (score > 0.0f && random * scoreSum < score) {
                slot = candidate;
                selectedScore = score;
            }
        }
        const uint mixture = FBZZ_ManyLightHash(stratumSeed ^ 0x68BC21EBu);
        if (scoreSum <= 0.0f || (float)(mixture >> 8u) * (1.0f / 16777216.0f) < 0.05f) {
            const float random = (float)(FBZZ_ManyLightHash(stratumSeed ^ 0x02E5BE93u) >> 8u) * (1.0f / 16777216.0f);
            slot = first + min((uint)(random * (float)length), length - 1u);
            const PunctualLight proposed = FBZZ_PunctualAt(base, slot);
            selectedScore = FBZZ_ManyLightReceiverImportance(proposed, lookupPosition, receiver,
                asfloat(gClusterIndices[base + FBZZ_MANY_LIGHT_POOL_SIZE + slot]));
        }
    }
    PunctualLight light = FBZZ_PunctualAt(base, slot);
    const float estimate = asfloat(gClusterIndices[base + FBZZ_MANY_LIGHT_POOL_SIZE + slot]);
    const float probability = scoreSum > 0.0f
        ? 0.95f * selectedScore / scoreSum + 0.05f / (float)length : rcp((float)length);
    light.intensity *= estimate / probability;
    return light;
}

PunctualLight FBZZ_PunctualEvaluationAtBudget(uint base, uint count, uint sampleIndex,
                                       float2 pixelCenter, float3 lookupPosition, uint evaluations, uint sequence, float3 normal)
{
    return FBZZ_PunctualEvaluationAtBudget(base, count, sampleIndex, pixelCenter, lookupPosition,
        evaluations, sequence, FBZZ_ManyLightReceiver(normal));
}

/// @note Volume sampling has no surface cosine, including the separate two-sample water budget.
PunctualLight FBZZ_PunctualEvaluationAtBudget(uint base, uint count, uint sampleIndex,
                                       float2 pixelCenter, float3 lookupPosition, uint evaluations, uint sequence)
{
    return FBZZ_PunctualEvaluationAtBudget(base, count, sampleIndex, pixelCenter, lookupPosition,
        evaluations, sequence, float3(0.0f, 0.0f, 0.0f));
}

PunctualLight FBZZ_PunctualEvaluationAt(uint base, uint count, uint sampleIndex,
                                       float2 pixelCenter, float3 lookupPosition, FBZZManyLightReceiver receiver)
{
    return FBZZ_PunctualEvaluationAtBudget(base, count, sampleIndex, pixelCenter, lookupPosition,
        FBZZ_PunctualEvaluationCount(count), 0u, receiver);
}
#endif
