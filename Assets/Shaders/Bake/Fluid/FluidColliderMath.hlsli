/// @file    FluidColliderMath.hlsli
/// @brief   液体と気体で共有する障害物の符号付き距離。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#ifndef FBZZ_FLUID_COLLIDER_MATH_HLSLI
#define FBZZ_FLUID_COLLIDER_MATH_HLSLI

/// @note 寸法・法線は各ソルバーの packing 済み値を使い、ここでは広げ直さない。
float FluidColliderSignedDistance(float4 centerShape, float3 size, float3 normal, float3 p)
{
    const float3 d = p - centerShape.xyz;
    const float shape = centerShape.w;
    float signedDistance = dot(d, normal);
    if (shape < 0.5f)
    {
        signedDistance = length(d) - size.x;
    }
    else if (shape < 1.5f)
    {
        const float3 q = abs(d) - size.xyz;
        signedDistance = length(max(q, 0.0f)) + min(max(q.x, max(q.y, q.z)), 0.0f);
    }
    else if (shape > 2.5f)
    {
        /// @note カプセル (3) / 円柱 (4)。x = 半径、y = 軸方向の長さの半分、軸は normal.xyz。
        const float3 a = normal;
        const float segR = size.x;
        const float segH = size.y;
        if (shape < 3.5f)
        {
            const float hc = clamp(dot(d, a), -segH, segH);
            signedDistance = length(d - a * hc) - segR;
        }
        else
        {
            const float h = dot(d, a);
            const float2 q = float2(length(d - a * h) - segR, abs(h) - segH);
            signedDistance = length(max(q, 0.0f)) + min(max(q.x, q.y), 0.0f);
        }
    }
    return signedDistance;
}

#endif ///< @note FBZZ_FLUID_COLLIDER_MATH_HLSLI
