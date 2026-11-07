/// @file    TerrainNormalCone.hlsli
/// @brief   Conservative back-face tests against terrain meshlet normal cones.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#ifndef FBZZ_TERRAIN_NORMAL_CONE_HLSLI
#define FBZZ_TERRAIN_NORMAL_CONE_HLSLI

#ifndef FBZZ_TERRAIN_NORMAL_CONE_ENABLED
#define FBZZ_TERRAIN_NORMAL_CONE_ENABLED 1
#endif

/// @param packedCone Signed axis bytes and upward-rounded cutoff in boundsMin.w.
/// @param apexDistance Offset along the decoded axis from center to a point behind all triangle planes.
/// @param eye Local eye position for w=1, local direction toward the eye for w=2, or disabled.
/// @note Test the sign before squaring. The coordinate-scaled guard retains grazing and near-apex meshlets.
/// @see https://github.com/zeux/meshoptimizer/blob/master/src/meshoptimizer.h - Cluster bounds perspective and orthographic cone tests.
/// @see https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Samples/Desktop/D3D12MeshShaders/src/MeshletCull/MeshletAS.hlsl - Normal cone rejection.
bool IsTerrainMeshletBackFacing(uint packedCone, float apexDistance, float3 center, float4 eye)
{
#if FBZZ_TERRAIN_NORMAL_CONE_ENABLED
    const bool perspective = eye.w == 1.0f;
    const uint cutoffByte = packedCone >> 24u;
    if ((!perspective && eye.w != 2.0f) || cutoffByte == 0u || cutoffByte == 255u) return false;
    if (perspective && (!(apexDistance >= 0.0f) || !isfinite(apexDistance))) return false;

    const int3 axisBytes = asint(packedCone << uint3(24u, 16u, 8u)) >> 24;
    const float3 axis = float3(axisBytes) * (1.0f / 127.0f);
    const float3 origin = perspective ? center - axis * apexDistance : float3(0.0f, 0.0f, 0.0f);
    const float3 toEye = eye.xyz - origin;
    const float away = -dot(axis, toEye);
    [branch]
    if (!(away > 0.0f) || !isfinite(away)) return false;

    const float3 magnitude = max(abs(eye.xyz), abs(origin));
    const float guard = 2.0e-5f * max(1.0f, max(magnitude.x, max(magnitude.y, magnitude.z)));
    const float margin = away - guard;
    const float marginSquared = margin * margin;
    const float cutoff = float(cutoffByte) * (1.0f / 255.0f);
    const float limitSquared = (cutoff * cutoff) * dot(toEye, toEye);
    return margin > 0.0f && isfinite(marginSquared) && isfinite(limitSquared) && marginSquared > limitSquared;
#else
    return false;
#endif
}

#endif
