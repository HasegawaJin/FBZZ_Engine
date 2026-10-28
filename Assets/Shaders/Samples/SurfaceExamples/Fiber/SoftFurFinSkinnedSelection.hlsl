/// @file    SoftFurFinSkinnedSelection.hlsl
/// @brief   Project Fiber family wrapper sharing engine geometry, wind and coverage.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#define FIBER_FIN 1
#define FIBER_PASS 4
#define FIBER_SKINNED 1
#define FBZZ_FIBER_MATERIAL_FIELDS "Samples/SurfaceExamples/Fiber/AppearanceFields.hlsli"
#define FBZZ_FIBER_APPEARANCE "Samples/SurfaceExamples/Fiber/SoftFurAppearance.hlsli"
/// @note Offline precompile scans literal includes only; these inactive dependencies keep its cache current without including hooks twice.
#if 0
#include "Samples/SurfaceExamples/Fiber/AppearanceFields.hlsli"
#include "Samples/SurfaceExamples/Fiber/SoftFurAppearance.hlsli"
#endif
#include "Fiber/FiberSurface.hlsli"
