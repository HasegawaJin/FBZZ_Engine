/// @file    CompositeWaterRectangle.hlsl
/// @brief   視線が孤立する一定水深矩形の画素で共通の水中積分を特殊化する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
/// @note 条件外の画素はdiscardし、同じ判定のcomplement passが通常の積分を描く。乾燥区間は両方で共有する。
/// @see Docs/design/water-gameplay-visuals.md
#define FBZZ_WATER_CACHED_FAST 1
#include "PostProcess/Color/Composite.hlsl"
