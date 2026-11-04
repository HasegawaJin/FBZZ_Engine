/// @file    Api.hpp
/// @brief   Math DLL の明示的な公開境界。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#pragma once
#if defined(FBZZMath_EXPORTS)
#define FBZZ_MATH_API __declspec(dllexport)
#else
#define FBZZ_MATH_API __declspec(dllimport)
#endif
