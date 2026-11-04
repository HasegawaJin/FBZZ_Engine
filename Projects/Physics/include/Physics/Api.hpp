/// @file    Api.hpp
/// @brief   Physics DLL の明示的な公開境界。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#pragma once
#if defined(FBZZPhysics_EXPORTS)
#define FBZZ_PHYSICS_API __declspec(dllexport)
#else
#define FBZZ_PHYSICS_API __declspec(dllimport)
#endif
