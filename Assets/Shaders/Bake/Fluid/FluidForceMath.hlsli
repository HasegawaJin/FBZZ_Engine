/// @file    FluidForceMath.hlsli
/// @brief   GPU の気体・液体で共有する力の式と種類 ID。
/// @author  Hasegawa Jin
/// @date    2026-11-20

#ifndef FLUID_FORCE_MATH_HLSLI
#define FLUID_FORCE_MATH_HLSLI

/// @note 種類 ID は CPU が定数バッファに詰める FluidForceType (Fluid/FluidRecipe.hpp) と一致させる。
static const uint FLUID_FORCE_WIND = 0u;
static const uint FLUID_FORCE_ATTRACT = 1u;
static const uint FLUID_FORCE_REPULSE = 2u;
static const uint FLUID_FORCE_VORTEX = 3u;
static const uint FLUID_FORCE_NOISE = 4u;
static const uint FLUID_FORCE_DRAG = 5u;
static const float FLUID_FORCE_DIRECTION_EPSILON = 1.0e-5f;

/// @pre radius は正。全域に効く力と半径外の扱いは呼び出し元で決める。
/// @note 正の指数が大きいほど、力が中心付近に集中する。
float FluidForceFalloff(float distance, float radius, float power)
{
    return pow(saturate(1.0f - distance / radius), power);
}

/// @note 力ごとに別のノイズ領域を使い、時間でサンプル座標を動かす。
float3 FluidForceNoisePhase(uint index, float time, float speed)
{
    const float forceIndex = float(index);
    return float3(forceIndex * 17.31f, -time * speed, forceIndex * 5.73f);
}

/// @note 指数減衰で速度を 0 へ近づける。負の強さは速度を増幅するため 0 に丸める。
float3 FluidForceDragDelta(float3 velocity, float strength, float dt)
{
    return -velocity * (1.0f - exp(-max(strength, 0.0f) * dt));
}

#endif
