/// @file    Api.hpp
/// @brief   Fluid DLL の明示的な公開境界。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#pragma once
#if defined(FBZZFluid_EXPORTS)
#define FBZZ_FLUID_API __declspec(dllexport)
#else
#define FBZZ_FLUID_API __declspec(dllimport)
#endif
