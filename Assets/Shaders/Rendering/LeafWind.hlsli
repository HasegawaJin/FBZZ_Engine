/// @file    LeafWind.hlsli
/// @brief   Material-scoped leaf motion shared by color, shadow, selection and velocity.
/// @author  Hasegawa Jin
/// @date    2026-11-05
#ifndef FBZZ_LEAF_WIND_HLSLI
#define FBZZ_LEAF_WIND_HLSLI
/// @note Strength is in model metres; the root mask fixes the base and world phase decorrelates neighbouring trees.
float3 FbzzLeafWindDeform(float3 localPosition, float2 uv, float4x4 objectWorld, float sampleTime,
                        bool deformFrame, inout float3 normal, inout float3 tangent)
{
    if (leafWindStrength <= 0.0f) return localPosition;
    const float3 origin = mul(float4(0, 0, 0, 1), objectWorld).xyz;
    const float phase = dot(origin.xz, float2(0.173f, 0.291f));
    const float mask = saturate((localPosition.y - leafWindRootHeight) * 0.5f);
    const float t = sampleTime * max(leafWindFrequency, 0.01f);
    const float gust = sin(t + phase) * 0.65f + sin(t * 0.47f + phase * 1.7f) * 0.35f;
    const float3 flutterAxis = float3(1.9f, 0.8f, 2.3f);
    const float flutterPhase = t * 3.7f + dot(localPosition, flutterAxis);
    const float flutterAmplitude = 0.2f + 0.3f * saturate(uv.y);
    const float flutter = sin(flutterPhase) * flutterAmplitude;
    const float2 offset = float2(gust + flutter, gust * 0.36f - flutter * 0.4f);
    if (deformFrame)
    {
        /// @note Differentiate the spatial wind field at fixed material UV. The vertex layout carries no UV gradient.
        const float maskSlope = mask > 0.0f && mask < 1.0f ? 0.5f : 0.0f;
        const float3 maskGradient = float3(0.0f, maskSlope, 0.0f);
        const float3 flutterGradient = cos(flutterPhase) * flutterAmplitude * flutterAxis;
        const float3 offsetX = leafWindStrength * (offset.x * maskGradient + mask * flutterGradient);
        const float3 offsetZ = leafWindStrength * (offset.y * maskGradient - 0.4f * mask * flutterGradient);
        const float3 jacobianX = float3(1.0f + offsetX.x, 0.0f, offsetZ.x);
        const float3 jacobianY = float3(offsetX.y, 1.0f, offsetZ.y);
        const float3 jacobianZ = float3(offsetX.z, 0.0f, 1.0f + offsetZ.z);
        const float3 cofactorX = cross(jacobianY, jacobianZ);
        const float determinant = dot(jacobianX, cofactorX);
        if (abs(determinant) > 1.0e-4f)
        {
            /// @note Normals use the inverse transpose, tangents use the deformation Jacobian.
            normal = sign(determinant) * (normal.x * cofactorX
                + normal.y * cross(jacobianZ, jacobianX) + normal.z * cross(jacobianX, jacobianY));
            tangent += float3(dot(offsetX, tangent), 0.0f, dot(offsetZ, tangent));
        }
    }
    localPosition.xz += leafWindStrength * mask * offset;
    return localPosition;
}

float3 FbzzLeafWind(float3 localPosition, float2 uv, float4x4 objectWorld, float sampleTime)
{
    float3 normal = 0.0f;
    float3 tangent = 0.0f;
    return FbzzLeafWindDeform(localPosition, uv, objectWorld, sampleTime, false, normal, tangent);
}
#endif
