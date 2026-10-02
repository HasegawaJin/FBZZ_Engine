/// @file    RayReflectionMotion.hlsli
/// @brief   Conservative single-terminal reflection and thin-transmission correspondence guides.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#ifndef FBZZ_RAY_REFLECTION_MOTION_HLSLI
#define FBZZ_RAY_REFLECTION_MOTION_HLSLI

/// @note Kind1 is an opaque reflected mesh terminal; kind2 is a thin transmitted mesh terminal with a known constant reflected environment.
/// @note The immutable scene/content key proves unchanged terminal geometry and material; owner/generation/primitive still identify the actual terminal.
struct RayReflectionMotionGuide
{
    float4 terminalPositionParameter;
    uint4 objectPrimitiveKind;
};
struct RayReflectionSurface
{
    float4 positionDepth, normalRoughness, geometricNormalOffset;
    uint4 objectMaterialValid;
    RayReflectionMotionGuide motion;
};

/// @note These optional guides describe direct first-interface branches only, never the dominant branch of a multi-path sum.
struct RayReflectionGlassMotion
{
    RayReflectionMotionGuide reflected, transmitted;
    float3 reflectedRadiance, transmittedRadiance;
};

#endif /// @note FBZZ_RAY_REFLECTION_MOTION_HLSLI
