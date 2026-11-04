/// @file    Api.hpp
/// @brief   Graphics DLL の明示的な公開境界。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#pragma once
#if defined(FBZZGraphics_EXPORTS)
#define FBZZ_GRAPHICS_API __declspec(dllexport)
#else
#define FBZZ_GRAPHICS_API __declspec(dllimport)
#endif
